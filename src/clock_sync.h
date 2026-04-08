#pragma once
#include <cstdint>

class ClockSync
{
  public:
    enum class PulseMode
    {
        PULSE_PER_16TH,
        PULSE_PER_QUARTER,
        MIDI_24PPQN,
    };

    struct Config
    {
        float alpha               = 0.1f;  // tempo smoothing (EMA)
        float beta                = 0.25f; // phase correction strength
        float glitch_percent      = 0.25f; // reject dt outside +/- percent
        float debounce_ms         = 1.0f;  // ignore edges closer than this
        float missing_timeout_s   = 2.0f;  // missing pulse timeout
        bool  free_run_on_missing = false;
        float default_bpm         = 120.0f;
    };

    void Init(float sampleRate, PulseMode mode = PulseMode::PULSE_PER_16TH);
    void Init(float sampleRate, PulseMode mode, const Config& cfg);

    void SetInternalBpm(float bpm);
    void SetUseExternalClock(bool useExternal);

    // Called per-sample from the audio callback
    void ProcessSample(bool clockPinHigh, uint64_t globalSampleTime);

    // True exactly when a new 16th begins (can be called repeatedly to drain)
    bool ConsumeStepTick();
    // True when a valid external pulse arrives (scaled to 16th steps)
    bool ConsumeExternalStep();

    float GetBpmEstimate() const;
    float GetSamplesPer16th() const { return samples_per_16th_; }
    float GetLastMeasuredSp16() const { return last_measured_sp16_; }
    bool  IsLocked() const { return locked_; }

  private:
    float     sr_   = 48000.0f;
    PulseMode mode_ = PulseMode::PULSE_PER_16TH;

    Config cfg_{};

    bool use_external_ = false;
    bool running_      = true;
    bool locked_       = false;

    float internal_bpm_     = 120.0f;
    float internal_sp16_    = 0.0f;
    float sp16_est_          = 0.0f;
    float last_measured_sp16_ = 0.0f;
    float samples_per_16th_  = 0.0f;

    float steps_per_edge_ = 1.0f;

    bool     prev_clock_state_   = false;
    uint64_t last_edge_time_     = 0;
    double   edge_expected_time_ = 0.0;

    uint64_t next_boundary_time_ = 0;
    uint64_t last_boundary_time_ = 0;

    uint64_t last_process_time_ = 0;

    uint64_t min_samples_between_edges_ = 0;
    uint64_t missing_timeout_samples_   = 0;

    uint32_t pending_steps_ = 0;
    uint32_t pending_external_steps_ = 0;
    float    external_step_phase_ = 0.0f;

    float EdgeDtToSamplesPer16th(uint64_t dt) const;
    void  HandleEdge(uint64_t sampleTime);
    void  ResetExternalLockState();
};
