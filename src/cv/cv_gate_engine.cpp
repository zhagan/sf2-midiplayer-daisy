#include "cv_gate_engine.h"

#include <cmath>

#include "synth_tsf.h"

using namespace daisy;
using namespace patch_sm;

namespace major_midi
{

namespace
{
constexpr float kCvInMinBpm   = 20.0f;
constexpr float kCvInMaxBpm   = 300.0f;
constexpr float kCvOutMaxVolt = 5.0f;
constexpr float kGatePulseMs  = 10.0f;
constexpr int   kCvPitchBaseNote = 24;
constexpr int   kCvTriggerNoteSpan = 60;
constexpr uint8_t kGateTriggerVelocity = 100;

float Clamp01(float v)
{
    if(v < 0.0f)
        return 0.0f;
    if(v > 1.0f)
        return 1.0f;
    return v;
}

float MidiCcToVoltage(uint8_t value)
{
    return (static_cast<float>(value) / 127.0f) * kCvOutMaxVolt;
}

float MidiNoteToVoltage(int note)
{
    if(note < 0)
        return 0.0f;
    const int note_offset = note - kCvPitchBaseNote;
    if(note_offset <= 0)
        return 0.0f;
    const float volts = static_cast<float>(note_offset) / 12.0f;
    if(volts > kCvOutMaxVolt)
        return kCvOutMaxVolt;
    return volts;
}

float ApplyPitchScale(float voltage, uint16_t pitch_scale)
{
    const float scaled = voltage * (static_cast<float>(pitch_scale) / 1000.0f);
    if(scaled < 0.0f)
        return 0.0f;
    if(scaled > kCvOutMaxVolt)
        return kCvOutMaxVolt;
    return scaled;
}

int ResolutionDenominator(SyncResolution resolution)
{
    switch(resolution)
    {
        case SyncResolution::Div4: return 4;
        case SyncResolution::Div8: return 8;
        case SyncResolution::Div16: return 16;
        case SyncResolution::Div32: return 32;
        case SyncResolution::Div64: return 64;
    }
    return 16;
}

uint8_t QuantizeCvToMidiNote(float cv_value)
{
    const int note_offset
        = static_cast<int>(std::lround(Clamp01(cv_value) * static_cast<float>(kCvTriggerNoteSpan)));
    const int note = kCvPitchBaseNote + note_offset;
    if(note < 0)
        return 0;
    if(note > 127)
        return 127;
    return static_cast<uint8_t>(note);
}

MidiEvent MakeNoteEvent(MidiMessageType type, uint8_t channel, uint8_t note, uint8_t value)
{
    MidiEvent msg{};
    msg.type    = type;
    msg.channel = channel;
    msg.data[0] = note;
    msg.data[1] = value;
    return msg;
}

bool PulseWindowIntersectsBlock(uint64_t block_start_sample,
                                uint64_t block_end_sample,
                                uint64_t period_samples,
                                uint64_t gate_samples)
{
    if(period_samples == 0 || block_end_sample <= block_start_sample)
        return false;

    const uint64_t pulse_width = gate_samples > 0 ? gate_samples : 1u;
    const uint64_t first_period = block_start_sample / period_samples;
    const uint64_t last_period  = (block_end_sample - 1u) / period_samples;

    for(uint64_t period = first_period; period <= last_period; period++)
    {
        const uint64_t pulse_start = period * period_samples;
        const uint64_t pulse_end   = pulse_start + pulse_width;
        if(pulse_end > block_start_sample && pulse_start < block_end_sample)
            return true;
    }
    return false;
}

bool TickBoundaryInRange(uint64_t start_tick, uint64_t end_tick, uint64_t period_ticks)
{
    if(period_ticks == 0 || end_tick < start_tick)
        return false;

    uint64_t boundary_tick = (start_tick / period_ticks) * period_ticks;
    if(boundary_tick < start_tick)
        boundary_tick += period_ticks;
    return boundary_tick <= end_tick;
}

uint32_t CountTickBoundariesInRange(uint64_t start_tick, uint64_t end_tick, uint64_t period_ticks)
{
    if(period_ticks == 0 || end_tick < start_tick)
        return 0;

    uint64_t first_boundary = (start_tick / period_ticks) * period_ticks;
    if(first_boundary < start_tick)
        first_boundary += period_ticks;
    if(first_boundary > end_tick)
        return 0;

    return static_cast<uint32_t>(((end_tick - first_boundary) / period_ticks) + 1u);
}
} // namespace

void CvGateEngine::Init(DaisyPatchSM& hw, float sample_rate)
{
    hw_          = &hw;
    sample_rate_ = sample_rate;
    was_playing_ = false;
    for(size_t i = 0; i < 2; i++)
    {
        active_gate_note_[i]    = 0;
        active_gate_channel_[i] = 0;
        gate_note_active_[i]    = false;
        held_pitch_note_[i]     = -1;
        last_gate_note_on_counter_[i] = 0;
        last_gate_channel_active_[i]  = false;
        gate_retrigger_low_pending_[i] = false;
        gate_out_pulse_remaining_[i] = 0;
    }
}

float CvGateEngine::ReadCvInput(size_t index) const
{
    if(hw_ == nullptr || index > 1)
        return 0.0f;
    return Clamp01(hw_->GetAdcValue(index == 0 ? CV_5 : CV_6));
}

int CvGateEngine::EffectiveBpm(const AppState& state) const
{
    return live_bpm_ > 0 ? live_bpm_ : state.bpm;
}

float CvGateEngine::PitchVoltageForChannel(size_t                output_index,
                                           const MixerTransport& transport,
                                           const CvOutputConfig& config,
                                           uint16_t              pitch_scale,
                                           bool                  playing)
{
    const int note = transport.ChannelPitchNote(config.channel, config.priority);
    if(note >= 0)
        held_pitch_note_[output_index] = static_cast<int8_t>(note);
    else if(!playing)
        held_pitch_note_[output_index] = -1;
    return ApplyPitchScale(MidiNoteToVoltage(held_pitch_note_[output_index]), pitch_scale);
}

float CvGateEngine::CcVoltageForChannel(const MixerTransport& transport,
                                        const CvOutputConfig& config) const
{
    return MidiCcToVoltage(transport.ChannelCcValue(config.channel, config.cc));
}

bool CvGateEngine::SyncGateHigh(uint64_t cycle_sample,
                                int      bpm,
                                SyncResolution resolution) const
{
    if(bpm <= 0)
        return false;
    const int   denom            = ResolutionDenominator(resolution);
    const float quarter_samples  = (sample_rate_ * 60.0f) / static_cast<float>(bpm);
    const float pulse_samples_f  = quarter_samples * (4.0f / static_cast<float>(denom));
    const uint64_t period_samples = pulse_samples_f > 1.0f ? static_cast<uint64_t>(pulse_samples_f)
                                                           : 1u;
    const uint64_t gate_samples
        = static_cast<uint64_t>((sample_rate_ * kGatePulseMs) / 1000.0f);
    return (cycle_sample % period_samples) < (gate_samples > 0 ? gate_samples : 1u);
}

bool CvGateEngine::SyncGateHighInBlock(uint64_t block_start_sample,
                                       uint64_t block_end_sample,
                                       int      bpm,
                                       SyncResolution resolution) const
{
    if(bpm <= 0)
        return false;
    const int denom = ResolutionDenominator(resolution);
    const float quarter_samples = (sample_rate_ * 60.0f) / static_cast<float>(bpm);
    const float pulse_samples_f = quarter_samples * (4.0f / static_cast<float>(denom));
    const uint64_t period_samples = pulse_samples_f > 1.0f ? static_cast<uint64_t>(pulse_samples_f)
                                                           : 1u;
    const uint64_t gate_samples
        = static_cast<uint64_t>((sample_rate_ * kGatePulseMs) / 1000.0f);
    return PulseWindowIntersectsBlock(
        block_start_sample, block_end_sample, period_samples, gate_samples);
}

bool CvGateEngine::ResetGateHigh(uint64_t cycle_sample,
                                 int      bpm,
                                 int      time_sig_num,
                                 int      time_sig_den) const
{
    if(bpm <= 0)
        return false;
    if(time_sig_num <= 0)
        time_sig_num = 4;
    if(time_sig_den <= 0)
        time_sig_den = 4;

    const float quarter_samples = (sample_rate_ * 60.0f) / static_cast<float>(bpm);
    const float beat_samples    = quarter_samples * (4.0f / static_cast<float>(time_sig_den));
    const uint64_t measure_samples
        = static_cast<uint64_t>(beat_samples * static_cast<float>(time_sig_num));
    const uint64_t gate_samples
        = static_cast<uint64_t>((sample_rate_ * kGatePulseMs) / 1000.0f);
    if(measure_samples == 0)
        return false;
    return (cycle_sample % measure_samples) < (gate_samples > 0 ? gate_samples : 1u);
}

bool CvGateEngine::ResetGateHighInBlock(uint64_t block_start_sample,
                                        uint64_t block_end_sample,
                                        int      bpm,
                                        int      time_sig_num,
                                        int      time_sig_den) const
{
    if(bpm <= 0)
        return false;
    if(time_sig_num <= 0)
        time_sig_num = 4;
    if(time_sig_den <= 0)
        time_sig_den = 4;

    const float quarter_samples = (sample_rate_ * 60.0f) / static_cast<float>(bpm);
    const float beat_samples    = quarter_samples * (4.0f / static_cast<float>(time_sig_den));
    const uint64_t measure_samples
        = static_cast<uint64_t>(beat_samples * static_cast<float>(time_sig_num));
    const uint64_t gate_samples
        = static_cast<uint64_t>((sample_rate_ * kGatePulseMs) / 1000.0f);
    return PulseWindowIntersectsBlock(
        block_start_sample, block_end_sample, measure_samples, gate_samples);
}

void CvGateEngine::Update(const AppState& state, MixerTransport& transport)
{
    if(hw_ == nullptr)
        return;

    live_bpm_ = 0;

    const float base_gain = static_cast<float>(state.master_volume_pct) / 100.0f;

    bool master_volume_applied = false;
    for(size_t i = 0; i < 2; i++)
    {
        const float cv_value = ReadCvInput(i);
        switch(state.cv_gate.cv_in[i].mode)
        {
            case CvInMode::Off: break;
            case CvInMode::MasterVolume:
                SynthSetExternalGain(base_gain * cv_value);
                master_volume_applied = true;
                break;
            case CvInMode::Bpm:
                live_bpm_ = static_cast<int>(std::lround(kCvInMinBpm
                                                         + cv_value * (kCvInMaxBpm - kCvInMinBpm)));
                break;
            case CvInMode::ChannelPitch:
            {
                const uint16_t bend = static_cast<uint16_t>(std::lround(cv_value * 16383.0f));
                SynthPitchBend(state.cv_gate.cv_in[i].channel, bend);
            }
                break;
            case CvInMode::ChannelCc:
                SynthControlChange(state.cv_gate.cv_in[i].channel,
                                   state.cv_gate.cv_in[i].cc,
                                   static_cast<uint8_t>(std::lround(cv_value * 127.0f)));
                break;
            case CvInMode::NotePitch: break;
        }
    }

    if(!master_volume_applied)
    {
        SynthSetExternalGain(base_gain);
    }

    for(size_t i = 0; i < 2; i++)
    {
        const bool trigger_enabled = state.cv_gate.gate_in[i].mode == GateInMode::NoteTrigger;
        const bool gate_high = (i == 0) ? hw_->gate_in_1.State() : hw_->gate_in_2.State();

        if(trigger_enabled && gate_high)
        {
            const uint8_t note    = QuantizeCvToMidiNote(ReadCvInput(i));
            const uint8_t channel = state.cv_gate.gate_in[i].channel;
            if(gate_note_active_[i]
               && (active_gate_note_[i] != note || active_gate_channel_[i] != channel))
            {
                transport.HandleMidiMessage(MakeNoteEvent(MidiMessageType::NoteOff,
                                                          active_gate_channel_[i],
                                                          active_gate_note_[i],
                                                          0),
                                            state);
                gate_note_active_[i] = false;
            }

            if(!gate_note_active_[i])
            {
                transport.HandleMidiMessage(MakeNoteEvent(MidiMessageType::NoteOn,
                                                          channel,
                                                          note,
                                                          kGateTriggerVelocity),
                                            state);
                active_gate_note_[i]    = note;
                active_gate_channel_[i] = channel;
                gate_note_active_[i]    = true;
            }
        }
        else if(gate_note_active_[i])
        {
            transport.HandleMidiMessage(MakeNoteEvent(MidiMessageType::NoteOff,
                                                      active_gate_channel_[i],
                                                      active_gate_note_[i],
                                                      0),
                                        state);
            gate_note_active_[i] = false;
        }
    }

    const uint64_t block_size         = hw_->AudioBlockSize();
    const uint64_t block_sample_start = transport.SampleClock();
    const uint64_t block_sample_end   = block_sample_start + block_size;
    const uint64_t block_start_tick   = transport.SongTickAt(block_sample_start);
    const uint64_t block_end_tick     = transport.SongTickAt(block_sample_end);
    const uint64_t loop_length_samples = transport.CurrentLoopLengthSamples();
    const bool     wrapped_block
        = loop_length_samples > 0 && block_end_tick < block_start_tick;
    const bool playing = state.transport_playing;
    if(!playing)
    {
        for(size_t i = 0; i < 2; i++)
        {
            held_pitch_note_[i]            = -1;
            gate_retrigger_low_pending_[i] = false;
            last_gate_channel_active_[i]   = false;
            last_gate_note_on_counter_[i]  = 0;
        }
    }
    const int  ts_num  = transport.TimeSigNumerator();
    const int  ts_den  = transport.TimeSigDenominator();
    const uint16_t divisions = transport.Divisions();
    const uint64_t ticks_per_beat
        = (divisions > 0 && ts_den > 0) ? (uint64_t(divisions) * 4u) / uint64_t(ts_den) : 0u;
    const uint64_t ticks_per_measure = ticks_per_beat * uint64_t(ts_num > 0 ? ts_num : 4);
    const uint64_t gate_pulse_samples
        = static_cast<uint64_t>((sample_rate_ * kGatePulseMs) / 1000.0f);
    const bool started_playing = playing && !was_playing_;

    for(size_t i = 0; i < 2; i++)
    {
        if(gate_out_pulse_remaining_[i] > 0)
        {
            gate_out_pulse_remaining_[i]
                = gate_out_pulse_remaining_[i] > block_size
                      ? (gate_out_pulse_remaining_[i] - block_size)
                      : 0u;
        }

        bool gate_high = false;
        switch(state.cv_gate.gate_out[i].mode)
        {
            case GateOutMode::Off: gate_high = false; break;
            case GateOutMode::SyncOut:
                if(playing)
                {
                    const uint64_t ticks_per_pulse
                        = divisions > 0
                              ? (uint64_t(divisions) * 4u)
                                    / uint64_t(ResolutionDenominator(
                                        state.cv_gate.gate_out[i].sync_resolution))
                              : 0u;
                    uint32_t pulse_count = 0;
                    if(!wrapped_block)
                    {
                        pulse_count
                            = CountTickBoundariesInRange(block_start_tick,
                                                         block_end_tick,
                                                         ticks_per_pulse);
                        if(started_playing && pulse_count == 0 && ticks_per_pulse > 0
                           && (uint64_t(state.loop_start_tick) % ticks_per_pulse) == 0)
                            pulse_count = 1;
                    }
                    else
                    {
                        const uint64_t loop_end_tick
                            = uint64_t(state.loop_start_tick) + uint64_t(state.loop_length_ticks);
                        pulse_count = CountTickBoundariesInRange(block_start_tick,
                                                                 loop_end_tick,
                                                                 ticks_per_pulse)
                                      + CountTickBoundariesInRange(uint64_t(state.loop_start_tick),
                                                                   block_end_tick,
                                                                   ticks_per_pulse);
                        if(pulse_count == 0 && ticks_per_pulse > 0
                           && (uint64_t(state.loop_start_tick) % ticks_per_pulse) == 0)
                            pulse_count = 1;
                    }

                    if(pulse_count > 0)
                        gate_out_pulse_remaining_[i] = gate_pulse_samples;
                    gate_high = gate_out_pulse_remaining_[i] > 0;
                }
                break;
            case GateOutMode::ResetPulse:
                if(playing)
                {
                    bool pulse = false;
                    if(!wrapped_block)
                    {
                        pulse = TickBoundaryInRange(block_start_tick, block_end_tick, ticks_per_measure);
                        if(started_playing && !pulse && ticks_per_measure > 0
                           && (uint64_t(state.loop_start_tick) % ticks_per_measure) == 0)
                        {
                            pulse = true;
                        }
                    }
                    else
                    {
                        const uint64_t loop_end_tick
                            = uint64_t(state.loop_start_tick) + uint64_t(state.loop_length_ticks);
                        pulse = TickBoundaryInRange(block_start_tick, loop_end_tick, ticks_per_measure)
                                || TickBoundaryInRange(uint64_t(state.loop_start_tick),
                                                       block_end_tick,
                                                       ticks_per_measure);
                        if(!pulse && ticks_per_measure > 0
                           && (uint64_t(state.loop_start_tick) % ticks_per_measure) == 0)
                        {
                            pulse = true;
                        }
                    }
                    if(pulse)
                        gate_out_pulse_remaining_[i] = gate_pulse_samples;
                    gate_high = gate_out_pulse_remaining_[i] > 0;
                }
                break;
            case GateOutMode::ChannelGate:
            {
                const uint8_t channel = state.cv_gate.gate_out[i].channel;
                const bool active = transport.ChannelGateActive(channel);
                if(state.cv_gate.gate_out[i].trigger_mode == GateTriggerMode::Retrig)
                {
                    const uint32_t note_on_counter = transport.ChannelNoteOnCounter(channel);
                    const bool counter_changed = note_on_counter != last_gate_note_on_counter_[i];
                    if(counter_changed)
                    {
                        if(active && last_gate_channel_active_[i])
                            gate_retrigger_low_pending_[i] = true;
                        last_gate_note_on_counter_[i] = note_on_counter;
                    }

                    if(gate_retrigger_low_pending_[i])
                    {
                        gate_high = false;
                        gate_retrigger_low_pending_[i] = false;
                    }
                    else
                    {
                        gate_high = active;
                    }
                    last_gate_channel_active_[i] = active;
                }
                else
                {
                    gate_high = active;
                    last_gate_channel_active_[i] = active;
                    last_gate_note_on_counter_[i] = transport.ChannelNoteOnCounter(channel);
                }
            }
                break;
        }

        dsy_gpio_write(i == 0 ? &hw_->gate_out_1 : &hw_->gate_out_2, gate_high);
    }

    for(size_t i = 0; i < 2; i++)
    {
        float voltage = 0.0f;
        switch(state.cv_gate.cv_out[i].mode)
        {
            case CvOutMode::Off: voltage = 0.0f; break;
            case CvOutMode::ChannelPitch:
                voltage = PitchVoltageForChannel(
                    i,
                    transport,
                    state.cv_gate.cv_out[i],
                    i == 0 ? state.cv1_pitch_scale : state.cv2_pitch_scale,
                    playing);
                break;
            case CvOutMode::ChannelCc:
                voltage = CcVoltageForChannel(transport, state.cv_gate.cv_out[i]);
                break;
        }

        hw_->WriteCvOut(i == 0 ? CV_OUT_1 : CV_OUT_2, voltage);
    }

    was_playing_ = playing;
}

} // namespace major_midi
