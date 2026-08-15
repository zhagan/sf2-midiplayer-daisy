#include "ui_controller.h"

#include <cmath>
#include <cstdio>

namespace major_midi
{

namespace
{
constexpr uint8_t  kLoopEditItemCount  = 7;
constexpr uint8_t  kInstrumentFocusItemCount = 6;

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
    return 9;
}

size_t MenuPageItemCount(const AppState& state, const MediaLibrary& library)
{
    switch(state.menu_page)
    {
        case MenuPage::Main: return MainMenuItemCount();
        case MenuPage::General: return 7;
        case MenuPage::Fx: return 7;
        case MenuPage::Song: return 9;
        case MenuPage::Sf2: return 9;
        case MenuPage::Midi: return 14;
        case MenuPage::CvGate: return CvGateVisibleItemCount(state.cv_gate);
        case MenuPage::LoadMidi: return library.MidiBrowserCount();
        case MenuPage::LoadSf2: return library.SoundFontBrowserCount();
        case MenuPage::SaveAllConfirm: return 2;
    }
    return 0;
}

const char* LoopEditItemName(LoopEditItem item)
{
    switch(item)
    {
        case LoopEditItem::Active: return "Loop Active";
        case LoopEditItem::StartMeasure: return "Loop Start Measure";
        case LoopEditItem::StartBeat: return "Loop Start Beat";
        case LoopEditItem::StartTick: return "Loop Start Tick";
        case LoopEditItem::LengthMeasures: return "Loop Length Measures";
        case LoopEditItem::LengthBeats: return "Loop Length Beats";
        case LoopEditItem::LengthTick: return "Loop Length Tick";
    }
    return "Loop";
}

const char* InstrumentFocusItemName(uint8_t item)
{
    switch(item)
    {
        case 0: return "Mute";
        case 1: return "Volume";
        case 2: return "Pan";
        case 3: return "Reverb";
        case 4: return "Chorus";
        case 5: return "Program";
    }
    return "Channel";
}

uint32_t LoopTicksPerBeat(const AppState& state)
{
    const int den = state.time_sig_den > 0 ? state.time_sig_den : 4;
    return state.song_divisions > 0 ? ((static_cast<uint32_t>(state.song_divisions) * 4u)
                                       / static_cast<uint32_t>(den))
                                    : 0u;
}

uint32_t LoopTicksPerMeasure(const AppState& state)
{
    return LoopTicksPerBeat(state) * static_cast<uint32_t>(state.time_sig_num > 0 ? state.time_sig_num : 4);
}

void SyncLoopFieldsFromTicks(AppState& state)
{
    if(state.loop_length_ticks < 1)
        state.loop_length_ticks = 1;

    const uint32_t ticks_per_beat    = LoopTicksPerBeat(state);
    const uint32_t ticks_per_measure = LoopTicksPerMeasure(state);

    if(ticks_per_beat == 0 || ticks_per_measure == 0)
    {
        state.loop_start_measure   = 1;
        state.loop_start_beat      = 1;
        state.loop_length_measures = 0;
        state.loop_length_beats    = 0;
        state.loop_end_tick        = state.loop_start_tick + state.loop_length_ticks;
        return;
    }

    state.loop_start_measure
        = static_cast<int>(state.loop_start_tick / ticks_per_measure) + 1;
    state.loop_start_beat
        = static_cast<int>((state.loop_start_tick % ticks_per_measure) / ticks_per_beat) + 1;

    const uint32_t total_beats = state.loop_length_ticks / ticks_per_beat;
    const uint32_t beats_per_measure
        = static_cast<uint32_t>(state.time_sig_num > 0 ? state.time_sig_num : 4);
    state.loop_length_measures = static_cast<int>(total_beats / beats_per_measure);
    state.loop_length_beats    = static_cast<int>(total_beats % beats_per_measure);
    state.loop_end_tick        = state.loop_start_tick + state.loop_length_ticks;
}

void SyncLoopTicksFromCoarseStart(AppState& state)
{
    const uint32_t ticks_per_beat    = LoopTicksPerBeat(state);
    const uint32_t ticks_per_measure = LoopTicksPerMeasure(state);
    if(ticks_per_beat == 0 || ticks_per_measure == 0)
        return;

    const int beats_per_measure = state.time_sig_num > 0 ? state.time_sig_num : 4;
    const int safe_measure      = state.loop_start_measure < 1 ? 1 : state.loop_start_measure;
    const int safe_beat         = ClampInt(state.loop_start_beat, 1, beats_per_measure);
    state.loop_start_measure    = safe_measure;
    state.loop_start_beat       = safe_beat;
    state.loop_start_tick       = static_cast<uint32_t>(safe_measure - 1) * ticks_per_measure
                            + static_cast<uint32_t>(safe_beat - 1) * ticks_per_beat;
    state.loop_end_tick         = state.loop_start_tick + state.loop_length_ticks;
}

void SyncLoopTicksFromCoarseLength(AppState& state)
{
    const uint32_t ticks_per_beat = LoopTicksPerBeat(state);
    if(ticks_per_beat == 0)
        return;

    const int beats_per_measure = state.time_sig_num > 0 ? state.time_sig_num : 4;
    state.loop_length_measures  = state.loop_length_measures < 0 ? 0 : state.loop_length_measures;
    state.loop_length_beats     = ClampInt(state.loop_length_beats, 0, beats_per_measure - 1);

    uint32_t total_beats = static_cast<uint32_t>(state.loop_length_measures)
                           * static_cast<uint32_t>(beats_per_measure)
                           + static_cast<uint32_t>(state.loop_length_beats);
    if(total_beats == 0)
        total_beats = 1;
    state.loop_length_ticks = total_beats * ticks_per_beat;
    state.loop_end_tick     = state.loop_start_tick + state.loop_length_ticks;
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

void UiController::HandlePerformanceBankButton(uint8_t bank, uint32_t now_ms)
{
    if(state_->knob_page == KnobPage::Mute)
    {
        ToggleVisibleMute(bank, now_ms);
        return;
    }

    SelectBank(bank, now_ms);
}

void UiController::NormalizeLoopState()
{
    SyncLoopFieldsFromTicks(*state_);
}

bool UiController::HandleEvent(const UiEvent& event,
                               uint32_t       now_ms,
                               MediaLibrary&  library)
{
    if(state_ == nullptr)
        return false;

    switch(event.type)
    {
        case UiEventType::BankButtonPressed:
            if(state_->ui_mode == UiMode::Menu || state_->ui_mode == UiMode::MenuPage
               || state_->ui_mode == UiMode::LoopEdit || state_->ui_mode == UiMode::MidiMonitor
               || state_->ui_mode == UiMode::SongInfo)
                return true;
            if(event.index < 4)
                bank_before_press_[event.index] = state_->bank;
            HandlePerformanceBankButton(event.index, now_ms);
            return true;

        case UiEventType::BankComboPressed:
            if(state_->ui_mode == UiMode::Menu || state_->ui_mode == UiMode::MenuPage)
                return true;
            if(event.index == 1)
            {
                SetMode(UiMode::MidiMonitor, now_ms, "MIDI Monitor");
                return true;
            }
            if(event.index == 2)
            {
                SetMode(UiMode::SongInfo, now_ms, "Transport");
                return true;
            }
            if(event.index == 3)
            {
                state_->loop_edit_cursor = LoopEditItem::Active;
                state_->loop_editing     = false;
                NormalizeLoopState();
                SetMode(UiMode::LoopEdit, now_ms, "Loop Edit");
                return true;
            }
            return true;

        case UiEventType::BankButtonLongPress:
            if(state_->ui_mode == UiMode::Performance
               && state_->knob_page != KnobPage::Mute
               && event.index < 4)
            {
                const uint8_t bank = bank_before_press_[event.index];
                const int     ch   = VisibleChannelIndex(bank, event.index);
                if(ch >= 0 && ch < 16)
                {
                    state_->bank                    = bank;
                    state_->sf2_channel             = static_cast<uint8_t>(ch);
                    state_->instrument_focus_active = true;
                    state_->instrument_focus_cursor = 0;
                    state_->instrument_focus_editing = false;
                    char text[24];
                    std::snprintf(text, sizeof(text), "Ch %d Focus", ch + 1);
                    SetOverlay(*state_, text, now_ms);
                }
            }
            return true;

        case UiEventType::PlayButtonPressed:
            if(state_->ui_mode == UiMode::LoopEdit)
            {
                SetMode(UiMode::Performance, now_ms, "Loop Edit Off");
                return true;
            }
            if(state_->ui_mode == UiMode::MidiMonitor)
            {
                for(auto& channel : state_->midi_monitor_channels)
                    channel = MidiMonitorChannelState{};
                SetOverlay(*state_, "Monitor Cleared", now_ms, 500);
                return true;
            }
            if(state_->ui_mode == UiMode::SongInfo)
            {
                SetMode(UiMode::Performance, now_ms, "Song Info Off");
                return true;
            }
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
            if(event.index <= 3)
            {
                SelectBank(event.index, now_ms);
                return true;
            }
            return false;

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
                state_->loop_editing = !state_->loop_editing;
                SetOverlay(*state_,
                           state_->loop_editing ? LoopEditItemName(state_->loop_edit_cursor)
                                                : "Loop Select",
                           now_ms,
                           500);
                return true;
            }
            if(state_->ui_mode == UiMode::Mute)
            {
                SetMode(UiMode::Performance, now_ms, "Mute Off");
                return true;
            }
            if(state_->ui_mode == UiMode::MidiMonitor)
            {
                SetMode(UiMode::Performance, now_ms, "Monitor Off");
                return true;
            }
            if(state_->ui_mode == UiMode::SongInfo)
            {
                SetMode(UiMode::Performance, now_ms, "Song Info Off");
                return true;
            }
            if(state_->ui_mode == UiMode::Performance
               && state_->instrument_focus_active)
            {
                state_->instrument_focus_editing = !state_->instrument_focus_editing;
                SetOverlay(*state_,
                           state_->instrument_focus_editing
                               ? InstrumentFocusItemName(state_->instrument_focus_cursor)
                               : "Focus Select",
                           now_ms,
                           500);
                return true;
            }
            if(state_->ui_mode == UiMode::Performance
               && state_->knob_page == KnobPage::Bpm)
            {
                state_->bpm_editing = !state_->bpm_editing;
                SetOverlay(*state_,
                           state_->bpm_editing ? "Edit BPM" : "BPM Locked",
                           now_ms,
                           500);
                return true;
            }
            return false;

        case UiEventType::EncoderLongPress:
            if(state_->ui_mode == UiMode::LoopEdit)
            {
                state_->loop_editing = false;
                SetMode(UiMode::Performance, now_ms, "Loop Edit Off");
                return true;
            }
            if(state_->ui_mode == UiMode::Performance
               && state_->instrument_focus_active)
            {
                ExitInstrumentFocus(now_ms, "Bank View");
                return true;
            }
            if(state_->ui_mode == UiMode::MidiMonitor)
            {
                SetMode(UiMode::Performance, now_ms, "Monitor Off");
                return true;
            }
            if(state_->ui_mode == UiMode::SongInfo)
            {
                SetMode(UiMode::Performance, now_ms, "Song Info Off");
                return true;
            }
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
            if(state_->ui_mode == UiMode::LoopEdit)
            {
                if(state_->loop_editing)
                    AdjustLoopEditValue(event.delta, now_ms);
                else
                    MoveLoopEditCursor(event.delta, now_ms);
                return true;
            }
            if(state_->ui_mode == UiMode::MidiMonitor)
            {
                const int next = ClampInt(static_cast<int>(state_->midi_monitor_scroll)
                                              + (event.delta > 0 ? 1 : -1),
                                          0,
                                          11);
                state_->midi_monitor_scroll = static_cast<uint8_t>(next);
                return true;
            }
            if(state_->ui_mode == UiMode::SongInfo)
                return true;
            if(state_->ui_mode == UiMode::Performance)
            {
                if(state_->instrument_focus_active)
                {
                    if(state_->instrument_focus_editing)
                        AdjustInstrumentFocusValue(event.delta, now_ms);
                    else
                        MoveInstrumentFocusCursor(event.delta, now_ms);
                }
                else if(state_->knob_page == KnobPage::Bpm && state_->bpm_editing)
                {
                    state_->bpm = ClampInt(state_->bpm + event.delta, 20, 300);
                    char text[24];
                    std::snprintf(text, sizeof(text), "BPM %d", state_->bpm);
                    SetOverlay(*state_, text, now_ms, 700);
                }
                else
                {
                    CycleKnobPage(event.delta, now_ms);
                }
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

void UiController::CycleKnobPage(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    const int count = 7;
    int next = static_cast<int>(state_->knob_page) + (delta > 0 ? 1 : -1);
    if(next < 0)
        next = count - 1;
    if(next >= count)
        next = 0;
    state_->knob_page   = static_cast<KnobPage>(next);
    state_->bpm_editing = false;
    ResetKnobPickup();
    SetOverlay(*state_, KnobPageName(state_->knob_page), now_ms);
}

void UiController::SelectBank(uint8_t bank, uint32_t now_ms)
{
    state_->bank = bank % 4;
    state_->bpm_editing             = false;
    state_->instrument_focus_active = false;
    state_->instrument_focus_editing = false;
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

void UiController::ExitInstrumentFocus(uint32_t now_ms, const char* overlay)
{
    state_->instrument_focus_active  = false;
    state_->instrument_focus_editing = false;
    SetOverlay(*state_, overlay, now_ms, 500);
}

void UiController::MoveInstrumentFocusCursor(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    int next = static_cast<int>(state_->instrument_focus_cursor) + (delta > 0 ? 1 : -1);
    if(next < 0)
        next = kInstrumentFocusItemCount - 1;
    if(next >= kInstrumentFocusItemCount)
        next = 0;

    state_->instrument_focus_cursor = static_cast<uint8_t>(next);
    SetOverlay(*state_,
               InstrumentFocusItemName(state_->instrument_focus_cursor),
               now_ms,
               300);
}

void UiController::AdjustInstrumentFocusValue(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    ChannelState& channel = state_->channels[state_->sf2_channel];
    switch(state_->instrument_focus_cursor)
    {
        case 0:
            channel.muted = delta > 0 ? true : false;
            break;
        case 1:
            channel.volume = static_cast<uint8_t>(
                ClampInt(static_cast<int>(channel.volume) + (delta > 0 ? 1 : -1),
                         0,
                         127));
            break;
        case 2:
            channel.pan = static_cast<uint8_t>(
                ClampInt(static_cast<int>(channel.pan) + (delta > 0 ? 1 : -1), 0, 127));
            break;
        case 3:
            channel.reverb_send = static_cast<uint8_t>(
                ClampInt(static_cast<int>(channel.reverb_send) + (delta > 0 ? 1 : -1),
                         0,
                         127));
            break;
        case 4:
            channel.chorus_send = static_cast<uint8_t>(
                ClampInt(static_cast<int>(channel.chorus_send) + (delta > 0 ? 1 : -1),
                         0,
                         127));
            break;
        case 5:
            channel.program_override = static_cast<int8_t>(
                ClampInt(static_cast<int>(channel.program_override) + (delta > 0 ? 1 : -1),
                         -1,
                         127));
            break;
        default: return;
    }

    state_->settings_dirty = true;
    SetOverlay(*state_,
               InstrumentFocusItemName(state_->instrument_focus_cursor),
               now_ms,
               300);
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

void UiController::MoveLoopEditCursor(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    int next = static_cast<int>(state_->loop_edit_cursor) + (delta > 0 ? 1 : -1);
    if(next < 0)
        next = kLoopEditItemCount - 1;
    if(next >= kLoopEditItemCount)
        next = 0;

    state_->loop_edit_cursor = static_cast<LoopEditItem>(next);
    SetOverlay(*state_, LoopEditItemName(state_->loop_edit_cursor), now_ms, 300);
}

void UiController::AdjustLoopEditValue(int32_t delta, uint32_t now_ms)
{
    if(delta == 0)
        return;

    switch(state_->loop_edit_cursor)
    {
        case LoopEditItem::Active:
            state_->song_loop_enabled = delta > 0;
            break;
        case LoopEditItem::StartMeasure:
            state_->loop_start_measure
                = ClampInt(state_->loop_start_measure + (delta > 0 ? 1 : -1), 1, 9999);
            SyncLoopTicksFromCoarseStart(*state_);
            break;
        case LoopEditItem::StartBeat:
            state_->loop_start_beat
                = ClampInt(state_->loop_start_beat + (delta > 0 ? 1 : -1),
                           1,
                           state_->time_sig_num > 0 ? state_->time_sig_num : 4);
            SyncLoopTicksFromCoarseStart(*state_);
            break;
        case LoopEditItem::StartTick:
            state_->loop_start_tick = static_cast<uint32_t>(
                ClampInt(static_cast<int>(state_->loop_start_tick) + (delta > 0 ? 1 : -1),
                         0,
                         2000000000));
            break;
        case LoopEditItem::LengthMeasures:
            state_->loop_length_measures
                = ClampInt(state_->loop_length_measures + (delta > 0 ? 1 : -1), 0, 9999);
            SyncLoopTicksFromCoarseLength(*state_);
            break;
        case LoopEditItem::LengthBeats:
            state_->loop_length_beats
                = ClampInt(state_->loop_length_beats + (delta > 0 ? 1 : -1),
                           0,
                           (state_->time_sig_num > 0 ? state_->time_sig_num : 4) - 1);
            SyncLoopTicksFromCoarseLength(*state_);
            break;
        case LoopEditItem::LengthTick:
            state_->loop_length_ticks = static_cast<uint32_t>(
                ClampInt(static_cast<int>(state_->loop_length_ticks) + (delta > 0 ? 1 : -1),
                         1,
                         2000000000));
            break;
    }

    NormalizeLoopState();
    state_->settings_dirty = true;
    SetOverlay(*state_, LoopEditItemName(state_->loop_edit_cursor), now_ms, 300);
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

void UiController::ActivateMenuRoot(MediaLibrary& library, uint32_t now_ms)
{
    switch(state_->menu_root_cursor)
    {
        case 0:
            library.ResetMidiBrowser();
            EnterMenuPage(MenuPage::LoadMidi, now_ms);
            break;
        case 1:
            library.ResetSoundFontBrowser();
            EnterMenuPage(MenuPage::LoadSf2, now_ms);
            break;
        case 2: EnterMenuPage(MenuPage::General, now_ms); break;
        case 3: EnterMenuPage(MenuPage::Fx, now_ms); break;
        case 4: EnterMenuPage(MenuPage::Song, now_ms); break;
        case 5: EnterMenuPage(MenuPage::Sf2, now_ms); break;
        case 6: EnterMenuPage(MenuPage::Midi, now_ms); break;
        case 7: EnterMenuPage(MenuPage::CvGate, now_ms); break;
        case 8: EnterMenuPage(MenuPage::SaveAllConfirm, now_ms); break;
        default: break;
    }
}

void UiController::ActivateMenuPage(MediaLibrary& library, uint32_t now_ms)
{
    if(state_->menu_editing)
    {
        state_->menu_editing = false;
        SetOverlay(*state_, "Done", now_ms, 300);
        return;
    }

    switch(state_->menu_page)
    {
        case MenuPage::General:
            state_->menu_editing = true;
            SetOverlay(*state_, "Edit General", now_ms, 300);
            break;

        case MenuPage::Fx:
            state_->menu_editing = true;
            SetOverlay(*state_, "Edit FX", now_ms, 300);
            break;

        case MenuPage::Song:
            if(state_->menu_page_cursor == 8)
            {
                state_->pending_save_settings = true;
                SetOverlay(*state_, "Save Song", now_ms);
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

        case MenuPage::Midi:
            state_->menu_editing = true;
            SetOverlay(*state_, "Edit MIDI", now_ms, 300);
            break;

        case MenuPage::CvGate:
            state_->menu_editing = true;
            SetOverlay(*state_, "Edit CV/Gate", now_ms, 300);
            break;

        case MenuPage::LoadMidi:
        {
            if(library.MidiBrowserSelect(state_->menu_page_cursor,
                                         state_->selected_midi_path,
                                         sizeof(state_->selected_midi_path)))
            {
                state_->pending_midi_load = true;
                SetOverlay(*state_, "Load MIDI", now_ms);
            }
            else
            {
                state_->menu_page_cursor = 0;
                SetOverlay(*state_, "Browse MIDI", now_ms, 300);
            }
        }
        break;

        case MenuPage::LoadSf2:
        {
            if(library.SoundFontBrowserSelect(state_->menu_page_cursor,
                                              state_->selected_sf2_path,
                                              sizeof(state_->selected_sf2_path)))
            {
                state_->pending_sf2_load = true;
                SetOverlay(*state_, "Load SF2", now_ms);
            }
            else
            {
                state_->menu_page_cursor = 0;
                SetOverlay(*state_, "Browse SF2", now_ms, 300);
            }
        }
        break;

        case MenuPage::SaveAllConfirm:
            if(state_->menu_page_cursor == 0)
            {
                state_->pending_save_all = true;
                SetOverlay(*state_, "Saving...", now_ms, 500);
            }
            else
            {
                ExitMenuPage(now_ms);
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
        case MenuPage::General:
            switch(state_->menu_page_cursor)
            {
                case 0:
                {
                    static constexpr uint16_t kSaverValues[] = {0, 10, 30, 60, 120, 300, 600, 1800, 3600};
                    size_t idx = 0;
                    while(idx + 1 < (sizeof(kSaverValues) / sizeof(kSaverValues[0]))
                          && kSaverValues[idx] != state_->screen_saver_timeout_s)
                        idx++;
                    if(delta > 0)
                        idx = idx + 1 < (sizeof(kSaverValues) / sizeof(kSaverValues[0])) ? (idx + 1) : idx;
                    else
                        idx = idx > 0 ? (idx - 1) : 0;
                    state_->screen_saver_timeout_s = kSaverValues[idx];
                }
                break;
                case 1:
                    state_->knob_pickup_mode = state_->knob_pickup_mode == KnobPickupMode::Pickup
                                                   ? KnobPickupMode::Jump
                                                   : KnobPickupMode::Pickup;
                    ResetKnobPickup();
                    break;
                case 2:
                    state_->encoder_direction
                        = state_->encoder_direction == EncoderDirection::Normal
                              ? EncoderDirection::Reversed
                              : EncoderDirection::Normal;
                    break;
                case 3:
                    state_->oled_x_offset = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(state_->oled_x_offset) + delta, 0, 8));
                    break;
                case 4:
                    state_->cv1_pitch_scale = static_cast<uint16_t>(
                        ClampInt(static_cast<int>(state_->cv1_pitch_scale)
                                     + (delta > 0 ? 1 : -1),
                                 900,
                                 1100));
                    break;
                case 5:
                    state_->cv2_pitch_scale = static_cast<uint16_t>(
                        ClampInt(static_cast<int>(state_->cv2_pitch_scale)
                                     + (delta > 0 ? 1 : -1),
                                 900,
                                 1100));
                    break;
                case 6:
                    state_->master_volume_pct = static_cast<uint16_t>(
                        ClampInt(static_cast<int>(state_->master_volume_pct)
                                     + (delta > 0 ? 5 : -5),
                                 0,
                                 200));
                    break;
                default: return;
            }
            break;

        case MenuPage::Fx:
            switch(state_->menu_page_cursor)
            {
                case 0:
                    state_->fx_reverb_enabled = !state_->fx_reverb_enabled;
                    break;
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
                    state_->fx_chorus_enabled = !state_->fx_chorus_enabled;
                    break;
                case 5:
                    state_->fx_chorus_depth += delta > 0 ? 0.02f : -0.02f;
                    if(state_->fx_chorus_depth < 0.0f)
                        state_->fx_chorus_depth = 0.0f;
                    if(state_->fx_chorus_depth > 1.0f)
                        state_->fx_chorus_depth = 1.0f;
                    break;
                case 6:
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
                case 0:
                    if(state_->song_bpm_override == 0)
                        state_->song_bpm_override = static_cast<uint16_t>(state_->bpm);
                    state_->song_bpm_override
                        = static_cast<uint16_t>(ClampInt(static_cast<int>(state_->song_bpm_override)
                                                             + (delta > 0 ? 1 : -1),
                                                         20,
                                                         300));
                    break;
                case 1:
                    state_->song_loop_enabled = !state_->song_loop_enabled;
                    NormalizeLoopState();
                    break;
                case 2:
                    state_->loop_start_measure
                        = ClampInt(state_->loop_start_measure + (delta > 0 ? 1 : -1), 1, 9999);
                    SyncLoopTicksFromCoarseStart(*state_);
                    NormalizeLoopState();
                    break;
                case 3:
                    state_->loop_start_beat
                        = ClampInt(state_->loop_start_beat + (delta > 0 ? 1 : -1),
                                   1,
                                   state_->time_sig_num > 0 ? state_->time_sig_num : 4);
                    SyncLoopTicksFromCoarseStart(*state_);
                    NormalizeLoopState();
                    break;
                case 4:
                    state_->loop_start_tick = static_cast<uint32_t>(
                        ClampInt(static_cast<int>(state_->loop_start_tick) + (delta > 0 ? 1 : -1),
                                 0,
                                 2000000000));
                    NormalizeLoopState();
                    break;
                case 5:
                    state_->loop_length_measures
                        = ClampInt(state_->loop_length_measures + (delta > 0 ? 1 : -1), 0, 9999);
                    SyncLoopTicksFromCoarseLength(*state_);
                    NormalizeLoopState();
                    break;
                case 6:
                    state_->loop_length_beats
                        = ClampInt(state_->loop_length_beats + (delta > 0 ? 1 : -1),
                                   0,
                                   (state_->time_sig_num > 0 ? state_->time_sig_num : 4) - 1);
                    SyncLoopTicksFromCoarseLength(*state_);
                    NormalizeLoopState();
                    break;
                case 7:
                    state_->loop_length_ticks = static_cast<uint32_t>(
                        ClampInt(static_cast<int>(state_->loop_length_ticks) + (delta > 0 ? 1 : -1),
                                 1,
                                 2000000000));
                    NormalizeLoopState();
                    break;
                default: return;
            }
            break;

        case MenuPage::Sf2:
            switch(state_->menu_page_cursor)
            {
                case 0:
                    state_->sf2_max_voices
                        = static_cast<uint8_t>(ClampInt(static_cast<int>(state_->sf2_max_voices)
                                                           + (delta > 0 ? 1 : -1),
                                                       0,
                                                       32));
                    break;
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
                    state_->channels[state_->sf2_channel].program_override
                        = static_cast<int8_t>(
                            ClampInt(static_cast<int>(state_->channels[state_->sf2_channel].program_override)
                                         + (delta > 0 ? 1 : -1),
                                     -1,
                                     127));
                    break;
                case 8:
                    state_->sf2_transpose
                        = static_cast<int8_t>(ClampInt(static_cast<int>(state_->sf2_transpose)
                                                           + (delta > 0 ? 1 : -1),
                                                       -24,
                                                       24));
                    break;
                default: return;
            }
            break;

        case MenuPage::Midi:
        {
            MidiOutputRouting* port_routing
                = state_->midi_menu_port == MidiOutputPort::Usb ? &state_->midi_routing.usb
                                                                : &state_->midi_routing.uart;
            MidiChannelOutputRouting& channel_routing
                = port_routing->channels[state_->midi_menu_channel];
            switch(static_cast<MidiSettingsMenuItem>(state_->menu_page_cursor))
            {
                case MidiSettingsMenuItem::UsbMode:
                    port_routing = &state_->midi_routing.usb;
                    port_routing->mode = static_cast<MidiOutputMode>(
                        ClampInt(static_cast<int>(port_routing->mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(MidiOutputMode::Matrix)));
                    break;
                case MidiSettingsMenuItem::UartMode:
                    port_routing = &state_->midi_routing.uart;
                    port_routing->mode = static_cast<MidiOutputMode>(
                        ClampInt(static_cast<int>(port_routing->mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(MidiOutputMode::Matrix)));
                    break;
                case MidiSettingsMenuItem::MatrixPort:
                    state_->midi_menu_port = state_->midi_menu_port == MidiOutputPort::Usb
                                                 ? MidiOutputPort::Uart
                                                 : MidiOutputPort::Usb;
                    break;
                case MidiSettingsMenuItem::MatrixSourceChannel:
                    state_->midi_menu_channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(state_->midi_menu_channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case MidiSettingsMenuItem::MatrixDestChannel:
                    channel_routing.destination_channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(channel_routing.destination_channel)
                                     + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case MidiSettingsMenuItem::MatrixNotes:
                    channel_routing.notes = !channel_routing.notes;
                    break;
                case MidiSettingsMenuItem::MatrixCcs:
                    channel_routing.ccs = !channel_routing.ccs;
                    break;
                case MidiSettingsMenuItem::MatrixPrograms:
                    channel_routing.programs = !channel_routing.programs;
                    break;
                case MidiSettingsMenuItem::UsbTransport:
                    state_->midi_routing.usb.transport = !state_->midi_routing.usb.transport;
                    break;
                case MidiSettingsMenuItem::UsbClock:
                    state_->midi_routing.usb.clock = !state_->midi_routing.usb.clock;
                    break;
                case MidiSettingsMenuItem::UartTransport:
                    state_->midi_routing.uart.transport = !state_->midi_routing.uart.transport;
                    break;
                case MidiSettingsMenuItem::UartClock:
                    state_->midi_routing.uart.clock = !state_->midi_routing.uart.clock;
                    break;
                case MidiSettingsMenuItem::UsbInToUart:
                    state_->midi_routing.usb_in_to_uart = !state_->midi_routing.usb_in_to_uart;
                    break;
                case MidiSettingsMenuItem::UartInToUsb:
                    state_->midi_routing.uart_in_to_usb = !state_->midi_routing.uart_in_to_usb;
                    break;
                default: return;
            }
            state_->midi_routing_dirty = true;
            break;
        }

        case MenuPage::CvGate:
        {
            auto& cv_gate = state_->cv_gate;
            switch(CvGateVisibleItemAt(cv_gate, state_->menu_page_cursor))
            {
                case CvGateMenuItem::Cv1Mode:
                    cv_gate.cv_in[0].mode = static_cast<CvInMode>(
                        ClampInt(static_cast<int>(cv_gate.cv_in[0].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(CvInMode::NotePitch)));
                    break;
                case CvGateMenuItem::Cv1Channel:
                    cv_gate.cv_in[0].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_in[0].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::Cv1Cc:
                    cv_gate.cv_in[0].cc = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_in[0].cc) + (delta > 0 ? 1 : -1),
                                 0,
                                 127));
                    break;
                case CvGateMenuItem::Cv2Mode:
                    cv_gate.cv_in[1].mode = static_cast<CvInMode>(
                        ClampInt(static_cast<int>(cv_gate.cv_in[1].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(CvInMode::NotePitch)));
                    break;
                case CvGateMenuItem::Cv2Channel:
                    cv_gate.cv_in[1].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_in[1].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::Cv2Cc:
                    cv_gate.cv_in[1].cc = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_in[1].cc) + (delta > 0 ? 1 : -1),
                                 0,
                                 127));
                    break;
                case CvGateMenuItem::GateIn1Mode:
                    cv_gate.gate_in[0].mode = static_cast<GateInMode>(
                        ClampInt(static_cast<int>(cv_gate.gate_in[0].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(GateInMode::NoteTrigger)));
                    break;
                case CvGateMenuItem::GateIn1Channel:
                    cv_gate.gate_in[0].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.gate_in[0].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::GateIn2Mode:
                    cv_gate.gate_in[1].mode = static_cast<GateInMode>(
                        ClampInt(static_cast<int>(cv_gate.gate_in[1].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(GateInMode::NoteTrigger)));
                    break;
                case CvGateMenuItem::GateIn2Channel:
                    cv_gate.gate_in[1].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.gate_in[1].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::Gate1Mode:
                    cv_gate.gate_out[0].mode = static_cast<GateOutMode>(
                        ClampInt(static_cast<int>(cv_gate.gate_out[0].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(GateOutMode::ChannelGate)));
                    break;
                case CvGateMenuItem::Gate1Channel:
                    cv_gate.gate_out[0].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.gate_out[0].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::Gate1Trigger:
                    cv_gate.gate_out[0].trigger_mode
                        = cv_gate.gate_out[0].trigger_mode == GateTriggerMode::Legato
                              ? GateTriggerMode::Retrig
                              : GateTriggerMode::Legato;
                    break;
                case CvGateMenuItem::Gate1Resolution:
                    cv_gate.gate_out[0].sync_resolution = static_cast<SyncResolution>(
                        ClampInt(static_cast<int>(cv_gate.gate_out[0].sync_resolution)
                                     + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(SyncResolution::Div64)));
                    break;
                case CvGateMenuItem::Gate2Mode:
                    cv_gate.gate_out[1].mode = static_cast<GateOutMode>(
                        ClampInt(static_cast<int>(cv_gate.gate_out[1].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(GateOutMode::ChannelGate)));
                    break;
                case CvGateMenuItem::Gate2Channel:
                    cv_gate.gate_out[1].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.gate_out[1].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::Gate2Trigger:
                    cv_gate.gate_out[1].trigger_mode
                        = cv_gate.gate_out[1].trigger_mode == GateTriggerMode::Legato
                              ? GateTriggerMode::Retrig
                              : GateTriggerMode::Legato;
                    break;
                case CvGateMenuItem::Gate2Resolution:
                    cv_gate.gate_out[1].sync_resolution = static_cast<SyncResolution>(
                        ClampInt(static_cast<int>(cv_gate.gate_out[1].sync_resolution)
                                     + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(SyncResolution::Div64)));
                    break;
                case CvGateMenuItem::CvOut1Mode:
                    cv_gate.cv_out[0].mode = static_cast<CvOutMode>(
                        ClampInt(static_cast<int>(cv_gate.cv_out[0].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(CvOutMode::ChannelCc)));
                    break;
                case CvGateMenuItem::CvOut1Channel:
                    cv_gate.cv_out[0].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_out[0].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::CvOut1Cc:
                    cv_gate.cv_out[0].cc = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_out[0].cc) + (delta > 0 ? 1 : -1),
                                 0,
                                 127));
                    break;
                case CvGateMenuItem::CvOut1Priority:
                    cv_gate.cv_out[0].priority = cv_gate.cv_out[0].priority == NotePriority::Highest
                                                     ? NotePriority::Lowest
                                                     : NotePriority::Highest;
                    break;
                case CvGateMenuItem::CvOut2Mode:
                    cv_gate.cv_out[1].mode = static_cast<CvOutMode>(
                        ClampInt(static_cast<int>(cv_gate.cv_out[1].mode) + (delta > 0 ? 1 : -1),
                                 0,
                                 static_cast<int>(CvOutMode::ChannelCc)));
                    break;
                case CvGateMenuItem::CvOut2Channel:
                    cv_gate.cv_out[1].channel = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_out[1].channel) + (delta > 0 ? 1 : -1),
                                 0,
                                 15));
                    break;
                case CvGateMenuItem::CvOut2Cc:
                    cv_gate.cv_out[1].cc = static_cast<uint8_t>(
                        ClampInt(static_cast<int>(cv_gate.cv_out[1].cc) + (delta > 0 ? 1 : -1),
                                 0,
                                 127));
                    break;
                case CvGateMenuItem::CvOut2Priority:
                    cv_gate.cv_out[1].priority = cv_gate.cv_out[1].priority == NotePriority::Highest
                                                     ? NotePriority::Lowest
                                                     : NotePriority::Highest;
                    break;
                default: return;
            }
            state_->cv_gate_dirty = true;
        }
        break;

        case MenuPage::Main:
        case MenuPage::LoadMidi:
        case MenuPage::LoadSf2:
        case MenuPage::SaveAllConfirm: return;
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
        return false;

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
        case KnobPage::Program:
            target = MidiToNorm(state_->channels[ch].program_override >= 0
                                    ? static_cast<uint8_t>(state_->channels[ch].program_override)
                                    : state_->channels[ch].current_program);
            break;
        case KnobPage::Mute: target = state_->channels[ch].muted ? 1.0f : 0.0f; break;
        case KnobPage::Bpm: return false;
    }

    if(state_->knob_pickup_mode == KnobPickupMode::Pickup && !knob_caught_[index])
    {
        if(std::fabs(value - target) > 0.06f)
            return false;
        knob_caught_[index] = true;
    }
    else if(state_->knob_pickup_mode == KnobPickupMode::Jump)
    {
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
        case KnobPage::Program:
            state_->channels[ch].program_override = static_cast<int8_t>(midi_value);
            break;
        case KnobPage::Mute:
            state_->channels[ch].muted = value >= 0.5f;
            break;
        case KnobPage::Bpm: return false;
    }

    state_->settings_dirty = true;
    return true;
}

} // namespace major_midi
