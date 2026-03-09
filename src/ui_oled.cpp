#include "ui_oled.h"
#include <stdio.h>
#include <string.h>

namespace major_midi
{

static inline uint32_t now_ms()
{
    return System::GetNow();
}

void UiOled::Init(patch_sm::DaisyPatchSM& hw,
                  const Pins&             pins,
                  UiBackend&              backend)
{
    hw_      = &hw;
    backend_ = &backend;

    // Encoder rotation
    enc_.Init(pins.encA, pins.encB, pins.encClick, 24);

    // Encoder click: no resistors => explicit pull-up, active-low (pressed shorts to GND)
    // NOTE: If your libDaisy Switch::Init signature differs, tell me and I’ll match it.
    enc_sw_.Init(pins.encClick,
                 1000,
                 Switch::TYPE_MOMENTARY,
                 Switch::POLARITY_INVERTED,
                 Switch::PULL_UP);

    // OLED SPI config
    DisplayT::Config disp_cfg;
    disp_cfg.driver_config.transport_config.spi_config.periph = pins.spi_periph;

    auto& spi_pc
        = disp_cfg.driver_config.transport_config.spi_config.pin_config;
    spi_pc.sclk                                              = pins.sclk;
    spi_pc.mosi                                              = pins.mosi;
    spi_pc.nss                                               = pins.cs;
    disp_cfg.driver_config.transport_config.pin_config.dc    = pins.dc;
    disp_cfg.driver_config.transport_config.pin_config.reset = pins.rst;

    display_.Init(disp_cfg);
    display_.Fill(true);
    display_.Update();
    System::Delay(150);
    display_.Fill(false);
    display_.Update();

    last_draw_ms_          = now_ms();
    last_activity_tick_ms_ = now_ms();
    dirty_                 = true;
}

void UiOled::Process()
{
    enc_.Debounce();
    enc_sw_.Debounce();

    handleInput_();
    updateActivityDecay_();
    maybeDraw_();
}

void UiOled::handleInput_()
{
    const uint32_t t = now_ms();

    // Rotation
    const int inc = enc_.Increment();
    if(inc != 0)
    {
        if(mode_ == Mode::Nav)
            navMove_(inc);
        else if(mode_ == Mode::Edit)
            editMove_(inc);
        else if(mode_ == Mode::Picker)
        {
            picker_sel_ += (inc > 0 ? 1 : -1);
            dirty_ = true;
        }
    }

    // Click + long press
    const bool is_pressed = enc_sw_.Pressed();

    if(is_pressed && !pressed_)
    {
        pressed_        = true;
        press_start_ms_ = t;
        long_fired_     = false;
    }
    else if(!is_pressed && pressed_)
    {
        pressed_ = false;
        if(!long_fired_)
            onClick_();
    }
    else if(is_pressed && !long_fired_)
    {
        if(t - press_start_ms_ >= 650)
        {
            long_fired_ = true;
            onLongPress_();
        }
    }
}

void UiOled::navMove_(int delta)
{
    const int d = (delta > 0 ? 1 : -1);

    switch(page_)
    {
        case Page::Menu: sel_menu_ = clampi_(sel_menu_ + d, 0, 5); break;
        case Page::Main:
            sel_main_row_ = clampi_(sel_main_row_ + d, 0, 16);
            break;
        case Page::Channels:
            sel_main_row_ = clampi_(sel_main_row_ + d, 0, 16);
            break;
        case Page::System: sel_system_ = clampi_(sel_system_ + d, 0, 5); break;
        case Page::MidiFile:
            sel_midifile_ = clampi_(sel_midifile_ + d, 0, 7);
            break;
        case Page::SoundFont:
            sel_soundfont_ = clampi_(sel_soundfont_ + d, 0, 12);
            break;
        case Page::Fx: sel_fx_ = clampi_(sel_fx_ + d, 0, 5); break;
    }

    dirty_ = true;
}

void UiOled::editMove_(int delta)
{
    // Simple acceleration
    const int step = (abs(delta) >= 2) ? 4 : 1;
    const int d    = (delta > 0 ? step : -step);

    switch(page_)
    {
        case Page::Menu: break;
        case Page::Main:
        {
            // Main page counter is display-only
        }
        break;

        case Page::Channels:
        {
            if(sel_main_row_ == 0)
                break;
            const int chan = sel_main_row_ - 1;
            if(sel_main_field_ == MainField::Vol)
            {
                int v = (int)backend_->GetChanVolume(chan) + d;
                backend_->SetChanVolume(chan, clampu8_(v));
            }
            else if(sel_main_field_ == MainField::Pan)
            {
                int p = (int)backend_->GetChanPan(chan) + d;
                backend_->SetChanPan(chan, clampu8_(p));
            }
            else
            {
                int p = (int)backend_->GetChanProgram(chan) + d;
                backend_->SetChanProgram(chan, clampu8_(p));
            }
        }
        break;

        case Page::System:
        {
            // 0 Back
            // 1 Sync readonly
            // 2 Thru toggle
            // 3 Midi Sync tri
            // 4 Midi Messages quad
            // 5 Midi Program Messages quad
            if(sel_system_ == 0)
                break;
            if(sel_system_ == 2)
            {
                backend_->SetMidiThru(!backend_->GetMidiThru());
            }
            else if(sel_system_ == 3)
            {
                int iv = (int)backend_->GetMidiSync() + (d > 0 ? 1 : -1);
                iv     = wrapi_(iv, 0, 2);
                backend_->SetMidiSync((UiBackend::Tri)iv);
            }
            else if(sel_system_ == 4)
            {
                int iv = (int)backend_->GetMidiMessages() + (d > 0 ? 1 : -1);
                iv     = wrapi_(iv, 0, 3);
                backend_->SetMidiMessages((UiBackend::Quad)iv);
            }
            else if(sel_system_ == 5)
            {
                int iv = (int)backend_->GetMidiProgramMessages()
                         + (d > 0 ? 1 : -1);
                iv = wrapi_(iv, 0, 3);
                backend_->SetMidiProgramMessages((UiBackend::Quad)iv);
            }
        }
        break;

        case Page::MidiFile:
        {
            // 0 Back
            // 1 Load (picker)
            // 2 Save MIDI Settings
            // 3 BPM
            // 4 Play
            // 5 Loop
            // 6 Loop Start
            // 7 Loop Length
            if(sel_midifile_ == 0)
                break;
            if(sel_midifile_ == 2)
            {
                (void)backend_->SaveMidiSettings();
            }
            else if(sel_midifile_ == 3)
            {
                int bpm = backend_->GetBpm() + (d > 0 ? step : -step);
                bpm     = clampi_(bpm, 20, 300);
                backend_->SetBpm(bpm);
            }
            else if(sel_midifile_ == 4)
            {
                backend_->SetPlay(!backend_->GetPlay());
            }
            else if(sel_midifile_ == 5)
            {
                backend_->SetLoop(!backend_->GetLoop());
            }
            else if(sel_midifile_ == 6)
            {
                int m = 1, b = 1, s = 1;
                backend_->GetLoopStartMbs(&m, &b, &s);
                if(loop_start_field_ == LoopStartField::Measure)
                    m += (d > 0 ? 1 : -1);
                else if(loop_start_field_ == LoopStartField::Beat)
                    b += (d > 0 ? 1 : -1);
                else
                    s += (d > 0 ? 1 : -1);
                backend_->SetLoopStartMbs(m, b, s);
            }
            else if(sel_midifile_ == 7)
            {
                int b = backend_->GetLoopLengthBeats() + (d > 0 ? 1 : -1);
                backend_->SetLoopLengthBeats(clampi_(b, 1, 999));
            }
        }
        break;

        case Page::SoundFont:
        {
            if(sel_soundfont_ == 0)
                break;
            const int ch = backend_->GetSfChannel(); // 1..16

            // 0 Back
            // 1 Load SF2 (picker)
            // 2 Channel
            // 3 Name (readonly)
            // 4 Mute
            // 5 Volume
            // 6 Pan
            // 7 Reverb Send
            // 8 Chorus Send
            // 9 Velocity mod
            // 10 Pitch mod
            // 11 Mod Wheel
            // 12 Pitchbend
            if(sel_soundfont_ == 2)
            {
                int nch = ch + (d > 0 ? 1 : -1);
                nch     = wrapi_(nch, 1, 16);
                backend_->SetSfChannel(nch);
            }
            else if(sel_soundfont_ == 4)
            {
                backend_->SetSfMute(ch, !backend_->GetSfMute(ch));
            }
            else if(sel_soundfont_ == 5)
            {
                int v = (int)backend_->GetSfVolume(ch) + d;
                backend_->SetSfVolume(ch, clampu8_(v));
            }
            else if(sel_soundfont_ == 6)
            {
                int p = (int)backend_->GetSfPan(ch) + d;
                backend_->SetSfPan(ch, clampu8_(p));
            }
            else if(sel_soundfont_ == 7)
            {
                int v = (int)backend_->GetSfReverbSend(ch) + d;
                backend_->SetSfReverbSend(ch, clampu8_(v));
            }
            else if(sel_soundfont_ == 8)
            {
                int v = (int)backend_->GetSfChorusSend(ch) + d;
                backend_->SetSfChorusSend(ch, clampu8_(v));
            }
            else if(sel_soundfont_ == 9)
            {
                int v = (int)backend_->GetSfVelocityMod(ch) + d;
                backend_->SetSfVelocityMod(ch, clampu8_(v));
            }
            else if(sel_soundfont_ == 10)
            {
                int v = (int)backend_->GetSfPitchMod(ch) + d;
                backend_->SetSfPitchMod(ch, clampu8_(v));
            }
            else if(sel_soundfont_ == 11)
            {
                int v = (int)backend_->GetSfModWheel(ch) + d;
                backend_->SetSfModWheel(ch, clampu8_(v));
            }
            else if(sel_soundfont_ == 12)
            {
                int pb = (int)backend_->GetSfPitchbend(ch) + (d > 0 ? 2 : -2);
                pb     = clampi_(pb, -64, 63);
                backend_->SetSfPitchbend(ch, (int8_t)pb);
            }
        }
        break;

        case Page::Fx:
        {
            if(sel_fx_ == 0)
                break;
            if(sel_fx_ == 1)
            {
                float t
                    = backend_->GetFxReverbTime() + (d > 0 ? 0.02f : -0.02f);
                if(t < 0.0f)
                    t = 0.0f;
                if(t > 1.0f)
                    t = 1.0f;
                backend_->SetFxReverbTime(t);
            }
            else if(sel_fx_ == 2)
            {
                float hz = backend_->GetFxReverbLpFreq()
                           + (d > 0 ? 200.0f : -200.0f);
                if(hz < 200.0f)
                    hz = 200.0f;
                if(hz > 18000.0f)
                    hz = 18000.0f;
                backend_->SetFxReverbLpFreq(hz);
            }
            else if(sel_fx_ == 3)
            {
                float hz
                    = backend_->GetFxReverbHpFreq() + (d > 0 ? 50.0f : -50.0f);
                if(hz < 20.0f)
                    hz = 20.0f;
                if(hz > 1000.0f)
                    hz = 1000.0f;
                backend_->SetFxReverbHpFreq(hz);
            }
            else if(sel_fx_ == 4)
            {
                float dpt
                    = backend_->GetFxChorusDepth() + (d > 0 ? 0.02f : -0.02f);
                if(dpt < 0.0f)
                    dpt = 0.0f;
                if(dpt > 1.0f)
                    dpt = 1.0f;
                backend_->SetFxChorusDepth(dpt);
            }
            else if(sel_fx_ == 5)
            {
                float hz
                    = backend_->GetFxChorusSpeed() + (d > 0 ? 0.05f : -0.05f);
                if(hz < 0.05f)
                    hz = 0.05f;
                if(hz > 5.0f)
                    hz = 5.0f;
                backend_->SetFxChorusSpeed(hz);
            }
        }
        break;
    }

    dirty_ = true;
}

void UiOled::onClick_()
{
    if(mode_ == Mode::Picker)
    {
        // select item
        if(picker_sel_ == 0)
        {
            mode_  = Mode::Nav;
            dirty_ = true;
            return;
        }

        const int load_idx = picker_sel_ - 1;
        if(picker_kind_ == PickerKind::MidiFiles)
            backend_->LoadMidiFileByIndex(load_idx);
        else
            backend_->LoadSoundFontByIndex(load_idx);

        mode_  = Mode::Nav;
        dirty_ = true;
        return;
    }

    // NAV -> either open picker, or enter edit
    if(mode_ == Mode::Nav)
    {
        if(page_ == Page::Menu)
        {
            switch(sel_menu_)
            {
                case 0: page_ = Page::Main; break;
                case 1: page_ = Page::Channels; break;
                case 2: page_ = Page::System; break;
                case 3: page_ = Page::MidiFile; break;
                case 4: page_ = Page::SoundFont; break;
                case 5: page_ = Page::Fx; break;
                default: page_ = Page::Main; break;
            }
            mode_ = Mode::Nav;
        }
        else if((page_ == Page::Main && sel_main_row_ == 0)
                || (page_ == Page::Channels && sel_main_row_ == 0)
                || (page_ == Page::System && sel_system_ == 0)
                || (page_ == Page::MidiFile && sel_midifile_ == 0)
                || (page_ == Page::SoundFont && sel_soundfont_ == 0)
                || (page_ == Page::Fx && sel_fx_ == 0))
        {
            page_ = Page::Menu;
            mode_ = Mode::Nav;
        }
        else if(page_ == Page::Main)
        {
            // Main is display-only
            mode_ = Mode::Nav;
        }
        else if(page_ == Page::MidiFile && sel_midifile_ == 1)
        {
            picker_kind_   = PickerKind::MidiFiles;
            picker_sel_    = 0;
            picker_scroll_ = 0;
            mode_          = Mode::Picker;
        }
        else if(page_ == Page::MidiFile && sel_midifile_ == 2)
        {
            (void)backend_->SaveMidiSettings();
            mode_ = Mode::Nav;
        }
        else if(page_ == Page::SoundFont && sel_soundfont_ == 1)
        {
            picker_kind_   = PickerKind::SoundFonts;
            picker_sel_    = 0;
            picker_scroll_ = 0;
            mode_          = Mode::Picker;
        }
        else
        {
            mode_ = Mode::Edit;
        }
    }
    else
    {
        // EDIT -> NAV
        mode_ = Mode::Nav;
    }

    dirty_ = true;
}

void UiOled::onLongPress_()
{
    if(mode_ == Mode::Picker)
    {
        // cancel picker
        mode_  = Mode::Nav;
        dirty_ = true;
        return;
    }

    // On Main while editing: cycle edit field
    if(page_ == Page::Channels && mode_ == Mode::Edit)
    {
        if(sel_main_field_ == MainField::Vol)
            sel_main_field_ = MainField::Pan;
        else if(sel_main_field_ == MainField::Pan)
            sel_main_field_ = MainField::Program;
        else
            sel_main_field_ = MainField::Vol;
        dirty_ = true;
        return;
    }
    // On Midi File loop start while editing: cycle M/B/S
    if(page_ == Page::MidiFile && mode_ == Mode::Edit && sel_midifile_ == 6)
    {
        if(loop_start_field_ == LoopStartField::Measure)
            loop_start_field_ = LoopStartField::Beat;
        else if(loop_start_field_ == LoopStartField::Beat)
            loop_start_field_ = LoopStartField::Sub;
        else
            loop_start_field_ = LoopStartField::Measure;
        dirty_ = true;
        return;
    }
}

void UiOled::updateActivityDecay_()
{
    const uint32_t t  = now_ms();
    const uint32_t dt = t - last_activity_tick_ms_;
    if(dt == 0)
        return;
    last_activity_tick_ms_ = t;

    for(int ch = 0; ch < 16; ch++)
    {
        if(backend_->ConsumeChanActivityPulse(ch))
        {
            activity_ms_[ch] = 180;
            dirty_           = true;
        }

        if(activity_ms_[ch] > 0)
        {
            int v            = activity_ms_[ch] - (int)dt;
            activity_ms_[ch] = (int16_t)(v > 0 ? v : 0);
        }
    }
}

void UiOled::maybeDraw_()
{
    const uint32_t t = now_ms();
    if(!dirty_ && (t - last_draw_ms_) < draw_interval_ms_)
        return;

    if(dirty_ || (t - last_draw_ms_) >= draw_interval_ms_)
    {
        last_draw_ms_ = t;
        draw_();
        dirty_ = false;
    }
}

void UiOled::draw_()
{
    display_.Fill(false);

    switch(page_)
    {
        case Page::Menu: drawMenu_(); break;
        case Page::Main: drawMain_(); break;
        case Page::Channels: drawChannels_(); break;
        case Page::System: drawSystem_(); break;
        case Page::MidiFile: drawMidiFile_(); break;
        case Page::SoundFont: drawSoundFont_(); break;
        case Page::Fx: drawFx_(); break;
    }

    if(mode_ == Mode::Picker)
        drawPicker_();

    display_.Update();
}

// ---- draw helpers ----
void UiOled::drawMode_(DisplayT& d, int x, int y, Mode m)
{
    d.SetCursor(x, y);
    if(m == Mode::Edit)
        d.WriteString("E", Font_6x8, true);
    else if(m == Mode::Picker)
        d.WriteString("P", Font_6x8, true);
    else
        d.WriteString("N", Font_6x8, true);
}

void UiOled::drawRow_(int y, const char* text, bool selected, bool dim)
{
    const bool edit = (mode_ == Mode::Edit);
    display_.SetCursor(0, y);
    display_.WriteString((selected && !edit) ? ">" : " ", Font_6x8, !dim);
    display_.WriteString(text, Font_6x8, !dim);
    if(selected && edit)
    {
        display_.SetCursor(122, y);
        display_.WriteString("<", Font_6x8, !dim);
    }
}

void UiOled::drawMenu_()
{
    display_.SetCursor(0, 0);
    display_.WriteString("Menu", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    const char* rows[]
        = {"Main", "Channels", "System", "MIDI File", "SoundFont", "FX"};

    const int count   = 6;
    const int visible = 6;
    const int y0      = 12;
    for(int i = 0; i < count && i < visible; i++)
    {
        drawRow_(y0 + i * 8, rows[i], sel_menu_ == i, false);
    }
}

void UiOled::drawMain_()
{
    const char* midi = backend_->GetLoadedMidiName();
    if(!midi)
        midi = "";

    char playhead[24];
    backend_->GetPlayheadText(playhead, sizeof(playhead));

    display_.SetCursor(0, 0);
    display_.WriteString("Main", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    // MIDI name + playhead, single line
    char midiLine[22];
    snprintf(midiLine, sizeof(midiLine), "%.12s %.8s", midi, playhead);
    display_.SetCursor(0, 10);
    display_.WriteString(midiLine, Font_6x8, true);

    int pm = 1, pb = 1, ps = 1;
    backend_->GetPlayheadMbs(&pm, &pb, &ps);
    char playLine[22];
    snprintf(playLine, sizeof(playLine), "Pos %02d|%02d|%02d", pm, pb, ps);
    display_.SetCursor(0, 18);
    display_.WriteString(playLine, Font_6x8, true);
}

void UiOled::drawChannels_()
{
    display_.SetCursor(0, 0);
    display_.WriteString("Channels", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    // selected track name line
    const char* trk = "-";
    if(sel_main_row_ > 0)
    {
        const char* nm = backend_->GetChanTrackName(sel_main_row_ - 1);
        if(nm && nm[0] != '\0')
            trk = nm;
    }
    char trkLine[22];
    snprintf(trkLine, sizeof(trkLine), "Trk: %.16s", trk);
    display_.SetCursor(0, 12);
    display_.WriteString(trkLine, Font_6x8, true);

    // Back + 16 channels
    const int count   = 17;
    const int visible = 4;
    if(sel_main_row_ < scroll_main_)
        scroll_main_ = sel_main_row_;
    if(sel_main_row_ >= scroll_main_ + visible)
        scroll_main_ = sel_main_row_ - visible + 1;
    scroll_main_
        = clampi_(scroll_main_, 0, (count > visible) ? (count - visible) : 0);

    const int y0 = 20;
    for(int i = 0; i < visible; i++)
    {
        const int idx = scroll_main_ + i;
        if(idx >= count)
            break;

        if(idx == 0)
        {
            drawRow_(y0 + i * 8, "Back to Menu", sel_main_row_ == idx, false);
            continue;
        }

        const int  chan = idx - 1;
        const int  vol  = backend_->GetChanVolume(chan);
        const int  pan  = backend_->GetChanPan(chan);
        const int  prog = backend_->GetChanProgram(chan);
        const bool act  = activity_ms_[chan] > 0;

        char row[24];
        snprintf(row,
                 sizeof(row),
                 "%2d V%03d P%03d Pr%03d %c",
                 chan + 1,
                 vol,
                 pan,
                 prog,
                 act ? '*' : '.');

        drawRow_(y0 + i * 8, row, sel_main_row_ == idx, false);
    }
}

void UiOled::drawFx_()
{
    display_.SetCursor(0, 0);
    display_.WriteString("FX", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    char r1[24];
    snprintf(r1,
             sizeof(r1),
             "Rev Time: %3d",
             (int)(backend_->GetFxReverbTime() * 100.0f));
    char r2[24];
    snprintf(
        r2, sizeof(r2), "Rev LPF: %5d", (int)backend_->GetFxReverbLpFreq());
    char r3[24];
    snprintf(
        r3, sizeof(r3), "Rev HPF: %5d", (int)backend_->GetFxReverbHpFreq());
    char r4[24];
    snprintf(r4,
             sizeof(r4),
             "Ch Depth: %3d",
             (int)(backend_->GetFxChorusDepth() * 100.0f));
    char r5[24];
    snprintf(r5,
             sizeof(r5),
             "Ch Spd : %3d",
             (int)(backend_->GetFxChorusSpeed() * 100.0f));

    const char* rows[]  = {"Back to Menu", r1, r2, r3, r4, r5};
    const int   count   = 6;
    const int   visible = 6;

    if(sel_fx_ < scroll_fx_)
        scroll_fx_ = sel_fx_;
    if(sel_fx_ >= scroll_fx_ + visible)
        scroll_fx_ = sel_fx_ - visible + 1;
    scroll_fx_
        = clampi_(scroll_fx_, 0, (count > visible) ? (count - visible) : 0);

    const int y0 = 12;
    for(int i = 0; i < visible; i++)
    {
        const int idx = scroll_fx_ + i;
        if(idx >= count)
            break;
        drawRow_(y0 + i * 8, rows[idx], sel_fx_ == idx, false);
    }
}
void UiOled::drawSystem_()
{
    display_.SetCursor(0, 0);
    display_.WriteString("System", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    char r1[32];
    snprintf(r1, sizeof(r1), "Sync: %s", syncToStr_(backend_->GetSyncMode()));
    char r2[32];
    snprintf(r2,
             sizeof(r2),
             "MIDI Thru: %s",
             backend_->GetMidiThru() ? "On" : "Off");
    char r3[32];
    snprintf(
        r3, sizeof(r3), "MIDI Sync: %s", triToStr_(backend_->GetMidiSync()));
    char r4[32];
    snprintf(r4,
             sizeof(r4),
             "Messages: %s",
             quadToStr_(backend_->GetMidiMessages()));
    char r5[32];
    snprintf(r5,
             sizeof(r5),
             "Prog Msg: %s",
             quadToStr_(backend_->GetMidiProgramMessages()));

    const char* rows[] = {"Back to Menu", r1, r2, r3, r4, r5};

    const int count   = 6;
    const int visible = 6;
    if(sel_system_ < scroll_system_)
        scroll_system_ = sel_system_;
    if(sel_system_ >= scroll_system_ + visible)
        scroll_system_ = sel_system_ - visible + 1;
    scroll_system_
        = clampi_(scroll_system_, 0, (count > visible) ? (count - visible) : 0);

    const int y0 = 12;
    for(int i = 0; i < visible; i++)
    {
        const int idx = scroll_system_ + i;
        if(idx >= count)
            break;
        drawRow_(y0 + i * 8, rows[idx], sel_system_ == idx, false);
    }
}

void UiOled::drawMidiFile_()
{
    display_.SetCursor(0, 0);
    display_.WriteString("MIDI File", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    const bool loop = backend_->GetLoop();

    char r1[32];
    snprintf(r1, sizeof(r1), "Load: /midi");
    char r2[32];
    snprintf(r2, sizeof(r2), "Save MIDI Settings");
    char r3[32];
    snprintf(r3, sizeof(r3), "BPM: %d", backend_->GetBpm());
    char r4[32];
    snprintf(r4, sizeof(r4), "Play: %s", backend_->GetPlay() ? "On" : "Off");
    char r5[32];
    snprintf(r5, sizeof(r5), "Loop: %s", loop ? "On" : "Off");
    char r6[32];
    int  lm = 1, lb = 1, ls = 1;
    backend_->GetLoopStartMbs(&lm, &lb, &ls);
    const char ls_field
        = (loop_start_field_ == LoopStartField::Measure
               ? 'M'
               : (loop_start_field_ == LoopStartField::Beat ? 'B' : 'S'));
    if(mode_ == Mode::Edit && sel_midifile_ == 6)
        snprintf(
            r6, sizeof(r6), "Loop Start: %d|%d|%d %c", lm, lb, ls, ls_field);
    else
        snprintf(r6, sizeof(r6), "Loop Start: %d|%d|%d", lm, lb, ls);
    char r7[32];
    snprintf(r7, sizeof(r7), "Loop Len: %d bt", backend_->GetLoopLengthBeats());
    const char* rows[] = {"Back to Menu", r1, r2, r3, r4, r5, r6, r7};

    const int count   = 8;
    const int visible = 6;
    if(sel_midifile_ < scroll_midifile_)
        scroll_midifile_ = sel_midifile_;
    if(sel_midifile_ >= scroll_midifile_ + visible)
        scroll_midifile_ = sel_midifile_ - visible + 1;
    scroll_midifile_ = clampi_(
        scroll_midifile_, 0, (count > visible) ? (count - visible) : 0);

    const int y0 = 12;
    for(int i = 0; i < visible; i++)
    {
        const int idx = scroll_midifile_ + i;
        if(idx >= count)
            break;
        int y = y0 + i * 8;

        const bool sel = sel_midifile_ == idx;
        const bool dim = (!loop && (idx == 6 || idx == 7));
        drawRow_(y, rows[idx], sel, dim);
    }
}

void UiOled::drawSoundFont_()
{
    display_.SetCursor(0, 0);
    display_.WriteString("SoundFont", Font_7x10, true);
    drawMode_(display_, 118, 0, mode_);

    const int   ch   = backend_->GetSfChannel();
    const char* name = backend_->GetSfTrackName(ch);
    if(!name)
        name = "";

    char r1[24];
    snprintf(r1, sizeof(r1), "Load SF2...");
    char r2[24];
    snprintf(r2, sizeof(r2), "Ch: %02d", ch);
    char r3[24];
    snprintf(r3, sizeof(r3), "Name: %.14s", name);
    char r4[24];
    snprintf(
        r4, sizeof(r4), "Mute: %s", backend_->GetSfMute(ch) ? "On" : "Off");
    char r5[24];
    snprintf(r5, sizeof(r5), "Vol: %03d", (int)backend_->GetSfVolume(ch));
    char r6[24];
    snprintf(r6, sizeof(r6), "Pan: %03d", (int)backend_->GetSfPan(ch));
    char r7[24];
    snprintf(
        r7, sizeof(r7), "RevSend:%03d", (int)backend_->GetSfReverbSend(ch));
    char r8[24];
    snprintf(
        r8, sizeof(r8), "ChoSend:%03d", (int)backend_->GetSfChorusSend(ch));
    char r9[24];
    snprintf(
        r9, sizeof(r9), "VelMod: %03d", (int)backend_->GetSfVelocityMod(ch));
    char r10[24];
    snprintf(
        r10, sizeof(r10), "PitchMod:%03d", (int)backend_->GetSfPitchMod(ch));
    char r11[24];
    snprintf(
        r11, sizeof(r11), "ModWheel:%03d", (int)backend_->GetSfModWheel(ch));
    char r12[24];
    snprintf(
        r12, sizeof(r12), "PitchBnd:%d", (int)backend_->GetSfPitchbend(ch));

    const char* rows[]
        = {"Back to Menu", r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12};
    const int count = 13;

    const int visible = 6;
    if(sel_soundfont_ < scroll_soundfont_)
        scroll_soundfont_ = sel_soundfont_;
    if(sel_soundfont_ >= scroll_soundfont_ + visible)
        scroll_soundfont_ = sel_soundfont_ - visible + 1;
    scroll_soundfont_ = clampi_(
        scroll_soundfont_, 0, (count > visible) ? (count - visible) : 0);

    const int y0 = 12;
    for(int r = 0; r < visible; r++)
    {
        const int idx = scroll_soundfont_ + r;
        display_.SetCursor(0, y0 + r * 8);
        drawRow_(y0 + r * 8, rows[idx], sel_soundfont_ == idx, false);
    }
}

void UiOled::drawPicker_()
{
    int count = 0;
    if(picker_kind_ == PickerKind::MidiFiles)
        count = backend_->GetMidiFileCount();
    else
        count = backend_->GetSoundFontCount();

    const int total   = count + 1; // +1 for Back
    const int visible = 6;

    picker_sel_ = clampi_(picker_sel_, 0, total - 1);
    if(picker_sel_ < picker_scroll_)
        picker_scroll_ = picker_sel_;
    if(picker_sel_ >= picker_scroll_ + visible)
        picker_scroll_ = picker_sel_ - visible + 1;
    picker_scroll_
        = clampi_(picker_scroll_, 0, (total > visible) ? (total - visible) : 0);

    display_.Fill(false);
    display_.SetCursor(0, 0);
    display_.WriteString(picker_kind_ == PickerKind::MidiFiles ? "Pick MIDI"
                                                               : "Pick SF2",
                         Font_7x10,
                         true);

    const int y0 = 12;
    for(int r = 0; r < visible; r++)
    {
        int idx = picker_scroll_ + r;
        if(idx >= total)
            break;

        char line[24];
        if(idx == 0)
        {
            snprintf(line, sizeof(line), "Back");
        }
        else
        {
            const int   file_idx = idx - 1;
            const char* nm       = (picker_kind_ == PickerKind::MidiFiles)
                                       ? backend_->GetMidiFileName(file_idx)
                                       : backend_->GetSoundFontName(file_idx);
            if(!nm)
                nm = "";
            snprintf(line, sizeof(line), "%.22s", nm);
        }

        display_.SetCursor(0, y0 + r * 8);
        display_.WriteString(idx == picker_sel_ ? ">" : " ", Font_6x8, true);
        display_.WriteString(line, Font_6x8, true);
    }
}

// ---- formatting/helpers ----
const char* UiOled::triToStr_(UiBackend::Tri v)
{
    switch(v)
    {
        case UiBackend::Tri::Off: return "Off";
        case UiBackend::Tri::Send: return "Send";
        case UiBackend::Tri::Receive: return "Recv";
        default: return "?";
    }
}

const char* UiOled::quadToStr_(UiBackend::Quad v)
{
    switch(v)
    {
        case UiBackend::Quad::Off: return "Off";
        case UiBackend::Quad::Send: return "Send";
        case UiBackend::Quad::Receive: return "Recv";
        case UiBackend::Quad::Both: return "Both";
        default: return "?";
    }
}

const char* UiOled::syncToStr_(UiBackend::SyncMode v)
{
    return (v == UiBackend::SyncMode::Internal) ? "Internal" : "External";
}

int UiOled::clampi_(int v, int lo, int hi)
{
    if(v < lo)
        return lo;
    if(v > hi)
        return hi;
    return v;
}

int UiOled::wrapi_(int v, int lo, int hi)
{
    if(hi < lo)
        return lo;
    const int range = hi - lo + 1;
    while(v < lo)
        v += range;
    while(v > hi)
        v -= range;
    return v;
}

uint8_t UiOled::clampu8_(int v)
{
    if(v < 0)
        return 0;
    if(v > 127)
        return 127;
    return (uint8_t)v;
}

} // namespace major_midi
