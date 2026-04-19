#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "clock_sync.h"
#include "cv_gate_engine.h"
#include "cv_gate_persist.h"
#include "daisy_patch_sm.h"
#include "hid/midi.h"
#include "media_library.h"
#include "midi_routing_persist.h"
#include "mixer_transport.h"
#include "per/tim.h"
#include "performance_persist.h"
#include "sd_mount.h"
#include "song_config_persist.h"
#include "smf_player.h"
#include "synth_tsf.h"
#include "ui_controller.h"
#include "ui_input.h"
#include "ui_renderer.h"
#include "util/scopedirqblocker.h"

extern "C"
{
#include "ff.h"
}

using namespace daisy;
using namespace patch_sm;
using namespace major_midi;

static constexpr bool kEnableUsbLog = true;

#define LOG(...)                                  \
    do                                            \
    {                                             \
        if(kEnableUsbLog)                         \
            DaisyPatchSM::PrintLine(__VA_ARGS__); \
    } while(0)

namespace
{
DaisyPatchSM      hw;
SmfPlayer         smf_player;
MixerTransport    transport;
MediaLibrary      media_library;
UiHardwareInput   ui_input;
UiEventTranslator ui_events;
UiController      ui_controller;
UiRenderer        ui_renderer;
CvGateEngine      cv_gate_engine;
AppState          app_state;
MidiUsbHandler    usb_midi;
MidiUartHandler   uart_midi;
ClockSync         midi_clock_sync;
ClockSync         gate_clock_sync;
TimerHandle       midi_tx_timer;
bool              audio_started = false;
uint8_t           applied_sf2_max_voices = 0;
uint32_t          channel_flash_until[16]{};
uint32_t          channel_monitor_until[16]{};
volatile uint64_t sync_sample_counter      = 0;
volatile uint32_t pending_midi_clock_edges = 0;

constexpr uint32_t kLedFlashMs = 90;
constexpr uint32_t kMonitorFlashMs         = 250;
constexpr uint32_t kRenderIntervalStoppedMs    = 100;
constexpr uint32_t kRenderIntervalPlayingMs    = 250;
constexpr uint32_t kRenderIntervalUiActiveMs   = 100;
constexpr uint32_t kUiActiveHoldMs             = 1200;
constexpr uint64_t kScheduledMidiLeadSamples   = 512;
constexpr uint32_t kMidiTxTimerRateHz          = 2000;
enum class MidiOutputKind : uint8_t
{
    Notes,
    Ccs,
    Programs,
    Transport,
    Clock,
};

void UpdateMidiMonitor(const MidiEvent& msg);
void UpdateMidiMonitor(const MidiEv& ev);

bool MidiOutputEnabled(const MidiOutputRouting& routing, MidiOutputKind kind)
{
    switch(kind)
    {
        case MidiOutputKind::Notes: return routing.notes;
        case MidiOutputKind::Ccs: return routing.ccs;
        case MidiOutputKind::Programs: return routing.programs;
        case MidiOutputKind::Transport: return routing.transport;
        case MidiOutputKind::Clock: return routing.clock;
    }
    return false;
}

bool GateInputSyncEnabled(const CvGateConfig& config, size_t index)
{
    return index < 2 && config.gate_in[index].mode == GateInMode::SyncIn;
}

bool AnyGateInputSyncEnabled(const CvGateConfig& config)
{
    return GateInputSyncEnabled(config, 0) || GateInputSyncEnabled(config, 1);
}

const char* PersistWriteStageName(PersistWriteStage stage)
{
    switch(stage)
    {
        case PersistWriteStage::None: return "None";
        case PersistWriteStage::Open: return "Open";
        case PersistWriteStage::Write: return "Write";
        case PersistWriteStage::Sync: return "Sync";
        case PersistWriteStage::Close: return "Close";
        case PersistWriteStage::Done: return "Done";
    }
    return "?";
}

const char* FatFsResultName(int code)
{
    switch(code)
    {
        case FR_OK: return "FR_OK";
        case FR_DISK_ERR: return "FR_DISK_ERR";
        case FR_INT_ERR: return "FR_INT_ERR";
        case FR_NOT_READY: return "FR_NOT_READY";
        case FR_NO_FILE: return "FR_NO_FILE";
        case FR_NO_PATH: return "FR_NO_PATH";
        case FR_INVALID_NAME: return "FR_INVALID_NAME";
        case FR_DENIED: return "FR_DENIED";
        case FR_EXIST: return "FR_EXIST";
        case FR_INVALID_OBJECT: return "FR_INVALID_OBJECT";
        case FR_WRITE_PROTECTED: return "FR_WRITE_PROTECTED";
        case FR_INVALID_DRIVE: return "FR_INVALID_DRIVE";
        case FR_NOT_ENABLED: return "FR_NOT_ENABLED";
        case FR_NO_FILESYSTEM: return "FR_NO_FILESYSTEM";
        case FR_MKFS_ABORTED: return "FR_MKFS_ABORTED";
        case FR_TIMEOUT: return "FR_TIMEOUT";
        case FR_LOCKED: return "FR_LOCKED";
        case FR_NOT_ENOUGH_CORE: return "FR_NOT_ENOUGH_CORE";
        case FR_TOO_MANY_OPEN_FILES: return "FR_TOO_MANY_OPEN_FILES";
        case FR_INVALID_PARAMETER: return "FR_INVALID_PARAMETER";
        default: return "FR_UNKNOWN";
    }
}

struct SaveProgressContext
{
    uint32_t    now_ms;
    const char* prefix;
};

void SaveProgressOverlay(PersistWriteStage stage, void* context)
{
    if(stage == PersistWriteStage::None || context == nullptr)
        return;

    auto* ctx = static_cast<SaveProgressContext*>(context);
    char  text[24];
    std::snprintf(text, sizeof(text), "%s %s", ctx->prefix, PersistWriteStageName(stage));
    SetOverlay(app_state, text, ctx->now_ms, 400);
    if(app_state.saving_all)
        ui_renderer.Render(app_state, media_library, ctx->now_ms);
    LOG("Save stage: %s", text);
}

void BuildSongConfigPath(const char* midi_path, char* out, size_t out_sz)
{
    if(out_sz == 0)
        return;
    out[0] = '\0';
    if(midi_path == nullptr || midi_path[0] == '\0')
        return;

    std::snprintf(out, out_sz, "%s", midi_path);
    char* dot = std::strrchr(out, '.');
    if(dot != nullptr)
        std::snprintf(dot, out_sz - static_cast<size_t>(dot - out), ".cfg");
}

void ResetSongScopedSettings()
{
    app_state.cv_gate = CvGateConfig{};
    app_state.cv_gate.gate_in[0].mode = GateInMode::SyncIn;
    app_state.midi_routing            = MidiRoutingConfig{};
    for(int ch = 0; ch < 16; ch++)
    {
        app_state.channels[ch].volume           = 100;
        app_state.channels[ch].pan              = 64;
        app_state.channels[ch].reverb_send      = 0;
        app_state.channels[ch].chorus_send      = 0;
        app_state.channels[ch].program_override = -1;
        app_state.channels[ch].muted            = false;
    }
}

template <typename Handler>
void SendRawMidi(Handler& handler, const uint8_t* bytes, size_t size)
{
    uint8_t data[3]{};
    for(size_t i = 0; i < size && i < 3; i++)
        data[i] = bytes[i];
    handler.SendMessage(data, size);
}

void SendToConfiguredOutputs(MidiOutputKind kind, const uint8_t* bytes, size_t size)
{
    if(MidiOutputEnabled(app_state.midi_routing.usb, kind))
        SendRawMidi(usb_midi, bytes, size);
    if(MidiOutputEnabled(app_state.midi_routing.uart, kind))
        SendRawMidi(uart_midi, bytes, size);
}

void SendToDestinationOutput(bool to_usb, MidiOutputKind kind, const uint8_t* bytes, size_t size)
{
    const MidiOutputRouting& routing = to_usb ? app_state.midi_routing.usb : app_state.midi_routing.uart;
    if(!MidiOutputEnabled(routing, kind))
        return;
    if(to_usb)
        SendRawMidi(usb_midi, bytes, size);
    else
        SendRawMidi(uart_midi, bytes, size);
}

bool MidiEventToRawBytes(const MidiEvent& msg, uint8_t out[3], size_t& size, MidiOutputKind& kind)
{
    switch(msg.type)
    {
        case MidiMessageType::NoteOn:
            out[0] = static_cast<uint8_t>(0x90 | (msg.channel & 0x0F));
            out[1] = msg.data[0];
            out[2] = msg.data[1];
            size   = 3;
            kind   = MidiOutputKind::Notes;
            return true;

        case MidiMessageType::NoteOff:
            out[0] = static_cast<uint8_t>(0x80 | (msg.channel & 0x0F));
            out[1] = msg.data[0];
            out[2] = msg.data[1];
            size   = 3;
            kind   = MidiOutputKind::Notes;
            return true;

        case MidiMessageType::ControlChange:
            out[0] = static_cast<uint8_t>(0xB0 | (msg.channel & 0x0F));
            out[1] = msg.data[0];
            out[2] = msg.data[1];
            size   = 3;
            kind   = MidiOutputKind::Ccs;
            return true;

        case MidiMessageType::ProgramChange:
            out[0] = static_cast<uint8_t>(0xC0 | (msg.channel & 0x0F));
            out[1] = msg.data[0];
            size   = 2;
            kind   = MidiOutputKind::Programs;
            return true;

        case MidiMessageType::ChannelMode:
            if(msg.cm_type == ChannelModeType::AllNotesOff || msg.cm_type == ChannelModeType::AllSoundOff)
            {
                out[0] = static_cast<uint8_t>(0xB0 | (msg.channel & 0x0F));
                out[1] = msg.cm_type == ChannelModeType::AllSoundOff ? 120 : 123;
                out[2] = 0;
                size   = 3;
                kind   = MidiOutputKind::Ccs;
                return true;
            }
            break;

        case MidiMessageType::SystemRealTime:
            switch(msg.srt_type)
            {
                case SystemRealTimeType::TimingClock:
                    out[0] = 0xF8;
                    size   = 1;
                    kind   = MidiOutputKind::Clock;
                    return true;
                case SystemRealTimeType::Start:
                    out[0] = 0xFA;
                    size   = 1;
                    kind   = MidiOutputKind::Transport;
                    return true;
                case SystemRealTimeType::Continue:
                    out[0] = 0xFB;
                    size   = 1;
                    kind   = MidiOutputKind::Transport;
                    return true;
                case SystemRealTimeType::Stop:
                    out[0] = 0xFC;
                    size   = 1;
                    kind   = MidiOutputKind::Transport;
                    return true;
                default: break;
            }
            break;

        default: break;
    }

    return false;
}

bool MidiEvToRawBytes(const MidiEv& ev, uint8_t out[3], size_t& size, MidiOutputKind& kind)
{
    switch(ev.type)
    {
        case EvType::NoteOn:
            out[0] = static_cast<uint8_t>(0x90 | (ev.ch & 0x0F));
            out[1] = ev.a;
            out[2] = ev.b;
            size   = 3;
            kind   = MidiOutputKind::Notes;
            return true;
        case EvType::NoteOff:
            out[0] = static_cast<uint8_t>(0x80 | (ev.ch & 0x0F));
            out[1] = ev.a;
            out[2] = 0;
            size   = 3;
            kind   = MidiOutputKind::Notes;
            return true;
        case EvType::Program:
            out[0] = static_cast<uint8_t>(0xC0 | (ev.ch & 0x0F));
            out[1] = ev.a;
            size   = 2;
            kind   = MidiOutputKind::Programs;
            return true;
        case EvType::ControlChange:
            out[0] = static_cast<uint8_t>(0xB0 | (ev.ch & 0x0F));
            out[1] = ev.a;
            out[2] = ev.b;
            size   = 3;
            kind   = MidiOutputKind::Ccs;
            return true;
        case EvType::AllSoundOff:
            out[0] = static_cast<uint8_t>(0xB0 | (ev.ch & 0x0F));
            out[1] = 120;
            out[2] = 0;
            size   = 3;
            kind   = MidiOutputKind::Ccs;
            return true;
        case EvType::AllNotesOff:
            out[0] = static_cast<uint8_t>(0xB0 | (ev.ch & 0x0F));
            out[1] = 123;
            out[2] = 0;
            size   = 3;
            kind   = MidiOutputKind::Ccs;
            return true;
        case EvType::PitchBend:
        default: break;
    }
    return false;
}

void ForwardScheduledMidiOut(const MidiEv& ev, void*)
{
    UpdateMidiMonitor(ev);
}

bool ScheduledMidiOutputBlocked(const MidiEv& ev)
{
    if(ev.ch >= 16)
        return false;
    if(!app_state.channels[ev.ch].muted)
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

MidiEv PrepareScheduledMidiOutput(MidiEv ev)
{
    if((ev.type == EvType::NoteOn || ev.type == EvType::NoteOff) && ev.ch != 9)
    {
        int note = static_cast<int>(ev.a) + app_state.sf2_transpose;
        if(note < 0)
            note = 0;
        if(note > 127)
            note = 127;
        ev.a = static_cast<uint8_t>(note);
    }

    if(ev.type == EvType::Program && ev.ch < 16 && app_state.channels[ev.ch].program_override >= 0)
        ev.a = static_cast<uint8_t>(app_state.channels[ev.ch].program_override);

    if(ev.type == EvType::ControlChange && ev.a == 11)
        ev.b = static_cast<uint8_t>((uint16_t(ev.b) * uint16_t(app_state.sf2_expression_max)) / 127u);

    return ev;
}

void FlushScheduledMidiOut()
{
    const uint64_t due_sample = transport.SampleClock() + kScheduledMidiLeadSamples;
    MidiEv         ev{};
    while(transport.PopDueMidiOutputEvent(due_sample, ev))
    {
        if(ScheduledMidiOutputBlocked(ev))
            continue;

        MidiEv actual = PrepareScheduledMidiOutput(ev);
        uint8_t        bytes[3]{};
        size_t         size = 0;
        MidiOutputKind kind = MidiOutputKind::Notes;
        if(MidiEvToRawBytes(actual, bytes, size, kind))
            SendToConfiguredOutputs(kind, bytes, size);
    }
}

void MidiTxTimerCallback(void*)
{
    FlushScheduledMidiOut();
}

void MaybeForwardThru(const MidiEvent& msg, bool from_usb)
{
    const bool to_uart = from_usb && app_state.midi_routing.usb_in_to_uart;
    const bool to_usb  = !from_usb && app_state.midi_routing.uart_in_to_usb;
    if(!to_uart && !to_usb)
        return;

    uint8_t        bytes[3]{};
    size_t         size = 0;
    MidiOutputKind kind = MidiOutputKind::Notes;
    if(!MidiEventToRawBytes(msg, bytes, size, kind))
        return;

    if(to_uart)
        SendToDestinationOutput(false, kind, bytes, size);
    if(to_usb)
        SendToDestinationOutput(true, kind, bytes, size);
}

void UpdateMidiMonitor(const MidiEvent& msg)
{
    if(msg.channel >= 16)
        return;

    auto& channel = app_state.midi_monitor_channels[msg.channel];
    switch(msg.type)
    {
        case MidiMessageType::NoteOn:
        case MidiMessageType::NoteOff:
            channel.note       = msg.data[0];
            channel.note_valid = true;
            break;

        case MidiMessageType::PitchBend:
        {
            const uint16_t bend = (uint16_t(msg.data[1]) << 7) | msg.data[0];
            channel.pitchbend_coarse = static_cast<uint8_t>(bend >> 7);
            channel.pitchbend_valid  = true;
        }
        break;

        case MidiMessageType::ControlChange:
            channel.cc       = msg.data[0];
            channel.cc_value = msg.data[1];
            channel.cc_valid = true;
            break;

        default: break;
    }
}

void UpdateMidiMonitor(const MidiEv& ev)
{
    if(ev.ch >= 16)
        return;

    auto& channel = app_state.midi_monitor_channels[ev.ch];
    switch(ev.type)
    {
        case EvType::NoteOn:
        case EvType::NoteOff:
            channel.note       = ev.a;
            channel.note_valid = true;
            break;

        case EvType::PitchBend:
            channel.pitchbend_coarse = ev.b;
            channel.pitchbend_valid  = true;
            break;

        case EvType::ControlChange:
            channel.cc       = ev.a;
            channel.cc_value = ev.b;
            channel.cc_valid = true;
            break;

        default: break;
    }
}

void ServiceIncomingMidi()
{
    usb_midi.Listen();
    while(usb_midi.HasEvents())
    {
        const auto msg = usb_midi.PopEvent();
        if(msg.type == MidiMessageType::SystemRealTime)
        {
            switch(msg.srt_type)
            {
                case SystemRealTimeType::TimingClock: pending_midi_clock_edges++; break;
                case SystemRealTimeType::Start:
                case SystemRealTimeType::Continue:
                    if(app_state.sync_external)
                        app_state.transport_playing = true;
                    break;
                case SystemRealTimeType::Stop:
                    if(app_state.sync_external)
                        app_state.transport_playing = false;
                    break;
                default: break;
            }
        }
        UpdateMidiMonitor(msg);
        MaybeForwardThru(msg, true);
        transport.HandleMidiMessage(msg, app_state);
    }

    uart_midi.Listen();
    while(uart_midi.HasEvents())
    {
        const auto msg = uart_midi.PopEvent();
        if(msg.type == MidiMessageType::SystemRealTime)
        {
            switch(msg.srt_type)
            {
                case SystemRealTimeType::TimingClock: pending_midi_clock_edges++; break;
                case SystemRealTimeType::Start:
                case SystemRealTimeType::Continue:
                    if(app_state.sync_external)
                        app_state.transport_playing = true;
                    break;
                case SystemRealTimeType::Stop:
                    if(app_state.sync_external)
                        app_state.transport_playing = false;
                    break;
                default: break;
            }
        }
        UpdateMidiMonitor(msg);
        MaybeForwardThru(msg, false);
        transport.HandleMidiMessage(msg, app_state);
    }
}

void SyncFxStateFromSynth()
{
    app_state.fx_reverb_time     = SynthGetReverbTime();
    app_state.fx_reverb_lpf_hz   = SynthGetReverbLpFreq();
    app_state.fx_reverb_hpf_hz   = SynthGetReverbHpFreq();
    app_state.fx_chorus_depth    = SynthGetChorusDepth();
    app_state.fx_chorus_speed_hz = SynthGetChorusSpeed();
}

void SyncSongStateFromPlayer()
{
    const auto& settings         = smf_player.Settings();
    app_state.song_bpm_override  = settings.bpm_override;
    app_state.song_loop_enabled  = settings.loop_enabled;
    app_state.sf2_master_volume_max = settings.master_volume_max;
    app_state.sf2_expression_max    = settings.expression_max;
    app_state.sf2_reverb_max        = settings.reverb_max;
    app_state.sf2_chorus_max        = settings.chorus_max;
    app_state.sf2_transpose         = settings.transpose;
    for(int ch = 0; ch < 16; ch++)
    {
        app_state.channels[ch].program_override = settings.program_override[ch];
        app_state.channels[ch].pan
            = settings.pan_override[ch] >= 0 ? static_cast<uint8_t>(settings.pan_override[ch]) : 64;
        app_state.channels[ch].volume      = settings.volume[ch];
        app_state.channels[ch].reverb_send = settings.reverb_send[ch];
        app_state.channels[ch].chorus_send = settings.chorus_send[ch];
        app_state.channels[ch].muted       = settings.muted[ch];
        app_state.channels[ch].current_program
            = settings.program_override[ch] >= 0 ? static_cast<uint8_t>(settings.program_override[ch]) : 0;
    }
    if(app_state.sf2_max_voices < 4 || app_state.sf2_max_voices > 32)
        app_state.sf2_max_voices = 16;
    app_state.loop_start_measure = settings.loop_start_measure < 1 ? 1 : settings.loop_start_measure;
    app_state.loop_start_beat    = settings.loop_start_beat < 1 ? 1 : settings.loop_start_beat;
    app_state.loop_start_sub     = settings.loop_start_sub < 1 ? 1 : settings.loop_start_sub;
    app_state.loop_length_beats  = settings.loop_length_beats < 1 ? 1 : settings.loop_length_beats;

    if(settings.loop_enabled)
    {
        const int measure_span
            = settings.loop_length_beats > 0 ? ((settings.loop_length_beats + 3) / 4) : 1;
        app_state.loop_end_measure = app_state.loop_start_measure + measure_span;
    }
    else
    {
        app_state.loop_end_measure = app_state.loop_start_measure;
    }
}

void ApplyAppSettings()
{
    if(!app_state.settings_dirty)
        return;

    SynthSetReverbTime(app_state.fx_reverb_time);
    SynthSetReverbLpFreq(app_state.fx_reverb_lpf_hz);
    SynthSetReverbHpFreq(app_state.fx_reverb_hpf_hz);
    SynthSetChorusDepth(app_state.fx_chorus_depth);
    SynthSetChorusSpeed(app_state.fx_chorus_speed_hz);
    if(applied_sf2_max_voices != app_state.sf2_max_voices)
    {
        SynthSetMaxVoices(app_state.sf2_max_voices);
        applied_sf2_max_voices = app_state.sf2_max_voices;
    }

    auto& settings         = smf_player.MutableSettings();
    settings.bpm_override  = app_state.song_bpm_override;
    settings.loop_enabled  = app_state.song_loop_enabled;
    settings.master_volume_max = app_state.sf2_master_volume_max;
    settings.expression_max    = app_state.sf2_expression_max;
    settings.reverb_max        = app_state.sf2_reverb_max;
    settings.chorus_max        = app_state.sf2_chorus_max;
    settings.transpose         = app_state.sf2_transpose;
    for(int ch = 0; ch < 16; ch++)
    {
        settings.program_override[ch] = app_state.channels[ch].program_override;
        settings.pan_override[ch]     = static_cast<int8_t>(app_state.channels[ch].pan);
        settings.volume[ch]           = app_state.channels[ch].volume;
        settings.reverb_send[ch]      = app_state.channels[ch].reverb_send;
        settings.chorus_send[ch]      = app_state.channels[ch].chorus_send;
        settings.muted[ch]            = app_state.channels[ch].muted;
    }
    settings.loop_start_measure
        = app_state.loop_start_measure < 1 ? 1 : app_state.loop_start_measure;
    settings.loop_start_beat = app_state.loop_start_beat < 1 ? 1 : app_state.loop_start_beat;
    settings.loop_start_sub  = app_state.loop_start_sub < 1 ? 1 : app_state.loop_start_sub;

    if(app_state.song_loop_enabled)
    {
        if(app_state.loop_end_measure <= app_state.loop_start_measure)
            app_state.loop_end_measure = app_state.loop_start_measure + 1;
        if(app_state.loop_length_beats < 1)
        {
            app_state.loop_length_beats
                = (app_state.loop_end_measure - app_state.loop_start_measure) * 4;
            if(app_state.loop_length_beats < 1)
                app_state.loop_length_beats = 4;
        }
        settings.loop_length_beats = static_cast<uint16_t>(app_state.loop_length_beats);
    }
    else
    {
        app_state.loop_end_measure = app_state.loop_start_measure;
        settings.loop_length_beats = static_cast<uint16_t>(app_state.loop_length_beats < 1 ? 4
                                                                                            : app_state.loop_length_beats);
    }

    if(app_state.song_bpm_override > 0)
        app_state.bpm = app_state.song_bpm_override;

    app_state.settings_dirty = false;
}

int TempoUsecToBpm(uint32_t tempo_usec)
{
    if(tempo_usec == 0)
        return 120;
    const int bpm = static_cast<int>((60000000.0 + tempo_usec / 2.0) / tempo_usec);
    if(bpm < 20)
        return 20;
    if(bpm > 300)
        return 300;
    return bpm;
}

uint16_t TickToMeasure(uint64_t tick, const SmfPlayer& player)
{
    const uint16_t divisions = player.Divisions();
    if(divisions == 0)
        return 1;

    const int numerator        = player.TimeSigNumerator() > 0 ? player.TimeSigNumerator() : 4;
    const int denominator      = player.TimeSigDenominator() > 0 ? player.TimeSigDenominator() : 4;
    const uint64_t ticks_per_beat
        = (static_cast<uint64_t>(divisions) * 4u) / static_cast<uint64_t>(denominator);
    const uint64_t ticks_per_measure = ticks_per_beat * static_cast<uint64_t>(numerator);
    if(ticks_per_measure == 0)
        return 1;

    return static_cast<uint16_t>((tick / ticks_per_measure) + 1u);
}

uint8_t TickToBeat(uint64_t tick, const SmfPlayer& player)
{
    const uint16_t divisions = player.Divisions();
    if(divisions == 0)
        return 1;

    const int denominator = player.TimeSigDenominator() > 0 ? player.TimeSigDenominator() : 4;
    const uint64_t ticks_per_beat
        = (static_cast<uint64_t>(divisions) * 4u) / static_cast<uint64_t>(denominator);
    if(ticks_per_beat == 0)
        return 1;

    const int numerator = player.TimeSigNumerator() > 0 ? player.TimeSigNumerator() : 4;
    const uint64_t beat_index = (tick / ticks_per_beat) % static_cast<uint64_t>(numerator);
    return static_cast<uint8_t>(beat_index + 1u);
}

uint64_t MeasureBeatToTick(int measure, int beat, const SmfPlayer& player)
{
    const uint16_t divisions = player.Divisions();
    if(divisions == 0)
        return 0;

    const int numerator   = player.TimeSigNumerator() > 0 ? player.TimeSigNumerator() : 4;
    const int denominator = player.TimeSigDenominator() > 0 ? player.TimeSigDenominator() : 4;
    const uint64_t ticks_per_beat
        = (static_cast<uint64_t>(divisions) * 4u) / static_cast<uint64_t>(denominator);
    const uint64_t ticks_per_measure = ticks_per_beat * static_cast<uint64_t>(numerator);
    const int safe_measure = measure < 1 ? 1 : measure;
    const int safe_beat    = beat < 1 ? 1 : (beat > numerator ? numerator : beat);
    return static_cast<uint64_t>(safe_measure - 1) * ticks_per_measure
           + static_cast<uint64_t>(safe_beat - 1) * ticks_per_beat;
}

void InitDefaultState()
{
    app_state = AppState{};
    app_state.cv_gate.gate_in[0].mode = GateInMode::SyncIn;
    for(int ch = 0; ch < 16; ch++)
    {
        app_state.channels[ch].volume      = 100;
        app_state.channels[ch].pan         = 64;
        app_state.channels[ch].reverb_send = 0;
        app_state.channels[ch].chorus_send = 0;
        app_state.channels[ch].current_program = 0;
        app_state.channels[ch].program_override = -1;
        app_state.channels[ch].muted       = false;
    }
}

bool EnsureAudioRunning()
{
    if(!audio_started)
    {
        hw.StartAudio([](AudioHandle::InputBuffer  in,
                         AudioHandle::OutputBuffer out,
                         size_t                    size) {
            hw.ProcessAnalogControls();
            ui_input.ControlRateTick();
            ServiceIncomingMidi();
            for(size_t i = 0; i < size; i++)
            {
                const uint64_t sample_time = sync_sample_counter + i;
                bool           midi_edge   = false;
                bool           gate_edge   = false;
                if(pending_midi_clock_edges > 0)
                {
                    pending_midi_clock_edges--;
                    midi_edge = true;
                }
                midi_clock_sync.ProcessSample(midi_edge, sample_time);
                if(GateInputSyncEnabled(app_state.cv_gate, 0))
                    gate_edge = gate_edge || hw.gate_in_1.State();
                if(GateInputSyncEnabled(app_state.cv_gate, 1))
                    gate_edge = gate_edge || hw.gate_in_2.State();
                gate_clock_sync.ProcessSample(gate_edge, sample_time);
            }
            sync_sample_counter += size;
            transport.ProcessAudio(in, out, size);
            cv_gate_engine.Update(app_state, transport);
        });
        audio_started = true;
    }
    return audio_started;
}

void StopAudioIfRunning()
{
    if(audio_started)
    {
        hw.StopAudio();
        audio_started = false;
    }
}

bool LoadSelectedMedia(bool reload_midi, bool reload_sf2, uint32_t now_ms)
{
    char midi_path[MediaLibrary::kNameMax * 2]{};
    char sf2_path[MediaLibrary::kNameMax * 2]{};
    char song_cfg_path[MediaLibrary::kNameMax * 2 + 8]{};

    if(reload_midi)
    {
        media_library.BuildMidiPath(app_state.selected_midi_index, midi_path, sizeof(midi_path));
        BuildSongConfigPath(midi_path, song_cfg_path, sizeof(song_cfg_path));
    }
    if(reload_sf2)
        media_library.BuildSoundFontPath(
            app_state.selected_sf2_index, sf2_path, sizeof(sf2_path));

    const bool was_playing = app_state.transport_playing;
    app_state.transport_playing = false;

    StopAudioIfRunning();
    transport.Reset(app_state);

    bool sf_ok   = true;
    bool midi_ok = true;

    if(reload_sf2)
    {
        SynthUnloadSf2();
        sf_ok = sf2_path[0] != '\0'
                && SynthLoadSf2(sf2_path,
                                hw.AudioSampleRate(),
                                static_cast<int>(app_state.sf2_max_voices));
        if(sf_ok)
        {
            applied_sf2_max_voices = app_state.sf2_max_voices;
            SyncFxStateFromSynth();
        }
        else
        {
            applied_sf2_max_voices = 0;
        }
    }

    if(reload_midi)
    {
        ResetSongScopedSettings();
        smf_player.Close();
        midi_ok = midi_path[0] != '\0' && smf_player.Open(midi_path);
        if(midi_ok)
        {
            app_state.bpm = TempoUsecToBpm(smf_player.TempoUsecPerQuarter());
            transport.SetFileBpm(static_cast<float>(app_state.bpm));
            SyncSongStateFromPlayer();
            if(song_cfg_path[0] != '\0')
                LoadSongConfig(song_cfg_path, app_state);
            app_state.settings_dirty = true;
            ApplyAppSettings();
        }
    }

    if(sf_ok)
        EnsureAudioRunning();

    if(reload_midi)
        SetOverlay(app_state, midi_ok ? "MIDI Loaded" : "MIDI Load Fail", now_ms);
    else if(reload_sf2)
        SetOverlay(app_state, sf_ok ? "SF2 Loaded" : "SF2 Load Fail", now_ms);

    app_state.pending_midi_load = false;
    app_state.pending_sf2_load  = false;
    app_state.loading_midi      = false;
    app_state.loading_sf2       = false;
    app_state.transport_playing = (sf_ok && midi_ok) ? was_playing : false;
    return sf_ok && midi_ok;
}

bool SaveAllSettings(uint32_t now_ms)
{
    char midi_path[MediaLibrary::kNameMax * 2]{};
    char sf2_path[MediaLibrary::kNameMax * 2]{};
    char song_cfg_path[MediaLibrary::kNameMax * 2 + 8]{};
    const auto midi_settings = smf_player.Settings();
    const bool had_audio     = audio_started;
    PersistWriteStage song_stage  = PersistWriteStage::None;
    int               song_result_code = -1;
    SaveProgressContext song_progress{now_ms, "SONG"};
    auto show_save_stage     = [&](const char* text) {
        SetOverlay(app_state, text, now_ms, 400);
        if(app_state.saving_all)
            ui_renderer.Render(app_state, media_library, now_ms);
        LOG("Save stage: %s", text);
    };

    media_library.BuildMidiPath(app_state.selected_midi_index, midi_path, sizeof(midi_path));
    media_library.BuildSoundFontPath(app_state.selected_sf2_index, sf2_path, sizeof(sf2_path));
    BuildSongConfigPath(midi_path, song_cfg_path, sizeof(song_cfg_path));

    show_save_stage("Save Prep");
    app_state.transport_playing = false;
    StopAudioIfRunning();
    transport.Reset(app_state);
    if(midi_path[0] != '\0')
        smf_player.Close();

    bool midi_ok = true;
    if(midi_path[0] != '\0')
    {
        show_save_stage("Write MIDI");
        midi_ok = major_midi::WriteMajorMidiMetaEvent(midi_path, midi_settings);
    }

    show_save_stage("Write SONG CFG");
    const bool song_ok = song_cfg_path[0] != '\0'
                             && SaveSongConfig(song_cfg_path,
                                               app_state,
                                               &song_stage,
                                               &song_result_code,
                                               SaveProgressOverlay,
                                               &song_progress);
    if(!song_ok)
    {
        char text[32];
        std::snprintf(text,
                      sizeof(text),
                      "SONG %s %s",
                      PersistWriteStageName(song_stage),
                      FatFsResultName(song_result_code));
        show_save_stage(text);
    }

    bool midi_reload_ok = true;
    if(midi_path[0] != '\0')
    {
        show_save_stage("Reopen MIDI");
        midi_reload_ok = smf_player.Open(midi_path);
        if(midi_reload_ok)
        {
            app_state.bpm = TempoUsecToBpm(smf_player.TempoUsecPerQuarter());
            transport.SetFileBpm(static_cast<float>(app_state.bpm));
            SyncSongStateFromPlayer();
        }
    }

    if(song_ok && song_cfg_path[0] != '\0')
    {
        LoadSongConfig(song_cfg_path, app_state);
        app_state.settings_dirty = true;
        ApplyAppSettings();
    }

    if(had_audio && sf2_path[0] != '\0')
    {
        show_save_stage("Resume Audio");
        EnsureAudioRunning();
    }

    if(song_ok && midi_reload_ok)
    {
        app_state.ui_mode          = UiMode::Performance;
        app_state.menu_page        = MenuPage::Main;
        app_state.menu_page_cursor = 0;
        app_state.menu_root_cursor = 0;
        app_state.menu_editing     = false;
        app_state.midi_routing_dirty = false;
        SetOverlay(app_state, "Settings Saved", now_ms);
        return true;
    }

    SetOverlay(app_state, "Save Failed", now_ms);
    return false;
}
} // namespace

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(24);
    if(kEnableUsbLog)
        hw.StartLog(false);

    InitDefaultState();
    ui_controller.Init(app_state);
    ui_input.Init(hw);
    ui_renderer.Init();
    ui_renderer.ShowSplash();

    const bool sd_ok = SdMount();
    LOG("SD mount: %s", sd_ok ? "PASS" : "FAIL");

    media_library.Scan();

    SynthInit();
    smf_player.SetSampleRate(hw.AudioSampleRate());
    smf_player.SetLookaheadSamples(hw.AudioBlockSize() * 256);
    smf_player.SetTempoScale(1.0f);
    transport.Init(hw.AudioSampleRate(), smf_player);
    transport.SetMidiOutputCallback(ForwardScheduledMidiOut, nullptr);
    cv_gate_engine.Init(hw, hw.AudioSampleRate());
    midi_clock_sync.Init(hw.AudioSampleRate(), ClockSync::PulseMode::MIDI_24PPQN);
    midi_clock_sync.SetUseExternalClock(true);
    gate_clock_sync.Init(hw.AudioSampleRate(), ClockSync::PulseMode::PULSE_PER_16TH);
    gate_clock_sync.SetUseExternalClock(true);

    if(media_library.MidiCount() > 0)
        app_state.selected_midi_index = 0;
    if(media_library.SoundFontCount() > 0)
        app_state.selected_sf2_index = 0;

    if(sd_ok)
    {
        LoadSelectedMedia(true, true, System::GetNow());
        if(media_library.MidiCount() > 0 && media_library.SoundFontCount() > 0)
            SetOverlay(app_state, "Ready", System::GetNow());
    }

    MidiUsbHandler::Config usb_cfg{};
    usb_cfg.transport_config.periph         = MidiUsbTransport::Config::EXTERNAL;
    usb_cfg.transport_config.tx_retry_count = 10;
    usb_midi.Init(usb_cfg);
    usb_midi.StartReceive();

    MidiUartHandler::Config uart_cfg{};
    uart_cfg.transport_config.periph = UartHandler::Config::Peripheral::UART_4;
    uart_cfg.transport_config.rx     = DaisyPatchSM::A2;
    uart_cfg.transport_config.tx     = DaisyPatchSM::A3;
    uart_midi.Init(uart_cfg);
    uart_midi.StartReceive();

    TimerHandle::Config midi_tx_timer_cfg;
    midi_tx_timer_cfg.periph     = TimerHandle::Config::Peripheral::TIM_5;
    midi_tx_timer_cfg.enable_irq = true;
    midi_tx_timer.Init(midi_tx_timer_cfg);
    const uint32_t timer_base_hz = midi_tx_timer.GetFreq();
    const uint32_t prescaler
        = timer_base_hz > 1000000 ? ((timer_base_hz / 1000000) - 1) : 0;
    midi_tx_timer.SetPrescaler(prescaler);
    midi_tx_timer.SetPeriod((1000000 / kMidiTxTimerRateHz) - 1);
    midi_tx_timer.SetCallback(MidiTxTimerCallback, nullptr);
    midi_tx_timer.Start();

    uint32_t render_ms          = System::GetNow();
    uint32_t last_ui_activity_ms = render_ms;
    bool     ui_dirty            = true;
    bool     last_transport_playing = false;
    uint64_t next_midi_clock_sample = 0;
    while(1)
    {
        const uint32_t now = System::GetNow();
        RawInputState  raw{};
        UiEvent        events[20];
        uint8_t        channel_activity[16]{};

        ui_input.Sample(raw);
        app_state.sync_external = raw.sync_external;
        const size_t event_count = ui_events.Translate(raw, now, events, 20);
        for(size_t i = 0; i < event_count; i++)
            ui_controller.HandleEvent(events[i], now, media_library);
        if(event_count > 0)
        {
            last_ui_activity_ms = now;
            ui_dirty            = true;
        }

        ApplyAppSettings();

        if(app_state.pending_sf2_load || app_state.pending_midi_load)
        {
            app_state.loading_midi = app_state.pending_midi_load;
            app_state.loading_sf2  = app_state.pending_sf2_load;
            ui_renderer.Render(app_state, media_library, now);
            const bool load_ok
                = LoadSelectedMedia(app_state.pending_midi_load, app_state.pending_sf2_load, now);
            if(load_ok)
            {
                app_state.ui_mode          = UiMode::Performance;
                app_state.menu_page        = MenuPage::Main;
                app_state.menu_page_cursor = 0;
                app_state.menu_root_cursor = 0;
                app_state.menu_editing     = false;
            }
            last_ui_activity_ms = now;
            ui_dirty            = true;
        }

        if(app_state.pending_save_settings)
        {
            app_state.pending_save_settings = false;
            SetOverlay(app_state,
                       smf_player.SaveSettings() ? "MIDI Saved" : "Save Failed",
                       now);
            last_ui_activity_ms = now;
            ui_dirty            = true;
        }

        if(app_state.pending_save_all)
        {
            app_state.pending_save_all = false;

            if(app_state.transport_playing || transport.AnyChannelGateActive()
               || app_state.loading_midi || app_state.loading_sf2)
            {
                SetOverlay(app_state, "Stop Playback First", now);
            }
            else
            {
                app_state.saving_all = true;
                ui_renderer.Render(app_state, media_library, now);
                SaveAllSettings(now);
                app_state.saving_all = false;
            }

            last_ui_activity_ms = now;
            ui_dirty            = true;
        }

        AppState effective_state = app_state;
        effective_state.bpm      = cv_gate_engine.EffectiveBpm(app_state);
        effective_state.active_voices = static_cast<uint8_t>(SynthActiveVoiceCount());
        const bool gate_sync_enabled = AnyGateInputSyncEnabled(app_state.cv_gate);
        if(app_state.sync_external)
        {
            const float midi_bpm = midi_clock_sync.GetBpmEstimate();
            const float gate_bpm = gate_clock_sync.GetBpmEstimate();
            if(midi_clock_sync.IsLocked() && midi_bpm > 0.0f)
            {
                effective_state.bpm = TempoUsecToBpm(static_cast<uint32_t>(60000000.0f / midi_bpm));
                effective_state.sync_locked = true;
            }
            else if(gate_sync_enabled && gate_clock_sync.IsLocked() && gate_bpm > 0.0f)
            {
                effective_state.bpm = TempoUsecToBpm(static_cast<uint32_t>(60000000.0f / gate_bpm));
                effective_state.sync_locked = true;
            }
            else
            {
                effective_state.sync_locked = false;
            }

            if(!effective_state.sync_locked)
                effective_state.transport_playing = false;
        }
        else
        {
            effective_state.sync_locked = false;
        }

        if(audio_started)
            transport.Update(effective_state);

        if(audio_started && effective_state.transport_playing && !transport.IsPlaying())
        {
            app_state.transport_playing      = false;
            effective_state.transport_playing = false;
        }

        const bool internal_transport_master = !effective_state.sync_external;
        const bool transport_started
            = effective_state.transport_playing && !last_transport_playing && internal_transport_master;
        const bool transport_stopped
            = !effective_state.transport_playing && last_transport_playing && internal_transport_master;

        if(transport_started)
        {
            const uint8_t start = 0xFA;
            SendToConfiguredOutputs(MidiOutputKind::Transport, &start, 1);
            next_midi_clock_sample = transport.SampleClock();
        }
        else if(transport_stopped)
        {
            const uint8_t stop = 0xFC;
            SendToConfiguredOutputs(MidiOutputKind::Transport, &stop, 1);
        }

        if(audio_started && effective_state.transport_playing && internal_transport_master
           && (app_state.midi_routing.usb.clock || app_state.midi_routing.uart.clock))
        {
            const float bpm = effective_state.bpm > 0 ? static_cast<float>(effective_state.bpm) : 120.0f;
            const double samples_per_clock = (hw.AudioSampleRate() * 60.0) / (static_cast<double>(bpm) * 24.0);
            const uint64_t current_sample  = transport.SampleClock();
            if(next_midi_clock_sample == 0)
                next_midi_clock_sample = current_sample;
            while(current_sample >= next_midi_clock_sample)
            {
                const uint8_t clock = 0xF8;
                SendToConfiguredOutputs(MidiOutputKind::Clock, &clock, 1);
                next_midi_clock_sample += static_cast<uint64_t>(samples_per_clock > 1.0 ? samples_per_clock : 1.0);
            }
        }
        else if(!effective_state.transport_playing)
        {
            next_midi_clock_sample = 0;
        }

        last_transport_playing = effective_state.transport_playing;

        effective_state.current_measure
            = effective_state.transport_playing
                  ? TickToMeasure(transport.CurrentSongTick(), smf_player)
                  : 1;
        effective_state.current_beat
            = effective_state.transport_playing
                  ? TickToBeat(transport.CurrentSongTick(), smf_player)
                  : 1;
        effective_state.time_sig_num
            = smf_player.TimeSigNumerator() > 0 ? smf_player.TimeSigNumerator() : 4;
        effective_state.time_sig_den
            = smf_player.TimeSigDenominator() > 0 ? smf_player.TimeSigDenominator() : 4;
        effective_state.song_total_measures = TickToMeasure(smf_player.TotalTicks(), smf_player);
        const uint64_t loop_start_tick
            = MeasureBeatToTick(effective_state.loop_start_measure,
                                effective_state.loop_start_beat,
                                smf_player);
        const uint16_t divisions = smf_player.Divisions();
        const uint64_t ticks_per_beat
            = divisions > 0
                  ? ((static_cast<uint64_t>(divisions) * 4u)
                     / static_cast<uint64_t>(effective_state.time_sig_den > 0
                                                 ? effective_state.time_sig_den
                                                 : 4))
                  : 0;
        const uint64_t loop_end_tick
            = loop_start_tick
              + static_cast<uint64_t>(effective_state.loop_length_beats > 0
                                          ? effective_state.loop_length_beats
                                          : 1)
                    * ticks_per_beat;
        effective_state.loop_end_measure = TickToMeasure(loop_end_tick, smf_player);
        effective_state.loop_end_beat    = TickToBeat(loop_end_tick, smf_player);
        for(uint8_t ch = 0; ch < 16; ch++)
            app_state.channels[ch].current_program = transport.ChannelProgram(ch);

        transport.ConsumeChannelActivity(channel_activity);
        for(size_t ch = 0; ch < 16; ch++)
        {
            if(channel_activity[ch] != 0)
            {
                channel_flash_until[ch] = now + kLedFlashMs;
                channel_monitor_until[ch] = now + kMonitorFlashMs;
                app_state.midi_monitor_activity[ch] = channel_activity[ch];
            }
            else if(channel_monitor_until[ch] <= now)
            {
                app_state.midi_monitor_activity[ch] = 0;
            }
        }

        uint8_t led_mask = 0;
        for(uint8_t slot = 0; slot < 4; slot++)
        {
            const int ch = VisibleChannelIndex(app_state.bank, slot);
            if(ch >= 0 && ch < 16 && channel_flash_until[ch] > now)
                led_mask |= static_cast<uint8_t>(1u << slot);
        }
        ui_input.SetLedMask(led_mask);

        const bool overlay_active = app_state.overlay.until_ms > now;
        const bool ui_active = overlay_active || (now - last_ui_activity_ms) < kUiActiveHoldMs
                               || app_state.ui_mode != UiMode::Performance;
        const uint32_t render_interval_ms = app_state.transport_playing
                                                ? (ui_active ? kRenderIntervalUiActiveMs
                                                             : kRenderIntervalPlayingMs)
                                                : kRenderIntervalStoppedMs;

        if(ui_dirty || (now - render_ms >= render_interval_ms))
        {
            render_ms = now;
            ui_renderer.Render(effective_state, media_library, now);
            ui_dirty = false;
        }

        hw.SetLed(((now / 250) % 2) != 0);
    }
}
