#pragma once

#include "app_state.h"
#include "daisy_patch_sm.h"
#include "mixer_transport.h"

namespace major_midi
{

class CvGateEngine
{
  public:
    void Init(daisy::patch_sm::DaisyPatchSM& hw, float sample_rate);
    void Update(const AppState& state, const MixerTransport& transport);
    int  EffectiveBpm(const AppState& state) const;

  private:
    float ReadCvInput(size_t index) const;
    float PitchVoltageForChannel(const MixerTransport& transport,
                                 const CvOutputConfig& config) const;
    float CcVoltageForChannel(const MixerTransport& transport,
                              const CvOutputConfig& config) const;
    bool  SyncGateHigh(uint64_t cycle_sample,
                       int      bpm,
                       SyncResolution resolution) const;
    bool  ResetGateHigh(uint64_t cycle_sample,
                        int      bpm,
                        int      time_sig_num,
                        int      time_sig_den) const;

    daisy::patch_sm::DaisyPatchSM* hw_          = nullptr;
    float                          sample_rate_ = 48000.0f;
    int                            live_bpm_    = 0;
};

} // namespace major_midi
