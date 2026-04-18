#include "midi_routing_persist.h"
#include "persist_file.h"

namespace major_midi
{
namespace
{
static constexpr uint8_t kMagic[4] = {'M', 'M', 'M', 'R'};
static constexpr uint8_t kVersion  = 1;
static constexpr size_t  kFileSize = 9;

uint8_t PackOutput(const MidiOutputRouting& routing)
{
    return (routing.notes ? 0x01 : 0x00) | (routing.ccs ? 0x02 : 0x00)
           | (routing.programs ? 0x04 : 0x00) | (routing.transport ? 0x08 : 0x00)
           | (routing.clock ? 0x10 : 0x00);
}

void UnpackOutput(uint8_t packed, MidiOutputRouting& routing)
{
    routing.notes     = (packed & 0x01) != 0;
    routing.ccs       = (packed & 0x02) != 0;
    routing.programs  = (packed & 0x04) != 0;
    routing.transport = (packed & 0x08) != 0;
    routing.clock     = (packed & 0x10) != 0;
}

void WriteConfig(uint8_t* out, const MidiRoutingConfig& config)
{
    out[0] = kMagic[0];
    out[1] = kMagic[1];
    out[2] = kMagic[2];
    out[3] = kMagic[3];
    out[4] = kVersion;
    out[5] = PackOutput(config.usb);
    out[6] = PackOutput(config.uart);
    out[7] = config.usb_in_to_uart ? 1 : 0;
    out[8] = config.uart_in_to_usb ? 1 : 0;
}

bool ReadConfig(const uint8_t* in, MidiRoutingConfig& config)
{
    if(in[0] != kMagic[0] || in[1] != kMagic[1] || in[2] != kMagic[2]
       || in[3] != kMagic[3] || in[4] != kVersion)
        return false;

    if((in[5] & ~0x1F) != 0 || (in[6] & ~0x1F) != 0 || in[7] > 1 || in[8] > 1)
        return false;

    UnpackOutput(in[5], config.usb);
    UnpackOutput(in[6], config.uart);
    config.usb_in_to_uart = in[7] != 0;
    config.uart_in_to_usb = in[8] != 0;
    return true;
}
} // namespace

bool LoadMidiRoutingConfig(const char* path, MidiRoutingConfig& config)
{
    uint8_t data[kFileSize];
    FIL&    file = SharedPersistFile();
    if(f_open(&file, path, FA_READ) != FR_OK)
        return false;

    UINT read = 0;
    const FRESULT read_result  = f_read(&file, data, kFileSize, &read);
    const FRESULT close_result = f_close(&file);
    if(read_result != FR_OK || close_result != FR_OK || read != kFileSize)
        return false;

    return ReadConfig(data, config);
}

bool SaveMidiRoutingConfig(const char* path,
                           const MidiRoutingConfig& config,
                           PersistWriteStage*       failed_stage,
                           int*                     result_code,
                           PersistProgressFn        progress_fn,
                           void*                    progress_ctx)
{
    uint8_t data[kFileSize];
    WriteConfig(data, config);

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
