#include "major_midi_settings.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace major_midi
{
namespace
{
static constexpr uint8_t kSignature[4] = {'M', 'M', 'I', 'D'};
static constexpr uint8_t kFlagProgramOverrides = 1 << 0;
static constexpr uint8_t kFlagPanOverrides     = 1 << 1;
static constexpr uint8_t kFlagLoopSettings     = 1 << 2;
static constexpr uint8_t kFlagChannelVolume    = 1 << 3;
static constexpr uint8_t kFlagChannelReverb    = 1 << 4;
static constexpr uint8_t kFlagChannelChorus    = 1 << 5;
static constexpr uint8_t kFlagChannelMute      = 1 << 6;

uint8_t Clamp7Bit(int value)
{
    if(value < 0)
        return 0;
    if(value > 127)
        return 127;
    return (uint8_t)value;
}

uint32_t ReadUint32BE(const uint8_t* ptr)
{
    return (uint32_t(ptr[0]) << 24) | (uint32_t(ptr[1]) << 16)
           | (uint32_t(ptr[2]) << 8) | uint32_t(ptr[3]);
}

void WriteUint32BE(uint8_t* ptr, uint32_t value)
{
    ptr[0] = (uint8_t)((value >> 24) & 0xFF);
    ptr[1] = (uint8_t)((value >> 16) & 0xFF);
    ptr[2] = (uint8_t)((value >> 8) & 0xFF);
    ptr[3] = (uint8_t)(value & 0xFF);
}

void WriteVarLen(std::vector<uint8_t>& out, uint32_t value)
{
    uint8_t buf[5];
    int     idx = 0;
    buf[idx++]  = (uint8_t)(value & 0x7F);
    while((value >>= 7) != 0)
        buf[idx++] = (uint8_t)(0x80 | (value & 0x7F));
    while(idx-- > 0)
        out.push_back(buf[idx]);
}

bool ReadVarLen(const uint8_t* data,
                size_t         size,
                size_t&        offset,
                uint32_t&      value,
                size_t*        out_len = nullptr)
{
    value          = 0;
    const size_t start = offset;
    uint8_t      byte  = 0;
    do
    {
        if(offset >= size)
            return false;
        byte  = data[offset++];
        value = (value << 7) | (byte & 0x7F);
    } while(byte & 0x80);
    if(out_len)
        *out_len = offset - start;
    return true;
}

bool HasAnyOverride(const int8_t* values)
{
    for(uint8_t i = 0; i < kChannelCount; i++)
    {
        if(values[i] >= 0)
            return true;
    }
    return false;
}

bool LoadFile(const char* path, std::vector<uint8_t>& out)
{
    FILE* f = std::fopen(path, "rb");
    if(!f)
        return false;
    if(std::fseek(f, 0, SEEK_END) != 0)
    {
        std::fclose(f);
        return false;
    }
    const long size = std::ftell(f);
    if(size < 0)
    {
        std::fclose(f);
        return false;
    }
    if(std::fseek(f, 0, SEEK_SET) != 0)
    {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)size);
    if(size > 0 && std::fread(out.data(), 1, (size_t)size, f) != (size_t)size)
    {
        std::fclose(f);
        return false;
    }
    std::fclose(f);
    return true;
}

bool SaveFile(const char* path, const std::vector<uint8_t>& data)
{
    FILE* f = std::fopen(path, "wb");
    if(!f)
        return false;
    const bool ok
        = data.empty()
              || (std::fwrite(data.data(), 1, data.size(), f) == data.size());
    std::fclose(f);
    return ok;
}

bool FindTrackZero(const std::vector<uint8_t>& file,
                   size_t&                      track_len_offset,
                   size_t&                      track_data_offset,
                   uint32_t&                    track_size)
{
    if(file.size() < 14 || std::memcmp(file.data(), "MThd", 4) != 0)
        return false;
    const uint32_t header_len = ReadUint32BE(file.data() + 4);
    const size_t   first_track = 8u + (size_t)header_len;
    if(first_track + 8 > file.size())
        return false;
    if(std::memcmp(file.data() + first_track, "MTrk", 4) != 0)
        return false;
    track_len_offset  = first_track + 4;
    track_data_offset = first_track + 8;
    track_size        = ReadUint32BE(file.data() + track_len_offset);
    return track_data_offset + track_size <= file.size();
}

bool FindMetaInTrack0(const std::vector<uint8_t>& file,
                      MajorMidiMetaInfo&          info)
{
    info = {};

    size_t   track_len_offset  = 0;
    size_t   track_data_offset = 0;
    uint32_t track_size        = 0;
    if(!FindTrackZero(file, track_len_offset, track_data_offset, track_size))
        return false;

    const uint8_t* track = file.data() + track_data_offset;
    size_t         pos   = 0;
    uint8_t        running = 0;
    while(pos < track_size)
    {
        const size_t delta_start = pos;
        uint32_t delta = 0;
        if(!ReadVarLen(track, track_size, pos, delta))
            return false;
        if(pos >= track_size)
            return false;
        const size_t event_start = delta_start;
        uint8_t      status      = track[pos++];
        if(status == 0xFF)
        {
            if(pos >= track_size)
                return false;
            const uint8_t type = track[pos++];
            uint32_t      len  = 0;
            size_t        len_size = 0;
            if(!ReadVarLen(track, track_size, pos, len, &len_size))
                return false;
            if(pos + len > track_size)
                return false;
            if(type == 0x7F)
            {
                MajorMidiSettings parsed;
                uint8_t           version = 0;
                info.found               = true;
                if(ParseMajorMidiPayload(track + pos, len, parsed, &version))
                {
                    info.valid          = true;
                    info.version        = version;
                    info.event_offset   = (uint32_t)(track_data_offset + event_start);
                    info.payload_offset = (uint32_t)(track_data_offset + pos);
                    info.payload_size   = len;
                    info.event_size = (uint32_t)((pos + len) - event_start);
                    return true;
                }
            }
            if(type == 0x2F)
                return true;
            pos += len;
            continue;
        }

        if(status == 0xF0 || status == 0xF7)
        {
            uint32_t len = 0;
            if(!ReadVarLen(track, track_size, pos, len))
                return false;
            if(pos + len > track_size)
                return false;
            pos += len;
            continue;
        }

        uint8_t data1 = 0;
        if(status < 0x80)
        {
            if(running == 0)
                return false;
            data1  = status;
            status = running;
        }
        else
        {
            running = status;
            if(pos >= track_size)
                return false;
            data1 = track[pos++];
        }
        (void)data1;
        switch(status & 0xF0)
        {
            case 0x80:
            case 0x90:
            case 0xA0:
            case 0xB0:
            case 0xE0:
                if(pos >= track_size)
                    return false;
                pos++;
                break;
            case 0xC0:
            case 0xD0: break;
            default: return false;
        }
    }
    return true;
}
} // namespace

void MajorMidiSettings::Reset()
{
    master_volume_max = 127;
    expression_max    = 127;
    reverb_max        = 127;
    chorus_max        = 127;
    transpose         = 0;
    bpm_override      = 0;
    loop_enabled      = false;
    loop_start_measure = 1;
    loop_start_beat   = 1;
    loop_start_sub    = 1;
    loop_length_beats = 16;
    for(uint8_t i = 0; i < kChannelCount; i++)
    {
        program_override[i] = kNoOverride;
        pan_override[i]     = kNoOverride;
        volume[i]           = 100;
        reverb_send[i]      = 0;
        chorus_send[i]      = 0;
        muted[i]            = false;
    }
}

bool ParseMajorMidiPayload(const uint8_t* data,
                           size_t         size,
                           MajorMidiSettings& settings,
                           uint8_t*       out_version)
{
    settings.Reset();
    if(size < 13 || std::memcmp(data, kSignature, sizeof(kSignature)) != 0)
        return false;

    const uint8_t version = data[4];
    const uint8_t flags   = data[5];
    if(version != 1 && version != MajorMidiSettings::kVersion)
        return false;

    settings.master_volume_max = Clamp7Bit(data[6]);
    settings.expression_max    = Clamp7Bit(data[7]);
    settings.reverb_max        = Clamp7Bit(data[8]);
    settings.chorus_max        = Clamp7Bit(data[9]);
    settings.transpose         = (int8_t)data[10];
    settings.bpm_override      = (uint16_t(data[11]) << 8);
    settings.bpm_override |= data[12];
    size_t offset = 13;

    if(flags & kFlagLoopSettings)
    {
        if(offset + 7 > size)
            return false;
        settings.loop_enabled
            = data[offset++] != 0;
        settings.loop_start_measure
            = (uint16_t(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        settings.loop_start_beat  = data[offset++];
        settings.loop_start_sub   = data[offset++];
        settings.loop_length_beats
            = (uint16_t(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if(settings.loop_start_measure < 1)
            settings.loop_start_measure = 1;
        if(settings.loop_start_beat < 1)
            settings.loop_start_beat = 1;
        if(settings.loop_start_sub < 1)
            settings.loop_start_sub = 1;
        if(settings.loop_length_beats < 1)
            settings.loop_length_beats = 1;
    }

    if(flags & kFlagProgramOverrides)
    {
        if(offset + kChannelCount > size)
            return false;
        for(uint8_t i = 0; i < kChannelCount; i++)
        {
            const int8_t value = (int8_t)data[offset++];
            settings.program_override[i]
                = (value < 0) ? MajorMidiSettings::kNoOverride
                              : (int8_t)Clamp7Bit(value);
        }
    }
    if(flags & kFlagPanOverrides)
    {
        if(offset + kChannelCount > size)
            return false;
        for(uint8_t i = 0; i < kChannelCount; i++)
        {
            const int8_t value = (int8_t)data[offset++];
            settings.pan_override[i]
                = (value < 0) ? MajorMidiSettings::kNoOverride
                              : (int8_t)Clamp7Bit(value);
        }
    }
    if(flags & kFlagChannelVolume)
    {
        if(offset + kChannelCount > size)
            return false;
        for(uint8_t i = 0; i < kChannelCount; i++)
            settings.volume[i] = Clamp7Bit(data[offset++]);
    }
    if(flags & kFlagChannelReverb)
    {
        if(offset + kChannelCount > size)
            return false;
        for(uint8_t i = 0; i < kChannelCount; i++)
            settings.reverb_send[i] = Clamp7Bit(data[offset++]);
    }
    if(flags & kFlagChannelChorus)
    {
        if(offset + kChannelCount > size)
            return false;
        for(uint8_t i = 0; i < kChannelCount; i++)
            settings.chorus_send[i] = Clamp7Bit(data[offset++]);
    }
    if(flags & kFlagChannelMute)
    {
        if(offset + 2 > size)
            return false;
        const uint16_t mute_mask = (uint16_t(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        for(uint8_t i = 0; i < kChannelCount; i++)
            settings.muted[i] = ((mute_mask >> i) & 0x01u) != 0;
    }
    if(out_version)
        *out_version = version;
    return true;
}

size_t BuildMajorMidiPayload(const MajorMidiSettings& settings,
                             uint8_t*                 out,
                             size_t                   capacity)
{
    uint8_t flags = 0;
    if(HasAnyOverride(settings.program_override))
        flags |= kFlagProgramOverrides;
    if(HasAnyOverride(settings.pan_override))
        flags |= kFlagPanOverrides;
    if(settings.loop_enabled || settings.loop_start_measure != 1
       || settings.loop_start_beat != 1 || settings.loop_start_sub != 1
       || settings.loop_length_beats != 16)
        flags |= kFlagLoopSettings;
    flags |= kFlagChannelVolume;
    flags |= kFlagChannelReverb;
    flags |= kFlagChannelChorus;
    for(uint8_t i = 0; i < kChannelCount; i++)
    {
        if(settings.muted[i])
        {
            flags |= kFlagChannelMute;
            break;
        }
    }

    size_t size = 13;
    if(flags & kFlagLoopSettings)
        size += 7;
    if(flags & kFlagProgramOverrides)
        size += kChannelCount;
    if(flags & kFlagPanOverrides)
        size += kChannelCount;
    if(flags & kFlagChannelVolume)
        size += kChannelCount;
    if(flags & kFlagChannelReverb)
        size += kChannelCount;
    if(flags & kFlagChannelChorus)
        size += kChannelCount;
    if(flags & kFlagChannelMute)
        size += 2;
    if(!out || capacity < size)
        return size;

    std::memcpy(out, kSignature, sizeof(kSignature));
    out[4]  = MajorMidiSettings::kVersion;
    out[5]  = flags;
    out[6]  = settings.master_volume_max;
    out[7]  = settings.expression_max;
    out[8]  = settings.reverb_max;
    out[9]  = settings.chorus_max;
    out[10] = (uint8_t)settings.transpose;
    out[11] = (uint8_t)((settings.bpm_override >> 8) & 0xFF);
    out[12] = (uint8_t)(settings.bpm_override & 0xFF);
    size_t offset = 13;
    if(flags & kFlagLoopSettings)
    {
        out[offset++] = settings.loop_enabled ? 1 : 0;
        out[offset++] = (uint8_t)((settings.loop_start_measure >> 8) & 0xFF);
        out[offset++] = (uint8_t)(settings.loop_start_measure & 0xFF);
        out[offset++] = settings.loop_start_beat;
        out[offset++] = settings.loop_start_sub;
        out[offset++] = (uint8_t)((settings.loop_length_beats >> 8) & 0xFF);
        out[offset++] = (uint8_t)(settings.loop_length_beats & 0xFF);
    }
    if(flags & kFlagProgramOverrides)
    {
        for(uint8_t i = 0; i < kChannelCount; i++)
            out[offset++] = (uint8_t)settings.program_override[i];
    }
    if(flags & kFlagPanOverrides)
    {
        for(uint8_t i = 0; i < kChannelCount; i++)
            out[offset++] = (uint8_t)settings.pan_override[i];
    }
    if(flags & kFlagChannelVolume)
    {
        for(uint8_t i = 0; i < kChannelCount; i++)
            out[offset++] = settings.volume[i];
    }
    if(flags & kFlagChannelReverb)
    {
        for(uint8_t i = 0; i < kChannelCount; i++)
            out[offset++] = settings.reverb_send[i];
    }
    if(flags & kFlagChannelChorus)
    {
        for(uint8_t i = 0; i < kChannelCount; i++)
            out[offset++] = settings.chorus_send[i];
    }
    if(flags & kFlagChannelMute)
    {
        uint16_t mute_mask = 0;
        for(uint8_t i = 0; i < kChannelCount; i++)
        {
            if(settings.muted[i])
                mute_mask |= static_cast<uint16_t>(1u << i);
        }
        out[offset++] = static_cast<uint8_t>((mute_mask >> 8) & 0xFF);
        out[offset++] = static_cast<uint8_t>(mute_mask & 0xFF);
    }
    return offset;
}

uint8_t ScaleMajorMidiController(uint8_t file_value, uint8_t max_value)
{
    return (uint8_t)((uint16_t(file_value) * uint16_t(max_value)) / 127u);
}

uint8_t ResolveMajorMidiProgram(uint8_t channel,
                                uint8_t file_program,
                                const MajorMidiSettings& settings)
{
    if(channel >= kChannelCount || settings.program_override[channel] < 0)
        return file_program;
    return Clamp7Bit(settings.program_override[channel]);
}

bool HasMajorMidiProgramOverride(uint8_t channel,
                                 const MajorMidiSettings& settings)
{
    return channel < kChannelCount && settings.program_override[channel] >= 0;
}

bool HasMajorMidiPanOverride(uint8_t channel, const MajorMidiSettings& settings)
{
    return channel < kChannelCount && settings.pan_override[channel] >= 0;
}

uint8_t ResolveMajorMidiPan(uint8_t channel,
                            uint8_t file_pan,
                            const MajorMidiSettings& settings)
{
    if(channel >= kChannelCount || settings.pan_override[channel] < 0)
        return file_pan;
    return Clamp7Bit(settings.pan_override[channel]);
}

bool HasMajorMidiBpmOverride(const MajorMidiSettings& settings)
{
    return settings.bpm_override > 0;
}

uint32_t MajorMidiTempoUsecPerQuarter(const MajorMidiSettings& settings)
{
    if(!HasMajorMidiBpmOverride(settings))
        return 500000;
    if(settings.bpm_override == 0)
        return 500000;
    return 60000000u / settings.bpm_override;
}

bool ReadMajorMidiMetaEvent(const char*        path,
                            MajorMidiSettings& settings,
                            MajorMidiMetaInfo* out_info)
{
    settings.Reset();
    std::vector<uint8_t> file;
    if(!LoadFile(path, file))
        return false;

    MajorMidiMetaInfo info;
    if(!FindMetaInTrack0(file, info))
        return false;
    if(info.valid)
    {
        uint8_t version = 0;
        if(!ParseMajorMidiPayload(file.data() + info.payload_offset,
                                  info.payload_size,
                                  settings,
                                  &version))
            return false;
        info.version = version;
    }
    if(out_info)
        *out_info = info;
    return true;
}

bool WriteMajorMidiMetaEvent(const char*              path,
                             const MajorMidiSettings& settings)
{
    std::vector<uint8_t> file;
    if(!LoadFile(path, file))
        return false;

    size_t   track_len_offset  = 0;
    size_t   track_data_offset = 0;
    uint32_t track_size        = 0;
    if(!FindTrackZero(file, track_len_offset, track_data_offset, track_size))
        return false;

    MajorMidiMetaInfo info;
    if(!FindMetaInTrack0(file, info))
        return false;

    const size_t payload_size = BuildMajorMidiPayload(settings, nullptr, 0);
    std::vector<uint8_t> meta;
    meta.reserve(3 + payload_size + 5);
    meta.push_back(0x00);
    meta.push_back(0xFF);
    meta.push_back(0x7F);
    WriteVarLen(meta, (uint32_t)payload_size);
    const size_t old = meta.size();
    meta.resize(old + payload_size);
    (void)BuildMajorMidiPayload(settings, meta.data() + old, payload_size);

    std::vector<uint8_t> new_track;
    if(info.valid)
    {
        const size_t rel_event = info.event_offset - track_data_offset;
        const size_t rel_after = rel_event + info.event_size;
        new_track.insert(new_track.end(),
                         file.begin() + track_data_offset,
                         file.begin() + track_data_offset + rel_event);
        new_track.insert(new_track.end(), meta.begin(), meta.end());
        new_track.insert(new_track.end(),
                         file.begin() + track_data_offset + rel_after,
                         file.begin() + track_data_offset + track_size);
    }
    else
    {
        size_t insert_pos = track_size;
        size_t pos        = 0;
        while(pos < track_size)
        {
            size_t   delta_start = pos;
            uint32_t delta       = 0;
            if(!ReadVarLen(file.data() + track_data_offset, track_size, pos, delta))
                return false;
            if(track_data_offset + pos >= file.size())
                return false;
            uint8_t status = file[track_data_offset + pos++];
            if(status == 0xFF)
            {
                if(track_data_offset + pos >= file.size())
                    return false;
                const uint8_t type = file[track_data_offset + pos++];
                uint32_t      len  = 0;
                if(!ReadVarLen(file.data() + track_data_offset, track_size, pos, len))
                    return false;
                if(pos + len > track_size)
                    return false;
                if(type == 0x2F)
                {
                    insert_pos = delta_start;
                    break;
                }
                pos += len;
                continue;
            }
            if(status == 0xF0 || status == 0xF7)
            {
                uint32_t len = 0;
                if(!ReadVarLen(file.data() + track_data_offset, track_size, pos, len))
                    return false;
                if(pos + len > track_size)
                    return false;
                pos += len;
                continue;
            }

            uint8_t running = status;
            if(status < 0x80)
                return false;
            switch(running & 0xF0)
            {
                case 0x80:
                case 0x90:
                case 0xA0:
                case 0xB0:
                case 0xE0: pos += 2; break;
                case 0xC0:
                case 0xD0: pos += 1; break;
                default: return false;
            }
            if(pos > track_size)
                return false;
        }

        new_track.insert(new_track.end(),
                         file.begin() + track_data_offset,
                         file.begin() + track_data_offset + insert_pos);
        new_track.insert(new_track.end(), meta.begin(), meta.end());
        new_track.insert(new_track.end(),
                         file.begin() + track_data_offset + insert_pos,
                         file.begin() + track_data_offset + track_size);
    }

    const uint32_t new_track_size = (uint32_t)new_track.size();
    std::vector<uint8_t> out;
    out.reserve(file.size() - track_size + new_track.size());
    out.insert(out.end(), file.begin(), file.begin() + track_data_offset);
    out.insert(out.end(), new_track.begin(), new_track.end());
    out.insert(out.end(),
               file.begin() + track_data_offset + track_size,
               file.end());
    WriteUint32BE(out.data() + track_len_offset, new_track_size);
    return SaveFile(path, out);
}
} // namespace major_midi
