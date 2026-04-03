#pragma once

#include <cstddef>
#include "app_state.h"
#include "daisy_patch_sm.h"
#include "dev/mcp23x17.h"
#include "hid/encoder.h"
#include "ui_events.h"

namespace major_midi
{

struct RawInputState
{
    float knobs[4]{};
    bool  bank_buttons[4]{};
    bool  play_button   = false;
    bool  shift         = false;
    int   encoder_delta = 0;
};

class UiHardwareInput
{
  public:
    void Init(daisy::patch_sm::DaisyPatchSM& hw);
    void ControlRateTick();
    void Sample(RawInputState& state);
    void SetLedMask(uint8_t mask);

  private:
    daisy::patch_sm::DaisyPatchSM* hw_ = nullptr;
    daisy::Encoder                 encoder_;
    daisy::Mcp23017               mcp_;
    volatile int32_t              pending_encoder_delta_ = 0;
    uint8_t                       led_mask_              = 0;
};

class UiEventTranslator
{
  public:
    size_t Translate(const RawInputState& raw,
                     uint32_t             now_ms,
                     UiEvent*             out,
                     size_t               max_events);

  private:
    bool          initialized_      = false;
    bool          shift_combo_used_ = false;
    bool          shift_long_sent_  = false;
    uint32_t      shift_down_ms_    = 0;
    RawInputState prev_{};
    float         last_knobs_[4]{};
    float         knob_smoothed_[4]{};
};

} // namespace major_midi
