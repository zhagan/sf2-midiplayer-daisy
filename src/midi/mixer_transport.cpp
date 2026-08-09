#include "mixer_transport.h"
#include <cstring>
#include "synth_tsf.h"
#include "util/scopedirqblocker.h"

namespace major_midi
{

using namespace daisy;

namespace
{
SmfPlayer::LoopCacheEvent DSY_SDRAM_BSS s_loop_snapshot_events[kLoopSnapshotMaxEvents];
SmfPlayer::LoopCacheEvent DSY_SDRAM_BSS s_loop_events[kLoopCacheMaxEvents];

constexpr uint8_t  kActivityNote            = 1u << 0;
constexpr uint8_t  kActivityCc              = 1u << 1;
constexpr uint8_t  kActivityProgram         = 1u << 2;
constexpr uint8_t  kActivityPitch           = 1u << 3;

// How fast a user/song-driven tempo change is allowed to glide, in BPM per
// second of wall-clock time (derived from sample_clock_ deltas). A small
// knob nudge ramps in a few milliseconds; a large jump takes proportionally
// longer. External clock sync (already EMA-smoothed upstream) and looped
// playback (which restarts via a full reseek) bypass this and apply
// instantly instead - see MixerTransport::Update().
constexpr float kTempoRampBpmPerSecond = 240.0f;
}

void MixerTransport::Init(float sample_rate, SmfPlayer& player)
{
    sample_rate_ = sample_rate;
    player_      = &player;
    loop_snapshot_events_ = s_loop_snapshot_events;
    loop_events_          = s_loop_events;
    std::memset(program_override_, -1, sizeof(program_override_));
    std::memset(applied_program_override_, -1, sizeof(applied_program_override_));
    for(size_t ch = 0; ch < 16; ch++)
    {
        highest_note_[ch] = -1;
        lowest_note_[ch]  = -1;
    }
}

void MixerTransport::Reset(const AppState& state)
{
    if(player_ != nullptr && player_->IsPlaying())
        player_->Stop();
    ClearQueues();
    ClearLiveMixerOverrides();
    std::memset(current_program_, 0, sizeof(current_program_));
    std::memset(program_override_, -1, sizeof(program_override_));
    std::memset(applied_program_override_, -1, sizeof(applied_program_override_));
    std::memset(has_program_override_, 0, sizeof(has_program_override_));
    std::memset(note_refcount_, 0, sizeof(note_refcount_));
    std::memset(cc_value_, 0, sizeof(cc_value_));
    std::memset(file_volume_, 0, sizeof(file_volume_));
    std::memset(has_file_volume_, 0, sizeof(has_file_volume_));
    std::memset(active_note_count_, 0, sizeof(active_note_count_));
    std::memset(note_on_count_, 0, sizeof(note_on_count_));
    for(size_t ch = 0; ch < 16; ch++)
    {
        highest_note_[ch] = -1;
        lowest_note_[ch]  = -1;
    }
    SynthPanic();
    SynthResetChannels();
    has_applied_state_ = false;
    applied_bpm_       = -1;
    play_start_sample_ = 0;
    play_start_ticks_  = 0;
    phase_start_sample_ = 0;
    phase_start_ticks_  = 0;
    loop_length_samples_ = 0;
    ResetLoopCachePlayback();
    ApplyMixerState(state, true);
}

void MixerTransport::SetFileBpm(float bpm)
{
    file_bpm_ = bpm > 1.0f ? bpm : 120.0f;
    applied_bpm_ = -1;
}

void MixerTransport::ConsumeChannelActivity(uint8_t out[16])
{
    ScopedIrqBlocker lock;
    for(size_t i = 0; i < 16; i++)
    {
        out[i]               = channel_activity_[i];
        channel_activity_[i] = 0;
    }
}

void MixerTransport::EnqueueImmediate(const MidiEv& ev)
{
    ScopedIrqBlocker lock;
    immediate_.Push(ev);
}

bool MixerTransport::DequeueImmediate(MidiEv& ev)
{
    ScopedIrqBlocker lock;
    return immediate_.Pop(ev);
}

bool MixerTransport::EnqueueScheduled(const MidiEv& ev)
{
    ScopedIrqBlocker lock;
    return scheduled_.Push(ev);
}

bool MixerTransport::PeekScheduled(MidiEv& ev)
{
    ScopedIrqBlocker lock;
    return scheduled_.Peek(ev);
}

bool MixerTransport::PopScheduled(MidiEv& ev)
{
    ScopedIrqBlocker lock;
    return scheduled_.Pop(ev);
}

void MixerTransport::ClearQueues()
{
    ScopedIrqBlocker lock;
    scheduled_.Clear();
    parsed_.Clear();
    immediate_.Clear();
}

void MixerTransport::ClearLiveMixerOverrides()
{
    for(size_t i = 0; i < 16; i++)
    {
        has_live_volume_[i] = false;
        has_live_pan_[i]    = false;
        has_live_reverb_[i] = false;
        has_live_chorus_[i] = false;
        live_volume_[i]     = 0;
        live_pan_[i]        = 0;
        live_reverb_[i]     = 0;
        live_chorus_[i]     = 0;
    }
}

bool MixerTransport::ChannelGateActive(uint8_t ch) const
{
    return ch < 16 && active_note_count_[ch] > 0;
}

bool MixerTransport::AnyChannelGateActive() const
{
    for(size_t ch = 0; ch < 16; ch++)
    {
        if(active_note_count_[ch] > 0)
            return true;
    }
    return false;
}

bool MixerTransport::ChannelEventBlockedByMute(const MidiEv& ev, const AppState& state) const
{
    if(ev.ch >= 16 || !state.channels[ev.ch].muted)
        return false;

    switch(ev.type)
    {
        case EvType::NoteOn:
        case EvType::NoteOff:
        case EvType::Program:
        case EvType::ControlChange:
        case EvType::PitchBend: return true;
        case EvType::AllSoundOff:
        case EvType::AllNotesOff: return false;
    }
    return false;
}

void MixerTransport::FlushChannelNotes(uint8_t ch)
{
    if(ch >= 16)
        return;

    MidiEv ev{};
    ev.ch   = ch;
    ev.type = EvType::AllNotesOff;
    DispatchEvent(ev, false);
    ev.type = EvType::AllSoundOff;
    DispatchEvent(ev, false);
}

int MixerTransport::ChannelPitchNote(uint8_t ch, NotePriority priority) const
{
    if(ch >= 16)
        return -1;
    return priority == NotePriority::Highest ? highest_note_[ch] : lowest_note_[ch];
}

uint32_t MixerTransport::ChannelNoteOnCounter(uint8_t ch) const
{
    return ch < 16 ? note_on_count_[ch] : 0u;
}

uint8_t MixerTransport::ChannelCcValue(uint8_t ch, uint8_t cc) const
{
    return (ch < 16) ? cc_value_[ch][cc] : 0;
}

uint8_t MixerTransport::ChannelProgram(uint8_t ch) const
{
    return ch < 16 ? current_program_[ch] : 0;
}

int MixerTransport::TimeSigNumerator() const
{
    return player_ ? player_->TimeSigNumerator() : 4;
}

int MixerTransport::TimeSigDenominator() const
{
    return player_ ? player_->TimeSigDenominator() : 4;
}

uint16_t MixerTransport::Divisions() const
{
    return player_ ? player_->Divisions() : 480;
}

uint64_t MixerTransport::CurrentCycleSample() const
{
    return CycleSampleAt(sample_clock_);
}

uint64_t MixerTransport::CycleSampleAt(uint64_t absolute_sample) const
{
    uint64_t cycle_start_sample = phase_start_sample_;
    if(loop_cache_pending_ && absolute_sample >= play_start_sample_)
        cycle_start_sample = play_start_sample_;

    const uint64_t elapsed
        = absolute_sample >= cycle_start_sample ? (absolute_sample - cycle_start_sample) : 0;
    const uint64_t loop_length = loop_length_samples_;
    if(loop_active_ && loop_length > 0)
        return elapsed % loop_length;
    return elapsed;
}

uint64_t MixerTransport::CurrentSongTick() const
{
    return SongTickAt(sample_clock_);
}

uint64_t MixerTransport::SongTickAt(uint64_t absolute_sample) const
{
    if(player_ == nullptr)
        return 0;
    uint64_t cycle_start_ticks = phase_start_ticks_;
    if(loop_cache_pending_ && absolute_sample >= play_start_sample_)
        cycle_start_ticks = play_start_ticks_;
    return cycle_start_ticks + player_->TicksFromSamples(CycleSampleAt(absolute_sample));
}

uint8_t MixerTransport::ScaleController(uint8_t value, uint8_t max_value) const
{
    return static_cast<uint8_t>((uint16_t(value) * uint16_t(max_value)) / 127u);
}

uint8_t MixerTransport::ApplyTranspose(uint8_t ch, uint8_t note) const
{
    if(ch == 9)
        return note;

    int transposed = static_cast<int>(note) + static_cast<int>(transpose_);
    if(transposed < 0)
        transposed = 0;
    if(transposed > 127)
        transposed = 127;
    return static_cast<uint8_t>(transposed);
}

uint8_t MixerTransport::ComputeEffectiveVolume(uint8_t track_value,
                                               uint8_t ui_value,
                                               bool    muted,
                                               uint8_t master_max) const
{
    if(muted)
        return 0;
    return ScaleController(ScaleController(track_value, ui_value), master_max);
}

uint8_t MixerTransport::EffectiveVolume(uint8_t ch, const AppState& state) const
{
    // The performance volume knob scales the channel's own CC7 (the mix
    // baked into the file, or an external live controller) rather than
    // replacing it, so the file's original balance is preserved.
    const uint8_t track_value = has_live_volume_[ch] ? live_volume_[ch]
                                : has_file_volume_[ch] ? file_volume_[ch]
                                                        : 127;
    return ComputeEffectiveVolume(
        track_value, state.channels[ch].volume, state.channels[ch].muted, state.sf2_master_volume_max);
}

uint8_t MixerTransport::EffectivePan(uint8_t ch, const AppState& state) const
{
    const uint8_t ui_value = state.channels[ch].pan;
    return has_live_pan_[ch] ? ScaleController(live_pan_[ch], ui_value) : ui_value;
}

uint8_t MixerTransport::EffectiveReverb(uint8_t ch, const AppState& state) const
{
    const uint8_t ui_value = state.channels[ch].reverb_send;
    const uint8_t base = has_live_reverb_[ch] ? ScaleController(live_reverb_[ch], ui_value)
                                              : ui_value;
    return ScaleController(base, state.sf2_reverb_max);
}

uint8_t MixerTransport::EffectiveChorus(uint8_t ch, const AppState& state) const
{
    const uint8_t ui_value = state.channels[ch].chorus_send;
    const uint8_t base = has_live_chorus_[ch] ? ScaleController(live_chorus_[ch], ui_value)
                                              : ui_value;
    return ScaleController(base, state.sf2_chorus_max);
}

void MixerTransport::RecomputeNoteExtrema(uint8_t ch)
{
    highest_note_[ch] = -1;
    lowest_note_[ch]  = -1;
    for(int note = 0; note < 128; note++)
    {
        if(note_refcount_[ch][note] == 0)
            continue;
        if(lowest_note_[ch] < 0)
            lowest_note_[ch] = static_cast<int8_t>(note);
        highest_note_[ch] = static_cast<int8_t>(note);
    }
}

void MixerTransport::UpdateNoteState(const MidiEv& ev)
{
    if(ev.ch >= 16)
        return;

    switch(ev.type)
    {
        case EvType::NoteOn:
            if(ev.b == 0)
                break;
            note_on_count_[ev.ch]++;
            if(note_refcount_[ev.ch][ev.a] == 0)
            {
                active_note_count_[ev.ch]++;
                if(lowest_note_[ev.ch] < 0 || ev.a < lowest_note_[ev.ch])
                    lowest_note_[ev.ch] = static_cast<int8_t>(ev.a);
                if(highest_note_[ev.ch] < 0 || ev.a > highest_note_[ev.ch])
                    highest_note_[ev.ch] = static_cast<int8_t>(ev.a);
            }
            if(note_refcount_[ev.ch][ev.a] < 255)
                note_refcount_[ev.ch][ev.a]++;
            break;

        case EvType::NoteOff:
            if(note_refcount_[ev.ch][ev.a] > 0)
            {
                note_refcount_[ev.ch][ev.a]--;
                if(note_refcount_[ev.ch][ev.a] == 0)
                {
                    if(active_note_count_[ev.ch] > 0)
                        active_note_count_[ev.ch]--;
                    if(ev.a == static_cast<uint8_t>(highest_note_[ev.ch])
                       || ev.a == static_cast<uint8_t>(lowest_note_[ev.ch]))
                        RecomputeNoteExtrema(ev.ch);
                }
            }
            break;

        case EvType::ControlChange:
            cc_value_[ev.ch][ev.a] = ev.b;
            break;

        case EvType::AllNotesOff:
        case EvType::AllSoundOff:
            std::memset(note_refcount_[ev.ch], 0, sizeof(note_refcount_[ev.ch]));
            active_note_count_[ev.ch] = 0;
            highest_note_[ev.ch]      = -1;
            lowest_note_[ev.ch]       = -1;
            break;

        case EvType::Program:
        case EvType::PitchBend: break;
    }
}

void MixerTransport::SetMidiOutputCallback(MidiOutputCallback callback, void* context)
{
    midi_output_callback_ = callback;
    midi_output_context_  = context;
}

void MixerTransport::SetDebugMidiCallback(DebugMidiCallback callback, void* context)
{
    debug_midi_callback_ = callback;
    debug_midi_context_  = context;
}

void MixerTransport::DispatchEvent(const MidiEv& ev, bool scheduled_source)
{
    MidiEv actual = ev;

    if(actual.type == EvType::NoteOn || actual.type == EvType::NoteOff)
        actual.a = ApplyTranspose(actual.ch, actual.a);
    if(actual.type == EvType::Program && actual.ch < 16 && has_program_override_[actual.ch])
        actual.a = static_cast<uint8_t>(program_override_[actual.ch]);
    if(actual.type == EvType::ControlChange && actual.a == 11)
        actual.b = ScaleController(actual.b, expression_max_);

    UpdateNoteState(actual);
    if(actual.type == EvType::Program && actual.ch < 16)
        current_program_[actual.ch] = actual.a;

    if(ev.ch < 16)
    {
        switch(ev.type)
        {
            case EvType::NoteOn:
            case EvType::NoteOff: channel_activity_[ev.ch] |= kActivityNote; break;
            case EvType::Program: channel_activity_[ev.ch] |= kActivityProgram; break;
            case EvType::ControlChange: channel_activity_[ev.ch] |= kActivityCc; break;
            case EvType::PitchBend: channel_activity_[ev.ch] |= kActivityPitch; break;
            case EvType::AllSoundOff:
            case EvType::AllNotesOff: break;
        }
    }

    switch(ev.type)
    {
        case EvType::NoteOn: SynthNoteOn(actual.ch, actual.a, actual.b); break;
        case EvType::NoteOff: SynthNoteOff(actual.ch, actual.a); break;
        case EvType::Program: SynthProgramChange(actual.ch, actual.a); break;
        case EvType::ControlChange:
            if(scheduled_source && actual.a == 7)
            {
                // File-driven CC7 automation: scale it by the performance
                // volume knob instead of dropping it, so the knob adjusts
                // the track's own mix rather than overriding it.
                file_volume_[actual.ch]     = actual.b;
                has_file_volume_[actual.ch] = true;
                const uint8_t track_value
                    = has_live_volume_[actual.ch] ? live_volume_[actual.ch] : actual.b;
                SynthControlChange(actual.ch,
                                   7,
                                   ComputeEffectiveVolume(track_value,
                                                           applied_channels_[actual.ch].volume,
                                                           applied_channels_[actual.ch].muted,
                                                           master_volume_max_));
            }
            else if(!(scheduled_source && (actual.a == 10 || actual.a == 91 || actual.a == 93)))
                SynthControlChange(actual.ch, actual.a, actual.b);
            break;
        case EvType::PitchBend:
        {
            const uint16_t bend = (uint16_t(actual.b) << 7) | actual.a;
            SynthPitchBend(actual.ch, bend);
        }
        break;
        case EvType::AllSoundOff: SynthAllSoundOff(actual.ch); break;
        case EvType::AllNotesOff: SynthAllNotesOff(actual.ch); break;
    }

    if(scheduled_source && midi_output_callback_ != nullptr)
        midi_output_callback_(actual, midi_output_context_);
    if(scheduled_source && debug_midi_callback_ != nullptr)
        debug_midi_callback_("DSP", actual, debug_midi_context_);
}

void MixerTransport::RenderFrames(AudioHandle::OutputBuffer out,
                                  size_t                    offset,
                                  size_t                    frames)
{
    static float lbuf[256];
    static float rbuf[256];

    while(frames > 0)
    {
        const size_t chunk = frames > 256 ? 256 : frames;
        SynthRender(lbuf, rbuf, chunk);
        for(size_t i = 0; i < chunk; i++)
        {
            out[0][offset + i] = lbuf[i];
            out[1][offset + i] = rbuf[i];
        }
        offset += chunk;
        frames -= chunk;
    }
}

void MixerTransport::TransferScheduledFromParser(const AppState& state)
{
    MidiEv ev;
    const uint64_t loop_boundary_sample = LoopBoundarySample(state);
    while(parsed_.Peek(ev))
    {
        if(LoopActive(state) && ev.atSample >= loop_boundary_sample)
            break;
        if(!EnqueueScheduled(ev))
            break;
        if(debug_midi_callback_ != nullptr)
            debug_midi_callback_("SCH", ev, debug_midi_context_);
        parsed_.Pop(ev);
    }
}

bool MixerTransport::QueueScheduledLoopEvent(const SmfPlayer::LoopCacheEvent& ev, uint64_t at_sample)
{
    MidiEv out{};
    out.atSample = at_sample;
    out.type     = ev.type;
    out.ch       = ev.ch;
    out.a        = ev.a;
    out.b        = ev.b;
    if(!EnqueueScheduled(out))
        return false;
    return true;
}

bool MixerTransport::QueueLoopSeamEvents(uint64_t seam_sample)
{
    // Resumable: on a full scheduled_ queue we bail out mid-way and pick up
    // from the same channel/snapshot index next call, instead of dropping
    // the remaining seam events (stuck notes/voices at every loop boundary).
    while(loop_cache_seam_channel_ < 16)
    {
        SmfPlayer::LoopCacheEvent ev{};
        ev.type = EvType::AllSoundOff;
        ev.ch   = static_cast<uint8_t>(loop_cache_seam_channel_);
        if(!QueueScheduledLoopEvent(ev, seam_sample))
            return false;
        loop_cache_seam_channel_++;
    }

    while(loop_cache_seam_snapshot_cursor_ < loop_snapshot_event_count_)
    {
        if(!QueueScheduledLoopEvent(loop_snapshot_events_[loop_cache_seam_snapshot_cursor_],
                                    seam_sample))
            return false;
        loop_cache_seam_snapshot_cursor_++;
    }

    return true;
}

void MixerTransport::PumpLoopCache(uint64_t sample_now)
{
    // PumpLoopCache runs from the main-loop Update() call, while ProcessAudio
    // (audio ISR) can concurrently cross a loop boundary and rewrite the very
    // same cursor/phase state (loop_cache_cursor_, loop_cache_next_cursor_,
    // loop_cache_next_primed_, loop_cache_seam_*, phase_start_sample_) at any
    // point. Without locking, that interleaving corrupts the cursors right at
    // the boundary on every single repeat - it was the actual cause of a
    // pause that grew worse with each loop, not a queue-capacity issue.
    // Hold the whole function atomic w.r.t. that ISR-side mutation.
    ScopedIrqBlocker lock;

    if((!loop_cache_playback_ && !loop_cache_pending_) || !loop_cache_valid_)
        return;

    const uint64_t lookahead_limit = sample_now + player_->LookaheadSamples();
    const uint64_t cycle_start_sample = loop_cache_pending_ ? play_start_sample_ : phase_start_sample_;
    const uint64_t cycle_end_sample   = cycle_start_sample + loop_length_samples_;

    // The seam (AllSoundOff + program/CC/pitch snapshot) for the cycle that's
    // about to start must be fully queued before any of that cycle's own
    // loop events, or a partial send (scheduled_ full) leaves stale notes/CC
    // state hanging over - audible as a stuck/doubled note on the very next
    // repeat. Resumable across calls via loop_cache_seam_*: retry here until
    // it succeeds rather than giving up after one attempt.
    if(!loop_cache_seam_done_)
    {
        if(!QueueLoopSeamEvents(cycle_start_sample))
            return;
        loop_cache_seam_done_            = true;
        loop_cache_seam_channel_         = 0;
        loop_cache_seam_snapshot_cursor_ = 0;
    }

    while(loop_cache_cursor_ < loop_cache_event_count_)
    {
        const uint64_t event_sample
            = cycle_start_sample + uint64_t(loop_events_[loop_cache_cursor_].rel_sample);
        if(event_sample > lookahead_limit || event_sample >= cycle_end_sample)
            break;
        if(!QueueScheduledLoopEvent(loop_events_[loop_cache_cursor_], event_sample))
            break;
        loop_cache_cursor_++;
    }

    if(loop_cache_playback_ && lookahead_limit >= cycle_end_sample)
    {
        if(!loop_cache_next_primed_)
        {
            if(!QueueLoopSeamEvents(cycle_end_sample))
                return;
            loop_cache_next_primed_         = true;
            loop_cache_next_cursor_         = 0;
            loop_cache_seam_channel_         = 0;
            loop_cache_seam_snapshot_cursor_ = 0;
        }

        while(loop_cache_next_cursor_ < loop_cache_event_count_)
        {
            const uint64_t event_sample
                = cycle_end_sample + uint64_t(loop_events_[loop_cache_next_cursor_].rel_sample);
            if(event_sample > lookahead_limit)
                break;
            if(!QueueScheduledLoopEvent(loop_events_[loop_cache_next_cursor_], event_sample))
                break;
            loop_cache_next_cursor_++;
        }
    }
}

bool MixerTransport::EnsureLoopCache(const AppState& state)
{
    if(!LoopActive(state))
    {
        loop_cache_valid_ = false;
        return false;
    }

    const uint32_t start_tick  = static_cast<uint32_t>(LoopStartTicks(state));
    const uint32_t length_tick = static_cast<uint32_t>(LoopLengthTicks(state));
    if(loop_cache_valid_ && start_tick == loop_cache_start_tick_
       && length_tick == loop_cache_length_ticks_)
        return true;

    size_t snapshot_count = 0;
    size_t event_count    = 0;
    const bool ok         = player_->BuildLoopCache(start_tick,
                                            length_tick,
                                            loop_snapshot_events_,
                                            kLoopSnapshotMaxEvents,
                                            snapshot_count,
                                            loop_events_,
                                            kLoopCacheMaxEvents,
                                            event_count);
    if(!ok)
    {
        loop_cache_valid_          = false;
        loop_snapshot_event_count_ = 0;
        loop_cache_event_count_    = 0;
        return false;
    }

    loop_cache_valid_           = true;
    loop_cache_start_tick_      = start_tick;
    loop_cache_length_ticks_    = length_tick;
    loop_snapshot_event_count_  = snapshot_count;
    loop_cache_event_count_     = event_count;
    return true;
}

void MixerTransport::ResetLoopCachePlayback()
{
    loop_cache_playback_             = false;
    loop_cache_pending_              = false;
    loop_cache_next_primed_          = false;
    loop_cache_cursor_               = 0;
    loop_cache_next_cursor_          = 0;
    loop_cache_seam_channel_         = 0;
    loop_cache_seam_snapshot_cursor_ = 0;
    loop_cache_seam_done_            = true;
}

void MixerTransport::FlushLoopBoundaryNotes()
{
    for(uint8_t ch = 0; ch < 16; ch++)
    {
        if(active_note_count_[ch] == 0)
            continue;

        MidiEv ev{};
        ev.type = EvType::AllSoundOff;
        ev.ch   = ch;
        DispatchEvent(ev, false);
    }

    std::memset(note_refcount_, 0, sizeof(note_refcount_));
    std::memset(active_note_count_, 0, sizeof(active_note_count_));
    std::memset(note_on_count_, 0, sizeof(note_on_count_));
    for(size_t ch = 0; ch < 16; ch++)
    {
        highest_note_[ch] = -1;
        lowest_note_[ch]  = -1;
    }
}

bool MixerTransport::MaybeWrapLoopParser(const AppState& state, uint64_t sample_now)
{
    if(!LoopActive(state))
        return false;

    const uint64_t loop_boundary_sample = LoopBoundarySample(state);
    const uint64_t lookahead_limit      = sample_now + player_->LookaheadSamples();
    if(loop_boundary_sample > lookahead_limit)
        return false;

    MidiEv ev;
    if(parsed_.Peek(ev) && ev.atSample < loop_boundary_sample)
        return false;

    const uint64_t loop_start_ticks   = LoopStartTicks(state);
    const uint64_t restart_sample     = LoopEndSample(state);
    const uint64_t loop_start_samples = player_->SamplesFromTicks(loop_start_ticks);

    if(loop_cache_valid_)
    {
        player_->Stop();
        parsed_.Clear();
        {
            // Same cursor/phase state ProcessAudio reads on the audio ISR -
            // lock the transition so it can't observe a half-updated set of
            // fields (e.g. loop_cache_pending_ true before the cursors that
            // go with it are reset).
            ScopedIrqBlocker lock;
            play_start_sample_   = restart_sample;
            play_start_ticks_    = loop_start_ticks;
            loop_cache_playback_ = false;
            loop_cache_pending_  = true;
            loop_cache_cursor_      = 0;
            loop_cache_next_cursor_ = 0;
            loop_cache_next_primed_ = false;
            loop_cache_seam_done_             = false;
            loop_cache_seam_channel_          = 0;
            loop_cache_seam_snapshot_cursor_  = 0;
        }
        PumpLoopCache(sample_now);
        return true;
    }

    FlushLoopBoundaryNotes();
    parsed_.Clear();
    player_->SeekToSample(loop_start_samples, restart_sample);
    play_start_sample_ = restart_sample;
    play_start_ticks_  = loop_start_ticks;
    loop_end_sample_   = LoopBoundarySample(state);
    return true;
}

void MixerTransport::RemapQueuedEventTimes(uint64_t sample_now, double ratio)
{
    if(ratio <= 0.0)
        return;

    ScopedIrqBlocker lock;
    auto remap = [&](MidiEv& ev) {
        if(ev.atSample <= sample_now)
            return;
        const double delta      = double(ev.atSample - sample_now);
        const double remapped   = delta * ratio;
        const uint64_t new_time = sample_now + static_cast<uint64_t>(llround(remapped));
        ev.atSample             = new_time;
    };

    scheduled_.Transform(remap);
    parsed_.Transform(remap);
}

void MixerTransport::ShiftQueuedEventTimes(uint64_t sample_now, int64_t delta_samples)
{
    if(delta_samples == 0)
        return;

    ScopedIrqBlocker lock;
    auto shift = [&](MidiEv& ev) {
        if(ev.atSample <= sample_now)
            return;

        const int64_t shifted = static_cast<int64_t>(ev.atSample) + delta_samples;
        if(shifted <= static_cast<int64_t>(sample_now))
            ev.atSample = sample_now;
        else
            ev.atSample = static_cast<uint64_t>(shifted);
    };

    scheduled_.Transform(shift);
    parsed_.Transform(shift);
}

void MixerTransport::ProcessAudio(AudioHandle::InputBuffer  in,
                                  AudioHandle::OutputBuffer out,
                                  size_t                    size)
{
    (void)in;

    MidiEv ev;
    while(DequeueImmediate(ev))
        DispatchEvent(ev, false);

    uint64_t block_sample = sample_clock_;
    size_t   offset       = 0;
    while(offset < size)
    {
        MidiEv next_ev{};
        if(!PeekScheduled(next_ev))
        {
            RenderFrames(out, offset, size - offset);
            offset = size;
            break;
        }

        const uint64_t current_sample = block_sample + offset;
        if(next_ev.atSample > current_sample)
        {
            uint64_t frames_to_event = next_ev.atSample - current_sample;
            if(frames_to_event > (size - offset))
                frames_to_event = size - offset;
            RenderFrames(out, offset, static_cast<size_t>(frames_to_event));
            offset += static_cast<size_t>(frames_to_event);
            continue;
        }

        do
        {
            if(!PopScheduled(next_ev))
                break;
            if(!(next_ev.ch < 16 && applied_channels_[next_ev.ch].muted
                 && (next_ev.type == EvType::NoteOn || next_ev.type == EvType::NoteOff
                     || next_ev.type == EvType::Program || next_ev.type == EvType::ControlChange
                     || next_ev.type == EvType::PitchBend)))
                DispatchEvent(next_ev, true);
        } while(PeekScheduled(next_ev) && next_ev.atSample <= current_sample);
    }

    sample_clock_ = block_sample + size;
    {
        // Same cursor/phase state PumpLoopCache reads and writes from the
        // main loop - lock this section too so a boundary crossing here
        // can't interleave with an in-progress PumpLoopCache call and tear
        // the cursors (see the lock comment in PumpLoopCache).
        ScopedIrqBlocker lock;
        if(loop_cache_pending_ && sample_clock_ >= play_start_sample_)
        {
            phase_start_sample_  = play_start_sample_;
            phase_start_ticks_   = play_start_ticks_;
            loop_end_sample_     = phase_start_sample_ + loop_length_samples_;
            loop_cache_playback_ = true;
            loop_cache_pending_  = false;
        }
        if(loop_cache_playback_ && loop_length_samples_ > 0)
        {
            while(sample_clock_ >= loop_end_sample_)
            {
                phase_start_sample_ = loop_end_sample_;
                phase_start_ticks_  = loop_cache_start_tick_;
                play_start_sample_  = phase_start_sample_;
                play_start_ticks_   = phase_start_ticks_;
                loop_end_sample_    = phase_start_sample_ + loop_length_samples_;
                loop_cache_cursor_  = loop_cache_next_primed_ ? loop_cache_next_cursor_ : 0;
                loop_cache_next_cursor_ = 0;
                // If the next-cycle seam was already fully queued ahead of time
                // (the common case), the cycle we're entering needs no further
                // seam work. Otherwise leave loop_cache_seam_done_ false and the
                // seam cursors untouched so PumpLoopCache resumes sending the
                // (still-pending) seam for this same boundary sample instead of
                // restarting it from channel 0.
                loop_cache_seam_done_ = loop_cache_next_primed_;
                if(loop_cache_next_primed_)
                {
                    loop_cache_seam_channel_         = 0;
                    loop_cache_seam_snapshot_cursor_ = 0;
                }
                loop_cache_next_primed_ = false;
            }
        }
        else if(loop_active_ && phase_start_sample_ != play_start_sample_
                && sample_clock_ >= play_start_sample_)
        {
            phase_start_sample_ = play_start_sample_;
            phase_start_ticks_  = play_start_ticks_;
        }
    }
}

void MixerTransport::StartPlayback(const AppState& state)
{
    ResetLoopCachePlayback();
    ClearQueues();
    std::memset(current_program_, 0, sizeof(current_program_));
    std::memset(note_refcount_, 0, sizeof(note_refcount_));
    std::memset(cc_value_, 0, sizeof(cc_value_));
    std::memset(file_volume_, 0, sizeof(file_volume_));
    std::memset(has_file_volume_, 0, sizeof(has_file_volume_));
    std::memset(active_note_count_, 0, sizeof(active_note_count_));
    std::memset(note_on_count_, 0, sizeof(note_on_count_));
    for(size_t ch = 0; ch < 16; ch++)
    {
        highest_note_[ch] = -1;
        lowest_note_[ch]  = -1;
    }
    SynthPanic();
    SynthResetChannels();

    const uint64_t sample_now = sample_clock_;
    if(LoopActive(state))
    {
        const uint64_t loop_start_ticks   = LoopStartTicks(state);
        const uint64_t loop_start_samples = player_->SamplesFromTicks(loop_start_ticks);
        player_->SeekToSample(loop_start_samples, sample_now);
        play_start_sample_ = sample_now;
        play_start_ticks_  = loop_start_ticks;
        phase_start_sample_ = sample_now;
        phase_start_ticks_  = loop_start_ticks;

        if(loop_cache_valid_)
        {
            // Resync full pre-loop channel state (program, CC - including
            // CC7 volume - and pitch bend) the same way the cache-based
            // seam does for every later repeat. Without this, pass 1 starts
            // with whatever state happened to already be applied (e.g. no
            // file volume known yet, so EffectiveVolume() falls back to
            // max), which can make the first pass audibly louder than every
            // repeat after it. Queued as scheduled (not immediate) events so
            // they go through DispatchEvent's scheduled_source path, which
            // is what applies the CC7 volume scaling correctly.
            for(size_t i = 0; i < loop_snapshot_event_count_; i++)
                QueueScheduledLoopEvent(loop_snapshot_events_[i], sample_now);
        }
        else
        {
            for(uint8_t ch = 0; ch < 16; ch++)
            {
                if(player_->HasSeekProgramState(ch))
                {
                    MidiEv ev{};
                    ev.type = EvType::Program;
                    ev.ch   = ch;
                    ev.a    = player_->GetSeekProgramState(ch);
                    EnqueueImmediate(ev);
                }
            }
        }
    }
    else
    {
        player_->Start(sample_now);
        play_start_sample_ = sample_now;
        play_start_ticks_  = 0;
        phase_start_sample_ = sample_now;
        phase_start_ticks_  = 0;
    }

    ApplyMixerState(state, true);
}

void MixerTransport::StopPlayback(const AppState& state)
{
    player_->Stop();
    ResetLoopCachePlayback();
    ClearQueues();
    ClearLiveMixerOverrides();
    std::memset(current_program_, 0, sizeof(current_program_));
    std::memset(note_refcount_, 0, sizeof(note_refcount_));
    std::memset(active_note_count_, 0, sizeof(active_note_count_));
    std::memset(note_on_count_, 0, sizeof(note_on_count_));
    for(size_t ch = 0; ch < 16; ch++)
    {
        highest_note_[ch] = -1;
        lowest_note_[ch]  = -1;
    }
    SynthPanic();
    SynthResetChannels();
    play_start_sample_  = sample_clock_;
    play_start_ticks_   = 0;
    phase_start_sample_ = sample_clock_;
    phase_start_ticks_  = 0;
    ApplyMixerState(state, true);
}

void MixerTransport::EnqueueChannelMixerState(uint8_t ch, const AppState& state)
{
    MidiEv ev{};

    ev.type = EvType::ControlChange;
    ev.ch   = ch;
    ev.a    = 7;
    ev.b    = EffectiveVolume(ch, state);
    EnqueueImmediate(ev);

    ev.a = 10;
    ev.b = EffectivePan(ch, state);
    EnqueueImmediate(ev);

    ev.a = 91;
    ev.b = EffectiveReverb(ch, state);
    EnqueueImmediate(ev);

    ev.a = 93;
    ev.b = EffectiveChorus(ch, state);
    EnqueueImmediate(ev);
}

void MixerTransport::ApplyMixerState(const AppState& state, bool force)
{
    for(uint8_t ch = 0; ch < 16; ch++)
    {
        const ChannelState& desired = state.channels[ch];
        const bool mute_changed_to_on = (force || has_applied_state_) && desired.muted
                                        && !applied_channels_[ch].muted;
        if(force || !has_applied_state_ || desired.volume != applied_channels_[ch].volume
           || desired.pan != applied_channels_[ch].pan
           || desired.reverb_send != applied_channels_[ch].reverb_send
           || desired.chorus_send != applied_channels_[ch].chorus_send
           || desired.muted != applied_channels_[ch].muted)
        {
            if(mute_changed_to_on)
                FlushChannelNotes(ch);
            EnqueueChannelMixerState(ch, state);
            applied_channels_[ch] = desired;
        }

        const bool has_override = desired.program_override >= 0;
        has_program_override_[ch] = has_override;
        program_override_[ch]     = desired.program_override;
        if(force || applied_program_override_[ch] != desired.program_override)
        {
            applied_program_override_[ch] = desired.program_override;
            if(has_override)
            {
                MidiEv ev{};
                ev.type = EvType::Program;
                ev.ch   = ch;
                ev.a    = static_cast<uint8_t>(desired.program_override);
                EnqueueImmediate(ev);
            }
        }
    }

    has_applied_state_ = true;
}

bool MixerTransport::LoopActive(const AppState& state) const
{
    return state.song_loop_enabled
           && state.loop_length_ticks > 0;
}

uint64_t MixerTransport::LoopStartTicks(const AppState& state) const
{
    return static_cast<uint64_t>(state.loop_start_tick);
}

uint64_t MixerTransport::LoopLengthTicks(const AppState& state) const
{
    return static_cast<uint64_t>(state.loop_length_ticks > 0 ? state.loop_length_ticks : 1);
}

uint64_t MixerTransport::LoopLengthSamples(const AppState& state) const
{
    return player_->SamplesFromTicksRange(LoopStartTicks(state), LoopLengthTicks(state));
}

uint64_t MixerTransport::LoopEndSample(const AppState& state) const
{
    return phase_start_sample_ + LoopLengthSamples(state);
}

uint64_t MixerTransport::LoopBoundarySample(const AppState& state) const
{
    return LoopEndSample(state);
}

uint64_t MixerTransport::MeasureStartTicks(int measure) const
{
    const uint16_t divisions = player_->Divisions();
    if(divisions == 0)
        return 0;

    const int ts_num            = player_->TimeSigNumerator() > 0 ? player_->TimeSigNumerator() : 4;
    const int ts_den            = player_->TimeSigDenominator() > 0 ? player_->TimeSigDenominator() : 4;
    const int ticks_per_beat    = (divisions * 4) / ts_den;
    const int beats_per_measure = ts_num;
    const int m                 = measure < 1 ? 1 : measure;
    return static_cast<uint64_t>(m - 1) * static_cast<uint64_t>(beats_per_measure)
           * static_cast<uint64_t>(ticks_per_beat);
}

void MixerTransport::Update(const AppState& state)
{
    if(player_ == nullptr)
        return;

    const uint64_t requested_loop_start_tick  = LoopStartTicks(state);
    const uint64_t requested_loop_length_tick = LoopLengthTicks(state);
    master_volume_max_ = state.sf2_master_volume_max;
    expression_max_    = state.sf2_expression_max;
    reverb_max_        = state.sf2_reverb_max;
    chorus_max_        = state.sf2_chorus_max;
    transpose_         = state.sf2_transpose;
    loop_active_       = LoopActive(state);
    loop_length_samples_ = loop_active_ ? LoopLengthSamples(state) : 0;
    loop_end_sample_   = loop_active_ ? LoopBoundarySample(state) : UINT64_MAX;
    if(!loop_active_)
        loop_cache_valid_ = false;

    const bool loop_settings_changed
        = loop_active_
          && (requested_loop_start_tick != loop_cache_start_tick_
              || requested_loop_length_tick != loop_cache_length_ticks_);

    if(loop_active_ && !IsPlaying() && (!loop_cache_valid_ || loop_settings_changed))
        EnsureLoopCache(state);

    bool restart_loop_transport = false;
    if(state.bpm != applied_bpm_)
    {
        const bool     had_applied_bpm = applied_bpm_ > 0;
        const bool     ramp_eligible
            = had_applied_bpm && !state.sync_external
              && !(loop_active_ && state.transport_playing);

        int next_bpm = state.bpm;
        if(ramp_eligible && sample_rate_ > 0.0f)
        {
            const uint64_t elapsed_samples = sample_clock_ >= bpm_ramp_last_sample_
                                                  ? (sample_clock_ - bpm_ramp_last_sample_)
                                                  : 0;
            const float elapsed_seconds = static_cast<float>(elapsed_samples) / sample_rate_;
            const int   max_step
                = elapsed_seconds > 0.0f
                      ? static_cast<int>(kTempoRampBpmPerSecond * elapsed_seconds) : 0;
            const int diff     = state.bpm - applied_bpm_;
            const int abs_diff = diff < 0 ? -diff : diff;
            if(abs_diff > max_step)
                next_bpm = applied_bpm_ + (diff > 0 ? max_step : -max_step);
        }

        // If next_bpm == applied_bpm_, the ramp is waiting for more elapsed
        // time to justify at least a 1 BPM step: skip the remap work below
        // (avoids IRQ-blocked queue churn on a no-op every main-loop
        // iteration) without resetting the ramp timer, so elapsed time
        // keeps accumulating toward the next real step.
        if(next_bpm != applied_bpm_)
        {
            bpm_ramp_last_sample_ = sample_clock_;

            const uint64_t current_cycle   = CurrentCycleSample();
            const uint64_t current_tick    = CurrentSongTick();
            const double   old_bpm         = had_applied_bpm ? static_cast<double>(applied_bpm_)
                                                             : static_cast<double>(next_bpm);
            const double   new_bpm         = next_bpm > 0 ? static_cast<double>(next_bpm)
                                                           : old_bpm;
            const double   ratio           = (new_bpm > 0.0) ? (old_bpm / new_bpm) : 1.0;
            const float scale = file_bpm_ > 0.0f ? static_cast<float>(next_bpm) / file_bpm_
                                                 : 1.0f;
            player_->SetTempoScale(scale, sample_clock_);
            if((player_->IsPlaying() || loop_cache_playback_) && had_applied_bpm)
            {
                if(loop_active_ && state.transport_playing)
                {
                    const uint64_t loop_start_tick   = requested_loop_start_tick;
                    const uint64_t loop_length_ticks = requested_loop_length_tick;
                    const uint64_t ticks_into_cycle
                        = current_tick > loop_start_tick ? (current_tick - loop_start_tick) : 0;
                    const uint64_t clamped_ticks_into_cycle
                        = loop_length_ticks > 0
                              ? (ticks_into_cycle < loop_length_ticks ? ticks_into_cycle
                                                                      : (loop_length_ticks - 1))
                              : 0;
                    const uint64_t cycle_sample
                        = player_->SamplesFromTicksRange(loop_start_tick, clamped_ticks_into_cycle);

                    FlushLoopBoundaryNotes();
                    ClearQueues();
                    ResetLoopCachePlayback();
                    loop_cache_valid_ = false;
                    EnsureLoopCache(state);

                    player_->SeekToSample(player_->SamplesFromTicks(current_tick), sample_clock_);
                    play_start_sample_  = sample_clock_ >= cycle_sample ? (sample_clock_ - cycle_sample)
                                                                        : 0;
                    play_start_ticks_   = loop_start_tick;
                    phase_start_sample_ = play_start_sample_;
                    phase_start_ticks_  = loop_start_tick;
                    loop_length_samples_ = LoopLengthSamples(state);
                    loop_end_sample_     = phase_start_sample_ + loop_length_samples_;
                }
                else
                {
                    RemapQueuedEventTimes(sample_clock_, ratio);
                    const uint64_t new_ticks_into_cycle = player_->TicksFromSamples(current_cycle);
                    phase_start_ticks_ = current_tick >= new_ticks_into_cycle
                                             ? (current_tick - new_ticks_into_cycle)
                                             : 0;
                }
            }
            applied_bpm_ = next_bpm;
        }
    }

    if(loop_settings_changed && state.transport_playing)
        restart_loop_transport = true;

    if(restart_loop_transport)
    {
        StopPlayback(state);
        EnsureLoopCache(state);
        StartPlayback(state);
    }

    if(state.transport_playing && !IsPlaying())
        StartPlayback(state);
    else if(!state.transport_playing && IsPlaying())
        StopPlayback(state);

    const uint64_t sample_now = sample_clock_;
    if(loop_cache_playback_ || loop_cache_pending_)
    {
        PumpLoopCache(sample_now);
    }
    else if(player_->IsPlaying())
    {
        for(int i = 0; i < 2; i++)
        {
            player_->Pump(parsed_, sample_now);
            TransferScheduledFromParser(state);
            if(!MaybeWrapLoopParser(state, sample_now))
                break;
        }
    }

    ApplyMixerState(state);
}

void MixerTransport::SyncExternalTick(uint64_t tick, bool force_reseek)
{
    if(player_ == nullptr || !IsPlaying())
        return;

    const uint64_t current_tick = CurrentSongTick();
    if(!force_reseek && current_tick == tick)
        return;

    if(force_reseek)
    {
        ResetLoopCachePlayback();
        ClearQueues();
        FlushLoopBoundaryNotes();
        player_->SeekToSample(player_->SamplesFromTicks(tick), sample_clock_);
        phase_start_sample_ = sample_clock_;
        phase_start_ticks_  = tick;
        play_start_sample_  = sample_clock_;
        play_start_ticks_   = tick;
        loop_end_sample_    = loop_active_ ? (phase_start_sample_ + loop_length_samples_)
                                           : UINT64_MAX;
        return;
    }

    const uint64_t current_sample_pos = player_->SamplesFromTicks(current_tick);
    const uint64_t target_sample_pos  = player_->SamplesFromTicks(tick);
    const int64_t  delta_samples
        = static_cast<int64_t>(target_sample_pos)
          - static_cast<int64_t>(current_sample_pos);

    ShiftQueuedEventTimes(sample_clock_, delta_samples);
    player_->RebaseToTick(tick, sample_clock_);
    phase_start_sample_ = sample_clock_;
    phase_start_ticks_  = tick;
    if(!loop_cache_pending_)
    {
        play_start_sample_ = sample_clock_;
        play_start_ticks_  = tick;
    }
}

void MixerTransport::HandleMidiMessage(MidiEvent msg, const AppState& state)
{
    switch(msg.type)
    {
        case MidiMessageType::NoteOn:
        {
            const auto note = msg.AsNoteOn();
            MidiEv     ev{};
            ev.type = note.velocity == 0 ? EvType::NoteOff : EvType::NoteOn;
            ev.ch   = note.channel;
            ev.a    = note.note;
            ev.b    = note.velocity;
            if(!ChannelEventBlockedByMute(ev, state))
                EnqueueImmediate(ev);
        }
        break;

        case MidiMessageType::NoteOff:
        {
            const auto note = msg.AsNoteOff();
            MidiEv     ev{};
            ev.type = EvType::NoteOff;
            ev.ch   = note.channel;
            ev.a    = note.note;
            if(!ChannelEventBlockedByMute(ev, state))
                EnqueueImmediate(ev);
        }
        break;

        case MidiMessageType::ProgramChange:
        {
            const auto pgm = msg.AsProgramChange();
            MidiEv     ev{};
            ev.type = EvType::Program;
            ev.ch   = pgm.channel;
            ev.a    = pgm.program;
            if(!ChannelEventBlockedByMute(ev, state))
                EnqueueImmediate(ev);
        }
        break;

        case MidiMessageType::ControlChange:
        {
            const auto cc = msg.AsControlChange();
            if(cc.control_number == 120 || cc.control_number == 123)
            {
                MidiEv ev{};
                ev.type = cc.control_number == 120 ? EvType::AllSoundOff : EvType::AllNotesOff;
                ev.ch   = cc.channel;
                EnqueueImmediate(ev);
            }
            else if(cc.control_number == 7 || cc.control_number == 10
                    || cc.control_number == 91 || cc.control_number == 93)
            {
                switch(cc.control_number)
                {
                    case 7:
                        live_volume_[cc.channel]     = cc.value;
                        has_live_volume_[cc.channel] = true;
                        break;
                    case 10:
                        live_pan_[cc.channel]     = cc.value;
                        has_live_pan_[cc.channel] = true;
                        break;
                    case 91:
                        live_reverb_[cc.channel]     = cc.value;
                        has_live_reverb_[cc.channel] = true;
                        break;
                    case 93:
                        live_chorus_[cc.channel]     = cc.value;
                        has_live_chorus_[cc.channel] = true;
                        break;
                    default: break;
                }
                EnqueueChannelMixerState(cc.channel, state);
            }
            else
            {
                MidiEv ev{};
                ev.type = EvType::ControlChange;
                ev.ch   = cc.channel;
                ev.a    = cc.control_number;
                ev.b    = cc.control_number == 11
                              ? ScaleController(cc.value, state.sf2_expression_max)
                              : cc.value;
                if(!ChannelEventBlockedByMute(ev, state))
                    EnqueueImmediate(ev);
            }
        }
        break;

        case MidiMessageType::PitchBend:
        {
            const uint16_t bend = (uint16_t(msg.data[1]) << 7) | msg.data[0];
            MidiEv         ev{};
            ev.type = EvType::PitchBend;
            ev.ch   = msg.channel;
            ev.a    = bend & 0x7F;
            ev.b    = (bend >> 7) & 0x7F;
            if(!ChannelEventBlockedByMute(ev, state))
                EnqueueImmediate(ev);
        }
        break;

        case MidiMessageType::ChannelMode:
        {
            const auto mode = msg.AsChannelMode();
            if(mode.event_type == ChannelModeType::AllNotesOff
               || mode.event_type == ChannelModeType::AllSoundOff)
            {
                MidiEv ev{};
                ev.type = mode.event_type == ChannelModeType::AllNotesOff
                              ? EvType::AllNotesOff
                              : EvType::AllSoundOff;
                ev.ch = mode.channel;
                EnqueueImmediate(ev);
            }
        }
        break;

        default: break;
    }
}

} // namespace major_midi
