#include "clock_sync.h"
#include <cmath>

void ClockSync::Init(float sampleRate, PulseMode mode)
{
    Init(sampleRate, mode, Config{});
}

void ClockSync::Init(float sampleRate, PulseMode mode, const Config& cfg)
{
    sr_ = sampleRate;
    mode_ = mode;
    cfg_ = cfg;

    min_samples_between_edges_ = (uint64_t)(sr_ * (cfg_.debounce_ms * 0.001f));
    missing_timeout_samples_ = (uint64_t)(sr_ * cfg_.missing_timeout_s);

    switch(mode_)
    {
        case PulseMode::PULSE_PER_16TH: steps_per_edge_ = 1.0f; break;
        case PulseMode::PULSE_PER_QUARTER: steps_per_edge_ = 4.0f; break;
        case PulseMode::MIDI_24PPQN: steps_per_edge_ = 1.0f / 6.0f; break;
    }

    internal_bpm_ = cfg_.default_bpm;
    internal_sp16_ = (sr_ * 60.0f) / (internal_bpm_ * 4.0f);
    samples_per_16th_ = internal_sp16_;

    use_external_ = false;
    running_ = true;
    locked_ = false;
    prev_clock_state_ = false;
    last_edge_time_ = 0;
    edge_expected_time_ = 0.0;
    next_boundary_time_ = 0;
    last_boundary_time_ = 0;
    last_process_time_ = 0;
    pending_steps_ = 0;
    pending_external_steps_ = 0;
    external_step_phase_ = 0.0f;
    sp16_est_ = 0.0f;
    last_measured_sp16_ = 0.0f;
}

void ClockSync::SetInternalBpm(float bpm)
{
    if(bpm <= 0.0f)
        return;
    internal_bpm_ = bpm;
    internal_sp16_ = (sr_ * 60.0f) / (internal_bpm_ * 4.0f);
    if(!use_external_)
        samples_per_16th_ = internal_sp16_;
}

void ClockSync::SetUseExternalClock(bool useExternal)
{
    if(useExternal == use_external_)
        return;

    use_external_ = useExternal;
    if(use_external_)
    {
        ResetExternalLockState();
        if(last_process_time_ != 0)
        {
            const uint64_t sp16 = (uint64_t)std::lround(samples_per_16th_);
            if(next_boundary_time_ <= last_process_time_)
                next_boundary_time_ = last_process_time_ + sp16;
        }
    }
}

void ClockSync::ResetExternalLockState()
{
    locked_              = false;
    running_             = false;
    last_edge_time_      = 0;
    edge_expected_time_  = 0.0;
    sp16_est_            = 0.0f;
    last_measured_sp16_  = 0.0f;
    pending_external_steps_ = 0;
    external_step_phase_ = 0.0f;
}

float ClockSync::EdgeDtToSamplesPer16th(uint64_t dt) const
{
    switch(mode_)
    {
        case PulseMode::PULSE_PER_16TH: return (float)dt;
        case PulseMode::PULSE_PER_QUARTER: return (float)dt / 4.0f;
        case PulseMode::MIDI_24PPQN: return (float)dt * 6.0f;
    }
    return (float)dt;
}

void ClockSync::HandleEdge(uint64_t sampleTime)
{
    if(last_edge_time_ == 0)
    {
        last_edge_time_ = sampleTime;
        edge_expected_time_ = (double)sampleTime;
        running_ = true;
        return;
    }

    const uint64_t dt = sampleTime - last_edge_time_;
    if(dt < min_samples_between_edges_)
        return;

    const float measured_sp16 = EdgeDtToSamplesPer16th(dt);
    last_measured_sp16_ = measured_sp16;

    if(sp16_est_ > 0.0f)
    {
        const float ref_sp16 = sp16_est_;
        const float min_sp16 = ref_sp16 * (1.0f - cfg_.glitch_percent);
        const float max_sp16 = ref_sp16 * (1.0f + cfg_.glitch_percent);
        if(measured_sp16 < min_sp16 || measured_sp16 > max_sp16)
            return;
        sp16_est_ += cfg_.alpha * (measured_sp16 - sp16_est_);
    }
    else
    {
        // First measured interval defines the initial tempo estimate.
        sp16_est_ = measured_sp16;
    }

    locked_ = true;
    last_edge_time_ = sampleTime;

    // External step accounting (supports non-1:1 pulse modes).
    external_step_phase_ += steps_per_edge_;
    while(external_step_phase_ >= 1.0f)
    {
        pending_external_steps_++;
        external_step_phase_ -= 1.0f;
    }

    const double expected = edge_expected_time_ + (double)sp16_est_ * steps_per_edge_;
    const double error = (double)sampleTime - expected;
    edge_expected_time_ = expected;

    if(next_boundary_time_ != 0)
    {
        double corrected = (double)next_boundary_time_ - (error * cfg_.beta);
        if(corrected < (double)last_process_time_)
            corrected = (double)last_process_time_;
        if(corrected > 0.0)
            next_boundary_time_ = (uint64_t)corrected;
    }

    running_ = true;
}

void ClockSync::ProcessSample(bool clockPinHigh, uint64_t globalSampleTime)
{
    last_process_time_ = globalSampleTime;

    const bool rising = clockPinHigh && !prev_clock_state_;
    prev_clock_state_ = clockPinHigh;

    if(use_external_)
    {
        if(last_edge_time_ == 0 && !cfg_.free_run_on_missing)
            running_ = false;

        if(rising)
            HandleEdge(globalSampleTime);

        if(last_edge_time_ != 0 && missing_timeout_samples_ > 0
           && (globalSampleTime - last_edge_time_) > missing_timeout_samples_)
        {
            ResetExternalLockState();
            if(cfg_.free_run_on_missing)
                running_ = true;
        }

        if(sp16_est_ > 0.0f)
            samples_per_16th_ = sp16_est_;
        else
            samples_per_16th_ = internal_sp16_;
    }
    else
    {
        running_ = true;
        samples_per_16th_ = internal_sp16_;
    }

    if(!running_)
        return;

    if(next_boundary_time_ == 0)
        next_boundary_time_ = globalSampleTime + (uint64_t)std::lround(samples_per_16th_);

    while(globalSampleTime >= next_boundary_time_)
    {
        last_boundary_time_ = next_boundary_time_;
        pending_steps_++;
        next_boundary_time_ += (uint64_t)std::lround(samples_per_16th_);
    }
}

bool ClockSync::ConsumeStepTick()
{
    if(pending_steps_ == 0)
        return false;
    pending_steps_--;
    return true;
}

bool ClockSync::ConsumeExternalStep()
{
    if(pending_external_steps_ == 0)
        return false;
    pending_external_steps_--;
    return true;
}

float ClockSync::GetBpmEstimate() const
{
    const float sp16 = (sp16_est_ > 0.0f) ? sp16_est_ : internal_sp16_;
    if(sp16 <= 0.0f)
        return 0.0f;
    return (sr_ * 60.0f) / (sp16 * 4.0f);
}
