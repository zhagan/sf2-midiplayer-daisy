#include "ui_input.h"
#include <cmath>

namespace major_midi
{

namespace
{
constexpr uint32_t kEncoderLongPressMs = 700;
constexpr uint8_t kEncSwitchPin  = 10; // MCP GPB2
constexpr uint8_t kPlayButtonPin = 11; // MCP GPB3
constexpr uint8_t kBankButton0   = 12; // MCP GPB4
constexpr uint8_t kBankButton1   = 13; // MCP GPB5
constexpr uint8_t kBankButton2   = 14; // MCP GPB6
constexpr uint8_t kBankButton3   = 15; // MCP GPB7

float Clamp01(float v)
{
    if(v < 0.0f)
        return 0.0f;
    if(v > 1.0f)
        return 1.0f;
    return v;
}
} // namespace

void UiHardwareInput::Init(daisy::patch_sm::DaisyPatchSM& hw)
{
    hw_ = &hw;

    encoder_.Init(daisy::patch_sm::DaisyPatchSM::D9,
                  daisy::patch_sm::DaisyPatchSM::D10,
                  daisy::patch_sm::DaisyPatchSM::D10);

    daisy::Mcp23017::Config cfg;
    cfg.transport_config.Defaults();
    cfg.transport_config.i2c_address               = 0x20;
    cfg.transport_config.i2c_config.periph         = daisy::I2CHandle::Config::Peripheral::I2C_1;
    cfg.transport_config.i2c_config.speed          = daisy::I2CHandle::Config::Speed::I2C_1MHZ;
    cfg.transport_config.i2c_config.mode           = daisy::I2CHandle::Config::Mode::I2C_MASTER;
    cfg.transport_config.i2c_config.pin_config.scl = daisy::patch_sm::DaisyPatchSM::B7;
    cfg.transport_config.i2c_config.pin_config.sda = daisy::patch_sm::DaisyPatchSM::B8;
    mcp_.Init(cfg);
    mcp_.PortMode(daisy::MCPPort::A, 0x00, 0x00, 0x00);
    mcp_.PortMode(daisy::MCPPort::B, 0xFF, 0xFF, 0x00);
    mcp_.WritePort(daisy::MCPPort::A, led_mask_);
}

void UiHardwareInput::ControlRateTick()
{
    encoder_.Debounce();
    pending_encoder_delta_ += -encoder_.Increment();
}

void UiHardwareInput::Sample(RawInputState& state)
{
    mcp_.Read();

    state.encoder_delta   = pending_encoder_delta_;
    pending_encoder_delta_ = 0;
    state.shift           = mcp_.GetPin(kEncSwitchPin) == 0;
    state.play_button     = mcp_.GetPin(kPlayButtonPin) == 0;
    state.bank_buttons[0] = mcp_.GetPin(kBankButton0) == 0;
    state.bank_buttons[1] = mcp_.GetPin(kBankButton1) == 0;
    state.bank_buttons[2] = mcp_.GetPin(kBankButton2) == 0;
    state.bank_buttons[3] = mcp_.GetPin(kBankButton3) == 0;

    state.knobs[0] = Clamp01(hw_->GetAdcValue(daisy::patch_sm::CV_1));
    state.knobs[1] = Clamp01(hw_->GetAdcValue(daisy::patch_sm::CV_2));
    state.knobs[2] = Clamp01(hw_->GetAdcValue(daisy::patch_sm::CV_3));
    state.knobs[3] = Clamp01(hw_->GetAdcValue(daisy::patch_sm::CV_4));
}

void UiHardwareInput::SetLedMask(uint8_t mask)
{
    if(mask == led_mask_)
        return;
    led_mask_ = mask & 0x0F;
    mcp_.WritePort(daisy::MCPPort::A, led_mask_);
}

size_t UiEventTranslator::Translate(const RawInputState& raw,
                                    uint32_t             now_ms,
                                    UiEvent*             out,
                                    size_t               max_events)
{
    size_t count = 0;

    if(!initialized_)
    {
        initialized_ = true;
        prev_        = raw;
        for(size_t i = 0; i < 4; i++)
        {
            last_knobs_[i]   = raw.knobs[i];
            knob_smoothed_[i] = raw.knobs[i];
        }
        return 0;
    }

    auto push_event
        = [&](UiEventType type, uint8_t index, int32_t delta, float value) {
        if(count >= max_events)
            return;
        out[count].type  = type;
        out[count].index = index;
        out[count].delta = delta;
        out[count].value = value;
        count++;
    };

    if(raw.shift && !prev_.shift)
    {
        shift_down_ms_    = now_ms;
        shift_combo_used_ = false;
        shift_long_sent_  = false;
    }

    for(uint8_t i = 0; i < 4; i++)
    {
        if(raw.bank_buttons[i] && !prev_.bank_buttons[i])
        {
            if(raw.shift)
            {
                push_event(UiEventType::ShiftComboPressed, i, 0, 0.0f);
                shift_combo_used_ = true;
            }
            else
            {
                push_event(UiEventType::BankButtonPressed, i, 0, 0.0f);
            }
        }
    }

    if(raw.play_button && !prev_.play_button)
    {
        if(raw.shift)
        {
            push_event(UiEventType::ShiftComboPressed, 4, 0, 0.0f);
            shift_combo_used_ = true;
        }
        else
        {
            push_event(UiEventType::PlayButtonPressed, 0, 0, 0.0f);
        }
    }

    if(raw.encoder_delta != 0)
        push_event(UiEventType::EncoderTurn, 0, raw.encoder_delta, 0.0f);

    if(raw.shift && !shift_combo_used_ && !shift_long_sent_
       && now_ms - shift_down_ms_ >= kEncoderLongPressMs)
    {
        push_event(UiEventType::EncoderLongPress, 0, 0, 0.0f);
        shift_long_sent_ = true;
    }

    if(!raw.shift && prev_.shift)
    {
        if(!shift_combo_used_ && !shift_long_sent_)
            push_event(UiEventType::EncoderPressed, 0, 0, 0.0f);
        shift_combo_used_ = false;
        shift_long_sent_  = false;
    }

    for(uint8_t i = 0; i < 4; i++)
    {
        knob_smoothed_[i] += 0.2f * (raw.knobs[i] - knob_smoothed_[i]);
        if(std::fabs(knob_smoothed_[i] - last_knobs_[i]) >= 0.01f)
        {
            push_event(UiEventType::KnobMoved, i, 0, knob_smoothed_[i]);
            last_knobs_[i] = knob_smoothed_[i];
        }
    }

    prev_ = raw;
    return count;
}

} // namespace major_midi
