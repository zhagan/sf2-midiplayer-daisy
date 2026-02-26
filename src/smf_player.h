#pragma once
#include <cstdint>
#include "scheduler.h"

extern "C"
{
#include "ff.h"
}

class SmfPlayer
{
  public:
    bool Open(const char* path);
    void Close();

    void SetSampleRate(float sr);
    void SetLookaheadSamples(uint64_t samples);

    void Start(uint64_t sampleNow);
    bool IsPlaying() const;
    uint32_t RemainingBytes() const;

    // Parses ahead and pushes timestamped events
    void Pump(EventQueue<2048>& queue, uint64_t sampleNow);

  private:
    struct TrackState
    {
        FSIZE_t  start = 0;
        FSIZE_t  pos = 0;
        uint32_t length = 0;
        uint32_t remaining = 0;
        uint8_t  running = 0;
        double   sampleFrac = 0.0;
        uint64_t sampleOffset = 0;
        bool     finished = false;
        bool     hasEvent = false;
        MidiEv   nextEv{};
    };

    bool ParseNextEvent(TrackState& trk, MidiEv& out);
    bool PrepareNextEvent(TrackState& trk);
    bool ReadTrackByte(TrackState& trk, uint8_t& b);
    bool ReadVarLen(TrackState& trk, uint32_t& value);
    bool SkipBytes(TrackState& trk, uint32_t count);
    bool SeekTrackHeader(uint32_t& length);
    void UpdateSamplesPerTick();

    FIL      file_;
    bool     open_            = false;
    bool     playing_         = false;
    uint16_t trackCount_      = 0;
    static constexpr uint16_t kMaxTracks = 16;
    TrackState tracks_[kMaxTracks]{};

    float    sr_              = 48000.0f;
    uint64_t lookahead_       = 0;
    uint64_t startSample_     = 0;

    uint16_t divisions_       = 480;
    uint32_t tempo_           = 500000;
    double   samplesPerTick_  = 0.0;
};
