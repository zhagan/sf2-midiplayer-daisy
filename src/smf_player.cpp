#include "smf_player.h"

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
    for(uint16_t i = 0; i < trackCount_; i++)
    {
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
        tracks_[i].sampleOffset = 0;
        tracks_[i].finished     = false;
        tracks_[i].hasEvent     = false;

        f_lseek(&file_, tracks_[i].start + len);
    }

    playing_ = false;
    open_    = true;

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
            tracks_[i].sampleOffset = 0;
            tracks_[i].finished     = false;
            tracks_[i].hasEvent     = false;
        }

        for(uint16_t i = 0; i < trackCount_; i++)
            PrepareNextEvent(tracks_[i]);
    }
}

bool SmfPlayer::IsPlaying() const
{
    return playing_;
}

void SmfPlayer::Pump(EventQueue<2048>& queue, uint64_t)
{
    if(!open_ || !playing_)
        return;

    while(!queue.IsFull())
    {
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

        queue.Push(tracks_[nextIdx].nextEv);
        tracks_[nextIdx].hasEvent = false;
        PrepareNextEvent(tracks_[nextIdx]);
    }
}

bool SmfPlayer::ParseNextEvent(TrackState& trk, MidiEv& out)
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

        const double deltaSamples = deltaTicks * samplesPerTick_;
        trk.sampleFrac += deltaSamples;
        const uint64_t samples = static_cast<uint64_t>(trk.sampleFrac);
        trk.sampleFrac -= samples;
        trk.sampleOffset += samples;
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
                tempo_ = (uint32_t(buf[0]) << 16) | (uint32_t(buf[1]) << 8)
                         | uint32_t(buf[2]);
                UpdateSamplesPerTick();
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
                if(data1 == 0x7B)
                {
                    ev.type = EvType::AllNotesOff;
                }
                else
                {
                    continue;
                }
                break;
            case 0xC0: // Program change
                ev.type = EvType::Program;
                ev.a    = data1;
                break;
            default:
                continue;
        }

        out = ev;
        return true;
    }
}

bool SmfPlayer::PrepareNextEvent(TrackState& trk)
{
    if(trk.finished)
        return false;

    MidiEv ev{};
    if(!ParseNextEvent(trk, ev))
    {
        trk.hasEvent = false;
        return false;
    }
    trk.nextEv  = ev;
    trk.hasEvent = true;
    return true;
}

uint32_t SmfPlayer::RemainingBytes() const
{
    uint32_t total = 0;
    for(uint16_t i = 0; i < trackCount_; i++)
        total += tracks_[i].remaining;
    return total;
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
    if(divisions_ == 0)
    {
        samplesPerTick_ = 0.0;
        return;
    }

    samplesPerTick_ = (tempo_ * sr_) / (double(divisions_) * 1000000.0);
}
