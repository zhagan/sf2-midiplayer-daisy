#include "cv_gate_persist.h"

extern "C"
{
#include "ff.h"
}

namespace major_midi
{
namespace
{
static constexpr uint8_t kMagic[4]   = {'M', 'M', 'C', 'V'};
static constexpr uint8_t kVersion    = 1;
static constexpr UINT    kFileSize   = 25;

void WriteConfig(uint8_t* out, const CvGateConfig& config)
{
    size_t offset = 0;
    out[offset++] = kMagic[0];
    out[offset++] = kMagic[1];
    out[offset++] = kMagic[2];
    out[offset++] = kMagic[3];
    out[offset++] = kVersion;

    for(size_t i = 0; i < 2; i++)
    {
        out[offset++] = static_cast<uint8_t>(config.cv_in[i].mode);
        out[offset++] = config.cv_in[i].channel;
        out[offset++] = config.cv_in[i].cc;
    }

    for(size_t i = 0; i < 2; i++)
    {
        out[offset++] = static_cast<uint8_t>(config.gate_out[i].mode);
        out[offset++] = config.gate_out[i].channel;
        out[offset++] = static_cast<uint8_t>(config.gate_out[i].sync_resolution);
    }

    for(size_t i = 0; i < 2; i++)
    {
        out[offset++] = static_cast<uint8_t>(config.cv_out[i].mode);
        out[offset++] = config.cv_out[i].channel;
        out[offset++] = config.cv_out[i].cc;
        out[offset++] = static_cast<uint8_t>(config.cv_out[i].priority);
    }
}

bool ReadConfig(const uint8_t* in, CvGateConfig& config)
{
    if(in[0] != kMagic[0] || in[1] != kMagic[1] || in[2] != kMagic[2]
       || in[3] != kMagic[3] || in[4] != kVersion)
        return false;

    size_t offset = 5;

    for(size_t i = 0; i < 2; i++)
    {
        config.cv_in[i].mode    = static_cast<CvInMode>(in[offset++]);
        config.cv_in[i].channel = in[offset++];
        config.cv_in[i].cc      = in[offset++];
        if(static_cast<int>(config.cv_in[i].mode) < 0
           || config.cv_in[i].mode > CvInMode::ChannelCc)
            return false;
        if(config.cv_in[i].channel > 15 || config.cv_in[i].cc > 127)
            return false;
    }

    for(size_t i = 0; i < 2; i++)
    {
        config.gate_out[i].mode            = static_cast<GateOutMode>(in[offset++]);
        config.gate_out[i].channel         = in[offset++];
        config.gate_out[i].sync_resolution = static_cast<SyncResolution>(in[offset++]);
        if(static_cast<int>(config.gate_out[i].mode) < 0
           || config.gate_out[i].mode > GateOutMode::ChannelGate)
            return false;
        if(config.gate_out[i].channel > 15
           || config.gate_out[i].sync_resolution > SyncResolution::Div64)
            return false;
    }

    for(size_t i = 0; i < 2; i++)
    {
        config.cv_out[i].mode     = static_cast<CvOutMode>(in[offset++]);
        config.cv_out[i].channel  = in[offset++];
        config.cv_out[i].cc       = in[offset++];
        config.cv_out[i].priority = static_cast<NotePriority>(in[offset++]);
        if(static_cast<int>(config.cv_out[i].mode) < 0
           || config.cv_out[i].mode > CvOutMode::ChannelCc)
            return false;
        if(config.cv_out[i].channel > 15 || config.cv_out[i].cc > 127
           || config.cv_out[i].priority > NotePriority::Lowest)
            return false;
    }

    return true;
}
} // namespace

bool LoadCvGateConfig(const char* path, CvGateConfig& config)
{
    FIL  file;
    UINT read = 0;
    uint8_t data[kFileSize];

    if(f_open(&file, path, FA_READ) != FR_OK)
        return false;

    const FRESULT result = f_read(&file, data, kFileSize, &read);
    f_close(&file);
    if(result != FR_OK || read != kFileSize)
        return false;

    return ReadConfig(data, config);
}

bool SaveCvGateConfig(const char* path, const CvGateConfig& config)
{
    FIL  file;
    UINT written = 0;
    uint8_t data[kFileSize];
    WriteConfig(data, config);

    if(f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK)
        return false;

    const FRESULT result = f_write(&file, data, kFileSize, &written);
    f_close(&file);
    return result == FR_OK && written == kFileSize;
}

} // namespace major_midi
