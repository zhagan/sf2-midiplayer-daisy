#include <cstdio>
#include <cstring>

#include "app_state.h"
#include "clock_sync.h"
#include "cv_gate_engine.h"
#include "cv_gate_persist.h"
#include "daisy_patch_sm.h"
#include "hid/midi.h"
#include "media_library.h"
#include "mixer_transport.h"
#include "sd_mount.h"
#include "smf_player.h"
#include "synth_tsf.h"
#include "ui_controller.h"
#include "ui_input.h"
#include "ui_renderer.h"

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
bool              audio_started = false;
uint8_t           applied_sf2_max_voices = 0;
uint32_t          channel_flash_until[16]{};
volatile uint64_t sync_sample_counter      = 0;
volatile uint32_t pending_midi_clock_edges = 0;

constexpr uint32_t kLedFlashMs = 90;
constexpr uint32_t kRenderIntervalStoppedMs    = 100;
constexpr uint32_t kRenderIntervalPlayingMs    = 250;
constexpr uint32_t kRenderIntervalUiActiveMs   = 100;
constexpr uint32_t kUiActiveHoldMs             = 1200;
const char*        kCvGateConfigPath           = "0:/major_midi_cv_gate.bin";

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
        settings.program_override[ch] = app_state.channels[ch].program_override;
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

void InitDefaultState()
{
    app_state = AppState{};
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
                if(pending_midi_clock_edges > 0)
                {
                    pending_midi_clock_edges--;
                    midi_edge = true;
                }
                midi_clock_sync.ProcessSample(midi_edge, sample_time);
                gate_clock_sync.ProcessSample(hw.gate_in_1.State(), sample_time);
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

    if(reload_midi)
        media_library.BuildMidiPath(app_state.selected_midi_index, midi_path, sizeof(midi_path));
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
        smf_player.Close();
        midi_ok = midi_path[0] != '\0' && smf_player.Open(midi_path);
        if(midi_ok)
        {
            app_state.bpm = TempoUsecToBpm(smf_player.TempoUsecPerQuarter());
            transport.SetFileBpm(static_cast<float>(app_state.bpm));
            SyncSongStateFromPlayer();
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
    const auto midi_settings = smf_player.Settings();

    media_library.BuildMidiPath(app_state.selected_midi_index, midi_path, sizeof(midi_path));
    media_library.BuildSoundFontPath(app_state.selected_sf2_index, sf2_path, sizeof(sf2_path));

    app_state.transport_playing = false;
    StopAudioIfRunning();
    transport.Reset(app_state);
    SynthUnloadSf2();
    smf_player.Close();

    bool midi_ok = true;
    if(midi_path[0] != '\0')
        midi_ok = major_midi::WriteMajorMidiMetaEvent(midi_path, midi_settings);

    const bool cv_ok = SaveCvGateConfig(kCvGateConfigPath, app_state.cv_gate);

    const bool reload_ok = LoadSelectedMedia(midi_path[0] != '\0', sf2_path[0] != '\0', now_ms);
    if(midi_ok && cv_ok && reload_ok)
    {
        app_state.ui_mode          = UiMode::Performance;
        app_state.menu_page        = MenuPage::Main;
        app_state.menu_page_cursor = 0;
        app_state.menu_root_cursor = 0;
        app_state.menu_editing     = false;
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
        LoadCvGateConfig(kCvGateConfigPath, app_state.cv_gate);
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

    uint32_t render_ms          = System::GetNow();
    uint32_t last_ui_activity_ms = render_ms;
    bool     ui_dirty            = true;
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
        if(app_state.sync_external)
        {
            const float midi_bpm = midi_clock_sync.GetBpmEstimate();
            const float gate_bpm = gate_clock_sync.GetBpmEstimate();
            if(midi_clock_sync.IsLocked() && midi_bpm > 0.0f)
            {
                effective_state.bpm = TempoUsecToBpm(static_cast<uint32_t>(60000000.0f / midi_bpm));
                effective_state.sync_locked = true;
            }
            else if(gate_clock_sync.IsLocked() && gate_bpm > 0.0f)
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

        effective_state.current_measure
            = effective_state.transport_playing
                  ? TickToMeasure(transport.CurrentSongTick(), smf_player)
                  : 1;
        for(uint8_t ch = 0; ch < 16; ch++)
            app_state.channels[ch].current_program = transport.ChannelProgram(ch);

        transport.ConsumeChannelActivity(channel_activity);
        for(size_t ch = 0; ch < 16; ch++)
        {
            if(channel_activity[ch] != 0)
                channel_flash_until[ch] = now + kLedFlashMs;
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
