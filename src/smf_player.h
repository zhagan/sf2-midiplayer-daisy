#pragma once
#include <cstdint>
#include "scheduler.h"
#include "major_midi_settings.h"

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
    void SetTempoScale(float scale);
    void SetTempoScale(float scale, uint64_t sampleNow);

    void Start(uint64_t sampleNow);
    void Stop();
    bool IsPlaying() const;
    void SeekToSample(uint64_t targetSample, uint64_t nowSample);
    uint32_t RemainingBytes() const;
    uint64_t SamplesPerQuarter() const;
    double SamplesPerQuarterF() const { return samplesPerTick_ * double(divisions_); }
    uint16_t Divisions() const { return divisions_; }
    uint32_t TempoUsecPerQuarter() const { return tempo_; }
    uint64_t LookaheadSamples() const { return lookahead_; }
    uint64_t SamplesFromTicks(uint64_t ticks) const;
    uint64_t SamplesFromTicksRange(uint64_t startTicks, uint64_t lengthTicks) const;
    uint64_t TicksFromSamples(uint64_t samples) const;
    uint8_t TimeSigNumerator() const { return ts_num_; }
    uint8_t TimeSigDenominator() const { return ts_den_; }
    const char* GetTrackNameForChannel(uint8_t ch) const;
    bool    HasSeekProgramState(uint8_t ch) const;
    uint8_t GetSeekProgramState(uint8_t ch) const;
    const major_midi::MajorMidiSettings& Settings() const { return settings_; }
    major_midi::MajorMidiSettings& MutableSettings() { return settings_; }
    bool SaveSettings();

    // Parses ahead and pushes timestamped events
    void Pump(EventQueue<1024>& queue, uint64_t sampleNow);

  private:
    struct TrackState
    {
        FSIZE_t  start = 0;
        FSIZE_t  pos = 0;
        uint32_t length = 0;
        uint32_t remaining = 0;
        uint8_t  running = 0;
        double   sampleFrac = 0.0;
        uint64_t tickOffset = 0;
        uint64_t sampleOffset = 0;
        bool     finished = false;
        bool     hasEvent = false;
        MidiEv   nextEv{};
    };

    bool ParseNextEvent(uint16_t trackIndex, TrackState& trk, MidiEv& out);
    bool PrepareNextEvent(uint16_t trackIndex, TrackState& trk);
    bool ReadTrackByte(TrackState& trk, uint8_t& b);
    bool ReadVarLen(TrackState& trk, uint32_t& value);
    bool SkipBytes(TrackState& trk, uint32_t count);
    bool SeekTrackHeader(uint32_t& length);
    void LoadMajorMidiSettings();
    bool HasBpmOverride() const;
    uint32_t EffectiveTempoUsec() const;
    void UpdateSamplesPerTick();
    void BuildTempoMap();
    void InsertTempoPoint(uint32_t tick, uint32_t tempo);

    FIL      file_;
    char     path_[64]{};
    bool     open_            = false;
    bool     playing_         = false;
    uint16_t trackCount_      = 0;
    static constexpr uint16_t kMaxTracks = 16;
    static constexpr uint16_t kTrackNameMax = 24;
    TrackState tracks_[kMaxTracks]{};
    char     trackNames_[kMaxTracks][kTrackNameMax]{};
    bool     trackHasName_[kMaxTracks]{};
    int8_t   trackChannel_[kMaxTracks]{};

    float    sr_              = 48000.0f;
    uint64_t lookahead_       = 0;
    uint64_t startSample_     = 0;
    uint64_t seekSample_      = 0;
    float    tempo_scale_     = 1.0f;

    uint16_t divisions_       = 480;
    uint32_t tempo_           = 500000;
    uint32_t fileTempoUsec_   = 500000;
    double   samplesPerTick_  = 0.0;
    uint8_t  ts_num_          = 4;
    uint8_t  ts_den_          = 4;
    bool     seek_program_valid_[16]{};
    uint8_t  seek_program_[16]{};
    static constexpr uint16_t kMaxTempoPoints = 256;
    uint16_t tempoCount_      = 0;
    uint32_t tempoTicks_[kMaxTempoPoints]{};
    uint32_t tempoUsec_[kMaxTempoPoints]{};
    major_midi::MajorMidiSettings settings_{};
};
