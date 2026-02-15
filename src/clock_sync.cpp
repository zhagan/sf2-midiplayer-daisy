#include "clock_sync.h"

void ClockSync::Init(float sampleRate)
{
    sr_ = sampleRate;
    Reset();
}

void ClockSync::Reset()
{
    running_         = false;
    startSample_     = 0;
    lastPulseSample_ = 0;
}

void ClockSync::OnPulse(uint64_t sampleNow)
{
    lastPulseSample_ = sampleNow;

    if(!running_)
    {
        running_     = true;
        startSample_ = sampleNow;
    }
}

bool ClockSync::IsRunning() const
{
    return running_;
}
