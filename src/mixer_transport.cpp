#include "mixer_transport.h"
#include "synth_tsf.h"
#include "util/scopedirqblocker.h"

namespace major_midi
{

using namespace daisy;

namespace
{
// Tick-to-sample conversion is rounded, so a seam event can quantize one sample
// before the theoretical loop boundary. Treat that as "at the boundary".
constexpr uint64_t kLoopBoundaryGuardSamples = 1;
}

void MixerTransport::Init(float sample_rate, SmfPlayer& player)
{
    sample_rate_ = sample_rate;
    player_      = &player;
}

void MixerTransport::Reset(const AppState& state)
{
    if(player_ != nullptr && player_->IsPlaying())
        player_->Stop();
    ClearQueues();
    SynthPanic();
    SynthResetChannels();
    has_applied_state_ = false;
    applied_bpm_       = -1;
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
    scheduled_.Clear();
    parsed_.Clear();
    immediate_.Clear();
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

void MixerTransport::DispatchEvent(const MidiEv& ev, bool scheduled_source)
{
    MidiEv actual = ev;

    if(actual.type == EvType::NoteOn || actual.type == EvType::NoteOff)
        actual.a = ApplyTranspose(actual.ch, actual.a);
    if(actual.type == EvType::ControlChange && actual.a == 11)
        actual.b = ScaleController(actual.b, expression_max_);

    if(ev.ch < 16)
    {
        switch(ev.type)
        {
            case EvType::NoteOn:
            case EvType::NoteOff:
            case EvType::Program:
            case EvType::ControlChange:
            case EvType::PitchBend: channel_activity_[ev.ch] = 1; break;
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
            if(!(scheduled_source
                 && (actual.a == 7 || actual.a == 10 || actual.a == 91 || actual.a == 93)))
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
        parsed_.Pop(ev);
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
    uint64_t       loop_start_samples = player_->SamplesFromTicks(loop_start_ticks);
    if(loop_start_samples > 0)
        loop_start_samples -= 1;

    parsed_.Clear();
    player_->SeekToSample(loop_start_samples, restart_sample);
    play_start_sample_ = restart_sample;
    play_start_ticks_  = loop_start_ticks;
    loop_end_sample_   = LoopBoundarySample(state);
    return true;
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
            DispatchEvent(next_ev, true);
        } while(PeekScheduled(next_ev) && next_ev.atSample <= current_sample);
    }

    sample_clock_ = block_sample + size;
}

void MixerTransport::StartPlayback(const AppState& state)
{
    ClearQueues();
    SynthPanic();
    SynthResetChannels();

    const uint64_t sample_now = sample_clock_;
    if(LoopActive(state))
    {
        const uint64_t loop_start_ticks   = LoopStartTicks(state);
        uint64_t       loop_start_samples = player_->SamplesFromTicks(loop_start_ticks);
        if(loop_start_samples > 0)
            loop_start_samples -= 1;
        player_->SeekToSample(loop_start_samples, sample_now);
        play_start_sample_ = sample_now;
        play_start_ticks_  = loop_start_ticks;
    }
    else
    {
        player_->Start(sample_now);
        play_start_sample_ = sample_now;
        play_start_ticks_  = 0;
    }

    ApplyMixerState(state, true);
}

void MixerTransport::StopPlayback(const AppState& state)
{
    player_->Stop();
    ClearQueues();
    SynthPanic();
    SynthResetChannels();
    ApplyMixerState(state, true);
}

void MixerTransport::EnqueueChannelMixerState(uint8_t ch, const AppState& state)
{
    MidiEv ev{};
    const ChannelState& channel       = state.channels[ch];
    const uint8_t       effective_vol = (state.mute_all || channel.muted)
                                            ? 0
                                            : ScaleController(channel.volume, state.sf2_master_volume_max);

    ev.type = EvType::ControlChange;
    ev.ch   = ch;
    ev.a    = 7;
    ev.b    = effective_vol;
    EnqueueImmediate(ev);

    ev.a = 10;
    ev.b = channel.pan;
    EnqueueImmediate(ev);

    ev.a = 91;
    ev.b = ScaleController(channel.reverb_send, state.sf2_reverb_max);
    EnqueueImmediate(ev);

    ev.a = 93;
    ev.b = ScaleController(channel.chorus_send, state.sf2_chorus_max);
    EnqueueImmediate(ev);
}

void MixerTransport::ApplyMixerState(const AppState& state, bool force)
{
    for(uint8_t ch = 0; ch < 16; ch++)
    {
        const ChannelState& desired = state.channels[ch];
        if(force || !has_applied_state_ || applied_mute_all_ != state.mute_all
           || desired.volume != applied_channels_[ch].volume
           || desired.pan != applied_channels_[ch].pan
           || desired.reverb_send != applied_channels_[ch].reverb_send
           || desired.chorus_send != applied_channels_[ch].chorus_send
           || desired.muted != applied_channels_[ch].muted)
        {
            EnqueueChannelMixerState(ch, state);
            applied_channels_[ch] = desired;
        }
    }

    applied_mute_all_  = state.mute_all;
    has_applied_state_ = true;
}

bool MixerTransport::LoopActive(const AppState& state) const
{
    return state.song_loop_enabled
           && state.loop_length_beats > 0;
}

uint64_t MixerTransport::LoopStartTicks(const AppState& state) const
{
    const uint16_t divisions = player_->Divisions();
    if(divisions == 0)
        return 0;

    const int ts_num            = player_->TimeSigNumerator() > 0 ? player_->TimeSigNumerator() : 4;
    const int ts_den            = player_->TimeSigDenominator() > 0 ? player_->TimeSigDenominator() : 4;
    const int beats_per_measure = ts_num;
    const int ticks_per_beat    = (divisions * 4) / ts_den;
    const int sub_per_beat      = ts_den > 0 ? (16 / ts_den) : 4;
    const int ticks_per_sub     = sub_per_beat > 0 ? (ticks_per_beat / sub_per_beat) : ticks_per_beat;

    const int measure_index = state.loop_start_measure > 1 ? (state.loop_start_measure - 1) : 0;
    const int beat_index    = state.loop_start_beat > 1 ? (state.loop_start_beat - 1) : 0;
    const int sub_index     = state.loop_start_sub > 1 ? (state.loop_start_sub - 1) : 0;

    return static_cast<uint64_t>(measure_index) * static_cast<uint64_t>(beats_per_measure)
               * static_cast<uint64_t>(ticks_per_beat)
           + static_cast<uint64_t>(beat_index) * static_cast<uint64_t>(ticks_per_beat)
           + static_cast<uint64_t>(sub_index) * static_cast<uint64_t>(ticks_per_sub);
}

uint64_t MixerTransport::LoopLengthTicks(const AppState& state) const
{
    const uint16_t divisions = player_->Divisions();
    if(divisions == 0)
        return 0;

    const int ts_den         = player_->TimeSigDenominator() > 0 ? player_->TimeSigDenominator() : 4;
    const int ticks_per_beat = (divisions * 4) / ts_den;
    return static_cast<uint64_t>(state.loop_length_beats > 0 ? state.loop_length_beats : 1)
           * static_cast<uint64_t>(ticks_per_beat);
}

uint64_t MixerTransport::LoopLengthSamples(const AppState& state) const
{
    const int bpm = state.bpm > 0 ? state.bpm : 120;
    const int ts_den = player_->TimeSigDenominator() > 0 ? player_->TimeSigDenominator() : 4;
    const int beat_count = state.loop_length_beats > 0 ? state.loop_length_beats : 1;

    const double quarter_note_samples = (sample_rate_ * 60.0) / static_cast<double>(bpm);
    const double beat_unit_scale      = 4.0 / static_cast<double>(ts_den);
    const double loop_samples = quarter_note_samples * beat_unit_scale
                                * static_cast<double>(beat_count);
    return static_cast<uint64_t>(llround(loop_samples));
}

uint64_t MixerTransport::LoopEndSample(const AppState& state) const
{
    return play_start_sample_ + LoopLengthSamples(state);
}

uint64_t MixerTransport::LoopBoundarySample(const AppState& state) const
{
    const uint64_t loop_end_sample = LoopEndSample(state);
    if(loop_end_sample > kLoopBoundaryGuardSamples)
        return loop_end_sample - kLoopBoundaryGuardSamples;
    return 0;
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

    master_volume_max_ = state.sf2_master_volume_max;
    expression_max_    = state.sf2_expression_max;
    reverb_max_        = state.sf2_reverb_max;
    chorus_max_        = state.sf2_chorus_max;
    transpose_         = state.sf2_transpose;
    loop_active_       = LoopActive(state);
    loop_end_sample_   = loop_active_ ? LoopBoundarySample(state) : UINT64_MAX;

    if(state.bpm != applied_bpm_)
    {
        const float scale = file_bpm_ > 0.0f ? static_cast<float>(state.bpm) / file_bpm_
                                             : 1.0f;
        player_->SetTempoScale(scale, sample_clock_);
        applied_bpm_ = state.bpm;
    }

    if(state.transport_playing && !player_->IsPlaying())
        StartPlayback(state);
    else if(!state.transport_playing && player_->IsPlaying())
        StopPlayback(state);

    const uint64_t sample_now = sample_clock_;
    if(player_->IsPlaying())
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
