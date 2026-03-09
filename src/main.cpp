#include "daisy_patch_sm.h"
#include "daisysp.h"
#include "hid/midi.h"
#include "util/scopedirqblocker.h"
#include <cmath>
#include <cctype>
#include <cstring>
#include "synth_tsf.h"
#include "sd_mount.h"
#include "scheduler.h"
#include "clock_sync.h"
#include "smf_player.h"
#include "ui_oled.h"
#include "ff.h"

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;
using namespace major_midi;

static constexpr bool kEnableUsbLog  = true;
static constexpr bool kEnableUsbMidi = false;

#define LOG(...)                                  \
    do                                            \
    {                                             \
        if(kEnableUsbLog)                         \
            DaisyPatchSM::PrintLine(__VA_ARGS__); \
    } while(0)

static DaisyPatchSM      hw;
static char              g_midiPath[64] = "0:/midi/825.mid";
static volatile uint64_t sampleClock    = 0;
static EventQueue<2048>  g_queue;
static EventQueue<256>   g_immediate;
static MidiUsbHandler    g_usb;
static SmfPlayer         g_smfPlayer;
static ClockSync         g_clock;
static UiOled            g_ui;
static Switch            g_playButton;
static uint64_t          g_playStartSample = 0;
static uint64_t          g_playStartTicks  = 0;
static Switch            g_clockModeSwitch;
static dsy_gpio          g_clockIn;
static bool              g_midiRun              = true;
static float             g_sampleRate           = 48000.0f;
static volatile bool     g_useInternalClock     = true;
static float             g_tempoScale           = 1.0f;
static float             g_tempoScaleApplied    = 1.0f;
static int               g_transpose            = 0;
static int               g_gateCountdownSamples = 0;
static bool              g_gateHigh             = false;
static int               g_gatePulseSamples     = 0;
static volatile uint64_t g_transportSample      = 0;
static double            g_transportSampleF     = 0.0;
static volatile bool     g_externalStarted      = false;

static constexpr size_t kMaxMidiFiles = 64;
static constexpr size_t kMidiNameMax  = 32;
static char             g_midiFiles[kMaxMidiFiles][kMidiNameMax];
static size_t           g_midiFileCount   = 0;
static constexpr size_t kMaxSoundFonts    = 32;
static constexpr size_t kSoundFontNameMax = 32;
static char             g_soundFonts[kMaxSoundFonts][kSoundFontNameMax];
static size_t           g_soundFontCount = 0;

static void EnqueueImmediate(const MidiEv& ev);
static bool DequeueImmediate(MidiEv& ev);

static void     StartPlayback();
static void     StopPlayback();
static void     ApplyChannelStateFromUi();
static bool     SaveCurrentMidiSettings();
static uint64_t MbsToSamples(int measure, int beat, int sub);
static uint64_t MbsToTicks(int measure, int beat, int sub);
static bool     LoadMidiFileByName(const char* name);
static bool     LoadSoundFontByName(const char* name);
static void     ScanSoundFontDir();
static bool     g_audioRunning = false;
static void     ScanMidiDir();
static bool     HasExtCaseInsensitive(const char* name, const char* ext);
void            AudioCallback(AudioHandle::InputBuffer  in,
                              AudioHandle::OutputBuffer out,
                              size_t                    size);

class UiBackendImpl final : public UiBackend
{
  public:
    int  instrumentPage       = 1;
    char midiName[32]         = "825.mid";
    bool loop                 = false;
    int  loopStart            = 1;
    int  loopStartBeat        = 1;
    int  loopStartSub         = 1;
    int  loopLen              = 16;
    int  loopStartOffsetTicks = 0;
    int  loopLenOffsetTicks   = 0;

    uint8_t vol[16];
    uint8_t pan[16];
    uint8_t program[16];
    bool    activity[16];

    // SF state
    int     sfCh = 1;
    bool    sfMute[16];
    uint8_t sfVol[16];
    uint8_t sfPan[16];
    uint8_t sfRevSend[16];
    uint8_t sfChoSend[16];
    uint8_t velMod[16];
    uint8_t pitchMod[16];
    uint8_t modWheel[16];
    int8_t  pitchbend[16];

    // system
    bool     midiThru     = false;
    SyncMode sync         = SyncMode::Internal;
    Tri      midiSync     = Tri::Off;
    Quad     midiMsgs     = Quad::Both;
    Quad     midiProgMsgs = Quad::Both;

  private:
    static int clampi_(int v, int lo, int hi)
    {
        if(v < lo)
            return lo;
        if(v > hi)
            return hi;
        return v;
    }

  public:
    // FX
    float fxRevTime = 0.85f;
    float fxRevLp   = 8000.0f;
    float fxRevHp   = 80.0f;
    float fxChDepth = 0.35f;
    float fxChSpeed = 0.25f;

    UiBackendImpl()
    {
        for(int i = 0; i < 16; i++)
        {
            vol[i]      = 100;
            pan[i]      = 64;
            program[i]  = 0;
            activity[i] = false;

            sfMute[i]    = false;
            sfVol[i]     = 100;
            sfPan[i]     = 64;
            sfRevSend[i] = 0;
            sfChoSend[i] = 0;
            velMod[i]    = 0;
            pitchMod[i]  = 0;
            modWheel[i]  = 0;
            pitchbend[i] = 0;
        }
    }

    void PulseActivity(int ch)
    {
        if(ch >= 0 && ch < 16)
            activity[ch] = true;
    }

    int  GetInstrumentPage() const override { return instrumentPage; }
    void SetInstrumentPage(int p) override
    {
        instrumentPage = (p < 1 ? 1 : (p > 4 ? 4 : p));
    }

    const char* GetLoadedMidiName() const override { return midiName; }

    void GetPlayheadText(char* out, size_t out_sz) const override
    {
        int          bpm = 0;
        const double spq = g_smfPlayer.SamplesPerQuarterF();
        if(spq > 0.0)
            bpm = (int)lround((g_sampleRate * 60.0) / spq);
        snprintf(out,
                 out_sz,
                 "%s %3d BPM",
                 g_smfPlayer.IsPlaying() ? "PLAY" : "STOP",
                 bpm);
    }

    int GetChannelIndexForMainRow(int page_1to4, int row_0to3) const override
    {
        return (page_1to4 - 1) * 4 + row_0to3;
    }

    uint8_t GetChanVolume(int ch) const override { return vol[ch]; }
    void    SetChanVolume(int ch, uint8_t v) override
    {
        vol[ch] = v;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)ch;
        ev.a    = 7;
        ev.b    = v;
        EnqueueImmediate(ev);
    }

    uint8_t GetChanPan(int ch) const override { return pan[ch]; }
    void    SetChanPan(int ch, uint8_t p) override
    {
        pan[ch] = p;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)ch;
        ev.a    = 10;
        ev.b    = p;
        EnqueueImmediate(ev);
    }

    uint8_t GetChanProgram(int ch) const override { return program[ch]; }
    void    SetChanProgram(int ch, uint8_t p) override
    {
        program[ch] = p;
        MidiEv ev{};
        ev.type = EvType::Program;
        ev.ch   = (uint8_t)ch;
        ev.a    = p;
        EnqueueImmediate(ev);
    }

    const char* GetChanTrackName(int ch) const override
    {
        return g_smfPlayer.GetTrackNameForChannel((uint8_t)ch);
    }

    bool ConsumeChanActivityPulse(int ch) override
    {
        bool v       = activity[ch];
        activity[ch] = false;
        return v;
    }

    SyncMode GetSyncMode() const override
    {
        return g_useInternalClock ? SyncMode::Internal : SyncMode::External;
    }

    bool GetMidiThru() const override { return midiThru; }
    void SetMidiThru(bool on) override { midiThru = on; }

    Tri  GetMidiSync() const override { return midiSync; }
    void SetMidiSync(Tri v) override { midiSync = v; }

    Quad GetMidiMessages() const override { return midiMsgs; }
    void SetMidiMessages(Quad v) override { midiMsgs = v; }

    Quad GetMidiProgramMessages() const override { return midiProgMsgs; }
    void SetMidiProgramMessages(Quad v) override { midiProgMsgs = v; }

    int GetBpm() const override
    {
        const double spq = g_smfPlayer.SamplesPerQuarterF();
        if(spq <= 0.0)
            return 120;
        return (int)lround((g_sampleRate * 60.0) / spq);
    }

    void SetBpm(int b) override
    {
        if(b < 20)
            b = 20;
        if(b > 300)
            b = 300;
        const double spq = g_smfPlayer.SamplesPerQuarterF();
        if(spq > 0.0)
        {
            const double cur_bpm = (g_sampleRate * 60.0) / spq;
            if(cur_bpm > 0.0)
                g_tempoScale = (float)((double)g_tempoScale * (b / cur_bpm));
        }
        if(g_tempoScale < 0.25f)
            g_tempoScale = 0.25f;
        if(g_tempoScale > 3.0f)
            g_tempoScale = 3.0f;
        g_clock.SetInternalBpm((float)b);
    }

    bool GetPlay() const override { return g_smfPlayer.IsPlaying(); }
    void SetPlay(bool on) override
    {
        if(on && !g_smfPlayer.IsPlaying())
            StartPlayback();
        else if(!on && g_smfPlayer.IsPlaying())
            StopPlayback();
    }

    bool GetLoop() const override { return loop; }
    void SetLoop(bool on) override { loop = on; }

    int  GetLoopStartMeasure() const override { return loopStart; }
    void SetLoopStartMeasure(int m) override
    {
        loopStart     = (m < 1) ? 1 : m;
        loopStartBeat = 1;
        loopStartSub  = 1;
    }

    int  GetLoopLengthBeats() const override { return loopLen; }
    void SetLoopLengthBeats(int beats) override
    {
        loopLen = (beats < 1) ? 1 : beats;
    }
    int GetLoopStartOffsetTicks() const override
    {
        return loopStartOffsetTicks;
    }
    void SetLoopStartOffsetTicks(int ticks) override
    {
        loopStartOffsetTicks = clampi_(ticks, -999, 999);
    }
    int GetLoopLengthOffsetTicks() const override { return loopLenOffsetTicks; }
    void SetLoopLengthOffsetTicks(int ticks) override
    {
        loopLenOffsetTicks = clampi_(ticks, -999, 999);
    }

    void GetLoopStartMbs(int* measure, int* beat, int* sub) const override
    {
        if(measure)
            *measure = loopStart;
        if(beat)
            *beat = loopStartBeat;
        if(sub)
            *sub = loopStartSub;
    }

    void SetLoopStartMbs(int measure, int beat, int sub) override
    {
        const int ts_num            = g_smfPlayer.TimeSigNumerator();
        const int ts_den            = g_smfPlayer.TimeSigDenominator();
        const int beats_per_measure = (ts_num > 0 ? ts_num : 4);
        const int sub_per_beat      = (ts_den > 0 ? (16 / ts_den) : 4);
        loopStart                   = (measure < 1) ? 1 : measure;
        loopStartBeat
            = (beat < 1)
                  ? 1
                  : (beat > beats_per_measure ? beats_per_measure : beat);
        loopStartSub
            = (sub < 1) ? 1 : (sub > sub_per_beat ? sub_per_beat : sub);
    }

    void GetPlayheadMbs(int* measure, int* beat, int* sub) const override
    {
        if(!g_smfPlayer.IsPlaying())
        {
            if(measure)
                *measure = 1;
            if(beat)
                *beat = 1;
            if(sub)
                *sub = 1;
            return;
        }
        const uint64_t now
            = g_useInternalClock ? sampleClock : g_transportSample;
        uint64_t delta
            = (now >= g_playStartSample) ? (now - g_playStartSample) : 0;
        const uint16_t divisions = g_smfPlayer.Divisions();
        if(divisions == 0)
        {
            if(measure)
                *measure = 1;
            if(beat)
                *beat = 1;
            if(sub)
                *sub = 1;
            return;
        }
        const double spq = g_smfPlayer.SamplesPerQuarterF();
        if(spq <= 0.0)
        {
            if(measure)
                *measure = 1;
            if(beat)
                *beat = 1;
            if(sub)
                *sub = 1;
            return;
        }
        const double   samples_per_tick = spq / (double)divisions;
        const uint64_t ticks
            = (uint64_t)(delta / samples_per_tick) + g_playStartTicks;
        const int ts_num            = g_smfPlayer.TimeSigNumerator();
        const int ts_den            = g_smfPlayer.TimeSigDenominator();
        const int beats_per_measure = (ts_num > 0 ? ts_num : 4);
        const int ticks_per_beat = (divisions * 4) / (ts_den > 0 ? ts_den : 4);
        const int ticks_per_16th = (divisions / 4) ? (divisions / 4) : 1;
        const int ticks_per_measure = ticks_per_beat * beats_per_measure;
        const uint64_t meas
            = (ticks_per_measure > 0) ? (ticks / ticks_per_measure) : 0;
        const uint64_t rem
            = (ticks_per_measure > 0) ? (ticks % ticks_per_measure) : 0;
        const uint64_t beat_idx
            = (ticks_per_beat > 0) ? (rem / ticks_per_beat) : 0;
        const uint64_t rem2 = (ticks_per_beat > 0) ? (rem % ticks_per_beat) : 0;
        const uint64_t sub_idx
            = (ticks_per_16th > 0) ? (rem2 / ticks_per_16th) : 0;
        if(measure)
            *measure = (int)meas + 1;
        if(beat)
            *beat = (int)beat_idx + 1;
        if(sub)
            *sub = (int)sub_idx + 1;
    }

    int GetMidiFileCount() const override { return (int)g_midiFileCount; }
    const char* GetMidiFileName(int idx) const override
    {
        if(idx < 0 || idx >= (int)g_midiFileCount)
            return "";
        return g_midiFiles[idx];
    }
    void LoadMidiFileByIndex(int idx) override
    {
        if(idx < 0 || idx >= (int)g_midiFileCount)
            return;
        LoadMidiFileByName(g_midiFiles[idx]);
    }
    bool SaveMidiSettings() override { return SaveCurrentMidiSettings(); }

    int  GetSfChannel() const override { return sfCh; }
    void SetSfChannel(int ch) override
    {
        sfCh = (ch < 1 ? 1 : (ch > 16 ? 16 : ch));
    }

    const char* GetSfTrackName(int /*ch_1to16*/) const override
    {
        return "Track";
    }


    bool GetSfMute(int ch) const override { return sfMute[ch - 1]; }
    void SetSfMute(int ch, bool on) override { sfMute[ch - 1] = on; }

    uint8_t GetSfVolume(int ch) const override { return sfVol[ch - 1]; }
    void    SetSfVolume(int ch, uint8_t v) override
    {
        sfVol[ch - 1] = v;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)(ch - 1);
        ev.a    = 7;
        ev.b    = v;
        EnqueueImmediate(ev);
    }

    uint8_t GetSfPan(int ch) const override { return sfPan[ch - 1]; }
    void    SetSfPan(int ch, uint8_t p) override
    {
        sfPan[ch - 1] = p;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)(ch - 1);
        ev.a    = 10;
        ev.b    = p;
        EnqueueImmediate(ev);
    }

    uint8_t GetSfReverbSend(int ch) const override { return sfRevSend[ch - 1]; }
    void    SetSfReverbSend(int ch, uint8_t v) override
    {
        sfRevSend[ch - 1] = v;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)(ch - 1);
        ev.a    = 91;
        ev.b    = v;
        EnqueueImmediate(ev);
    }

    uint8_t GetSfChorusSend(int ch) const override { return sfChoSend[ch - 1]; }
    void    SetSfChorusSend(int ch, uint8_t v) override
    {
        sfChoSend[ch - 1] = v;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)(ch - 1);
        ev.a    = 93;
        ev.b    = v;
        EnqueueImmediate(ev);
    }

    uint8_t GetSfVelocityMod(int ch) const override { return velMod[ch - 1]; }
    void    SetSfVelocityMod(int ch, uint8_t v) override { velMod[ch - 1] = v; }

    uint8_t GetSfPitchMod(int ch) const override { return pitchMod[ch - 1]; }
    void    SetSfPitchMod(int ch, uint8_t v) override { pitchMod[ch - 1] = v; }

    uint8_t GetSfModWheel(int ch) const override { return modWheel[ch - 1]; }
    void    SetSfModWheel(int ch, uint8_t v) override
    {
        modWheel[ch - 1] = v;
        MidiEv ev{};
        ev.type = EvType::ControlChange;
        ev.ch   = (uint8_t)(ch - 1);
        ev.a    = 1;
        ev.b    = v;
        EnqueueImmediate(ev);
    }

    int8_t GetSfPitchbend(int ch) const override { return pitchbend[ch - 1]; }
    void   SetSfPitchbend(int ch, int8_t v) override
    {
        pitchbend[ch - 1]   = v;
        const uint16_t bend = (uint16_t)(v + 64) << 7;
        MidiEv         ev{};
        ev.type = EvType::PitchBend;
        ev.ch   = (uint8_t)(ch - 1);
        ev.a    = bend & 0x7F;
        ev.b    = (bend >> 7) & 0x7F;
        EnqueueImmediate(ev);
    }

    int GetSoundFontCount() const override { return (int)g_soundFontCount; }
    const char* GetSoundFontName(int idx) const override
    {
        if(idx < 0 || idx >= (int)g_soundFontCount)
            return "";
        return g_soundFonts[idx];
    }
    void LoadSoundFontByIndex(int idx) override
    {
        if(idx < 0 || idx >= (int)g_soundFontCount)
            return;
        LoadSoundFontByName(g_soundFonts[idx]);
    }

    float GetFxReverbTime() const override { return fxRevTime; }
    void  SetFxReverbTime(float t01) override
    {
        fxRevTime = t01;
        SynthSetReverbTime(t01);
    }

    float GetFxReverbLpFreq() const override { return fxRevLp; }
    void  SetFxReverbLpFreq(float hz) override
    {
        fxRevLp = hz;
        SynthSetReverbLpFreq(hz);
    }

    float GetFxReverbHpFreq() const override { return fxRevHp; }
    void  SetFxReverbHpFreq(float hz) override
    {
        fxRevHp = hz;
        SynthSetReverbHpFreq(hz);
    }

    float GetFxChorusDepth() const override { return fxChDepth; }
    void  SetFxChorusDepth(float d01) override
    {
        fxChDepth = d01;
        SynthSetChorusDepth(d01);
    }

    float GetFxChorusSpeed() const override { return fxChSpeed; }
    void  SetFxChorusSpeed(float hz) override
    {
        fxChSpeed = hz;
        SynthSetChorusSpeed(hz);
    }
};

static UiBackendImpl g_uiBackend;

static uint8_t ApplyTranspose(uint8_t ch, uint8_t note)
{
    if(ch == 9)
        return note;
    int n = int(note) + g_transpose;
    if(n < 0)
        n = 0;
    else if(n > 127)
        n = 127;
    return (uint8_t)n;
}

static int GetMajorMidiSongTranspose()
{
    return (int)g_smfPlayer.Settings().transpose;
}

static uint8_t ApplyPlaybackTranspose(uint8_t ch, uint8_t note)
{
    if(ch == 9)
        return note;
    int n = int(note) + g_transpose + GetMajorMidiSongTranspose();
    if(n < 0)
        n = 0;
    else if(n > 127)
        n = 127;
    return (uint8_t)n;
}

static void SyncUiFromMajorMidiSettings()
{
    const auto& settings = g_smfPlayer.Settings();
    g_uiBackend.loop      = settings.loop_enabled;
    g_uiBackend.loopStart = settings.loop_start_measure;
    g_uiBackend.loopStartBeat
        = settings.loop_start_beat < 1 ? 1 : settings.loop_start_beat;
    g_uiBackend.loopStartSub
        = settings.loop_start_sub < 1 ? 1 : settings.loop_start_sub;
    g_uiBackend.loopLen
        = settings.loop_length_beats < 1 ? 1 : settings.loop_length_beats;
    for(uint8_t ch = 0; ch < major_midi::kChannelCount; ch++)
    {
        if(major_midi::HasMajorMidiProgramOverride(ch, settings))
            g_uiBackend.program[ch]
                = major_midi::ResolveMajorMidiProgram(ch, g_uiBackend.program[ch], settings);
        if(major_midi::HasMajorMidiPanOverride(ch, settings))
        {
            const uint8_t pan
                = major_midi::ResolveMajorMidiPan(ch, g_uiBackend.pan[ch], settings);
            g_uiBackend.pan[ch]   = pan;
            g_uiBackend.sfPan[ch] = pan;
        }
    }
}

static bool SaveCurrentMidiSettings()
{
    auto& settings        = g_smfPlayer.MutableSettings();
    settings.bpm_override = (uint16_t)g_uiBackend.GetBpm();
    settings.loop_enabled = g_uiBackend.GetLoop();

    int lm = 1, lb = 1, ls = 1;
    g_uiBackend.GetLoopStartMbs(&lm, &lb, &ls);
    if(lm < 1)
        lm = 1;
    if(lb < 1)
        lb = 1;
    if(ls < 1)
        ls = 1;
    settings.loop_start_measure = (uint16_t)lm;
    settings.loop_start_beat    = (uint8_t)lb;
    settings.loop_start_sub     = (uint8_t)ls;

    int loop_len = g_uiBackend.GetLoopLengthBeats();
    if(loop_len < 1)
        loop_len = 1;
    settings.loop_length_beats = (uint16_t)loop_len;
    return g_smfPlayer.SaveSettings();
}

static void ApplyMajorMidiPlaybackSettings(MidiEv& ev)
{
    const auto& settings = g_smfPlayer.Settings();
    switch(ev.type)
    {
        case EvType::Program:
            ev.a = major_midi::ResolveMajorMidiProgram(ev.ch, ev.a, settings);
            break;
        case EvType::ControlChange:
            switch(ev.a)
            {
                case 7:
                    ev.b = major_midi::ScaleMajorMidiController(
                        ev.b, settings.master_volume_max);
                    break;
                case 10:
                    ev.b = major_midi::ResolveMajorMidiPan(ev.ch, ev.b, settings);
                    break;
                case 11:
                    ev.b = major_midi::ScaleMajorMidiController(
                        ev.b, settings.expression_max);
                    break;
                case 91:
                    ev.b = major_midi::ScaleMajorMidiController(
                        ev.b, settings.reverb_max);
                    break;
                case 93:
                    ev.b = major_midi::ScaleMajorMidiController(
                        ev.b, settings.chorus_max);
                    break;
                default: break;
            }
            break;
        default: break;
    }
}

static void EnqueueImmediate(const MidiEv& ev)
{
    daisy::ScopedIrqBlocker lock;
    g_immediate.Push(ev);
}

static bool DequeueImmediate(MidiEv& ev)
{
    daisy::ScopedIrqBlocker lock;
    return g_immediate.Pop(ev);
}

static void StopPlayback()
{
    g_smfPlayer.Stop();
    g_queue.Clear();
    g_immediate.Clear();
    SynthPanic();
    SynthResetChannels();
    g_gateCountdownSamples = 0;
    g_gateHigh             = false;
    dsy_gpio_write(&hw.gate_out_1, 0);
    g_externalStarted = false;
    g_playStartSample = 0;
    g_playStartTicks  = 0;
}

static void ApplyChannelStateFromUi()
{
    SynthResetChannels();
    const auto& settings = g_smfPlayer.Settings();
    for(int ch = 0; ch < 16; ch++)
    {
        const uint8_t program = major_midi::ResolveMajorMidiProgram(
            (uint8_t)ch, g_uiBackend.GetChanProgram(ch), settings);
        const uint8_t pan
            = major_midi::ResolveMajorMidiPan((uint8_t)ch, g_uiBackend.GetChanPan(ch), settings);
        SynthProgramChange((uint8_t)ch, program);
        SynthControlChange((uint8_t)ch, 7, g_uiBackend.GetChanVolume(ch));
        SynthControlChange((uint8_t)ch, 10, pan);
    }
}

static void StartPlayback()
{
    const bool useInternalClock = g_clockModeSwitch.Pressed();
    uint64_t   startClock       = 0;
    if(useInternalClock)
    {
        startClock = sampleClock;
    }
    else
    {
        daisy::ScopedIrqBlocker lock;
        g_transportSample  = 0;
        g_transportSampleF = 0.0;
        g_externalStarted  = false;
        startClock         = g_transportSample;
    }
    g_queue.Clear();
    g_immediate.Clear();
    SynthPanic();
    SynthResetChannels();
    if(g_uiBackend.GetLoop())
    {
        int lm = 1, lb = 1, ls = 1;
        g_uiBackend.GetLoopStartMbs(&lm, &lb, &ls);
        int64_t loop_start_ticks = (int64_t)MbsToTicks(lm, lb, ls);
        // Offsets disabled for now; keep for future tuning.
        // loop_start_ticks += (int64_t)g_uiBackend.GetLoopStartOffsetTicks();
        if(loop_start_ticks < 0)
            loop_start_ticks = 0;
        uint64_t loop_start
            = g_smfPlayer.SamplesFromTicks((uint64_t)loop_start_ticks);
        if(loop_start > 0)
            loop_start -= 1;
        g_smfPlayer.SeekToSample(loop_start, startClock);
        g_playStartSample = startClock;
        g_playStartTicks  = (uint64_t)loop_start_ticks;
        // Ensure program/CC state is applied at loop start
        ApplyChannelStateFromUi();
    }
    else
    {
        g_smfPlayer.Start(startClock);
        g_playStartSample = startClock;
        g_playStartTicks  = 0;
        ApplyChannelStateFromUi();
    }
}

static uint64_t MbsToTicks(int measure, int beat, int sub)
{
    const uint16_t divisions = g_smfPlayer.Divisions();
    if(divisions == 0)
        return 0;

    const int ts_num            = g_smfPlayer.TimeSigNumerator();
    const int ts_den            = g_smfPlayer.TimeSigDenominator();
    const int beats_per_measure = (ts_num > 0 ? ts_num : 4);
    const int ticks_per_beat    = (divisions * 4) / (ts_den > 0 ? ts_den : 4);
    const int ticks_per_16th    = (divisions / 4) ? (divisions / 4) : 1;

    const int m = (measure < 1) ? 1 : measure;
    const int b = (beat < 1) ? 1 : beat;
    const int s = (sub < 1) ? 1 : sub;

    const uint64_t ticks = (uint64_t)(m - 1) * (uint64_t)beats_per_measure
                               * (uint64_t)ticks_per_beat
                           + (uint64_t)(b - 1) * (uint64_t)ticks_per_beat
                           + (uint64_t)(s - 1) * (uint64_t)ticks_per_16th;

    return ticks;
}

static uint64_t MbsToSamples(int measure, int beat, int sub)
{
    const double   spq       = g_smfPlayer.SamplesPerQuarterF();
    const uint16_t divisions = g_smfPlayer.Divisions();
    if(spq <= 0.0 || divisions == 0)
        return 0;
    const uint64_t ticks            = MbsToTicks(measure, beat, sub);
    const double   samples_per_tick = spq / (double)divisions;
    return (uint64_t)llround((double)ticks * samples_per_tick);
}

static uint64_t BeatsToSamples(int beats)
{
    const double   spq       = g_smfPlayer.SamplesPerQuarterF();
    const uint16_t divisions = g_smfPlayer.Divisions();
    if(spq <= 0.0 || divisions == 0)
        return 0;
    const int    ts_den           = g_smfPlayer.TimeSigDenominator();
    const int    ticks_per_beat   = (divisions * 4) / (ts_den > 0 ? ts_den : 4);
    const double samples_per_tick = spq / (double)divisions;
    return (uint64_t)llround((double)beats * (double)ticks_per_beat
                             * samples_per_tick);
}

static bool HasExtCaseInsensitive(const char* name, const char* ext)
{
    if(!name || !ext)
        return false;
    const char* dot = std::strrchr(name, '.');
    if(!dot || dot[1] == '\0')
        return false;
    dot++;
    while(*dot && *ext)
    {
        if(std::tolower((unsigned char)*dot)
           != std::tolower((unsigned char)*ext))
            return false;
        dot++;
        ext++;
    }
    return *dot == '\0' && *ext == '\0';
}

static void ScanMidiDir()
{
    g_midiFileCount = 0;

    DIR     dir;
    FILINFO fno;
    if(f_opendir(&dir, "0:/midi") != FR_OK)
        return;

    while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0')
    {
        if(fno.fname[0] == '.' || (fno.fname[0] == '_' && fno.fname[1] == '.')
           || (fno.fname[0] == '.' && fno.fname[1] == '_'))
            continue;
        if(fno.fattrib & AM_DIR)
            continue;
        if(!HasExtCaseInsensitive(fno.fname, "mid"))
            continue;
        if(g_midiFileCount >= kMaxMidiFiles)
            break;

        std::strncpy(g_midiFiles[g_midiFileCount], fno.fname, kMidiNameMax - 1);
        g_midiFiles[g_midiFileCount][kMidiNameMax - 1] = '\0';
        g_midiFileCount++;
    }

    f_closedir(&dir);
}

static void ScanSoundFontDir()
{
    g_soundFontCount = 0;

    DIR     dir;
    FILINFO fno;
    if(f_opendir(&dir, "0:/soundfonts") != FR_OK)
        return;

    while(f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != '\0')
    {
        if(fno.fname[0] == '.' || (fno.fname[0] == '_' && fno.fname[1] == '.')
           || (fno.fname[0] == '.' && fno.fname[1] == '_'))
            continue;
        if(fno.fattrib & AM_DIR)
            continue;
        if(!HasExtCaseInsensitive(fno.fname, "sf2"))
            continue;
        if(g_soundFontCount >= kMaxSoundFonts)
            break;

        std::strncpy(
            g_soundFonts[g_soundFontCount], fno.fname, kSoundFontNameMax - 1);
        g_soundFonts[g_soundFontCount][kSoundFontNameMax - 1] = '\0';
        g_soundFontCount++;
    }

    f_closedir(&dir);
}

static bool LoadMidiFileByName(const char* name)
{
    if(!name)
        return false;
    StopPlayback();
    const int res
        = snprintf(g_midiPath, sizeof(g_midiPath), "0:/midi/%s", name);
    if(res <= 0 || res >= (int)sizeof(g_midiPath))
        return false;
    const bool ok = g_smfPlayer.Open(g_midiPath);
    if(ok)
    {
        g_tempoScale        = 1.0f;
        g_tempoScaleApplied = 0.0f;
        std::strncpy(
            g_uiBackend.midiName, name, sizeof(g_uiBackend.midiName) - 1);
        g_uiBackend.midiName[sizeof(g_uiBackend.midiName) - 1] = '\0';
        SyncUiFromMajorMidiSettings();
        g_ui.Invalidate();
    }
    return ok;
}

static bool LoadSoundFontByName(const char* name)
{
    if(!name)
        return false;
    char      path[64];
    const int res = snprintf(path, sizeof(path), "0:/soundfonts/%s", name);
    if(res <= 0 || res >= (int)sizeof(path))
        return false;
    LOG("SF2 load: %s", path);
    const bool wasRunning = g_audioRunning;
    if(wasRunning)
    {
        hw.StopAudio();
        g_audioRunning = false;
    }

    SynthUnloadSf2();
    const bool ok = SynthLoadSf2(path, g_sampleRate, 32);
    LOG("SF2 mem: used=%lu cap=%lu oom=%d",
        (unsigned long)SynthArenaUsed(),
        (unsigned long)SynthArenaCap(),
        SynthArenaOom() ? 1 : 0);
    if(wasRunning)
    {
        hw.StartAudio(AudioCallback);
        g_audioRunning = true;
    }
    LOG("SF2 load %s: %s", ok ? "OK" : "FAIL", path);
    return ok;
}

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    (void)in;

    const bool use_external = !g_useInternalClock;
    g_clock.SetUseExternalClock(use_external);
    const bool playing = g_smfPlayer.IsPlaying();

    MidiEv ev;
    while(DequeueImmediate(ev))
    {
        switch(ev.type)
        {
            case EvType::NoteOn:
                SynthNoteOn(ev.ch, ev.a, ev.b);
                g_uiBackend.PulseActivity(ev.ch);
                break;
            case EvType::NoteOff:
                SynthNoteOff(ev.ch, ev.a);
                g_uiBackend.PulseActivity(ev.ch);
                break;
            case EvType::Program: SynthProgramChange(ev.ch, ev.a); break;
            case EvType::ControlChange:
                SynthControlChange(ev.ch, ev.a, ev.b);
                break;
            case EvType::PitchBend:
            {
                const uint16_t bend = (uint16_t(ev.b) << 7) | ev.a;
                SynthPitchBend(ev.ch, bend);
            }
            break;
            case EvType::AllNotesOff: SynthPanic(); break;
        }
    }

    static float lbuf[256];
    static float rbuf[256];

    size_t offset = 0;
    while(offset < size)
    {
        size_t frames = size - offset;
        if(frames > 256)
            frames = 256;

        SynthRender(lbuf, rbuf, frames);

        for(size_t i = 0; i < frames; i++)
        {
            const uint64_t t         = sampleClock + offset + i;
            const bool     clockHigh = dsy_gpio_read(&g_clockIn);
            g_clock.ProcessSample(clockHigh, t);

            if(playing)
            {
                if(g_useInternalClock)
                {
                    while(g_clock.ConsumeStepTick())
                        g_gateCountdownSamples = g_gatePulseSamples;
                }
                else
                {
                    while(g_clock.ConsumeExternalStep())
                    {
                        g_gateCountdownSamples = g_gatePulseSamples;
                        double stepSamples
                            = (double)g_clock.GetLastMeasuredSp16();
                        if(stepSamples <= 0.0)
                            stepSamples = (double)g_clock.GetSamplesPer16th();
                        daisy::ScopedIrqBlocker lock;
                        g_externalStarted = true;
                        g_transportSampleF += stepSamples;
                        g_transportSample
                            = (uint64_t)std::llround(g_transportSampleF);
                    }
                    // Drain internal ticks to keep counters from backing up.
                    while(g_clock.ConsumeStepTick()) {}
                }
            }
            else
            {
                while(g_clock.ConsumeStepTick()) {}
                while(g_clock.ConsumeExternalStep()) {}
            }

            const bool gateOn = g_gateCountdownSamples > 0;
            if(g_gateCountdownSamples > 0)
                g_gateCountdownSamples--;

            if(gateOn != g_gateHigh)
            {
                g_gateHigh = gateOn;
                dsy_gpio_write(&hw.gate_out_1, gateOn ? 1 : 0);
            }

            out[0][offset + i] = lbuf[i];
            out[1][offset + i] = rbuf[i];
        }
        offset += frames;
    }

    sampleClock += size;
}

static void DispatchMidiMessage(MidiEvent msg)
{
    switch(msg.type)
    {
        case MidiMessageType::NoteOn:
        {
            auto note = msg.AsNoteOn();
            if(note.velocity > 0 && !g_midiRun)
                g_midiRun = true;
            if(note.velocity > 0)
                g_uiBackend.PulseActivity(note.channel);
            {
                MidiEv ev{};
                ev.type
                    = (note.velocity == 0) ? EvType::NoteOff : EvType::NoteOn;
                ev.ch = note.channel;
                ev.a  = ApplyTranspose(note.channel, note.note);
                ev.b  = note.velocity;
                EnqueueImmediate(ev);
            }
        }
        break;

        case MidiMessageType::NoteOff:
        {
            auto note = msg.AsNoteOff();
            g_uiBackend.PulseActivity(note.channel);
            MidiEv ev{};
            ev.type = EvType::NoteOff;
            ev.ch   = note.channel;
            ev.a    = ApplyTranspose(note.channel, note.note);
            EnqueueImmediate(ev);
        }
        break;

        case MidiMessageType::ProgramChange:
        {
            auto   pgm = msg.AsProgramChange();
            MidiEv ev{};
            ev.type                          = EvType::Program;
            ev.ch                            = pgm.channel;
            ev.a                             = pgm.program;
            g_uiBackend.program[pgm.channel] = pgm.program;
            EnqueueImmediate(ev);
        }
        break;

        case MidiMessageType::ControlChange:
        {
            auto cc = msg.AsControlChange();
            if(cc.control_number == 120 || cc.control_number == 123)
            {
                MidiEv ev{};
                ev.type = EvType::AllNotesOff;
                EnqueueImmediate(ev);
            }
            else
            {
                MidiEv ev{};
                ev.type = EvType::ControlChange;
                ev.ch   = cc.channel;
                ev.a    = cc.control_number;
                ev.b    = cc.value;
                EnqueueImmediate(ev);
            }
        }
        break;

        case MidiMessageType::ChannelMode:
        {
            auto mode = msg.AsChannelMode();
            if(mode.event_type == ChannelModeType::AllNotesOff
               || mode.event_type == ChannelModeType::AllSoundOff)
            {
                SynthPanic();
            }
        }
        break;

        case MidiMessageType::SystemRealTime:
        {
            switch(msg.srt_type)
            {
                case SystemRealTimeType::Start:
                case SystemRealTimeType::Continue: g_midiRun = true; break;
                case SystemRealTimeType::Stop:
                    g_midiRun = false;
                    {
                        MidiEv ev{};
                        ev.type = EvType::AllNotesOff;
                        EnqueueImmediate(ev);
                    }
                    break;
                default: break;
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

        default: break;
    }
}

static void ProcessScheduledEvents(uint64_t sampleNow)
{
    MidiEv ev;
    while(g_queue.Peek(ev) && ev.atSample <= sampleNow)
    {
        g_queue.Pop(ev);
        const char* name = "Unknown";
        (void)name;
        switch(ev.type)
        {
            case EvType::NoteOn: name = "NoteOn"; break;
            case EvType::NoteOff: name = "NoteOff"; break;
            case EvType::Program: name = "Program"; break;
            case EvType::ControlChange: name = "CC"; break;
            case EvType::PitchBend: name = "PitchBend"; break;
            case EvType::AllNotesOff: name = "AllNotesOff"; break;
        }
        LOG("Scheduled %s ch:%u note:%u vel:%u sample:%lu",
            name,
            ev.ch,
            ev.a,
            ev.b,
            (unsigned long)ev.atSample);
        switch(ev.type)
        {
            case EvType::NoteOn:
                ev.a = ApplyPlaybackTranspose(ev.ch, ev.a);
                EnqueueImmediate(ev);
                break;
            case EvType::NoteOff:
                ev.a = ApplyPlaybackTranspose(ev.ch, ev.a);
                EnqueueImmediate(ev);
                break;
            case EvType::Program:
                ApplyMajorMidiPlaybackSettings(ev);
                g_uiBackend.program[ev.ch] = ev.a;
                EnqueueImmediate(ev);
                break;
            case EvType::ControlChange:
                ApplyMajorMidiPlaybackSettings(ev);
                if(ev.a == 10)
                {
                    g_uiBackend.pan[ev.ch]   = ev.b;
                    g_uiBackend.sfPan[ev.ch] = ev.b;
                }
                EnqueueImmediate(ev);
                break;
            case EvType::PitchBend: EnqueueImmediate(ev); break;
            case EvType::AllNotesOff: EnqueueImmediate(ev); break;
        }
    }
}

int main(void)
{
    hw.Init();
    if(kEnableUsbLog)
        hw.StartLog(false);
    LOG("SF2 test: init");

    SynthInit();

    const bool sdOk = SdMount();
    LOG("SdMount: %s", sdOk ? "PASS" : "FAIL");

    const float sr = hw.AudioSampleRate();
    g_sampleRate   = sr;
    if(sdOk)
    {
        ScanMidiDir();
        ScanSoundFontDir();
    }

    if(g_midiFileCount > 0)
    {
        snprintf(g_midiPath, sizeof(g_midiPath), "0:/midi/%s", g_midiFiles[0]);
        std::strncpy(g_uiBackend.midiName,
                     g_midiFiles[0],
                     sizeof(g_uiBackend.midiName) - 1);
        g_uiBackend.midiName[sizeof(g_uiBackend.midiName) - 1] = '\0';
    }

    char sf_path[64] = "0:/soundfonts/microgm.sf2";
    if(g_soundFontCount > 0)
        snprintf(sf_path, sizeof(sf_path), "0:/soundfonts/%s", g_soundFonts[0]);

    const bool sfOk = sdOk && SynthLoadSf2(sf_path, sr, 32);
    LOG("SF2 mem: used=%lu cap=%lu oom=%d",
        (unsigned long)SynthArenaUsed(),
        (unsigned long)SynthArenaCap(),
        SynthArenaOom() ? 1 : 0);
    LOG("SoundFont: %s", sfOk ? "PASS" : "FAIL");

    bool smfOk = false;
    if(sfOk)
    {
        ClockSync::Config clock_cfg{};
        clock_cfg.alpha               = 0.1f;
        clock_cfg.beta                = 0.25f;
        clock_cfg.glitch_percent      = 0.25f;
        clock_cfg.debounce_ms         = 1.0f;
        clock_cfg.missing_timeout_s   = 2.0f;
        clock_cfg.free_run_on_missing = false;
        clock_cfg.default_bpm         = 120.0f;
        g_clock.Init(sr, ClockSync::PulseMode::PULSE_PER_16TH, clock_cfg);
        g_smfPlayer.SetSampleRate(sr);
        g_smfPlayer.SetLookaheadSamples(0);
        g_smfPlayer.SetTempoScale(g_tempoScale);
        smfOk = g_smfPlayer.Open(g_midiPath);
        LOG("SMF open %s (%s)", smfOk ? "PASS" : "FAIL", g_midiPath);
    }

    if(kEnableUsbMidi)
    {
        MidiUsbHandler::Config usb_cfg{};
        usb_cfg.transport_config = MidiUsbTransport::Config();
        g_usb.Init(usb_cfg);
        g_usb.StartReceive();
    }
    LOG("Event queue capacity: %d", (int)2048);

    bool usbLogged = false;

    if(sfOk)
    {
        hw.StartAudio(AudioCallback);
        g_audioRunning = true;
    }

    // TODO: Pin mapping (Patch SM)
    // - Clock input pin: DaisyPatchSM::B10 (external clock pulses)
    // - Gate output pin: hw.gate_out_1 (16th note gate)
    // - Clock mode switch: DaisyPatchSM::B8 (internal/external)
    // - UI/OLED pins: verify against your wiring
    g_clockIn.pin  = DaisyPatchSM::B10;
    g_clockIn.mode = DSY_GPIO_MODE_INPUT;
    g_clockIn.pull = DSY_GPIO_PULLDOWN;
    dsy_gpio_init(&g_clockIn);
    g_playButton.Init(DaisyPatchSM::B7, hw.AudioCallbackRate());
    g_clockModeSwitch.Init(DaisyPatchSM::B8, hw.AudioCallbackRate());
    g_gatePulseSamples = (int)(g_sampleRate * 0.005f);
    dsy_gpio_write(&hw.gate_out_1, 0);

    UiOled::Pins ui_pins;
    ui_pins.spi_periph = SpiHandle::Config::Peripheral::SPI_2;
    ui_pins.sclk       = DaisyPatchSM::D10;
    ui_pins.mosi       = DaisyPatchSM::D9;
    ui_pins.dc         = DaisyPatchSM::D1;
    ui_pins.rst        = DaisyPatchSM::A3;
    // TODO: Choose encoder pins that do not conflict with your wiring.
    ui_pins.encA     = DaisyPatchSM::A9;
    ui_pins.encB     = DaisyPatchSM::A8;
    ui_pins.encClick = DaisyPatchSM::A2;
    System::Delay(500);
    g_ui.Init(hw, ui_pins, g_uiBackend);

    uint32_t lastStatusLog = System::GetNow();
    uint32_t lastHeartbeat = System::GetNow();
    bool     heartbeatOn   = false;
    while(1)
    {
        if(!sfOk)
        {
            System::Delay(500);
            continue;
        }

        uint32_t now = System::GetNow();
        if(now - lastHeartbeat >= 500)
        {
            lastHeartbeat = now;
            heartbeatOn   = !heartbeatOn;
            hw.SetLed(heartbeatOn);
        }
        hw.ProcessAnalogControls();
        g_ui.Process();

        // CV_1 / CV_2 tempo + transpose control disabled for now.
        g_playButton.Debounce();
        g_clockModeSwitch.Debounce();
        if(g_playButton.RisingEdge() && smfOk)
        {
            if(g_smfPlayer.IsPlaying())
            {
                StopPlayback();
                LOG("SMF stop");
            }
            else
            {
                StartPlayback();
                LOG("SMF start");
            }
        }

        if(kEnableUsbMidi)
        {
            g_usb.Listen();
            while(g_usb.HasEvents())
            {
                if(!usbLogged)
                {
                    LOG("USB MIDI events processing");
                    usbLogged = true;
                }
                DispatchMidiMessage(g_usb.PopEvent());
            }
        }

        const bool useInternalClock = g_clockModeSwitch.Pressed();
        g_useInternalClock          = useInternalClock;

        uint64_t clockNow = sampleClock;
        if(useInternalClock)
        {
            daisy::ScopedIrqBlocker lock;
            g_transportSample  = sampleClock;
            g_transportSampleF = (double)sampleClock;
        }
        else
        {
            daisy::ScopedIrqBlocker lock;
            clockNow = g_transportSample;
        }

        if(useInternalClock && g_tempoScaleApplied != g_tempoScale)
        {
            g_smfPlayer.SetTempoScale(g_tempoScale, clockNow);
            g_tempoScaleApplied = g_tempoScale;
        }

        const double spq = g_smfPlayer.SamplesPerQuarterF();
        if(spq > 0.0)
        {
            const float bpm = (g_sampleRate * 60.0f) / (float)spq;
            g_clock.SetInternalBpm(bpm);
        }
        if(useInternalClock || g_externalStarted)
        {
            g_smfPlayer.Pump(g_queue, clockNow);
            ProcessScheduledEvents(clockNow);
        }

        if(g_uiBackend.GetLoop() && g_smfPlayer.IsPlaying())
        {
            int lm = 1, lb = 1, ls = 1;
            g_uiBackend.GetLoopStartMbs(&lm, &lb, &ls);
            int64_t loop_start_ticks = (int64_t)MbsToTicks(lm, lb, ls);
            // Offsets disabled for now; keep for future tuning.
            // loop_start_ticks += (int64_t)g_uiBackend.GetLoopStartOffsetTicks();
            if(loop_start_ticks < 0)
                loop_start_ticks = 0;
            int64_t loop_len_ticks
                = (int64_t)g_uiBackend.GetLoopLengthBeats()
                  * (int64_t)((g_smfPlayer.Divisions() * 4)
                              / (g_smfPlayer.TimeSigDenominator()
                                     ? g_smfPlayer.TimeSigDenominator()
                                     : 4));
            // loop_len_ticks += (int64_t)g_uiBackend.GetLoopLengthOffsetTicks();
            if(loop_len_ticks < 1)
                loop_len_ticks = 1;
            const uint64_t play_pos_ticks
                = g_smfPlayer.TicksFromSamples(
                      (clockNow >= g_playStartSample)
                          ? (clockNow - g_playStartSample)
                          : 0)
                  + g_playStartTicks;
            if(play_pos_ticks >= (uint64_t)(loop_start_ticks + loop_len_ticks))
            {
                g_queue.Clear();
                g_immediate.Clear();
                uint64_t loop_start_samples
                    = g_smfPlayer.SamplesFromTicks((uint64_t)loop_start_ticks);
                if(loop_start_samples > 0)
                    loop_start_samples -= 1;
                g_smfPlayer.SeekToSample(loop_start_samples, clockNow);
                g_playStartSample = clockNow;
                g_playStartTicks  = (uint64_t)loop_start_ticks;
                // Reapply channel state so programs/CCs are correct after seek
                ApplyChannelStateFromUi();
            }
        }

        const bool playing = g_smfPlayer.IsPlaying();
        if(!playing)
        {
            if(g_gateHigh)
            {
                g_gateHigh = false;
                dsy_gpio_write(&hw.gate_out_1, 0);
            }
            g_gateCountdownSamples = 0;
        }

        if(now - lastStatusLog >= 1000)
        {
            lastStatusLog = now;
            // LOG("Clock %lu, queue %d",
            //     (unsigned long)sampleClock,
            //     (int)g_queue.Size());
            // LOG("SMF playing %d, remaining %lu",
            //     (int)g_smfPlayer.IsPlaying(),
            //     (unsigned long)g_smfPlayer.RemainingBytes());
        }
    }
}
