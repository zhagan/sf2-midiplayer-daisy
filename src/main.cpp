#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "boot_state_persist.h"
#include "clock_sync.h"
#include "cv_gate_engine.h"
#include "cv_gate_persist.h"
#include "daisy_patch_sm.h"
#include "dev/sdram.h"
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
#include "sysex_file_transfer.h"
#include "sysex_remote_control.h"
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

static constexpr bool kEnableUsbLog = false;

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
MediaLibrary      DSY_SDRAM_BSS media_library;
UiHardwareInput   ui_input;
UiEventTranslator ui_events;
UiController      ui_controller;
UiRenderer        ui_renderer;
CvGateEngine      cv_gate_engine;
AppState          app_state;
MidiUsbHandler    usb_midi;
MidiUartHandler   uart_midi;
SysExFileTransfer sysex_file_transfer;
SysExRemoteControl sysex_remote_control;
ClockSync         midi_clock_sync;
ClockSync         gate_clock_sync;
TimerHandle       midi_tx_timer;
bool              audio_started                  = false;
uint64_t          external_midi_step_count       = 0;
uint64_t          last_external_midi_tick        = 0;
bool              external_start_armed           = false;
bool              last_sync_play_active          = false;
uint8_t           applied_sf2_max_voices         = 0;
uint32_t          channel_flash_until[16]{};
uint32_t          channel_monitor_until[16]{};
volatile uint64_t sync_sample_counter      = 0;
volatile uint32_t pending_midi_clock_edges = 0;
volatile uint64_t next_midi_clock_edge_sample = 0;
volatile uint32_t pending_midi_tx_flushes  = 0;
volatile float    audio_output_gain        = 1.0f;
volatile float    audio_fade_target_gain   = 1.0f;
volatile float    audio_fade_step          = 0.0f;
volatile uint32_t audio_fade_remaining     = 0;

constexpr uint32_t kLedFlashMs                = 90;
constexpr uint32_t kMonitorFlashMs            = 250;
constexpr uint32_t kRenderIntervalStoppedMs   = 50;
constexpr uint32_t kRenderIntervalPlayingMs   = 500;
constexpr uint32_t kRenderIntervalUiActiveMs  = 50;
constexpr uint32_t kUiActiveHoldMs            = 1200;
constexpr uint32_t kPlaybackUiRenderHoldMs    = 180;
constexpr uint32_t kMidiTxTimerRateHz         = 2000;
constexpr uint32_t kAudioFadeMs               = 8;
ScheduledMidiOutputScheduler scheduled_midi_output;
uint8_t                      uart_running_status = 0;

void     UpdateMidiMonitor(const MidiEvent& msg);
void     UpdateMidiMonitor(const MidiEv& ev);
void     RefreshMediaLibrary(void*);
void     CompleteMidiUpload(bool success, void*);
void     PrepareForMidiUpload(uint32_t now_ms);
void     SyncLoopStateForRemote(void*);
void     StopAudioIfRunning();
void     BuildSongConfigPath(const char* midi_path, char* out, size_t out_sz);
bool     SaveSelectedSongConfig();
uint16_t TickToMeasure(uint64_t tick, const SmfPlayer& player);
uint8_t  TickToBeat(uint64_t tick, const SmfPlayer& player);
void SyncLoopDisplayFieldsFromTicks(AppState& state, const SmfPlayer& player);
bool ScheduledMidiOutputBlocked(const MidiEv& ev);
bool ScheduledMidiOutputBlockedAdapter(const MidiEv& ev, void*);
void UpdateMidiMonitorAdapter(const MidiEv& ev, void*);
bool MidiEvToRawBytesAdapter(const MidiEv&   ev,
                             uint8_t         out[3],
                             size_t&         size,
                             MidiOutputKind& kind,
                             void*);
bool PrepareScheduledMidiPacket(bool           to_usb,
                                MidiOutputKind kind,
                                const uint8_t* bytes,
                                size_t         size,
                                uint8_t        out[3],
                                size_t&        out_size,
                                void*);
void SendScheduledMidiPacket(bool           to_usb,
                             const uint8_t* bytes,
                             size_t         size,
                             void*);

bool DebugTraceCandidate(const MidiEv& ev)
{
    return ev.ch == 9
           && (ev.type == EvType::NoteOn || ev.type == EvType::NoteOff);
}

uint64_t MidiClockEdgeIntervalSamples()
{
    const float samples_per_16th = midi_clock_sync.GetSamplesPer16th();
    const float samples_per_clock
        = samples_per_16th > 0.0f
              ? (samples_per_16th / 6.0f)
              : ((hw.AudioSampleRate() * 60.0f) / (120.0f * 24.0f));
    return static_cast<uint64_t>(samples_per_clock > 1.0f ? samples_per_clock
                                                          : 1.0f);
}

const char* DebugTraceTypeName(EvType type)
{
    switch(type)
    {
        case EvType::NoteOn: return "ON";
        case EvType::NoteOff: return "OFF";
        case EvType::Program: return "PRG";
        case EvType::ControlChange: return "CC";
        case EvType::PitchBend: return "PB";
        case EvType::AllSoundOff: return "ASO";
        case EvType::AllNotesOff: return "ANO";
    }
    return "?";
}

void DebugMidiTrace(const char* stage, const MidiEv& ev, void*)
{
    if(!kEnableUsbLog || !DebugTraceCandidate(ev))
        return;
    LOG("MDBG %s at=%lu now=%lu t=%s ch=%u a=%u b=%u",
        stage,
        static_cast<unsigned long>(ev.atSample),
        static_cast<unsigned long>(transport.SampleClock()),
        DebugTraceTypeName(ev.type),
        static_cast<unsigned>(ev.ch + 1),
        static_cast<unsigned>(ev.a),
        static_cast<unsigned>(ev.b));
}

bool MidiOutputEnabled(const MidiOutputRouting& routing, MidiOutputKind kind)
{
    switch(kind)
    {
        case MidiOutputKind::Transport: return routing.transport;
        case MidiOutputKind::Clock: return routing.clock;
        case MidiOutputKind::Notes:
        case MidiOutputKind::Ccs:
        case MidiOutputKind::Programs: break;
    }
    return false;
}

bool ExtractMidiChannel(const uint8_t* bytes, size_t size, uint8_t& channel)
{
    if(bytes == nullptr || size == 0)
        return false;
    const uint8_t status = bytes[0];
    if(status >= 0x80 && status <= 0xEF)
    {
        channel = static_cast<uint8_t>(status & 0x0F);
        return true;
    }
    return false;
}

void InitMidiRoutingDefaults(MidiRoutingConfig& routing)
{
    routing = MidiRoutingConfig{};
    routing.usb.mode  = MidiOutputMode::Matrix;
    routing.uart.mode = MidiOutputMode::Matrix;
    for(uint8_t ch = 0; ch < 16; ch++)
    {
        routing.usb.channels[ch].destination_channel  = ch;
        routing.uart.channels[ch].destination_channel = ch;
    }
}

bool MidiOutputRoutingAllows(const MidiOutputRouting& routing,
                             MidiOutputKind           kind,
                             uint8_t                  channel)
{
    if(channel >= 16)
        return false;

    switch(kind)
    {
        case MidiOutputKind::Notes:
            switch(routing.mode)
            {
                case MidiOutputMode::Off: return false;
                case MidiOutputMode::Notes:
                case MidiOutputMode::NotesCcs:
                case MidiOutputMode::NotesCcsPrograms: return true;
                case MidiOutputMode::Matrix: return routing.channels[channel].notes;
            }
            break;
        case MidiOutputKind::Ccs:
            switch(routing.mode)
            {
                case MidiOutputMode::Off: return false;
                case MidiOutputMode::Notes: return false;
                case MidiOutputMode::NotesCcs:
                case MidiOutputMode::NotesCcsPrograms: return true;
                case MidiOutputMode::Matrix: return routing.channels[channel].ccs;
            }
            break;
        case MidiOutputKind::Programs:
            switch(routing.mode)
            {
                case MidiOutputMode::Off: return false;
                case MidiOutputMode::Notes:
                case MidiOutputMode::NotesCcs: return false;
                case MidiOutputMode::NotesCcsPrograms: return true;
                case MidiOutputMode::Matrix:
                    return routing.channels[channel].programs;
            }
            break;
        case MidiOutputKind::Transport:
        case MidiOutputKind::Clock: break;
    }
    return false;
}

bool PrepareMidiOutput(const MidiOutputRouting& routing,
                       MidiOutputKind           kind,
                       const uint8_t*           bytes,
                       size_t                   size,
                       uint8_t                  out[3],
                       size_t&                  out_size)
{
    if(kind == MidiOutputKind::Transport || kind == MidiOutputKind::Clock)
    {
        for(size_t i = 0; i < size && i < 3; i++)
            out[i] = bytes[i];
        out_size = size;
        return MidiOutputEnabled(routing, kind);
    }

    uint8_t channel = 0;
    if(!ExtractMidiChannel(bytes, size, channel) || channel >= 16)
        return false;

    if(!MidiOutputRoutingAllows(routing, kind, channel))
        return false;

    for(size_t i = 0; i < size && i < 3; i++)
        out[i] = bytes[i];
    out_size = size;

    if(routing.mode == MidiOutputMode::Matrix)
        out[0] = static_cast<uint8_t>(
            (out[0] & 0xF0) | (routing.channels[channel].destination_channel & 0x0F));

    return true;
}

bool GateInputSyncEnabled(const CvGateConfig& config, size_t index)
{ return index < 2 && config.gate_in[index].mode == GateInMode::SyncIn; }

bool AnyGateInputSyncEnabled(const CvGateConfig& config)
{ return GateInputSyncEnabled(config, 0) || GateInputSyncEnabled(config, 1); }

const char* Sf2LoadOverlayText()
{
    switch(SynthLastLoadResult())
    {
        case SynthLoadResult::Ok: return "SF2 Loaded";
        case SynthLoadResult::FileOpenFailed: return "SF2 Open Fail";
        case SynthLoadResult::FileTooLarge: return "SF2 Too Big";
        case SynthLoadResult::ParseFailed: return "SF2 Load Fail";
    }
    return "SF2 Load Fail";
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

void SetAudioOutputGainImmediate(float gain)
{
    if(gain < 0.0f)
        gain = 0.0f;
    if(gain > 1.0f)
        gain = 1.0f;

    ScopedIrqBlocker lock;
    audio_output_gain      = gain;
    audio_fade_target_gain = gain;
    audio_fade_step        = 0.0f;
    audio_fade_remaining   = 0;
}

void StartAudioFade(float target_gain, uint32_t duration_ms)
{
    if(target_gain < 0.0f)
        target_gain = 0.0f;
    if(target_gain > 1.0f)
        target_gain = 1.0f;

    const uint32_t fade_samples
        = duration_ms == 0
              ? 0
              : static_cast<uint32_t>(
                    (hw.AudioSampleRate() * static_cast<float>(duration_ms))
                    / 1000.0f);

    ScopedIrqBlocker lock;
    if(fade_samples == 0)
    {
        audio_output_gain      = target_gain;
        audio_fade_target_gain = target_gain;
        audio_fade_step        = 0.0f;
        audio_fade_remaining   = 0;
        return;
    }

    audio_fade_target_gain = target_gain;
    audio_fade_remaining   = fade_samples;
    audio_fade_step        = (audio_fade_target_gain - audio_output_gain)
                             / static_cast<float>(fade_samples);
}

void ApplyAudioOutputGain(AudioHandle::OutputBuffer out, size_t size)
{
    float    gain      = audio_output_gain;
    float    step      = audio_fade_step;
    float    target    = audio_fade_target_gain;
    uint32_t remaining = audio_fade_remaining;

    for(size_t i = 0; i < size; i++)
    {
        out[0][i] *= gain;
        out[1][i] *= gain;

        if(remaining > 0)
        {
            gain += step;
            remaining--;
            if(remaining == 0)
                gain = target;
        }
    }

    audio_output_gain    = gain;
    audio_fade_remaining = remaining;
    if(remaining == 0)
        audio_fade_step = 0.0f;
}

void WaitForAudioFadeComplete(uint32_t timeout_ms)
{
    while(audio_fade_remaining > 0 && timeout_ms > 0)
    {
        System::Delay(1);
        timeout_ms--;
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
    std::snprintf(
        text, sizeof(text), "%s %s", ctx->prefix, PersistWriteStageName(stage));
    SetOverlay(app_state, text, ctx->now_ms, 400);
    if(app_state.saving_all)
        ui_renderer.Render(app_state, media_library, ctx->now_ms);
    LOG("Save stage: %s", text);
}

bool SaveSelectedSongConfig()
{
    char        midi_path[MediaLibrary::kPathMax + 16]{};
    char        song_cfg_path[MediaLibrary::kPathMax + 24]{};
    const char* current_sf2_name = app_state.selected_sf2_path;
    media_library.BuildMidiPath(
        app_state.selected_midi_path, midi_path, sizeof(midi_path));
    BuildSongConfigPath(midi_path, song_cfg_path, sizeof(song_cfg_path));
    if(song_cfg_path[0] == '\0')
        return false;

    const bool ok = SaveSongConfig(song_cfg_path, app_state, current_sf2_name);
    if(ok)
        SaveBootState(app_state, app_state.selected_midi_path);
    return ok;
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
    app_state.cv_gate      = CvGateConfig{};
    InitMidiRoutingDefaults(app_state.midi_routing);
    app_state.song_bpm_override     = 0;
    app_state.song_loop_enabled     = false;
    app_state.loop_start_tick       = 0;
    app_state.loop_length_ticks     = 1920;
    app_state.sf2_master_volume_max = 127;
    app_state.sf2_expression_max    = 127;
    app_state.sf2_reverb_max        = 127;
    app_state.sf2_chorus_max        = 127;
    app_state.sf2_transpose         = 0;
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

void SendRawMidiUart(const uint8_t* bytes, size_t size)
{
    if(bytes == nullptr || size == 0)
        return;

    const uint8_t status               = bytes[0];
    const bool    channel_voice_status = status >= 0x80 && status <= 0xEF;

    if(channel_voice_status && size >= 2)
    {
        if(status == uart_running_status)
        {
            uint8_t data[2]{};
            for(size_t i = 1; i < size && i < 3; i++)
                data[i - 1] = bytes[i];
            uart_midi.SendMessage(data, size - 1);
            return;
        }

        uart_running_status = status;
    }

    SendRawMidi(uart_midi, bytes, size);
}

void SendToConfiguredOutputs(MidiOutputKind kind,
                             const uint8_t* bytes,
                             size_t         size)
{
    uint8_t routed[3]{};
    size_t  routed_size = 0;
    if(PrepareMidiOutput(
           app_state.midi_routing.usb, kind, bytes, size, routed, routed_size))
        SendRawMidi(usb_midi, routed, routed_size);
    if(PrepareMidiOutput(app_state.midi_routing.uart,
                         kind,
                         bytes,
                         size,
                         routed,
                         routed_size))
        SendRawMidiUart(routed, routed_size);
}

void SendToDestinationOutput(bool           to_usb,
                             MidiOutputKind kind,
                             const uint8_t* bytes,
                             size_t         size)
{
    const MidiOutputRouting& routing
        = to_usb ? app_state.midi_routing.usb : app_state.midi_routing.uart;
    uint8_t routed[3]{};
    size_t  routed_size = 0;
    if(!PrepareMidiOutput(routing, kind, bytes, size, routed, routed_size))
        return;
    if(to_usb)
        SendRawMidi(usb_midi, routed, routed_size);
    else
        SendRawMidiUart(routed, routed_size);
}

void SendAllNotesAndSoundOffToConfiguredOutputs()
{
    uint8_t bytes[3]{};
    bytes[2] = 0;
    for(uint8_t ch = 0; ch < 16; ch++)
    {
        const bool usb_enabled
            = MidiOutputRoutingAllows(
                  app_state.midi_routing.usb, MidiOutputKind::Notes, ch)
              || MidiOutputRoutingAllows(
                  app_state.midi_routing.usb, MidiOutputKind::Ccs, ch)
              || MidiOutputRoutingAllows(
                  app_state.midi_routing.usb, MidiOutputKind::Programs, ch);
        const bool uart_enabled
            = MidiOutputRoutingAllows(
                  app_state.midi_routing.uart, MidiOutputKind::Notes, ch)
              || MidiOutputRoutingAllows(
                  app_state.midi_routing.uart, MidiOutputKind::Ccs, ch)
              || MidiOutputRoutingAllows(
                  app_state.midi_routing.uart, MidiOutputKind::Programs, ch);
        bytes[0] = static_cast<uint8_t>(0xB0 | (ch & 0x0F));
        bytes[1] = 123;
        if(usb_enabled)
            SendRawMidi(usb_midi, bytes, 3);
        if(uart_enabled)
            SendRawMidiUart(bytes, 3);
        bytes[1] = 120;
        if(usb_enabled)
            SendRawMidi(usb_midi, bytes, 3);
        if(uart_enabled)
            SendRawMidiUart(bytes, 3);
    }
}

void UpdateMidiMonitorAdapter(const MidiEv& ev, void*)
{
    UpdateMidiMonitor(ev);
}

bool ScheduledMidiOutputBlockedAdapter(const MidiEv& ev, void*)
{
    return ScheduledMidiOutputBlocked(ev);
}

bool MidiEvToRawBytesAdapter(const MidiEv&   ev,
                             uint8_t         out[3],
                             size_t&         size,
                             MidiOutputKind& kind,
                             void*)
{
    return BuildRawMidiFromScheduledEvent(ev, out, size, kind);
}

bool PrepareScheduledMidiPacket(bool           to_usb,
                                MidiOutputKind kind,
                                const uint8_t* bytes,
                                size_t         size,
                                uint8_t        out[3],
                                size_t&        out_size,
                                void*)
{
    const MidiOutputRouting& routing
        = to_usb ? app_state.midi_routing.usb : app_state.midi_routing.uart;
    return PrepareMidiOutput(routing, kind, bytes, size, out, out_size);
}

void SendScheduledMidiPacket(bool           to_usb,
                             const uint8_t* bytes,
                             size_t         size,
                             void*)
{
    if(to_usb)
        SendRawMidi(usb_midi, bytes, size);
    else
        SendRawMidiUart(bytes, size);
}

bool ScheduledMidiOutQueueEmpty()
{ return scheduled_midi_output.Empty(); }

void ForwardScheduledMidiOut(const MidiEv& ev, void*)
{
    scheduled_midi_output.ForwardScheduledMidiOut(ev,
                                                  hw.AudioSampleRate(),
                                                  UpdateMidiMonitorAdapter,
                                                  ScheduledMidiOutputBlockedAdapter,
                                                  MidiEvToRawBytesAdapter,
                                                  PrepareScheduledMidiPacket,
                                                  nullptr);
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

void FlushScheduledMidiOut()
{ scheduled_midi_output.FlushScheduledMidiOut(transport.SampleClock(),
                                              PrepareScheduledMidiPacket,
                                              SendScheduledMidiPacket,
                                              nullptr); }

inline void ServiceMidiOutputs()
{ uart_midi.ServiceOutput(); }

void MidiTxTimerCallback(void*)
{ pending_midi_tx_flushes++; }

void MaybeForwardThru(const MidiEvent& msg, bool from_usb)
{
    const bool to_uart = from_usb && app_state.midi_routing.usb_in_to_uart;
    const bool to_usb  = !from_usb && app_state.midi_routing.uart_in_to_usb;
    if(!to_uart && !to_usb)
        return;

    uint8_t        bytes[3]{};
    size_t         size = 0;
    MidiOutputKind kind = MidiOutputKind::Notes;
    if(!BuildRawMidiFromIncomingEvent(msg, bytes, size, kind))
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

void RefreshMediaLibrary(void*)
{
    media_library.Scan();
}

void CompleteMidiUpload(bool success, void*)
{
    app_state.pending_midi_load = false;
    app_state.pending_sf2_load  = false;
    app_state.loading_midi      = false;
    app_state.loading_sf2       = false;
    app_state.ui_mode           = UiMode::Performance;
    app_state.menu_page         = MenuPage::Main;
    app_state.menu_page_cursor  = 0;
    app_state.menu_root_cursor  = 0;
    app_state.menu_editing      = false;

    if(app_state.selected_midi_path[0] == '\0'
       || !media_library.MidiFileExists(app_state.selected_midi_path))
    {
        char first_path[MediaLibrary::kPathMax]{};
        if(media_library.FindFirstMidiFile(first_path, sizeof(first_path)))
            std::snprintf(app_state.selected_midi_path,
                         sizeof(app_state.selected_midi_path),
                         "%s",
                         first_path);
        else
            app_state.selected_midi_path[0] = '\0';
    }
    if(app_state.selected_sf2_path[0] == '\0'
       || !media_library.SoundFontFileExists(app_state.selected_sf2_path))
    {
        char first_path[MediaLibrary::kPathMax]{};
        if(media_library.FindFirstSoundFont(first_path, sizeof(first_path)))
            std::snprintf(app_state.selected_sf2_path,
                         sizeof(app_state.selected_sf2_path),
                         "%s",
                         first_path);
        else
            app_state.selected_sf2_path[0] = '\0';
    }

    if(app_state.selected_sf2_path[0] != '\0')
        app_state.pending_sf2_load = true;
    if(app_state.selected_midi_path[0] != '\0')
        app_state.pending_midi_load = true;

    SetOverlay(app_state, success ? "USB MIDI Done" : "USB MIDI Aborted", System::GetNow(), 1200);
}

void SyncLoopStateForRemote(void*)
{
    SyncLoopDisplayFieldsFromTicks(app_state, smf_player);
}

bool IsUploadTransferCommand(const MidiEvent& msg, uint8_t command)
{
    return msg.type == MidiMessageType::SystemCommon
           && msg.sc_type == SystemCommonType::SystemExclusive
           && msg.sysex_message_len >= 4 && msg.sysex_data[0] == 0x7D
           && msg.sysex_data[1] == 'M' && msg.sysex_data[2] == 'M'
           && msg.sysex_data[3] == command;
}

void PrepareForMidiUpload(uint32_t now_ms)
{
    app_state.transport_playing = false;

    if(audio_started)
    {
        StartAudioFade(0.0f, kAudioFadeMs);
        WaitForAudioFadeComplete(kAudioFadeMs + 8);
    }

    StopAudioIfRunning();
    SetAudioOutputGainImmediate(0.0f);
    transport.Reset(app_state);
    smf_player.Close();
    SynthUnloadSf2();
    SetOverlay(app_state, "USB MIDI Upload", now_ms, 1000);
}

void ServiceIncomingMidi()
{
    usb_midi.Listen();
    while(usb_midi.HasEvents())
    {
        const auto msg = usb_midi.PopEvent();
        if(IsUploadTransferCommand(msg, 0x01))
            PrepareForMidiUpload(System::GetNow());
        if(sysex_file_transfer.HandleUsbMidiEvent(msg, usb_midi))
            continue;
        if(sysex_remote_control.HandleUsbMidiEvent(msg, usb_midi))
            continue;
        if(msg.type == MidiMessageType::SystemRealTime)
        {
            switch(msg.srt_type)
            {
                case SystemRealTimeType::TimingClock:
                    pending_midi_clock_edges++;
                    break;
                case SystemRealTimeType::Start:
                    if(app_state.sync_external)
                    {
                        next_midi_clock_edge_sample = 0;
                        external_midi_step_count    = 0;
                        last_external_midi_tick     = 0;
                        external_start_armed        = true;
                        app_state.transport_playing = true;
                    }
                    break;
                case SystemRealTimeType::Continue:
                    break;
                case SystemRealTimeType::Stop:
                    if(app_state.sync_external)
                    {
                        pending_midi_clock_edges    = 0;
                        next_midi_clock_edge_sample = 0;
                        external_midi_step_count    = 0;
                        last_external_midi_tick     = 0;
                        external_start_armed        = false;
                        app_state.transport_playing = false;
                    }
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
                case SystemRealTimeType::TimingClock:
                    pending_midi_clock_edges++;
                    break;
                case SystemRealTimeType::Start:
                    if(app_state.sync_external)
                    {
                        next_midi_clock_edge_sample = 0;
                        external_midi_step_count    = 0;
                        last_external_midi_tick     = 0;
                        external_start_armed        = true;
                        app_state.transport_playing = true;
                    }
                    break;
                case SystemRealTimeType::Continue:
                    break;
                case SystemRealTimeType::Stop:
                    if(app_state.sync_external)
                    {
                        pending_midi_clock_edges    = 0;
                        next_midi_clock_edge_sample = 0;
                        external_midi_step_count    = 0;
                        last_external_midi_tick     = 0;
                        external_start_armed        = false;
                        app_state.transport_playing = false;
                    }
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
    app_state.fx_reverb_enabled  = SynthGetReverbEnabled();
    app_state.fx_chorus_depth    = SynthGetChorusDepth();
    app_state.fx_chorus_speed_hz = SynthGetChorusSpeed();
    app_state.fx_chorus_enabled  = SynthGetChorusEnabled();
}

void SyncSongStateFromPlayer()
{
    const auto& settings            = smf_player.Settings();
    app_state.song_bpm_override     = settings.bpm_override;
    app_state.song_loop_enabled     = settings.loop_enabled;
    app_state.sf2_master_volume_max = settings.master_volume_max;
    app_state.sf2_expression_max    = settings.expression_max;
    app_state.sf2_reverb_max        = settings.reverb_max;
    app_state.sf2_chorus_max        = settings.chorus_max;
    app_state.sf2_transpose         = settings.transpose;
    for(int ch = 0; ch < 16; ch++)
    {
        app_state.channels[ch].program_override = settings.program_override[ch];
        app_state.channels[ch].pan
            = settings.pan_override[ch] >= 0
                  ? static_cast<uint8_t>(settings.pan_override[ch])
                  : 64;
        app_state.channels[ch].volume      = settings.volume[ch];
        app_state.channels[ch].reverb_send = settings.reverb_send[ch];
        app_state.channels[ch].chorus_send = settings.chorus_send[ch];
        app_state.channels[ch].muted       = settings.muted[ch];
        app_state.channels[ch].current_program
            = settings.program_override[ch] >= 0
                  ? static_cast<uint8_t>(settings.program_override[ch])
                  : 0;
    }
    if(app_state.sf2_max_voices > 32)
        app_state.sf2_max_voices = 16;
    const uint16_t divisions = smf_player.Divisions();
    const int      ts_den    = smf_player.TimeSigDenominator() > 0
                                   ? smf_player.TimeSigDenominator()
                                   : 4;
    const int      ts_num
        = smf_player.TimeSigNumerator() > 0 ? smf_player.TimeSigNumerator() : 4;
    app_state.song_divisions = divisions > 0 ? divisions : 480;
    app_state.time_sig_num   = static_cast<uint8_t>(ts_num);
    app_state.time_sig_den   = static_cast<uint8_t>(ts_den);
    const uint32_t ticks_per_beat
        = divisions > 0 ? ((static_cast<uint32_t>(divisions) * 4u)
                           / static_cast<uint32_t>(ts_den))
                        : 0u;
    const uint32_t sub_per_beat
        = ts_den > 0 ? static_cast<uint32_t>(16 / ts_den) : 4u;
    const uint32_t ticks_per_sub     = (sub_per_beat > 0 && ticks_per_beat > 0)
                                           ? (ticks_per_beat / sub_per_beat)
                                           : ticks_per_beat;
    const uint32_t legacy_start_tick = static_cast<uint32_t>(
        (settings.loop_start_measure > 1 ? (settings.loop_start_measure - 1)
                                         : 0)
            * ts_num * ticks_per_beat
        + (settings.loop_start_beat > 1 ? (settings.loop_start_beat - 1) : 0)
              * ticks_per_beat
        + (settings.loop_start_sub > 1 ? (settings.loop_start_sub - 1) : 0)
              * ticks_per_sub);
    app_state.loop_start_tick = settings.loop_start_tick > 0
                                    ? settings.loop_start_tick
                                    : legacy_start_tick;
    app_state.loop_length_ticks
        = settings.loop_length_ticks > 0
              ? settings.loop_length_ticks
              : static_cast<uint32_t>((settings.loop_length_beats > 0
                                           ? settings.loop_length_beats
                                           : 1)
                                      * ticks_per_beat);
    if(app_state.loop_length_ticks < 1)
        app_state.loop_length_ticks = ticks_per_beat > 0 ? ticks_per_beat : 1;
    SyncLoopDisplayFieldsFromTicks(app_state, smf_player);
}

void ApplyAppSettings()
{
    if(!app_state.settings_dirty)
        return;

    SynthSetReverbTime(app_state.fx_reverb_time);
    SynthSetReverbLpFreq(app_state.fx_reverb_lpf_hz);
    SynthSetReverbHpFreq(app_state.fx_reverb_hpf_hz);
    SynthSetReverbEnabled(app_state.fx_reverb_enabled);
    SynthSetChorusDepth(app_state.fx_chorus_depth);
    SynthSetChorusSpeed(app_state.fx_chorus_speed_hz);
    SynthSetChorusEnabled(app_state.fx_chorus_enabled);
    if(applied_sf2_max_voices != app_state.sf2_max_voices)
    {
        SynthSetMaxVoices(app_state.sf2_max_voices);
        applied_sf2_max_voices = app_state.sf2_max_voices;
    }

    auto& settings             = smf_player.MutableSettings();
    settings.bpm_override      = app_state.song_bpm_override;
    settings.loop_enabled      = app_state.song_loop_enabled;
    settings.master_volume_max = app_state.sf2_master_volume_max;
    settings.expression_max    = app_state.sf2_expression_max;
    settings.reverb_max        = app_state.sf2_reverb_max;
    settings.chorus_max        = app_state.sf2_chorus_max;
    settings.transpose         = app_state.sf2_transpose;
    for(int ch = 0; ch < 16; ch++)
    {
        settings.program_override[ch] = app_state.channels[ch].program_override;
        settings.pan_override[ch]
            = static_cast<int8_t>(app_state.channels[ch].pan);
        settings.volume[ch]      = app_state.channels[ch].volume;
        settings.reverb_send[ch] = app_state.channels[ch].reverb_send;
        settings.chorus_send[ch] = app_state.channels[ch].chorus_send;
        settings.muted[ch]       = app_state.channels[ch].muted;
    }
    settings.loop_start_tick = app_state.loop_start_tick;
    settings.loop_length_ticks
        = app_state.loop_length_ticks < 1 ? 1 : app_state.loop_length_ticks;
    settings.loop_start_measure = 1;
    settings.loop_start_beat    = 1;
    settings.loop_start_sub     = 1;
    settings.loop_length_beats  = 0;

    if(app_state.song_bpm_override > 0)
        app_state.bpm = app_state.song_bpm_override;

    app_state.settings_dirty = false;
}

int TempoUsecToBpm(uint32_t tempo_usec)
{
    if(tempo_usec == 0)
        return 120;
    const int bpm
        = static_cast<int>((60000000.0 + tempo_usec / 2.0) / tempo_usec);
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

    const int numerator
        = player.TimeSigNumerator() > 0 ? player.TimeSigNumerator() : 4;
    const int denominator
        = player.TimeSigDenominator() > 0 ? player.TimeSigDenominator() : 4;
    const uint64_t ticks_per_beat = (static_cast<uint64_t>(divisions) * 4u)
                                    / static_cast<uint64_t>(denominator);
    const uint64_t ticks_per_measure
        = ticks_per_beat * static_cast<uint64_t>(numerator);
    if(ticks_per_measure == 0)
        return 1;

    return static_cast<uint16_t>((tick / ticks_per_measure) + 1u);
}

uint8_t TickToBeat(uint64_t tick, const SmfPlayer& player)
{
    const uint16_t divisions = player.Divisions();
    if(divisions == 0)
        return 1;

    const int denominator
        = player.TimeSigDenominator() > 0 ? player.TimeSigDenominator() : 4;
    const uint64_t ticks_per_beat = (static_cast<uint64_t>(divisions) * 4u)
                                    / static_cast<uint64_t>(denominator);
    if(ticks_per_beat == 0)
        return 1;

    const int numerator
        = player.TimeSigNumerator() > 0 ? player.TimeSigNumerator() : 4;
    const uint64_t beat_index
        = (tick / ticks_per_beat) % static_cast<uint64_t>(numerator);
    return static_cast<uint8_t>(beat_index + 1u);
}

void SyncLoopDisplayFieldsFromTicks(AppState& state, const SmfPlayer& player)
{
    state.loop_end_tick    = state.loop_start_tick + state.loop_length_ticks;
    state.loop_end_measure = TickToMeasure(state.loop_end_tick, player);
    state.loop_end_beat    = TickToBeat(state.loop_end_tick, player);

    const uint16_t divisions = player.Divisions();
    const int      numerator
        = player.TimeSigNumerator() > 0 ? player.TimeSigNumerator() : 4;
    const int denominator
        = player.TimeSigDenominator() > 0 ? player.TimeSigDenominator() : 4;
    const uint32_t ticks_per_beat
        = divisions > 0 ? ((static_cast<uint32_t>(divisions) * 4u)
                           / static_cast<uint32_t>(denominator))
                        : 0u;
    const uint32_t ticks_per_measure
        = ticks_per_beat * static_cast<uint32_t>(numerator);

    if(ticks_per_beat == 0 || ticks_per_measure == 0)
    {
        state.loop_start_measure   = 1;
        state.loop_start_beat      = 1;
        state.loop_length_measures = 0;
        state.loop_length_beats    = 0;
        return;
    }

    state.loop_start_measure
        = static_cast<int>(state.loop_start_tick / ticks_per_measure) + 1;
    state.loop_start_beat
        = static_cast<int>((state.loop_start_tick % ticks_per_measure)
                           / ticks_per_beat)
          + 1;

    const uint32_t total_beats = state.loop_length_ticks / ticks_per_beat;
    state.loop_length_measures
        = static_cast<int>(total_beats / static_cast<uint32_t>(numerator));
    state.loop_length_beats
        = static_cast<int>(total_beats % static_cast<uint32_t>(numerator));
}

void InitDefaultState()
{
    app_state = AppState{};
    InitMidiRoutingDefaults(app_state.midi_routing);
    for(int ch = 0; ch < 16; ch++)
    {
        app_state.channels[ch].volume           = 100;
        app_state.channels[ch].pan              = 64;
        app_state.channels[ch].reverb_send      = 0;
        app_state.channels[ch].chorus_send      = 0;
        app_state.channels[ch].current_program  = 0;
        app_state.channels[ch].program_override = -1;
        app_state.channels[ch].muted            = false;
    }
}

bool EnsureAudioRunning()
{
    if(!audio_started)
    {
        hw.StartAudio(
            [](AudioHandle::InputBuffer  in,
               AudioHandle::OutputBuffer out,
               size_t                    size)
            {
                hw.ProcessAnalogControls();
                for(size_t i = 0; i < size; i++)
                {
                    const uint64_t sample_time = sync_sample_counter + i;
                    bool           midi_edge   = false;
                    bool           gate_edge   = false;
                    if(pending_midi_clock_edges > 0
                       && next_midi_clock_edge_sample == 0)
                    {
                        next_midi_clock_edge_sample = sample_time;
                    }
                    if(pending_midi_clock_edges > 0
                       && sample_time >= next_midi_clock_edge_sample)
                    {
                        pending_midi_clock_edges--;
                        midi_edge = true;
                        if(pending_midi_clock_edges > 0)
                        {
                            next_midi_clock_edge_sample
                                = sample_time + MidiClockEdgeIntervalSamples();
                        }
                        else
                        {
                            next_midi_clock_edge_sample = 0;
                        }
                    }
                    midi_clock_sync.ProcessSample(midi_edge, sample_time);
                    if(GateInputSyncEnabled(app_state.cv_gate, 0))
                        gate_edge = gate_edge || hw.gate_in_1.State();
                    if(GateInputSyncEnabled(app_state.cv_gate, 1))
                        gate_edge = gate_edge || hw.gate_in_2.State();
                    gate_clock_sync.ProcessSample(gate_edge, sample_time);
                }
                sync_sample_counter += size;
                cv_gate_engine.Update(app_state, transport);
                transport.ProcessAudio(in, out, size);
                ApplyAudioOutputGain(out, size);
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
    char midi_path[MediaLibrary::kPathMax + 16]{};
    char sf2_path[MediaLibrary::kPathMax + 16]{};
    char song_cfg_path[MediaLibrary::kPathMax + 24]{};
    char cfg_sf2_name[MediaLibrary::kPathMax]{};

    if(reload_midi)
    {
        media_library.BuildMidiPath(
            app_state.selected_midi_path, midi_path, sizeof(midi_path));
        BuildSongConfigPath(midi_path, song_cfg_path, sizeof(song_cfg_path));
    }
    if(reload_sf2)
        media_library.BuildSoundFontPath(
            app_state.selected_sf2_path, sf2_path, sizeof(sf2_path));

    if(reload_midi && song_cfg_path[0] != '\0')
    {
        AppState cfg_state = app_state;
        if(LoadSongConfig(
               song_cfg_path, cfg_state, cfg_sf2_name, sizeof(cfg_sf2_name)))
        {
            char desired_path[MediaLibrary::kPathMax]{};
            bool have_desired = false;
            if(cfg_sf2_name[0] != '\0' && media_library.SoundFontFileExists(cfg_sf2_name))
            {
                std::snprintf(desired_path, sizeof(desired_path), "%s", cfg_sf2_name);
                have_desired = true;
            }
            else
            {
                have_desired = media_library.FindFirstSoundFont(
                    desired_path, sizeof(desired_path));
            }
            if(have_desired
               && std::strncmp(desired_path,
                              app_state.selected_sf2_path,
                              MediaLibrary::kPathMax)
                      != 0)
            {
                std::snprintf(app_state.selected_sf2_path,
                             sizeof(app_state.selected_sf2_path),
                             "%s",
                             desired_path);
                reload_sf2 = true;
                media_library.BuildSoundFontPath(
                    app_state.selected_sf2_path, sf2_path, sizeof(sf2_path));
            }
        }
    }

    const bool was_playing      = app_state.transport_playing;
    app_state.transport_playing = false;

    if(audio_started)
    {
        StartAudioFade(0.0f, kAudioFadeMs);
        WaitForAudioFadeComplete(kAudioFadeMs + 8);
    }

    StopAudioIfRunning();
    SetAudioOutputGainImmediate(0.0f);
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
            SaveBootState(app_state, app_state.selected_midi_path);
            app_state.bpm = TempoUsecToBpm(smf_player.TempoUsecPerQuarter());
            transport.SetFileBpm(static_cast<float>(app_state.bpm));
            SyncSongStateFromPlayer();
            if(song_cfg_path[0] != '\0')
                LoadSongConfig(song_cfg_path,
                               app_state,
                               cfg_sf2_name,
                               sizeof(cfg_sf2_name));
            SyncLoopDisplayFieldsFromTicks(app_state, smf_player);
            app_state.settings_dirty = true;
            ApplyAppSettings();
        }
    }

    if(sf_ok)
    {
        EnsureAudioRunning();
        StartAudioFade(1.0f, kAudioFadeMs);
    }

    if(reload_midi)
        SetOverlay(
            app_state, midi_ok ? "MIDI Loaded" : "MIDI Load Fail", now_ms);
    else if(reload_sf2)
        SetOverlay(
            app_state, sf_ok ? "SF2 Loaded" : Sf2LoadOverlayText(), now_ms);

    app_state.pending_midi_load = false;
    app_state.pending_sf2_load  = false;
    app_state.loading_midi      = false;
    app_state.loading_sf2       = false;
    app_state.transport_playing = (sf_ok && midi_ok) ? was_playing : false;
    return sf_ok && midi_ok;
}

bool SaveAllSettings(uint32_t now_ms)
{
    char        midi_path[MediaLibrary::kPathMax + 16]{};
    char        sf2_path[MediaLibrary::kPathMax + 16]{};
    char        song_cfg_path[MediaLibrary::kPathMax + 24]{};
    const char*         current_sf2_name = app_state.selected_sf2_path;
    const bool          had_audio        = audio_started;
    PersistWriteStage   song_stage       = PersistWriteStage::None;
    int                 song_result_code = -1;
    SaveProgressContext song_progress{now_ms, "SONG"};
    auto                show_save_stage = [&](const char* text)
    {
        SetOverlay(app_state, text, now_ms, 400);
        if(app_state.saving_all)
            ui_renderer.Render(app_state, media_library, now_ms);
        LOG("Save stage: %s", text);
    };

    media_library.BuildMidiPath(
        app_state.selected_midi_path, midi_path, sizeof(midi_path));
    media_library.BuildSoundFontPath(
        app_state.selected_sf2_path, sf2_path, sizeof(sf2_path));
    BuildSongConfigPath(midi_path, song_cfg_path, sizeof(song_cfg_path));

    show_save_stage("Save Prep");
    app_state.transport_playing = false;
    StopAudioIfRunning();
    transport.Reset(app_state);

    show_save_stage("Write SONG CFG");
    const bool song_ok = song_cfg_path[0] != '\0'
                         && SaveSongConfig(song_cfg_path,
                                           app_state,
                                           current_sf2_name,
                                           &song_stage,
                                           &song_result_code,
                                           SaveProgressOverlay,
                                           &song_progress);
    SaveBootState(app_state, app_state.selected_midi_path);
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

    if(song_ok && song_cfg_path[0] != '\0')
    {
        LoadSongConfig(song_cfg_path, app_state);
        SyncLoopDisplayFieldsFromTicks(app_state, smf_player);
        app_state.settings_dirty = true;
        ApplyAppSettings();
    }

    if(had_audio && sf2_path[0] != '\0')
    {
        show_save_stage("Resume Audio");
        EnsureAudioRunning();
    }

    if(song_ok)
    {
        app_state.ui_mode            = UiMode::Performance;
        app_state.menu_page          = MenuPage::Main;
        app_state.menu_page_cursor   = 0;
        app_state.menu_root_cursor   = 0;
        app_state.menu_editing       = false;
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
        hw.StartLog(true);

    InitDefaultState();
    ui_controller.Init(app_state);
    ui_input.Init(hw);
    ui_renderer.Init();
    ui_renderer.ShowSplash();

    const bool sd_ok = SdMount();
    LOG("SD mount: %s", sd_ok ? "PASS" : "FAIL");

    char boot_midi_name[MediaLibrary::kPathMax]{};
    media_library.Scan();

    if(sd_ok)
        LoadBootState(app_state, boot_midi_name, sizeof(boot_midi_name));

    ui_renderer.SetDisplayXOffset(app_state.oled_x_offset);

    SynthInit();
    smf_player.SetSampleRate(hw.AudioSampleRate());
    smf_player.SetLookaheadSamples(hw.AudioBlockSize() * 256);
    smf_player.SetTempoScale(1.0f);
    transport.Init(hw.AudioSampleRate(), smf_player);
    transport.SetMidiOutputCallback(ForwardScheduledMidiOut, nullptr);
    transport.SetDebugMidiCallback(DebugMidiTrace, nullptr);
    cv_gate_engine.Init(hw, hw.AudioSampleRate());
    ClockSync::Config midi_clock_cfg{};
    midi_clock_cfg.alpha             = 0.05f;
    midi_clock_cfg.beta              = 0.15f;
    midi_clock_cfg.glitch_percent    = 0.20f;
    midi_clock_cfg.debounce_ms       = 0.20f;
    midi_clock_cfg.missing_timeout_s = 1.00f;
    midi_clock_sync.Init(hw.AudioSampleRate(),
                         ClockSync::PulseMode::MIDI_24PPQN,
                         midi_clock_cfg);
    midi_clock_sync.SetUseExternalClock(true);
    gate_clock_sync.Init(hw.AudioSampleRate(),
                         ClockSync::PulseMode::PULSE_PER_16TH);
    gate_clock_sync.SetUseExternalClock(true);

    if(boot_midi_name[0] != '\0' && media_library.MidiFileExists(boot_midi_name))
    {
        std::snprintf(app_state.selected_midi_path,
                     sizeof(app_state.selected_midi_path),
                     "%s",
                     boot_midi_name);
    }
    else
    {
        char first_path[MediaLibrary::kPathMax]{};
        if(media_library.FindFirstMidiFile(first_path, sizeof(first_path)))
            std::snprintf(app_state.selected_midi_path,
                         sizeof(app_state.selected_midi_path),
                         "%s",
                         first_path);
    }
    {
        char first_sf2_path[MediaLibrary::kPathMax]{};
        if(media_library.FindFirstSoundFont(first_sf2_path, sizeof(first_sf2_path)))
            std::snprintf(app_state.selected_sf2_path,
                         sizeof(app_state.selected_sf2_path),
                         "%s",
                         first_sf2_path);
    }

    if(sd_ok)
    {
        LoadSelectedMedia(true, true, System::GetNow());
        if(app_state.selected_midi_path[0] != '\0'
           && app_state.selected_sf2_path[0] != '\0')
            SetOverlay(app_state, "Ready", System::GetNow());
    }

    MidiUsbHandler::Config usb_cfg{};
    usb_cfg.transport_config.periph = MidiUsbTransport::Config::EXTERNAL;
    usb_cfg.transport_config.tx_retry_count = 10;
    usb_midi.Init(usb_cfg);
    usb_midi.StartReceive();
    sysex_file_transfer.Init(RefreshMediaLibrary, CompleteMidiUpload, nullptr);
    sysex_remote_control.Init(&app_state,
                              &media_library,
                              SyncLoopStateForRemote,
                              nullptr);

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

    uint32_t render_ms               = System::GetNow();
    uint32_t last_ui_activity_ms     = render_ms;
    bool     ui_dirty                = true;
    bool     last_transport_playing  = false;
    uint64_t next_midi_clock_sample  = 0;
    while(1)
    {
        const uint32_t now = System::GetNow();
        RawInputState  raw{};
        UiEvent        events[20];
        uint8_t        channel_activity[16]{};

        if(pending_midi_tx_flushes > 0)
        {
            pending_midi_tx_flushes = 0;
            FlushScheduledMidiOut();
            ServiceMidiOutputs();
        }

        // Always service MIDI from the main loop. SysEx upload start handling
        // can stop audio and touch FatFs, which is not safe from the audio callback.
        ServiceIncomingMidi();

        ui_input.ControlRateTick();
        ui_input.Sample(raw);
        if(app_state.encoder_direction == EncoderDirection::Reversed)
            raw.encoder_delta = -raw.encoder_delta;
        app_state.sync_external  = raw.sync_external;
        const size_t event_count = ui_events.Translate(raw, now, events, 20);
        for(size_t i = 0; i < event_count; i++)
            ui_controller.HandleEvent(events[i], now, media_library);
        ui_renderer.SetDisplayXOffset(app_state.oled_x_offset);
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
            const bool load_ok = LoadSelectedMedia(
                app_state.pending_midi_load, app_state.pending_sf2_load, now);
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
                       SaveSelectedSongConfig() ? "Song Saved" : "Save Failed",
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
        effective_state.active_voices
            = static_cast<uint8_t>(SynthActiveVoiceCount());
        bool external_midi_step_advanced = false;
        uint64_t external_midi_tick      = 0;
        const bool gate_sync_enabled
            = AnyGateInputSyncEnabled(app_state.cv_gate);
        // A gate wired as Sync In is authoritative over the free-running MIDI
        // clock estimate: its pulses drive playback directly instead of
        // merely informing the BPM display.
        ClockSync& active_external_clock
            = gate_sync_enabled ? gate_clock_sync : midi_clock_sync;
        // Gate sync has no Start/Stop message of its own (unlike incoming
        // MIDI transport, handled in ServiceIncomingMidi), so treat the
        // module's own Play toggle as that edge: rewind to tick 0 and drop
        // any steps pulses queued up while playback was stopped, instead of
        // resuming mid-song from wherever the free-running pulse count had
        // drifted to.
        const bool sync_play_active
            = app_state.sync_external && app_state.transport_playing;
        if(sync_play_active != last_sync_play_active)
        {
            external_midi_step_count = 0;
            last_external_midi_tick  = 0;
            external_start_armed     = sync_play_active;
            active_external_clock.DiscardPendingExternalSteps();
        }
        last_sync_play_active = sync_play_active;
        if(app_state.sync_external)
        {
            const uint64_t ticks_per_step
                = smf_player.Divisions() > 0 ? (smf_player.Divisions() / 4u) : 120u;
            while(active_external_clock.ConsumeExternalStep())
            {
                external_midi_step_count++;
                external_midi_step_advanced = true;
            }
            const uint64_t absolute_tick = external_midi_step_count * ticks_per_step;
            if(app_state.song_loop_enabled && app_state.loop_length_ticks > 0)
            {
                const uint64_t loop_start_tick
                    = static_cast<uint64_t>(app_state.loop_start_tick);
                const uint64_t loop_length_tick
                    = static_cast<uint64_t>(app_state.loop_length_ticks);
                external_midi_tick
                    = loop_start_tick + (absolute_tick % loop_length_tick);
            }
            else
            {
                external_midi_tick = absolute_tick;
            }
        }
        if(app_state.sync_external)
        {
            const float active_bpm = active_external_clock.GetBpmEstimate();
            if(active_external_clock.IsLocked() && active_bpm > 0.0f)
            {
                effective_state.bpm = TempoUsecToBpm(
                    static_cast<uint32_t>(60000000.0f / active_bpm));
                effective_state.sync_locked = true;
            }
            else
            {
                effective_state.sync_locked = false;
            }

            if(!effective_state.sync_locked && !transport.IsPlaying()
               && !external_start_armed)
                effective_state.transport_playing = false;
        }
        else
        {
            effective_state.sync_locked = false;
        }

        if(audio_started)
            transport.Update(effective_state);

        if(audio_started && app_state.sync_external
           && effective_state.transport_playing
           && external_midi_step_advanced)
        {
            const bool external_tick_wrapped
                = external_midi_tick < last_external_midi_tick;
            transport.SyncExternalTick(external_midi_tick, external_tick_wrapped);
            last_external_midi_tick = external_midi_tick;
            external_start_armed    = false;
        }
        else if(!app_state.sync_external || !effective_state.transport_playing)
        {
            last_external_midi_tick = 0;
            if(!app_state.sync_external)
                external_start_armed = false;
        }

        FlushScheduledMidiOut();
        ServiceMidiOutputs();

        if(audio_started && effective_state.transport_playing
           && !transport.IsPlaying())
        {
            app_state.transport_playing       = false;
            effective_state.transport_playing = false;
        }

        const bool internal_transport_master = !effective_state.sync_external;
        const bool transport_started         = effective_state.transport_playing
                                               && !last_transport_playing
                                               && internal_transport_master;
        const bool transport_stopped = !effective_state.transport_playing
                                       && last_transport_playing
                                       && internal_transport_master;

        if(transport_started)
        {
            const uint8_t start = 0xFA;
            SendToConfiguredOutputs(MidiOutputKind::Transport, &start, 1);
            next_midi_clock_sample = transport.SampleClock();
        }
        else if(transport_stopped)
        {
            SendAllNotesAndSoundOffToConfiguredOutputs();
            const uint8_t stop = 0xFC;
            SendToConfiguredOutputs(MidiOutputKind::Transport, &stop, 1);
        }

        if(audio_started && effective_state.transport_playing
           && internal_transport_master
           && (app_state.midi_routing.usb.clock
               || app_state.midi_routing.uart.clock))
        {
            const float  bpm = effective_state.bpm > 0
                                   ? static_cast<float>(effective_state.bpm)
                                   : 120.0f;
            const double samples_per_clock
                = (hw.AudioSampleRate() * 60.0)
                  / (static_cast<double>(bpm) * 24.0);
            const uint64_t current_sample = transport.SampleClock();
            if(next_midi_clock_sample == 0)
                next_midi_clock_sample = current_sample;
            while(current_sample >= next_midi_clock_sample)
            {
                const uint8_t clock = 0xF8;
                SendToConfiguredOutputs(MidiOutputKind::Clock, &clock, 1);
                next_midi_clock_sample += static_cast<uint64_t>(
                    samples_per_clock > 1.0 ? samples_per_clock : 1.0);
            }
        }
        else if(!effective_state.transport_playing)
        {
            next_midi_clock_sample = 0;
        }

        last_transport_playing = effective_state.transport_playing;

        effective_state.current_song_tick
            = effective_state.transport_playing
                  ? static_cast<uint32_t>(transport.CurrentSongTick())
                  : 0u;

        effective_state.current_measure
            = effective_state.transport_playing
                  ? TickToMeasure(transport.CurrentSongTick(), smf_player)
                  : 1;
        effective_state.current_beat
            = effective_state.transport_playing
                  ? TickToBeat(transport.CurrentSongTick(), smf_player)
                  : 1;
        effective_state.song_divisions = smf_player.Divisions() > 0
                                             ? smf_player.Divisions()
                                             : effective_state.song_divisions;
        effective_state.time_sig_num   = smf_player.TimeSigNumerator() > 0
                                             ? smf_player.TimeSigNumerator()
                                             : 4;
        effective_state.time_sig_den   = smf_player.TimeSigDenominator() > 0
                                             ? smf_player.TimeSigDenominator()
                                             : 4;
        effective_state.song_total_measures
            = TickToMeasure(smf_player.TotalTicks(), smf_player);
        SyncLoopDisplayFieldsFromTicks(effective_state, smf_player);
        for(uint8_t ch = 0; ch < 16; ch++)
            app_state.channels[ch].current_program
                = transport.ChannelProgram(ch);

        transport.ConsumeChannelActivity(channel_activity);
        for(size_t ch = 0; ch < 16; ch++)
        {
            if(channel_activity[ch] != 0)
            {
                channel_flash_until[ch]             = now + kLedFlashMs;
                channel_monitor_until[ch]           = now + kMonitorFlashMs;
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
        const bool ui_active = overlay_active
                               || (now - last_ui_activity_ms) < kUiActiveHoldMs
                               || app_state.ui_mode != UiMode::Performance;
        const bool screen_saver_active
            = app_state.ui_mode == UiMode::Performance && !overlay_active
              && app_state.screen_saver_timeout_s > 0
              && (now - last_ui_activity_ms)
                     >= (static_cast<uint32_t>(app_state.screen_saver_timeout_s)
                         * 1000u);
        const uint32_t render_interval_ms
            = app_state.transport_playing
                  ? (ui_active ? kRenderIntervalUiActiveMs
                               : kRenderIntervalPlayingMs)
                  : kRenderIntervalStoppedMs;
        const bool midi_output_idle
            = pending_midi_tx_flushes == 0 && ScheduledMidiOutQueueEmpty();
        const bool ui_edit_settled
            = !app_state.transport_playing || overlay_active
              || (now - last_ui_activity_ms) >= kPlaybackUiRenderHoldMs;

        const bool periodic_render_due
            = (now - render_ms >= render_interval_ms);
        const bool redraw_due
            = (ui_dirty && ui_edit_settled) || periodic_render_due;
        const bool chunked_playback_render = app_state.transport_playing;
        const bool allow_render_now
            = periodic_render_due ? true
                                  : (!app_state.transport_playing
                                     || midi_output_idle);

        if(allow_render_now && redraw_due)
        {
            FlushScheduledMidiOut();
            ServiceMidiOutputs();
            render_ms = now;
            if(screen_saver_active)
                ui_renderer.RenderScreenSaver(now, chunked_playback_render);
            else
                ui_renderer.Render(effective_state,
                                   media_library,
                                   now,
                                   chunked_playback_render);
            FlushScheduledMidiOut();
            ServiceMidiOutputs();
            ui_dirty = false;
        }

        if(app_state.transport_playing && midi_output_idle
           && ui_renderer.IsChunkedUpdateActive())
        {
            ui_renderer.ServiceChunkedUpdate(16);
        }

        ServiceMidiOutputs();
        hw.SetLed(((now / 250) % 2) != 0);
    }
}
