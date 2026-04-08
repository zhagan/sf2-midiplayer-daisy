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
    const float volts = static_cast<float>(note) / 12.0f;
    if(volts > kCvOutMaxVolt)
        return kCvOutMaxVolt;
    return volts;
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
} // namespace

void CvGateEngine::Init(DaisyPatchSM& hw, float sample_rate)
{
    hw_          = &hw;
    sample_rate_ = sample_rate;
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

float CvGateEngine::PitchVoltageForChannel(const MixerTransport& transport,
                                           const CvOutputConfig& config) const
{
    const int note = transport.ChannelPitchNote(config.channel, config.priority);
    return MidiNoteToVoltage(note);
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

void CvGateEngine::Update(const AppState& state, const MixerTransport& transport)
{
    if(hw_ == nullptr)
        return;

    live_bpm_ = 0;

    for(size_t i = 0; i < 2; i++)
    {
        const float cv_value = ReadCvInput(i);
        switch(state.cv_gate.cv_in[i].mode)
        {
            case CvInMode::Off: break;
            case CvInMode::MasterVolume: SynthSetExternalGain(cv_value); break;
            case CvInMode::Bpm:
                live_bpm_ = static_cast<int>(std::lround(kCvInMinBpm
                                                         + cv_value * (kCvInMaxBpm - kCvInMinBpm)));
                break;
            case CvInMode::ChannelCc:
                SynthControlChange(state.cv_gate.cv_in[i].channel,
                                   state.cv_gate.cv_in[i].cc,
                                   static_cast<uint8_t>(std::lround(cv_value * 127.0f)));
                break;
        }
    }

    if(state.cv_gate.cv_in[0].mode != CvInMode::MasterVolume
       && state.cv_gate.cv_in[1].mode != CvInMode::MasterVolume)
    {
        SynthSetExternalGain(1.0f);
    }

    const int      bpm          = EffectiveBpm(state);
    const uint64_t cycle_sample = transport.CurrentCycleSample();
    const bool     playing      = state.transport_playing;
    const int      ts_num       = transport.TimeSigNumerator();
    const int      ts_den       = transport.TimeSigDenominator();

    for(size_t i = 0; i < 2; i++)
    {
        bool gate_high = false;
        switch(state.cv_gate.gate_out[i].mode)
        {
            case GateOutMode::Off: gate_high = false; break;
            case GateOutMode::SyncOut:
                gate_high = playing && SyncGateHigh(cycle_sample,
                                                    bpm,
                                                    state.cv_gate.gate_out[i].sync_resolution);
                break;
            case GateOutMode::ResetPulse:
                gate_high = playing && ResetGateHigh(cycle_sample, bpm, ts_num, ts_den);
                break;
            case GateOutMode::ChannelGate:
                gate_high = transport.ChannelGateActive(state.cv_gate.gate_out[i].channel);
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
                voltage = PitchVoltageForChannel(transport, state.cv_gate.cv_out[i]);
                break;
            case CvOutMode::ChannelCc:
                voltage = CcVoltageForChannel(transport, state.cv_gate.cv_out[i]);
                break;
        }

        hw_->WriteCvOut(i == 0 ? CV_OUT_1 : CV_OUT_2, voltage);
    }
}

} // namespace major_midi
