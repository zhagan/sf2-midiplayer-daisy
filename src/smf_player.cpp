#include "smf_player.h"

#include <cmath>
#include <cstring>

extern "C"
{
#include "ff.h"
}

namespace
{
uint32_t ReadUint32BE(FIL& f)
{
    uint8_t buf[4];
    UINT    read = 0;
    if(f_read(&f, buf, sizeof(buf), &read) != FR_OK || read != sizeof(buf))
        return 0;
    return (uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) | (uint32_t(buf[2]) << 8)
           | uint32_t(buf[3]);
}

uint16_t ReadUint16BE(FIL& f)
{
    uint8_t buf[2];
    UINT    read = 0;
    if(f_read(&f, buf, sizeof(buf), &read) != FR_OK || read != sizeof(buf))
        return 0;
    return (uint16_t(buf[0]) << 8) | uint16_t(buf[1]);
}
} // namespace

bool SmfPlayer::Open(const char* path)
{
    Close();
    settings_.Reset();
    std::strncpy(path_, path ? path : "", sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    if(f_open(&file_, path, FA_READ) != FR_OK)
    {
        return false;
    }

    uint8_t signature[4];
    UINT    read = 0;
    if(f_read(&file_, signature, sizeof(signature), &read) != FR_OK || read != sizeof(signature)
       || std::memcmp(signature, "MThd", 4) != 0)
    {
        Close();
        return false;
    }

    const uint32_t headerLen = ReadUint32BE(file_);
    const uint16_t format    = ReadUint16BE(file_);
    const uint16_t tracks    = ReadUint16BE(file_);
    divisions_               = ReadUint16BE(file_);

    if(divisions_ == 0)
    {
        Close();
        return false;
    }

    if(headerLen > 6)
    {
        const FSIZE_t pos = f_tell(&file_);
        f_lseek(&file_, pos + (headerLen - 6));
    }

    if(tracks == 0 || tracks > kMaxTracks)
    {
        Close();
        return false;
    }

    (void)format;
    trackCount_ = tracks;
    fileTempoUsec_ = 500000;
    tempo_         = 500000;
    ts_num_        = 4;
    ts_den_        = 4;
    for(uint16_t i = 0; i < trackCount_; i++)
    {
        trackNames_[i][0]  = '\0';
        trackHasName_[i]   = false;
        trackChannel_[i]   = -1;

        uint32_t len = 0;
        if(!SeekTrackHeader(len))
        {
            Close();
            return false;
        }
        tracks_[i].start        = f_tell(&file_);
        tracks_[i].pos          = tracks_[i].start;
        tracks_[i].length       = len;
        tracks_[i].remaining    = len;
        tracks_[i].running      = 0;
        tracks_[i].sampleFrac   = 0.0;
        tracks_[i].tickOffset   = 0;
        tracks_[i].sampleOffset = 0;
        tracks_[i].finished     = false;
        tracks_[i].hasEvent     = false;

        f_lseek(&file_, tracks_[i].start + len);
    }

    playing_ = false;
    open_    = true;

    LoadMajorMidiSettings();
    BuildTempoMap();
    UpdateSamplesPerTick();
    return true;
}

void SmfPlayer::Close()
{
    if(open_)
    {
        f_close(&file_);
    }

    open_       = false;
    playing_    = false;
    trackCount_ = 0;
    path_[0]    = '\0';
    settings_.Reset();
}

void SmfPlayer::SetSampleRate(float sr)
{
    sr_ = sr;
    UpdateSamplesPerTick();
}

void SmfPlayer::SetLookaheadSamples(uint64_t samples)
{
    lookahead_ = samples;
}

void SmfPlayer::SetTempoScale(float scale)
{
    if(scale < 0.01f)
        scale = 0.01f;
    tempo_scale_ = scale;
    UpdateSamplesPerTick();
}

void SmfPlayer::SetTempoScale(float scale, uint64_t sampleNow)
{
    if(scale < 0.01f)
        scale = 0.01f;
    if(scale == tempo_scale_)
        return;

    const double old_spt = samplesPerTick_;
    tempo_scale_          = scale;
    UpdateSamplesPerTick();
    const double new_spt = samplesPerTick_;

    if(!playing_ || old_spt <= 0.0 || new_spt <= 0.0)
        return;

    const uint64_t play_pos = (sampleNow >= startSample_) ? (sampleNow - startSample_) : 0;
    const double   ratio    = new_spt / old_spt;

    for(uint16_t i = 0; i < trackCount_; i++)
    {
        if(!tracks_[i].hasEvent)
            continue;

        uint64_t next_offset = tracks_[i].sampleOffset;
        if(next_offset < play_pos)
            next_offset = play_pos;

        const double rem = double(next_offset - play_pos);
        const double adj = rem * ratio;
        const uint64_t new_offset = play_pos + (uint64_t)llround(adj);

        tracks_[i].sampleOffset   = new_offset;
        tracks_[i].sampleFrac     = 0.0;
        tracks_[i].nextEv.atSample = startSample_ + tracks_[i].sampleOffset;
    }
}

void SmfPlayer::Start(uint64_t sampleNow)
{
    if(open_)
    {
        playing_     = true;
        startSample_ = sampleNow;
        for(uint16_t i = 0; i < trackCount_; i++)
        {
            tracks_[i].pos          = tracks_[i].start;
            tracks_[i].remaining    = tracks_[i].length;
            tracks_[i].running      = 0;
            tracks_[i].sampleFrac   = 0.0;
            tracks_[i].tickOffset   = 0;
            tracks_[i].sampleOffset = 0;
            tracks_[i].finished     = false;
            tracks_[i].hasEvent     = false;
            trackChannel_[i]        = -1;
        }

        for(uint16_t i = 0; i < trackCount_; i++)
            PrepareNextEvent(i, tracks_[i]);
    }
}

void SmfPlayer::Stop()
{
    playing_ = false;
}

void SmfPlayer::SeekToSample(uint64_t targetSample, uint64_t nowSample)
{
    if(!open_)
        return;

    playing_     = true;
    startSample_ = (nowSample >= targetSample) ? (nowSample - targetSample) : 0;

    for(uint16_t i = 0; i < trackCount_; i++)
    {
        tracks_[i].pos          = tracks_[i].start;
        tracks_[i].remaining    = tracks_[i].length;
        tracks_[i].running      = 0;
        tracks_[i].sampleFrac   = 0.0;
        tracks_[i].tickOffset   = 0;
        tracks_[i].sampleOffset = 0;
        tracks_[i].finished     = false;
        tracks_[i].hasEvent     = false;
        trackChannel_[i]        = -1;
    }

    for(uint16_t i = 0; i < trackCount_; i++)
    {
        MidiEv ev{};
        while(true)
        {
            if(!ParseNextEvent(i, tracks_[i], ev))
            {
                tracks_[i].hasEvent = false;
                break;
            }
            if(tracks_[i].sampleOffset >= targetSample)
            {
                tracks_[i].nextEv  = ev;
                tracks_[i].hasEvent = true;
                break;
            }
        }
    }
}

bool SmfPlayer::IsPlaying() const
{
    return playing_;
}

void SmfPlayer::Pump(EventQueue<2048>& queue, uint64_t sampleNow)
{
    if(!open_ || !playing_)
        return;

    while(!queue.IsFull())
    {
        const uint64_t limitSample = sampleNow + lookahead_;
        uint16_t nextIdx    = kMaxTracks;
        uint64_t nextSample = UINT64_MAX;
        for(uint16_t i = 0; i < trackCount_; i++)
        {
            if(tracks_[i].hasEvent && tracks_[i].nextEv.atSample < nextSample)
            {
                nextSample = tracks_[i].nextEv.atSample;
                nextIdx    = i;
            }
        }

        if(nextIdx == kMaxTracks)
        {
            playing_ = false;
            return;
        }

        if(nextSample > limitSample)
            return;

        queue.Push(tracks_[nextIdx].nextEv);
        tracks_[nextIdx].hasEvent = false;
        PrepareNextEvent(nextIdx, tracks_[nextIdx]);
    }
}

bool SmfPlayer::ParseNextEvent(uint16_t trackIndex, TrackState& trk, MidiEv& out)
{
    while(true)
    {
        if(trk.remaining == 0)
        {
            trk.finished = true;
            return false;
        }

        if(f_lseek(&file_, trk.pos) != FR_OK)
        {
            trk.finished = true;
            return false;
        }

        uint32_t deltaTicks = 0;
        if(!ReadVarLen(trk, deltaTicks))
        {
            trk.finished = true;
            return false;
        }

        trk.tickOffset += deltaTicks;
        trk.sampleOffset = SamplesFromTicks(trk.tickOffset);
        trk.sampleFrac = 0.0;
        const uint64_t eventSample = startSample_ + trk.sampleOffset;

        uint8_t statusByte = 0;
        if(!ReadTrackByte(trk, statusByte))
        {
            trk.finished = true;
            return false;
        }

        if(statusByte == 0xFF)
        {
            uint8_t type = 0;
            if(!ReadTrackByte(trk, type))
            {
                trk.finished = true;
                return false;
            }
            uint32_t length = 0;
            if(!ReadVarLen(trk, length))
            {
                trk.finished = true;
                return false;
            }
            if(type == 0x51 && length == 3)
            {
                uint8_t buf[3];
                for(uint32_t i = 0; i < 3; i++)
                {
                    if(!ReadTrackByte(trk, buf[i]))
                    {
                        trk.finished = true;
                        return false;
                    }
                }
                if(!HasBpmOverride())
                {
                    tempo_ = (uint32_t(buf[0]) << 16) | (uint32_t(buf[1]) << 8)
                             | uint32_t(buf[2]);
                    UpdateSamplesPerTick();
                }
            }
            else if(type == 0x58 && length == 4)
            {
                uint8_t buf[4];
                for(uint32_t i = 0; i < 4; i++)
                {
                    if(!ReadTrackByte(trk, buf[i]))
                    {
                        trk.finished = true;
                        return false;
                    }
                }
                ts_num_ = buf[0];
                ts_den_ = 1 << buf[1];
            }
            else if(type == 0x03) // Track name
            {
                const uint32_t copy = (length >= (kTrackNameMax - 1))
                                          ? (kTrackNameMax - 1)
                                          : length;
                for(uint32_t i = 0; i < copy; i++)
                {
                    uint8_t b = 0;
                    if(!ReadTrackByte(trk, b))
                    {
                        trk.finished = true;
                        return false;
                    }
                    trackNames_[trackIndex][i] = (char)b;
                }
                trackNames_[trackIndex][copy] = '\0';
                trackHasName_[trackIndex]     = true;
                if(length > copy)
                {
                    if(!SkipBytes(trk, length - copy))
                    {
                        trk.finished = true;
                        return false;
                    }
                }
            }
            else
            {
                if(!SkipBytes(trk, length))
                {
                    trk.finished = true;
                    return false;
                }
            }

            if(type == 0x2F)
            {
                MidiEv ev{};
                ev.type     = EvType::AllNotesOff;
                ev.atSample = eventSample;
                trk.finished = true;
                out = ev;
                return true;
            }
            continue;
        }

        if(statusByte == 0xF0 || statusByte == 0xF7)
        {
            uint32_t length = 0;
            if(!ReadVarLen(trk, length) || !SkipBytes(trk, length))
            {
                trk.finished = true;
                return false;
            }
            continue;
        }

        uint8_t status = statusByte;
        uint8_t data1  = 0;
        if(status < 0x80)
        {
            if(trk.running == 0)
            {
                trk.finished = true;
                return false;
            }
            data1  = status;
            status = trk.running;
        }
        else
        {
            trk.running = status;
            if(!ReadTrackByte(trk, data1))
            {
                trk.finished = true;
                return false;
            }
        }

        uint8_t data2 = 0;
        switch(status & 0xF0)
        {
            case 0x80:
            case 0x90:
            case 0xB0:
            case 0xE0:
                if(!ReadTrackByte(trk, data2))
                {
                    trk.finished = true;
                    return false;
                }
                break;
            default:
                break;
        }

        MidiEv ev{};
        ev.atSample = eventSample;
        ev.ch       = status & 0x0F;
        if(trackChannel_[trackIndex] < 0)
            trackChannel_[trackIndex] = (int8_t)ev.ch;

        switch(status & 0xF0)
        {
            case 0x80: // Note off
                ev.type = EvType::NoteOff;
                ev.a    = data1;
                break;
            case 0x90: // Note on
                if(data2 == 0)
                {
                    ev.type = EvType::NoteOff;
                    ev.a    = data1;
                }
                else
                {
                    ev.type = EvType::NoteOn;
                    ev.a    = data1;
                    ev.b    = data2;
                }
                break;
            case 0xB0: // Control change - handle All Notes Off (0x7B)
                if(data1 == 0x7B || data1 == 0x78)
                {
                    ev.type = EvType::AllNotesOff;
                }
                else
                {
                    ev.type = EvType::ControlChange;
                    ev.a    = data1;
                    ev.b    = data2;
                }
                break;
            case 0xC0: // Program change
                ev.type = EvType::Program;
                ev.a    = data1;
                break;
            case 0xE0: // Pitch bend
                ev.type = EvType::PitchBend;
                ev.a    = data1;
                ev.b    = data2;
                break;
            default:
                continue;
        }

        out = ev;
        return true;
    }
}

bool SmfPlayer::PrepareNextEvent(uint16_t trackIndex, TrackState& trk)
{
    if(trk.finished)
        return false;

    MidiEv ev{};
    if(!ParseNextEvent(trackIndex, trk, ev))
    {
        trk.hasEvent = false;
        return false;
    }
    trk.nextEv  = ev;
    trk.hasEvent = true;
    return true;
}

const char* SmfPlayer::GetTrackNameForChannel(uint8_t ch) const
{
    for(uint16_t i = 0; i < trackCount_; i++)
    {
        if(trackChannel_[i] == (int8_t)ch && trackHasName_[i])
            return trackNames_[i];
    }
    return "";
}

uint32_t SmfPlayer::RemainingBytes() const
{
    uint32_t total = 0;
    for(uint16_t i = 0; i < trackCount_; i++)
        total += tracks_[i].remaining;
    return total;
}

uint64_t SmfPlayer::SamplesPerQuarter() const
{
    return static_cast<uint64_t>(samplesPerTick_ * double(divisions_));
}

bool SmfPlayer::ReadTrackByte(TrackState& trk, uint8_t& b)
{
    if(trk.remaining == 0)
        return false;

    UINT read = 0;
    if(f_read(&file_, &b, 1, &read) != FR_OK || read != 1)
        return false;

    trk.pos++;
    trk.remaining--;
    return true;
}

bool SmfPlayer::ReadVarLen(TrackState& trk, uint32_t& value)
{
    value = 0;
    uint8_t byte = 0;
    do
    {
        if(!ReadTrackByte(trk, byte))
            return false;
        value = (value << 7) | (byte & 0x7F);
    } while(byte & 0x80);
    return true;
}

bool SmfPlayer::SkipBytes(TrackState& trk, uint32_t count)
{
    while(count--)
    {
        uint8_t tmp = 0;
        if(!ReadTrackByte(trk, tmp))
            return false;
    }
    return true;
}

bool SmfPlayer::SeekTrackHeader(uint32_t& length)
{
    uint8_t chunkId[4];
    UINT    read = 0;
    if(f_read(&file_, chunkId, sizeof(chunkId), &read) != FR_OK || read != sizeof(chunkId)
       || std::memcmp(chunkId, "MTrk", 4) != 0)
    {
        return false;
    }

    length = ReadUint32BE(file_);
    return length > 0;
}

void SmfPlayer::UpdateSamplesPerTick()
{
    tempo_ = EffectiveTempoUsec();
    if(divisions_ == 0)
    {
        samplesPerTick_ = 0.0;
        return;
    }

    samplesPerTick_
        = ((tempo_ / double(tempo_scale_)) * sr_) / (double(divisions_) * 1000000.0);
}

uint64_t SmfPlayer::SamplesFromTicks(uint64_t ticks) const
{
    if(divisions_ == 0)
        return 0;

    const uint16_t count = (tempoCount_ == 0) ? 1 : tempoCount_;
    const uint32_t defaultTempo = (tempoCount_ == 0) ? tempo_ : tempoUsec_[0];

    uint64_t curTick = 0;
    double   samples = 0.0;

    for(uint16_t i = 0; i < count; i++)
    {
        const uint32_t tempo = (tempoCount_ == 0) ? defaultTempo : tempoUsec_[i];
        const uint64_t nextTick = (i + 1 < count) ? tempoTicks_[i + 1] : ticks;
        const uint64_t segEnd = (ticks < nextTick) ? ticks : nextTick;
        if(segEnd <= curTick)
            break;

        const double samplesPerTick
            = ((tempo / double(tempo_scale_)) * sr_) / (double(divisions_) * 1000000.0);
        samples += double(segEnd - curTick) * samplesPerTick;
        curTick = segEnd;
        if(curTick >= ticks)
            break;
    }

    return (uint64_t)llround(samples);
}

uint64_t SmfPlayer::SamplesFromTicksRange(uint64_t startTicks, uint64_t lengthTicks) const
{
    if(lengthTicks == 0)
        return 0;
    const uint64_t a = SamplesFromTicks(startTicks);
    const uint64_t b = SamplesFromTicks(startTicks + lengthTicks);
    return (b >= a) ? (b - a) : 0;
}

void SmfPlayer::InsertTempoPoint(uint32_t tick, uint32_t tempo)
{
    if(tempoCount_ >= kMaxTempoPoints)
        return;
    tempoTicks_[tempoCount_] = tick;
    tempoUsec_[tempoCount_]  = tempo;
    tempoCount_++;
}

uint64_t SmfPlayer::TicksFromSamples(uint64_t samples) const
{
    if(divisions_ == 0)
        return 0;

    const uint16_t count = (tempoCount_ == 0) ? 1 : tempoCount_;
    const uint32_t defaultTempo = (tempoCount_ == 0) ? tempo_ : tempoUsec_[0];

    uint64_t curTick = 0;
    double   remSamples = (double)samples;

    for(uint16_t i = 0; i < count; i++)
    {
        const uint32_t tempo = (tempoCount_ == 0) ? defaultTempo : tempoUsec_[i];
        const uint64_t nextTick = (i + 1 < count) ? tempoTicks_[i + 1] : UINT64_MAX;

        const double spt
            = ((tempo / double(tempo_scale_)) * sr_) / (double(divisions_) * 1000000.0);
        if(spt <= 0.0)
            break;

        if(nextTick == UINT64_MAX)
        {
            curTick += (uint64_t)floor(remSamples / spt);
            return curTick;
        }

        const uint64_t segTicks = (nextTick > curTick) ? (nextTick - curTick) : 0;
        const double   segSamples = (double)segTicks * spt;

        if(remSamples < segSamples)
        {
            curTick += (uint64_t)floor(remSamples / spt);
            return curTick;
        }

        remSamples -= segSamples;
        curTick = nextTick;
        if(remSamples <= 0.0)
            return curTick;
    }

    return curTick;
}

void SmfPlayer::BuildTempoMap()
{
    tempoCount_ = 0;
    if(trackCount_ == 0)
        return;

    if(HasBpmOverride())
    {
        InsertTempoPoint(0, EffectiveTempoUsec());
        tempo_ = EffectiveTempoUsec();
        return;
    }

    InsertTempoPoint(0, fileTempoUsec_);

    for(uint16_t ti = 0; ti < trackCount_; ti++)
    {
        TrackState trk{};
        trk.start     = tracks_[ti].start;
        trk.pos       = tracks_[ti].start;
        trk.length    = tracks_[ti].length;
        trk.remaining = tracks_[ti].length;
        trk.running   = 0;

        uint64_t absTicks = 0;

        while(trk.remaining > 0)
        {
            if(f_lseek(&file_, trk.pos) != FR_OK)
                break;
            uint32_t deltaTicks = 0;
            if(!ReadVarLen(trk, deltaTicks))
                break;
            absTicks += deltaTicks;

            uint8_t statusByte = 0;
            if(!ReadTrackByte(trk, statusByte))
                break;

            if(statusByte == 0xFF)
            {
                uint8_t type = 0;
                if(!ReadTrackByte(trk, type))
                    break;
                uint32_t length = 0;
                if(!ReadVarLen(trk, length))
                    break;

                if(type == 0x51 && length == 3)
                {
                    uint8_t buf[3];
                    for(uint32_t i = 0; i < 3; i++)
                    {
                        if(!ReadTrackByte(trk, buf[i]))
                            return;
                    }
                    const uint32_t tempo
                        = (uint32_t(buf[0]) << 16) | (uint32_t(buf[1]) << 8)
                          | uint32_t(buf[2]);
                    if(tempoCount_ == 1 && absTicks == 0)
                        fileTempoUsec_ = tempo;
                    InsertTempoPoint((uint32_t)absTicks, tempo);
                }
                else
                {
                    if(!SkipBytes(trk, length))
                        break;
                }
                if(type == 0x2F)
                    break;
                continue;
            }

            if(statusByte == 0xF0 || statusByte == 0xF7)
            {
                uint32_t length = 0;
                if(!ReadVarLen(trk, length) || !SkipBytes(trk, length))
                    break;
                continue;
            }

            uint8_t status = statusByte;
            uint8_t data1  = 0;
            if(status < 0x80)
            {
                if(trk.running == 0)
                    break;
                data1  = status;
                status = trk.running;
            }
            else
            {
                trk.running = status;
                if(!ReadTrackByte(trk, data1))
                    break;
            }

            switch(status & 0xF0)
            {
                case 0x80:
                case 0x90:
                case 0xA0:
                case 0xB0:
                case 0xE0:
                {
                    uint8_t data2 = 0;
                    if(!ReadTrackByte(trk, data2))
                        return;
                }
                break;
                case 0xC0:
                case 0xD0:
                    break;
                default:
                    break;
            }
        }
    }

    if(tempoCount_ == 0)
        return;

    for(uint16_t i = 1; i < tempoCount_; i++)
    {
        const uint32_t keyTick  = tempoTicks_[i];
        const uint32_t keyTempo = tempoUsec_[i];
        int            j        = (int)i - 1;
        while(j >= 0 && tempoTicks_[j] > keyTick)
        {
            tempoTicks_[j + 1] = tempoTicks_[j];
            tempoUsec_[j + 1]  = tempoUsec_[j];
            j--;
        }
        tempoTicks_[j + 1] = keyTick;
        tempoUsec_[j + 1]  = keyTempo;
    }

    uint16_t out = 0;
    for(uint16_t i = 0; i < tempoCount_; i++)
    {
        if(out == 0 || tempoTicks_[i] != tempoTicks_[out - 1])
        {
            tempoTicks_[out] = tempoTicks_[i];
            tempoUsec_[out]  = tempoUsec_[i];
            out++;
        }
        else
        {
            tempoUsec_[out - 1] = tempoUsec_[i];
        }
    }
    tempoCount_ = out;
    if(tempoCount_ > 0)
    {
        fileTempoUsec_ = tempoUsec_[0];
        tempo_         = tempoUsec_[0];
    }
}

void SmfPlayer::LoadMajorMidiSettings()
{
    settings_.Reset();
    if(!open_ || path_[0] == '\0')
        return;
    major_midi::ReadMajorMidiMetaEvent(path_, settings_, nullptr);
}

bool SmfPlayer::HasBpmOverride() const
{
    return major_midi::HasMajorMidiBpmOverride(settings_);
}

uint32_t SmfPlayer::EffectiveTempoUsec() const
{
    if(HasBpmOverride())
        return major_midi::MajorMidiTempoUsecPerQuarter(settings_);
    return fileTempoUsec_;
}

bool SmfPlayer::SaveSettings()
{
    if(path_[0] == '\0')
        return false;
    return major_midi::WriteMajorMidiMetaEvent(path_, settings_);
}
