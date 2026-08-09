#include "boot_state_persist.h"

#include <cstring>

#include "media_library.h"
#include "persist_file.h"

namespace major_midi
{
namespace
{
static constexpr char    kBootStatePath[] = "0:/major_midi_boot.cfg";
static constexpr uint8_t kMagic[4]        = {'M', 'M', 'B', 'T'};
static constexpr uint8_t kVersion         = 8;
static constexpr size_t  kLegacyNameMax   = 32;
static constexpr size_t  kLegacyTimeoutOffset = 5 + kLegacyNameMax;
static constexpr size_t  kLegacyKnobModeOffset = kLegacyTimeoutOffset + 2;
static constexpr size_t  kLegacyEncoderDirOffset = kLegacyKnobModeOffset + 1;
static constexpr size_t  kLegacyFreezeUiOffset = kLegacyEncoderDirOffset + 1;
static constexpr size_t  kLegacyFileSize = kLegacyFreezeUiOffset + 1;
static constexpr size_t  kNameOffset      = 5;
static constexpr size_t  kTimeoutOffset   = kNameOffset + MediaLibrary::kPathMax;
static constexpr size_t  kKnobModeOffset  = kTimeoutOffset + 2;
static constexpr size_t  kEncoderDirOffset = kKnobModeOffset + 1;
static constexpr size_t  kFreezeUiOffset  = kEncoderDirOffset + 1;
static constexpr size_t  kOledXOffsetOffset = kFreezeUiOffset + 1;
static constexpr size_t  kFileSizeV6      = kOledXOffsetOffset + 1;
static constexpr size_t  kCv1PitchScaleOffset = kOledXOffsetOffset + 1;
static constexpr size_t  kCv2PitchScaleOffset = kCv1PitchScaleOffset + 2;
static constexpr size_t  kFileSizeV7      = kCv2PitchScaleOffset + 2;
static constexpr size_t  kMasterVolumeOffset = kCv2PitchScaleOffset + 2;
static constexpr size_t  kFileSize        = kMasterVolumeOffset + 2;

uint16_t ReadUint16BE(const uint8_t* data)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

void WriteUint16BE(uint8_t* data, uint16_t value)
{
    data[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    data[1] = static_cast<uint8_t>(value & 0xFFu);
}

bool ValidKnobPickupMode(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(KnobPickupMode::Jump);
}

bool ValidEncoderDirection(uint8_t raw)
{
    return raw <= static_cast<uint8_t>(EncoderDirection::Reversed);
}

bool ValidOledXOffset(uint8_t raw)
{
    return raw <= 8;
}

bool ValidPitchScale(uint16_t raw)
{
    return raw >= 900 && raw <= 1100;
}

bool ValidMasterVolumePct(uint16_t raw)
{
    return raw <= 200;
}

} // namespace

bool LoadBootState(AppState& state, char* midi_name, size_t midi_name_sz)
{
    if(midi_name == nullptr || midi_name_sz == 0)
        return false;

    midi_name[0] = '\0';
    uint8_t data[kFileSize]{};
    FIL&    file = SharedPersistFile();
    if(f_open(&file, kBootStatePath, FA_READ) != FR_OK)
        return false;

    UINT read = 0;
    const FRESULT read_result  = f_read(&file, data, kFileSize, &read);
    const FRESULT close_result = f_close(&file);
    if(read_result != FR_OK || close_result != FR_OK || read < (5 + kLegacyNameMax))
        return false;
    if(data[0] != kMagic[0] || data[1] != kMagic[1] || data[2] != kMagic[2] || data[3] != kMagic[3])
        return false;

    const uint8_t version = data[4];
    if(version < 1 || version > kVersion)
        return false;

    const size_t name_field_sz = version >= 5 ? MediaLibrary::kPathMax : kLegacyNameMax;
    const size_t copy_len = name_field_sz < (midi_name_sz - 1) ? name_field_sz
                                                                : (midi_name_sz - 1);
    std::memcpy(midi_name, data + kNameOffset, copy_len);
    midi_name[copy_len] = '\0';

    if(version >= 6 && read >= kFileSizeV6)
    {
        const uint16_t timeout_s = ReadUint16BE(data + kTimeoutOffset);
        state.screen_saver_timeout_s = timeout_s;
        if(ValidKnobPickupMode(data[kKnobModeOffset]))
            state.knob_pickup_mode = static_cast<KnobPickupMode>(data[kKnobModeOffset]);
        if(ValidEncoderDirection(data[kEncoderDirOffset]))
            state.encoder_direction = static_cast<EncoderDirection>(data[kEncoderDirOffset]);
        if(ValidOledXOffset(data[kOledXOffsetOffset]))
            state.oled_x_offset = data[kOledXOffsetOffset];
        if(version >= 7 && read >= kFileSizeV7)
        {
            const uint16_t cv1_scale = ReadUint16BE(data + kCv1PitchScaleOffset);
            const uint16_t cv2_scale = ReadUint16BE(data + kCv2PitchScaleOffset);
            if(ValidPitchScale(cv1_scale))
                state.cv1_pitch_scale = cv1_scale;
            if(ValidPitchScale(cv2_scale))
                state.cv2_pitch_scale = cv2_scale;
            if(version >= 8 && read >= kFileSize)
            {
                const uint16_t master_volume_pct = ReadUint16BE(data + kMasterVolumeOffset);
                if(ValidMasterVolumePct(master_volume_pct))
                    state.master_volume_pct = master_volume_pct;
            }
        }
    }
    else if(version >= 5 && read >= kFreezeUiOffset + 1)
    {
        const uint16_t timeout_s = ReadUint16BE(data + kTimeoutOffset);
        state.screen_saver_timeout_s = timeout_s;
        if(ValidKnobPickupMode(data[kKnobModeOffset]))
            state.knob_pickup_mode = static_cast<KnobPickupMode>(data[kKnobModeOffset]);
        if(ValidEncoderDirection(data[kEncoderDirOffset]))
            state.encoder_direction = static_cast<EncoderDirection>(data[kEncoderDirOffset]);
    }
    else if(version >= 3 && read >= kLegacyEncoderDirOffset + 1)
    {
        const uint16_t timeout_s = ReadUint16BE(data + kLegacyTimeoutOffset);
        state.screen_saver_timeout_s = timeout_s;
        if(ValidKnobPickupMode(data[kLegacyKnobModeOffset]))
            state.knob_pickup_mode = static_cast<KnobPickupMode>(data[kLegacyKnobModeOffset]);
        if(ValidEncoderDirection(data[kLegacyEncoderDirOffset]))
            state.encoder_direction = static_cast<EncoderDirection>(data[kLegacyEncoderDirOffset]);
    }
    else if(version >= 2 && read >= kLegacyKnobModeOffset + 1)
    {
        const uint16_t timeout_s = ReadUint16BE(data + kLegacyTimeoutOffset);
        state.screen_saver_timeout_s = timeout_s;
        if(ValidKnobPickupMode(data[kLegacyKnobModeOffset]))
            state.knob_pickup_mode = static_cast<KnobPickupMode>(data[kLegacyKnobModeOffset]);
    }
    return midi_name[0] != '\0';
}

bool SaveBootState(const AppState& state, const char* midi_name)
{
    if(midi_name == nullptr || midi_name[0] == '\0')
        return false;

    uint8_t data[kFileSize]{};
    data[0] = kMagic[0];
    data[1] = kMagic[1];
    data[2] = kMagic[2];
    data[3] = kMagic[3];
    data[4] = kVersion;

    const size_t name_len = std::strlen(midi_name);
    const size_t copy_len = name_len < (MediaLibrary::kPathMax - 1) ? name_len
                                                                    : (MediaLibrary::kPathMax - 1);
    std::memcpy(data + kNameOffset, midi_name, copy_len);
    WriteUint16BE(data + kTimeoutOffset, state.screen_saver_timeout_s);
    data[kKnobModeOffset] = static_cast<uint8_t>(state.knob_pickup_mode);
    data[kEncoderDirOffset] = static_cast<uint8_t>(state.encoder_direction);
    data[kFreezeUiOffset] = 0u;
    data[kOledXOffsetOffset] = state.oled_x_offset;
    WriteUint16BE(data + kCv1PitchScaleOffset, state.cv1_pitch_scale);
    WriteUint16BE(data + kCv2PitchScaleOffset, state.cv2_pitch_scale);
    WriteUint16BE(data + kMasterVolumeOffset, state.master_volume_pct);

    FIL&          file        = SharedPersistFile();
    const FRESULT open_result = f_open(&file, kBootStatePath, FA_CREATE_ALWAYS | FA_WRITE);
    if(open_result != FR_OK)
        return false;

    UINT written = 0;
    const FRESULT write_result = f_write(&file, data, kFileSize, &written);
    const FRESULT close_result = f_close(&file);
    return write_result == FR_OK && close_result == FR_OK && written == kFileSize;
}

} // namespace major_midi
