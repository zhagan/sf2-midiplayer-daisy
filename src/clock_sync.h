#pragma once
#include <cstdint>

class ClockSync
{
  public:
    void Init(float sampleRate)
    {
        sr_ = sampleRate;
        Reset();
    }

    void Reset()
    {
        running_         = false;
        startSample_     = 0;
        lastPulseSample_ = 0;
    }

    // Call on rising clock pulse (CV gate input later)
    void OnPulse(uint64_t sampleNow)
    {
        lastPulseSample_ = sampleNow;

        if(!running_)
        {
            running_     = true;
            startSample_ = sampleNow;
        }
    }

    bool IsRunning() const { return running_; }

  private:
    float sr_      = 48000.0f;
    bool  running_ = false;

    uint64_t startSample_     = 0;
    uint64_t lastPulseSample_ = 0;

    int ppqn_ = 24; // pulses per quarter note
};
