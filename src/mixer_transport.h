#pragma once

#include "app_state.h"
#include "daisy_patch_sm.h"
#include "hid/midi.h"
#include "scheduler.h"
#include "smf_player.h"

namespace major_midi
{

static constexpr size_t kScheduledQueueSize = 1024;
static constexpr size_t kParsedQueueSize    = 1024;
static constexpr size_t kImmediateQueueSize = 256;

class MixerTransport
{
  public:
    void Init(float sample_rate, SmfPlayer& player);
    void Reset(const AppState& state);
    void SetFileBpm(float bpm);
    void ProcessAudio(daisy::AudioHandle::InputBuffer  in,
                      daisy::AudioHandle::OutputBuffer out,
                      size_t                           size);
    void Update(const AppState& state);
    void HandleMidiMessage(daisy::MidiEvent msg, const AppState& state);
    void ConsumeChannelActivity(uint8_t out[16]);
    bool ChannelGateActive(uint8_t ch) const;
    bool AnyChannelGateActive() const;
    int  ChannelPitchNote(uint8_t ch, NotePriority priority) const;
    uint8_t ChannelCcValue(uint8_t ch, uint8_t cc) const;
    uint8_t ChannelProgram(uint8_t ch) const;
    int TimeSigNumerator() const;
    int TimeSigDenominator() const;
    uint64_t CurrentCycleSample() const;
    uint64_t CurrentSongTick() const;

    uint64_t SampleClock() const { return sample_clock_; }

  private:
    void EnqueueImmediate(const MidiEv& ev);
    bool DequeueImmediate(MidiEv& ev);
    bool EnqueueScheduled(const MidiEv& ev);
    bool PeekScheduled(MidiEv& ev);
    bool PopScheduled(MidiEv& ev);
    void ClearQueues();
    void ClearLiveMixerOverrides();
    void DispatchEvent(const MidiEv& ev, bool scheduled_source);
    void UpdateNoteState(const MidiEv& ev);
    void RecomputeNoteExtrema(uint8_t ch);
    uint8_t ScaleController(uint8_t value, uint8_t max_value) const;
    uint8_t ApplyTranspose(uint8_t ch, uint8_t note) const;
    uint8_t EffectiveVolume(uint8_t ch, const AppState& state) const;
    uint8_t EffectivePan(uint8_t ch, const AppState& state) const;
    uint8_t EffectiveReverb(uint8_t ch, const AppState& state) const;
    uint8_t EffectiveChorus(uint8_t ch, const AppState& state) const;
    void RenderFrames(daisy::AudioHandle::OutputBuffer out,
                      size_t                           offset,
                      size_t                           frames);
    void TransferScheduledFromParser(const AppState& state);
    void FlushLoopBoundaryNotes();
    bool MaybeWrapLoopParser(const AppState& state, uint64_t sample_now);

    void StartPlayback(const AppState& state);
    void StopPlayback(const AppState& state);
    void ApplyMixerState(const AppState& state, bool force = false);
    void EnqueueChannelMixerState(uint8_t ch, const AppState& state);
    uint64_t LoopStartTicks(const AppState& state) const;
    uint64_t LoopLengthTicks(const AppState& state) const;
    uint64_t LoopLengthSamples(const AppState& state) const;
    uint64_t LoopEndSample(const AppState& state) const;
    uint64_t LoopBoundarySample(const AppState& state) const;
    uint64_t MeasureStartTicks(int measure) const;
    bool LoopActive(const AppState& state) const;

    SmfPlayer*         player_ = nullptr;
    float              sample_rate_ = 48000.0f;
    volatile uint64_t  sample_clock_ = 0;
    EventQueue<kScheduledQueueSize> scheduled_{};
    EventQueue<kParsedQueueSize>    parsed_{};
    EventQueue<kImmediateQueueSize> immediate_{};
    ChannelState       applied_channels_[16]{};
    bool               applied_mute_all_ = false;
    bool               has_applied_state_ = false;
    uint64_t           play_start_sample_ = 0;
    uint64_t           play_start_ticks_  = 0;
    float              file_bpm_          = 120.0f;
    int                applied_bpm_       = -1;
    volatile uint8_t   channel_activity_[16]{};
    volatile uint8_t   master_volume_max_ = 127;
    volatile uint8_t   expression_max_    = 127;
    volatile uint8_t   reverb_max_        = 127;
    volatile uint8_t   chorus_max_        = 127;
    volatile int8_t    transpose_         = 0;
    uint8_t            live_volume_[16]{};
    uint8_t            live_pan_[16]{};
    uint8_t            live_reverb_[16]{};
    uint8_t            live_chorus_[16]{};
    bool               has_live_volume_[16]{};
    bool               has_live_pan_[16]{};
    bool               has_live_reverb_[16]{};
    bool               has_live_chorus_[16]{};
    bool               has_program_override_[16]{};
    int8_t             program_override_[16]{};
    int8_t             applied_program_override_[16]{};
    uint8_t            note_refcount_[16][128]{};
    uint8_t            cc_value_[16][128]{};
    uint8_t            current_program_[16]{};
    uint8_t            active_note_count_[16]{};
    int8_t             highest_note_[16]{};
    int8_t             lowest_note_[16]{};
    volatile uint64_t  loop_end_sample_   = UINT64_MAX;
    volatile bool      loop_active_       = false;
};

} // namespace major_midi
