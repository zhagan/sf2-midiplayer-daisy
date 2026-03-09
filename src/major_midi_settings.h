#pragma once

#include <cstddef>
#include <cstdint>

namespace major_midi
{
static constexpr uint8_t kChannelCount = 16;

struct MajorMidiSettings
{
    static constexpr uint8_t kVersion = 1;
    static constexpr int8_t  kNoOverride = -1;

    uint8_t master_volume_max = 127;
    uint8_t expression_max    = 127;
    uint8_t reverb_max        = 127;
    uint8_t chorus_max        = 127;
    int8_t  transpose         = 0;
    uint16_t bpm_override     = 0;
    bool     loop_enabled     = false;
    uint16_t loop_start_measure = 1;
    uint8_t  loop_start_beat  = 1;
    uint8_t  loop_start_sub   = 1;
    uint16_t loop_length_beats = 16;
    int8_t  program_override[kChannelCount];
    int8_t  pan_override[kChannelCount];

    void Reset();
};

struct MajorMidiMetaInfo
{
    bool     found         = false;
    bool     valid         = false;
    uint8_t  version       = 0;
    uint32_t event_offset  = 0;
    uint32_t event_size    = 0;
    uint32_t payload_offset = 0;
    uint32_t payload_size  = 0;
};

bool ParseMajorMidiPayload(const uint8_t* data,
                           size_t         size,
                           MajorMidiSettings& settings,
                           uint8_t*       out_version = nullptr);
size_t BuildMajorMidiPayload(const MajorMidiSettings& settings,
                             uint8_t*                 out,
                             size_t                   capacity);
uint8_t ScaleMajorMidiController(uint8_t file_value, uint8_t max_value);
uint8_t ResolveMajorMidiProgram(uint8_t channel,
                                uint8_t file_program,
                                const MajorMidiSettings& settings);
bool    HasMajorMidiProgramOverride(uint8_t channel,
                                    const MajorMidiSettings& settings);
bool    HasMajorMidiPanOverride(uint8_t channel,
                                const MajorMidiSettings& settings);
uint8_t ResolveMajorMidiPan(uint8_t channel,
                            uint8_t file_pan,
                            const MajorMidiSettings& settings);
bool    HasMajorMidiBpmOverride(const MajorMidiSettings& settings);
uint32_t MajorMidiTempoUsecPerQuarter(const MajorMidiSettings& settings);

bool ReadMajorMidiMetaEvent(const char*            path,
                            MajorMidiSettings&     settings,
                            MajorMidiMetaInfo*     out_info = nullptr);
bool WriteMajorMidiMetaEvent(const char*                 path,
                             const MajorMidiSettings&    settings);
} // namespace major_midi
