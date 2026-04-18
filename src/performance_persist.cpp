#include "performance_persist.h"

#include "persist_file.h"

namespace major_midi
{
namespace
{
static constexpr uint8_t kMagic[4] = {'M', 'M', 'P', 'F'};
static constexpr uint8_t kVersion  = 1;
static constexpr size_t  kFileSize = 87;

void WriteConfig(uint8_t* out, const AppState& state)
{
    size_t offset = 0;
    out[offset++] = kMagic[0];
    out[offset++] = kMagic[1];
    out[offset++] = kMagic[2];
    out[offset++] = kMagic[3];
    out[offset++] = kVersion;

    uint16_t mute_mask = 0;
    for(size_t i = 0; i < 16; i++)
    {
        const ChannelState& channel = state.channels[i];
        out[offset++]               = channel.volume;
        out[offset++]               = channel.pan;
        out[offset++]               = channel.reverb_send;
        out[offset++]               = channel.chorus_send;
        out[offset++]               = static_cast<uint8_t>(channel.program_override);
        if(channel.muted)
            mute_mask |= static_cast<uint16_t>(1u << i);
    }

    out[offset++] = static_cast<uint8_t>((mute_mask >> 8) & 0xFF);
    out[offset++] = static_cast<uint8_t>(mute_mask & 0xFF);
}

bool ReadConfig(const uint8_t* in, AppState& state)
{
    if(in[0] != kMagic[0] || in[1] != kMagic[1] || in[2] != kMagic[2]
       || in[3] != kMagic[3] || in[4] != kVersion)
        return false;

    size_t offset = 5;
    for(size_t i = 0; i < 16; i++)
    {
        ChannelState& channel = state.channels[i];
        channel.volume        = in[offset++];
        channel.pan           = in[offset++];
        channel.reverb_send   = in[offset++];
        channel.chorus_send   = in[offset++];
        channel.program_override = static_cast<int8_t>(in[offset++]);

        if(channel.volume > 127 || channel.pan > 127 || channel.reverb_send > 127
           || channel.chorus_send > 127 || channel.program_override > 127)
            return false;
    }

    const uint16_t mute_mask = (uint16_t(in[offset]) << 8) | in[offset + 1];
    for(size_t i = 0; i < 16; i++)
        state.channels[i].muted = ((mute_mask >> i) & 0x01u) != 0;

    return true;
}
} // namespace

bool LoadPerformanceConfig(const char* path, AppState& state)
{
    uint8_t data[kFileSize]{};
    FIL&    file = SharedPersistFile();
    if(f_open(&file, path, FA_READ) != FR_OK)
        return false;

    UINT read = 0;
    const FRESULT read_result  = f_read(&file, data, kFileSize, &read);
    const FRESULT close_result = f_close(&file);
    if(read_result != FR_OK || close_result != FR_OK || read != kFileSize)
        return false;

    return ReadConfig(data, state);
}

bool SavePerformanceConfig(const char* path,
                           const AppState& state,
                           PersistWriteStage* failed_stage,
                           int*               result_code,
                           PersistProgressFn  progress_fn,
                           void*              progress_ctx)
{
    uint8_t data[kFileSize];
    WriteConfig(data, state);

    if(result_code != nullptr)
        *result_code = -1;

    if(failed_stage != nullptr)
        *failed_stage = PersistWriteStage::Open;
    if(progress_fn != nullptr)
        progress_fn(PersistWriteStage::Open, progress_ctx);
    FIL&          file        = SharedPersistFile();
    const FRESULT open_result = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
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
