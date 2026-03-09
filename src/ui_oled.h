#pragma once
#include "daisy_patch_sm.h"
#include "dev/oled_ssd130x.h"
#include "hid/encoder.h"
#include "hid/switch.h"
#include <stdint.h>
#include <stddef.h>

namespace major_midi
{

using namespace daisy;

// SSD1306/SSD1309 family, 128x64, 4-wire SPI
using DisplayT = OledDisplay<SSD130x4WireSpi128x64Driver>;

struct UiBackend
{
    virtual ~UiBackend() = default;

    // ---- Main page ----
    virtual int  GetInstrumentPage() const        = 0; // 1..4
    virtual void SetInstrumentPage(int page_1to4) = 0;

    virtual const char* GetLoadedMidiName() const
        = 0; // stable pointer while displayed
    virtual void GetPlayheadText(char* out, size_t out_sz) const = 0;
    virtual void GetPlayheadMbs(int* measure, int* beat, int* sub) const = 0;

    // Map (page 1..4) + row(0..3) -> channel index 0..15
    virtual int GetChannelIndexForMainRow(int page_1to4, int row_0to3) const
        = 0;

    virtual uint8_t GetChanVolume(int chan_0to15) const      = 0; // 0..127
    virtual void    SetChanVolume(int chan_0to15, uint8_t v) = 0;

    virtual uint8_t GetChanPan(int chan_0to15) const      = 0; // 0..127
    virtual void    SetChanPan(int chan_0to15, uint8_t p) = 0;

    virtual uint8_t GetChanProgram(int chan_0to15) const      = 0; // 0..127
    virtual void    SetChanProgram(int chan_0to15, uint8_t p) = 0;

    virtual const char* GetChanTrackName(int chan_0to15) const
        = 0; // from MIDI metadata if available

    // UI blinks if pulse is consumed; set true on MIDI event in your engine
    virtual bool ConsumeChanActivityPulse(int chan_0to15) = 0;

    // ---- System page ----
    enum class SyncMode
    {
        Internal,
        External
    };
    virtual SyncMode GetSyncMode() const
        = 0; // per your sheet, UI renders as readonly

    virtual bool GetMidiThru() const  = 0;
    virtual void SetMidiThru(bool on) = 0;

    enum class Tri
    {
        Off,
        Send,
        Receive
    };
    enum class Quad
    {
        Off,
        Send,
        Receive,
        Both
    };

    virtual Tri  GetMidiSync() const = 0;
    virtual void SetMidiSync(Tri v)  = 0;

    virtual Quad GetMidiMessages() const = 0;
    virtual void SetMidiMessages(Quad v) = 0;

    virtual Quad GetMidiProgramMessages() const = 0;
    virtual void SetMidiProgramMessages(Quad v) = 0;

    // ---- MIDI File page ----
    virtual int  GetBpm() const  = 0; // 20..300
    virtual void SetBpm(int bpm) = 0;

    virtual bool GetPlay() const  = 0;
    virtual void SetPlay(bool on) = 0;

    virtual bool GetLoop() const  = 0;
    virtual void SetLoop(bool on) = 0;

    virtual int  GetLoopStartMeasure() const = 0; // >=1
    virtual void SetLoopStartMeasure(int m)  = 0;

    virtual int  GetLoopLengthBeats() const    = 0; // >=1
    virtual void SetLoopLengthBeats(int beats) = 0;
    virtual int  GetLoopStartOffsetTicks() const = 0;
    virtual void SetLoopStartOffsetTicks(int ticks) = 0;
    virtual int  GetLoopLengthOffsetTicks() const = 0;
    virtual void SetLoopLengthOffsetTicks(int ticks) = 0;

    virtual void GetLoopStartMbs(int* measure, int* beat, int* sub) const = 0;
    virtual void SetLoopStartMbs(int measure, int beat, int sub)          = 0;

    // Picker lists - stable while picker open
    virtual int         GetMidiFileCount() const         = 0;
    virtual const char* GetMidiFileName(int index) const = 0;
    virtual void        LoadMidiFileByIndex(int index)   = 0;
    virtual bool        SaveMidiSettings()               = 0;

    // ---- SoundFont page ----
    virtual int  GetSfChannel() const       = 0; // 1..16
    virtual void SetSfChannel(int ch_1to16) = 0;

    virtual const char* GetSfTrackName(int ch_1to16) const = 0;

    virtual bool GetSfMute(int ch_1to16) const    = 0;
    virtual void SetSfMute(int ch_1to16, bool on) = 0;

    virtual uint8_t GetSfVolume(int ch_1to16) const      = 0; // 0..127
    virtual void    SetSfVolume(int ch_1to16, uint8_t v) = 0;

    virtual uint8_t GetSfPan(int ch_1to16) const      = 0; // 0..127
    virtual void    SetSfPan(int ch_1to16, uint8_t p) = 0;

    virtual uint8_t GetSfReverbSend(int ch_1to16) const      = 0;
    virtual void    SetSfReverbSend(int ch_1to16, uint8_t v) = 0;

    virtual uint8_t GetSfChorusSend(int ch_1to16) const      = 0;
    virtual void    SetSfChorusSend(int ch_1to16, uint8_t v) = 0;

    virtual uint8_t GetSfVelocityMod(int ch_1to16) const      = 0;
    virtual void    SetSfVelocityMod(int ch_1to16, uint8_t v) = 0;

    virtual uint8_t GetSfPitchMod(int ch_1to16) const      = 0;
    virtual void    SetSfPitchMod(int ch_1to16, uint8_t v) = 0;

    virtual uint8_t GetSfModWheel(int ch_1to16) const      = 0;
    virtual void    SetSfModWheel(int ch_1to16, uint8_t v) = 0;

    // Pitchbend shown as -64..+63 (UI-friendly)
    virtual int8_t GetSfPitchbend(int ch_1to16) const     = 0;
    virtual void   SetSfPitchbend(int ch_1to16, int8_t v) = 0;

    // Soundfont picker
    virtual int         GetSoundFontCount() const         = 0;
    virtual const char* GetSoundFontName(int index) const = 0;
    virtual void        LoadSoundFontByIndex(int index)   = 0;

    // ---- FX page ----
    virtual float GetFxReverbTime() const = 0; // 0..1
    virtual void  SetFxReverbTime(float t01) = 0;

    virtual float GetFxReverbLpFreq() const = 0; // Hz
    virtual void  SetFxReverbLpFreq(float hz) = 0;

    virtual float GetFxReverbHpFreq() const = 0; // Hz
    virtual void  SetFxReverbHpFreq(float hz) = 0;

    virtual float GetFxChorusDepth() const = 0; // 0..1
    virtual void  SetFxChorusDepth(float d01) = 0;

    virtual float GetFxChorusSpeed() const = 0; // Hz
    virtual void  SetFxChorusSpeed(float hz) = 0;
};

class UiOled
{
  public:
    struct Pins
    {
        // OLED SPI
        Pin                           sclk;
        Pin                           mosi;
        Pin                           cs;
        Pin                           dc;
        Pin                           rst;
        SpiHandle::Config::Peripheral spi_periph
            = SpiHandle::Config::Peripheral::SPI_2;

        // Encoder
        Pin encA;
        Pin encB;
        Pin encClick;
    };

    void Init(patch_sm::DaisyPatchSM& hw,
              const Pins&            pins,
              UiBackend&             backend);

    // Call in your main loop frequently (~100-500Hz)
    void Process();

    // Force a redraw soon (optional)
    void Invalidate() { dirty_ = true; }

  private:
    enum class Page
    {
        Menu,
        Main,
        Channels,
        System,
        MidiFile,
        SoundFont,
        Fx
    };
    enum class Mode
    {
        Nav,
        Edit,
        Picker
    };
    enum class MainField
    {
        Vol,
        Pan,
        Program
    };
    enum class MainCounterField
    {
        Measure,
        Beat,
        Sub
    };
    enum class LoopStartField
    {
        Measure,
        Beat,
        Sub
    };
    enum class PickerKind
    {
        MidiFiles,
        SoundFonts
    };

    void handleInput_();
    void updateActivityDecay_();
    void maybeDraw_();
    void draw_();

    // Page draw
    void drawMain_();
    void drawChannels_();
    void drawMenu_();
    void drawSystem_();
    void drawMidiFile_();
    void drawSoundFont_();
    void drawFx_();
    void drawPicker_();
    void drawMode_(DisplayT& d, int x, int y, Mode m);
    void drawRow_(int y, const char* text, bool selected, bool dim);

    // Interaction
    void navMove_(int delta);
    void editMove_(int delta);
    void onClick_();
    void onLongPress_();

    // Formatting/helpers
    static const char* triToStr_(UiBackend::Tri v);
    static const char* quadToStr_(UiBackend::Quad v);
    static const char* syncToStr_(UiBackend::SyncMode v);

    static int     clampi_(int v, int lo, int hi);
    static int     wrapi_(int v, int lo, int hi);
    static uint8_t clampu8_(int v);

  private:
    patch_sm::DaisyPatchSM* hw_      = nullptr;
    UiBackend*  backend_ = nullptr;

    DisplayT display_;
    Encoder  enc_;
    Switch   enc_sw_; // reliable click with pullup

    // State
    Page page_ = Page::Main;
    Mode mode_ = Mode::Nav;

    // Selections
    int       sel_menu_       = 0; // 0..5
    int       sel_main_row_   = 0; // 0..16 (0=Back)
    MainField sel_main_field_ = MainField::Vol;
    MainCounterField sel_main_counter_field_ = MainCounterField::Measure;
    LoopStartField loop_start_field_ = LoopStartField::Measure;

    int sel_system_       = 0; // 0..5 (0=Back)
    int sel_midifile_     = 0; // 0..7 (0=Back)
    int sel_soundfont_    = 0; // 0..12 (0=Back)
    int sel_fx_           = 0; // 0..5 (0=Back)
    int scroll_main_      = 0;
    int scroll_system_    = 0;
    int scroll_midifile_  = 0;
    int scroll_soundfont_ = 0;
    int scroll_fx_        = 0;

    // Picker
    PickerKind picker_kind_   = PickerKind::MidiFiles;
    int        picker_sel_    = 0;
    int        picker_scroll_ = 0;

    // Activity decay
    int16_t  activity_ms_[16]       = {0};
    uint32_t last_activity_tick_ms_ = 0;

    // Long press
    bool     pressed_        = false;
    uint32_t press_start_ms_ = 0;
    bool     long_fired_     = false;

    // Draw throttling
    bool     dirty_            = true;
    uint32_t last_draw_ms_     = 0;
    uint32_t draw_interval_ms_ = 33; // ~30fps
};

} // namespace major_midi
