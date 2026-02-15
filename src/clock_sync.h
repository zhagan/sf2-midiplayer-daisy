#pragma once
#include <cstdint>

class ClockSync
{
  public:
    void Init(float sampleRate);
    void Reset();

    // Call on rising clock pulse (CV gate input later)
    void OnPulse(uint64_t sampleNow);

    bool IsRunning() const;

  private:
    float sr_      = 48000.0f;
    bool  running_ = false;

    uint64_t startSample_     = 0;
    uint64_t lastPulseSample_ = 0;

    int ppqn_ = 24; // pulses per quarter note
};
