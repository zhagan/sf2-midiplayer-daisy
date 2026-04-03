#include "ui_controller.h"

#include <cmath>
#include <cstdio>

namespace major_midi
{

namespace
{
float MidiToNorm(uint8_t value)
{
    return static_cast<float>(value) / 127.0f;
}

int ClampInt(int value, int min_value, int max_value)
{
    if(value < min_value)
        return min_value;
    if(value > max_value)
        return max_value;
    return value;
}

size_t MainMenuItemCount()
{
    return 5;
}

size_t MenuPageItemCount(const AppState& state, const MediaLibrary& library)
{
    switch(state.menu_page)
    {
        case MenuPage::Main: return MainMenuItemCount();
        case MenuPage::Fx: return 6;
        case MenuPage::Song: return 6;
        case MenuPage::Sf2: return 8;
        case MenuPage::LoadMidi: return 1 + library.MidiCount();
        case MenuPage::LoadSf2: return 1 + library.SoundFontCount();
    }
    return 0;
}
} // namespace

void UiController::Init(AppState& state)
{
    state_ = &state;
    ResetKnobPickup();
}

void UiController::EnterMenu(uint32_t now_ms)
{
    state_->ui_mode          = UiMode::Menu;
    state_->menu_page        = MenuPage::Main;
    state_->menu_root_cursor = 0;
    state_->menu_editing     = false;
    ResetKnobPickup();
    SetOverlay(*state_, "Menu", now_ms, 500);
}

void UiController::ExitMenu(uint32_t now_ms)
{
    state_->ui_mode      = UiMode::Performance;
    state_->menu_page    = MenuPage::Main;
    state_->menu_editing = false;
    ResetKnobPickup();
    SetOverlay(*state_, "Performance", now_ms, 500);
}

void UiController::EnterMenuPage(MenuPage page, uint32_t now_ms)
{
    state_->ui_mode          = UiMode::MenuPage;
    state_->menu_page        = page;
    state_->menu_page_cursor = 0;
    state_->menu_editing     = false;
    ResetKnobPickup();
    SetOverlay(*state_, MenuPageName(page), now_ms, 500);
}

void UiController::ExitMenuPage(uint32_t now_ms)
{
    state_->ui_mode          = UiMode::Menu;
    state_->menu_page        = MenuPage::Main;
    state_->menu_page_cursor = 0;
    state_->menu_editing     = false;
    ResetKnobPickup();
    SetOverlay(*state_, "Menu", now_ms, 500);
}

bool UiController::HandleEvent(const UiEvent& event,
                               uint32_t       now_ms,
                               const MediaLibrary& library)
{
    if(state_ == nullptr)
        return false;

    switch(event.type)
    {
        case UiEventType::BankButtonPressed:
            if(state_->ui_mode == UiMode::Mute)
            {
                ToggleVisibleMute(event.index, now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::Menu || state_->ui_mode == UiMode::MenuPage)
                return true;
            SelectBank(event.index, now_ms);
            return true;

        case UiEventType::PlayButtonPressed:
            if(state_->ui_mode == UiMode::MenuPage)
            {
                ExitMenuPage(now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::Menu)
            {
                ExitMenu(now_ms);
                return true;
            }
            state_->transport_playing = !state_->transport_playing;
            SetOverlay(*state_,
                       state_->transport_playing ? "Play" : "Stop",
                       now_ms);
            return true;

        case UiEventType::ShiftComboPressed:
            if(event.index == 4)
            {
                ToggleMenu(now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::Menu || state_->ui_mode == UiMode::MenuPage)
                return true;
            if(state_->ui_mode == UiMode::Mute)
            {
                SelectBank(event.index, now_ms);
                SetMode(UiMode::Mute, now_ms, "Mute Mode");
                return true;
            }
            switch(event.index)
            {
                case 0: CycleKnobPage(now_ms); return true;
                case 1:
                    SetMode(state_->ui_mode == UiMode::Mute ? UiMode::Performance
                                                            : UiMode::Mute,
                            now_ms,
                            state_->ui_mode == UiMode::Mute ? "Mute Off"
                                                            : "Mute Mode");
                    return true;
                case 2:
                    state_->mute_all = !state_->mute_all;
                    SetOverlay(*state_,
                               state_->mute_all ? "Mute All On" : "Mute All Off",
                               now_ms);
                    return true;
                case 3:
                    SetMode(UiMode::LoopEdit, now_ms, "Loop Edit");
                    return true;
                default: return false;
            }

        case UiEventType::EncoderPressed:
            if(state_->ui_mode == UiMode::Menu)
            {
                ActivateMenuRoot(library, now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::MenuPage)
            {
                ActivateMenuPage(library, now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::LoopEdit)
            {
                SetMode(UiMode::Performance, now_ms, "Loop Edit Off");
                return true;
            }
            if(state_->ui_mode == UiMode::Mute)
            {
                SetMode(UiMode::Performance, now_ms, "Mute Off");
                return true;
            }
            return false;

        case UiEventType::EncoderLongPress:
            ToggleMenu(now_ms);
            return true;

        case UiEventType::EncoderTurn:
            if(state_->ui_mode == UiMode::Menu)
            {
                MoveMenuRootCursor(event.delta, now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::MenuPage)
            {
                if(state_->menu_editing)
                    AdjustMenuValue(event.delta, now_ms);
                else
                    MoveMenuPageCursor(event.delta, library, now_ms);
                return true;
            }
            if(state_->ui_mode != UiMode::LoopEdit)
            {
                state_->bpm = ClampInt(state_->bpm + event.delta, 20, 300);
                char text[24];
                std::snprintf(text, sizeof(text), "BPM %d", state_->bpm);
                SetOverlay(*state_, text, now_ms, 700);
                return true;
            }
            return false;

        case UiEventType::KnobMoved:
            if(state_->ui_mode == UiMode::Menu || state_->ui_mode == UiMode::MenuPage)
                return false;
            return HandleKnob(event.index, event.value, now_ms);
    }

    return false;
}

void UiController::ResetKnobPickup()
{
    for(bool& caught : knob_caught_)
        caught = false;
}

void UiController::SetMode(UiMode mode, uint32_t now_ms, const char* overlay)
{
    state_->ui_mode = mode;
    ResetKnobPickup();
    SetOverlay(*state_, overlay, now_ms);
}

void UiController::CycleKnobPage(uint32_t now_ms)
{
    const auto next = (static_cast<uint8_t>(state_->knob_page) + 1) % 4;
    state_->knob_page = static_cast<KnobPage>(next);
    ResetKnobPickup();
    SetOverlay(*state_, KnobPageName(state_->knob_page), now_ms);
}

void UiController::SelectBank(uint8_t bank, uint32_t now_ms)
{
    state_->bank = bank % 4;
    ResetKnobPickup();

    char text[24];
    std::snprintf(text,
                  sizeof(text),
                  "Bank %d-%d",
                  static_cast<int>(state_->bank) * 4 + 1,
                  static_cast<int>(state_->bank) * 4 + 4);
    SetOverlay(*state_, text, now_ms);
}

void UiController::ToggleVisibleMute(uint8_t slot, uint32_t now_ms)
{
    const int ch = VisibleChannelIndex(state_->bank, slot);
    if(ch < 0 || ch >= 16)
        return;

    state_->channels[ch].muted = !state_->channels[ch].muted;

    char text[24];
    std::snprintf(text,
                  sizeof(text),
                  "Ch %d %s",
                  ch + 1,
                  state_->channels[ch].muted ? "Muted" : "Unmuted");
    SetOverlay(*state_, text, now_ms);
}

void UiController::ToggleMenu(uint32_t now_ms)
{
    if(state_->ui_mode == UiMode::Menu || state_->ui_mode == UiMode::MenuPage)
    {
        ExitMenu(now_ms);
        return;
    }

    EnterMenu(now_ms);
}

void UiController::MoveMenuRootCursor(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    const int step = delta > 0 ? 1 : -1;
    int       next = static_cast<int>(state_->menu_root_cursor) + step;
    if(next < 0)
        next = static_cast<int>(MainMenuItemCount()) - 1;
    if(next >= static_cast<int>(MainMenuItemCount()))
        next = 0;

    state_->menu_root_cursor = static_cast<size_t>(next);
    SetOverlay(*state_, "Menu", now_ms, 250);
}

void UiController::MoveMenuPageCursor(int32_t           delta,
                                      const MediaLibrary& library,
                                      uint32_t           now_ms)
{
    const size_t count = MenuPageItemCount(*state_, library);
    if(count == 0 || delta == 0)
        return;

    const int step = delta > 0 ? 1 : -1;
    int       next = static_cast<int>(state_->menu_page_cursor) + step;
    if(next < 0)
        next = static_cast<int>(count) - 1;
    if(next >= static_cast<int>(count))
        next = 0;

    state_->menu_page_cursor = static_cast<size_t>(next);
    SetOverlay(*state_, MenuPageName(state_->menu_page), now_ms, 250);
}

void UiController::ActivateMenuRoot(const MediaLibrary&, uint32_t now_ms)
{
    switch(state_->menu_root_cursor)
    {
        case 0: EnterMenuPage(MenuPage::LoadMidi, now_ms); break;
        case 1: EnterMenuPage(MenuPage::LoadSf2, now_ms); break;
        case 2: EnterMenuPage(MenuPage::Fx, now_ms); break;
        case 3: EnterMenuPage(MenuPage::Song, now_ms); break;
        case 4: EnterMenuPage(MenuPage::Sf2, now_ms); break;
        default: break;
    }
}

void UiController::ActivateMenuPage(const MediaLibrary& library, uint32_t now_ms)
{
    if(state_->menu_editing)
    {
        state_->menu_editing = false;
        SetOverlay(*state_, "Done", now_ms, 300);
        return;
    }

    if(state_->menu_page_cursor == 0)
    {
        ExitMenuPage(now_ms);
        return;
    }

    switch(state_->menu_page)
    {
        case MenuPage::Fx:
            state_->menu_editing = true;
            SetOverlay(*state_, "Edit FX", now_ms, 300);
            break;

        case MenuPage::Song:
            if(state_->menu_page_cursor == 5)
            {
                state_->pending_save_settings = true;
                SetOverlay(*state_, "Save MIDI", now_ms);
            }
            else
            {
                state_->menu_editing = true;
                SetOverlay(*state_, "Edit Song", now_ms, 300);
            }
            break;

        case MenuPage::Sf2:
            state_->menu_editing = true;
            SetOverlay(*state_, "Edit SF2", now_ms, 300);
            break;

        case MenuPage::LoadMidi:
        {
            const size_t idx = state_->menu_page_cursor - 1;
            if(idx < library.MidiCount())
            {
                state_->selected_midi_index = idx;
                state_->pending_midi_load   = true;
                SetOverlay(*state_, "Load MIDI", now_ms);
            }
        }
        break;

        case MenuPage::LoadSf2:
        {
            const size_t idx = state_->menu_page_cursor - 1;
            if(idx < library.SoundFontCount())
            {
                state_->selected_sf2_index = idx;
                state_->pending_sf2_load   = true;
                SetOverlay(*state_, "Load SF2", now_ms);
            }
        }
        break;

        case MenuPage::Main: break;
    }
}

void UiController::AdjustMenuValue(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    switch(state_->menu_page)
    {
        case MenuPage::Fx:
            switch(state_->menu_page_cursor)
            {
                case 1:
                    state_->fx_reverb_time += delta > 0 ? 0.02f : -0.02f;
                    if(state_->fx_reverb_time < 0.0f)
                        state_->fx_reverb_time = 0.0f;
                    if(state_->fx_reverb_time > 1.0f)
                        state_->fx_reverb_time = 1.0f;
                    break;
                case 2:
                    state_->fx_reverb_lpf_hz += delta > 0 ? 200.0f : -200.0f;
                    if(state_->fx_reverb_lpf_hz < 200.0f)
                        state_->fx_reverb_lpf_hz = 200.0f;
                    if(state_->fx_reverb_lpf_hz > 18000.0f)
                        state_->fx_reverb_lpf_hz = 18000.0f;
                    break;
                case 3:
                    state_->fx_reverb_hpf_hz += delta > 0 ? 50.0f : -50.0f;
                    if(state_->fx_reverb_hpf_hz < 20.0f)
                        state_->fx_reverb_hpf_hz = 20.0f;
                    if(state_->fx_reverb_hpf_hz > 1000.0f)
                        state_->fx_reverb_hpf_hz = 1000.0f;
                    break;
                case 4:
                    state_->fx_chorus_depth += delta > 0 ? 0.02f : -0.02f;
                    if(state_->fx_chorus_depth < 0.0f)
                        state_->fx_chorus_depth = 0.0f;
                    if(state_->fx_chorus_depth > 1.0f)
                        state_->fx_chorus_depth = 1.0f;
                    break;
                case 5:
                    state_->fx_chorus_speed_hz += delta > 0 ? 0.05f : -0.05f;
                    if(state_->fx_chorus_speed_hz < 0.05f)
                        state_->fx_chorus_speed_hz = 0.05f;
                    if(state_->fx_chorus_speed_hz > 5.0f)
                        state_->fx_chorus_speed_hz = 5.0f;
                    break;
                default: return;
            }
            break;

        case MenuPage::Song:
            switch(state_->menu_page_cursor)
            {
                case 1:
                    if(state_->song_bpm_override == 0)
                        state_->song_bpm_override = static_cast<uint16_t>(state_->bpm);
                    state_->song_bpm_override
                        = static_cast<uint16_t>(ClampInt(static_cast<int>(state_->song_bpm_override)
                                                             + (delta > 0 ? 1 : -1),
                                                         20,
                                                         300));
                    break;
                case 2:
                    state_->song_loop_enabled = !state_->song_loop_enabled;
                    if(!state_->song_loop_enabled)
                        state_->loop_end_measure = state_->loop_start_measure;
                    else if(state_->loop_end_measure <= state_->loop_start_measure)
                        state_->loop_end_measure = state_->loop_start_measure + 1;
                    state_->loop_length_beats
                        = state_->song_loop_enabled
                              ? (state_->loop_end_measure - state_->loop_start_measure) * 4
                              : state_->loop_length_beats;
                    break;
                case 3:
                    state_->loop_start_measure
                        = ClampInt(state_->loop_start_measure + (delta > 0 ? 1 : -1), 1, 999);
                    if(state_->song_loop_enabled
                       && state_->loop_end_measure <= state_->loop_start_measure)
                        state_->loop_end_measure = state_->loop_start_measure + 1;
                    if(!state_->song_loop_enabled)
                        state_->loop_end_measure = state_->loop_start_measure;
                    state_->loop_length_beats
                        = state_->song_loop_enabled
                              ? (state_->loop_end_measure - state_->loop_start_measure) * 4
                              : state_->loop_length_beats;
                    break;
                case 4:
                    state_->loop_end_measure
                        = ClampInt(state_->loop_end_measure + (delta > 0 ? 1 : -1), 1, 999);
                    if(state_->loop_end_measure < state_->loop_start_measure)
                        state_->loop_end_measure = state_->loop_start_measure;
                    if(state_->song_loop_enabled
                       && state_->loop_end_measure == state_->loop_start_measure)
                        state_->loop_end_measure = state_->loop_start_measure + 1;
                    state_->loop_length_beats
                        = state_->song_loop_enabled
                              ? (state_->loop_end_measure - state_->loop_start_measure) * 4
                              : state_->loop_length_beats;
                    break;
                default: return;
            }
            break;

        case MenuPage::Sf2:
            switch(state_->menu_page_cursor)
            {
                case 1:
                    state_->sf2_channel
                        = static_cast<uint8_t>(ClampInt(static_cast<int>(state_->sf2_channel)
                                                            + (delta > 0 ? 1 : -1),
                                                        0,
                                                        15));
                    break;
                case 2:
                    state_->channels[state_->sf2_channel].muted
                        = !state_->channels[state_->sf2_channel].muted;
                    break;
                case 3:
                    state_->channels[state_->sf2_channel].volume
                        = static_cast<uint8_t>(
                            ClampInt(static_cast<int>(state_->channels[state_->sf2_channel].volume)
                                         + (delta > 0 ? 1 : -1),
                                     0,
                                     127));
                    break;
                case 4:
                    state_->channels[state_->sf2_channel].pan
                        = static_cast<uint8_t>(
                            ClampInt(static_cast<int>(state_->channels[state_->sf2_channel].pan)
                                         + (delta > 0 ? 1 : -1),
                                     0,
                                     127));
                    break;
                case 5:
                    state_->channels[state_->sf2_channel].reverb_send
                        = static_cast<uint8_t>(
                            ClampInt(static_cast<int>(state_->channels[state_->sf2_channel].reverb_send)
                                         + (delta > 0 ? 1 : -1),
                                     0,
                                     127));
                    break;
                case 6:
                    state_->channels[state_->sf2_channel].chorus_send
                        = static_cast<uint8_t>(
                            ClampInt(static_cast<int>(state_->channels[state_->sf2_channel].chorus_send)
                                         + (delta > 0 ? 1 : -1),
                                     0,
                                     127));
                    break;
                case 7:
                    state_->sf2_transpose
                        = static_cast<int8_t>(ClampInt(static_cast<int>(state_->sf2_transpose)
                                                           + (delta > 0 ? 1 : -1),
                                                       -24,
                                                       24));
                    break;
                default: return;
            }
            break;

        case MenuPage::Main:
        case MenuPage::LoadMidi:
        case MenuPage::LoadSf2: return;
    }

    state_->settings_dirty = true;
    SetOverlay(*state_, "Updated", now_ms, 250);
}

uint8_t UiController::NormToMidi(float value)
{
    if(value < 0.0f)
        value = 0.0f;
    if(value > 1.0f)
        value = 1.0f;
    return static_cast<uint8_t>(std::lround(value * 127.0f));
}

bool UiController::HandleKnob(uint8_t index, float value, uint32_t now_ms)
{
    if(index >= 4)
        return false;

    if(state_->ui_mode == UiMode::LoopEdit)
    {
        const int measure = 1 + static_cast<int>(std::lround(value * 63.0f));
        if(index == 0)
        {
            state_->loop_start_measure = measure;
            if(state_->loop_end_measure < state_->loop_start_measure)
                state_->loop_end_measure = state_->loop_start_measure;
            state_->song_loop_enabled = state_->loop_end_measure > state_->loop_start_measure;
            if(state_->song_loop_enabled)
                state_->loop_length_beats
                    = (state_->loop_end_measure - state_->loop_start_measure) * 4;
            SetOverlay(*state_, "Loop Start", now_ms, 600);
            return true;
        }
        if(index == 1)
        {
            state_->loop_end_measure = measure;
            if(state_->loop_end_measure < state_->loop_start_measure)
                state_->loop_end_measure = state_->loop_start_measure;
            state_->song_loop_enabled = state_->loop_end_measure > state_->loop_start_measure;
            if(state_->song_loop_enabled)
                state_->loop_length_beats
                    = (state_->loop_end_measure - state_->loop_start_measure) * 4;
            SetOverlay(*state_, "Loop End", now_ms, 600);
            return true;
        }
        return false;
    }

    const int ch = VisibleChannelIndex(state_->bank, index);
    if(ch < 0 || ch >= 16)
        return false;

    float target = 0.0f;
    switch(state_->knob_page)
    {
        case KnobPage::Volume: target = MidiToNorm(state_->channels[ch].volume); break;
        case KnobPage::Pan: target = MidiToNorm(state_->channels[ch].pan); break;
        case KnobPage::ReverbSend:
            target = MidiToNorm(state_->channels[ch].reverb_send);
            break;
        case KnobPage::ChorusSend:
            target = MidiToNorm(state_->channels[ch].chorus_send);
            break;
    }

    if(!knob_caught_[index])
    {
        if(std::fabs(value - target) > 0.06f)
            return false;
        knob_caught_[index] = true;
    }

    const uint8_t midi_value = NormToMidi(value);
    switch(state_->knob_page)
    {
        case KnobPage::Volume: state_->channels[ch].volume = midi_value; break;
        case KnobPage::Pan: state_->channels[ch].pan = midi_value; break;
        case KnobPage::ReverbSend:
            state_->channels[ch].reverb_send = midi_value;
            break;
        case KnobPage::ChorusSend:
            state_->channels[ch].chorus_send = midi_value;
            break;
    }

    return true;
}

} // namespace major_midi
