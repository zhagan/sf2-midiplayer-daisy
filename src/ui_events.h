#pragma once

#include <cstdint>

namespace major_midi
{

enum class UiEventType : uint8_t
{
    BankButtonPressed,
    PlayButtonPressed,
    ShiftComboPressed,
    EncoderPressed,
    EncoderLongPress,
    EncoderTurn,
    KnobMoved,
};

struct UiEvent
{
    UiEventType type  = UiEventType::EncoderPressed;
    uint8_t     index = 0;
    int32_t     delta = 0;
    float       value = 0.0f;
};

} // namespace major_midi
