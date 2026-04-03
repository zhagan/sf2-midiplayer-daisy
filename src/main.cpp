#include <cstdio>
#include <cstring>

#include "app_state.h"
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
AppState          app_state;
MidiUsbHandler    usb_midi;
MidiUartHandler   uart_midi;
bool              audio_started = false;
uint32_t          channel_flash_until[16]{};

constexpr uint32_t kLedFlashMs = 90;

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

    auto& settings         = smf_player.MutableSettings();
    settings.bpm_override  = app_state.song_bpm_override;
    settings.loop_enabled  = app_state.song_loop_enabled;
    settings.master_volume_max = app_state.sf2_master_volume_max;
    settings.expression_max    = app_state.sf2_expression_max;
    settings.reverb_max        = app_state.sf2_reverb_max;
    settings.chorus_max        = app_state.sf2_chorus_max;
    settings.transpose         = app_state.sf2_transpose;
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

void InitDefaultState()
{
    app_state = AppState{};
    for(int ch = 0; ch < 16; ch++)
    {
        app_state.channels[ch].volume      = 100;
        app_state.channels[ch].pan         = 64;
        app_state.channels[ch].reverb_send = 0;
        app_state.channels[ch].chorus_send = 0;
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
            transport.ProcessAudio(in, out, size);
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
                && SynthLoadSf2(sf2_path, hw.AudioSampleRate(), 32);
        if(sf_ok)
            SyncFxStateFromSynth();
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
    app_state.transport_playing = (sf_ok && midi_ok) ? was_playing : false;
    return sf_ok && midi_ok;
}
} // namespace

int main(void)
{
    hw.Init();
    hw.SetAudioBlockSize(48);
    if(kEnableUsbLog)
        hw.StartLog(false);

    InitDefaultState();
    ui_controller.Init(app_state);
    ui_input.Init(hw);
    ui_renderer.Init();

    const bool sd_ok = SdMount();
    LOG("SD mount: %s", sd_ok ? "PASS" : "FAIL");

    media_library.Scan();

    SynthInit();
    smf_player.SetSampleRate(hw.AudioSampleRate());
    smf_player.SetLookaheadSamples(hw.AudioBlockSize() * 256);
    smf_player.SetTempoScale(1.0f);
    transport.Init(hw.AudioSampleRate(), smf_player);

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

    uint32_t render_ms = System::GetNow();
    while(1)
    {
        const uint32_t now = System::GetNow();
        RawInputState  raw{};
        UiEvent        events[20];
        uint8_t        channel_activity[16]{};

        ui_input.Sample(raw);
        const size_t event_count = ui_events.Translate(raw, now, events, 20);
        for(size_t i = 0; i < event_count; i++)
            ui_controller.HandleEvent(events[i], now, media_library);

        ApplyAppSettings();

        if(app_state.pending_sf2_load || app_state.pending_midi_load)
            LoadSelectedMedia(app_state.pending_midi_load, app_state.pending_sf2_load, now);

        if(app_state.pending_save_settings)
        {
            app_state.pending_save_settings = false;
            SetOverlay(app_state,
                       smf_player.SaveSettings() ? "MIDI Saved" : "Save Failed",
                       now);
        }

        usb_midi.Listen();
        while(usb_midi.HasEvents())
            transport.HandleMidiMessage(usb_midi.PopEvent(), app_state);

        uart_midi.Listen();
        while(uart_midi.HasEvents())
            transport.HandleMidiMessage(uart_midi.PopEvent(), app_state);

        if(audio_started)
            transport.Update(app_state);

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

        const uint32_t render_interval_ms = app_state.transport_playing ? 75u : 50u;
        if(now - render_ms >= render_interval_ms)
        {
            render_ms = now;
            ui_renderer.Render(app_state, media_library, now);
        }

        hw.SetLed(((now / 250) % 2) != 0);
    }
}
