#pragma once
#include <cstdint>
#include "scheduler.h"

class SmfPlayer
{
  public:
    bool Open(const char* path);
    void Close();

    void SetSampleRate(float sr);
    void SetLookaheadSamples(uint64_t samples);

    void Start(uint64_t sampleNow);
    bool IsPlaying() const;

    // Parses ahead and pushes timestamped events
    void Pump(EventQueue<2048>& queue, uint64_t sampleNow);

  private:
    void* file_    = nullptr; // hidden FIL*
    bool  open_    = false;
    bool  playing_ = false;

    float    sr_          = 48000.0f;
    uint64_t lookahead_   = 0;
    uint64_t startSample_ = 0;
};
