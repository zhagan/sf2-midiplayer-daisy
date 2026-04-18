#include "cv_gate_persist.h"
#include "persist_file.h"

namespace major_midi
{
namespace
{
static constexpr uint8_t kMagic[4]   = {'M', 'M', 'C', 'V'};
static constexpr uint8_t kVersion    = 2;
static constexpr size_t  kFileSizeV1 = 25;
static constexpr size_t  kFileSize   = 27;

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
        out[offset++] = static_cast<uint8_t>(config.gate_in[i].mode);

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
       || in[3] != kMagic[3] || (in[4] != 1 && in[4] != kVersion))
        return false;

    size_t offset = 5;
    const bool has_gate_in = in[4] >= 2;

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

    if(has_gate_in)
    {
        for(size_t i = 0; i < 2; i++)
        {
            config.gate_in[i].mode = static_cast<GateInMode>(in[offset++]);
            if(config.gate_in[i].mode > GateInMode::SyncIn)
                return false;
        }
    }
    else
    {
        config.gate_in[0].mode = GateInMode::SyncIn;
        config.gate_in[1].mode = GateInMode::Off;
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
    uint8_t data[kFileSize]{};
    FIL&    file = SharedPersistFile();
    if(f_open(&file, path, FA_READ) != FR_OK)
        return false;

    UINT read = 0;
    const FRESULT read_result  = f_read(&file, data, kFileSize, &read);
    const FRESULT close_result = f_close(&file);
    if(read_result != FR_OK || close_result != FR_OK
       || (read != kFileSize && read != kFileSizeV1))
        return false;

    return ReadConfig(data, config);
}

bool SaveCvGateConfig(const char* path,
                      const CvGateConfig& config,
                      PersistWriteStage*  failed_stage,
                      int*                result_code,
                      PersistProgressFn   progress_fn,
                      void*               progress_ctx)
{
    uint8_t data[kFileSize];
    WriteConfig(data, config);

    if(result_code != nullptr)
        *result_code = -1;

    if(failed_stage != nullptr)
        *failed_stage = PersistWriteStage::Open;
    if(progress_fn != nullptr)
        progress_fn(PersistWriteStage::Open, progress_ctx);
    FIL&           file        = SharedPersistFile();
    const FRESULT  open_result = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if(open_result != FR_OK)
    {
        if(result_code != nullptr)
            *result_code = static_cast<int>(open_result);
        return false;
    }

    if(failed_stage != nullptr)
        *failed_stage = PersistWriteStage::Write;
    if(progress_fn != nullptr)
        progress_fn(PersistWriteStage::Write, progress_ctx);
    UINT written = 0;
    const FRESULT write_result = f_write(&file, data, kFileSize, &written);
    if(write_result != FR_OK)
    {
        if(result_code != nullptr)
            *result_code = static_cast<int>(write_result);
    }
    if(failed_stage != nullptr)
        *failed_stage = PersistWriteStage::Close;
    if(progress_fn != nullptr)
        progress_fn(PersistWriteStage::Close, progress_ctx);
    const FRESULT close_result = f_close(&file);
    if(result_code != nullptr)
        *result_code = static_cast<int>(close_result != FR_OK ? close_result : write_result);
    if(failed_stage != nullptr)
        *failed_stage = PersistWriteStage::Done;
    if(progress_fn != nullptr)
        progress_fn(PersistWriteStage::Done, progress_ctx);
    return write_result == FR_OK && written == kFileSize && close_result == FR_OK;
}

} // namespace major_midi
