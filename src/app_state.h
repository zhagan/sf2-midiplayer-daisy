#pragma once

#include <cstdint>
#include <cstring>

namespace major_midi
{

enum class UiMode : uint8_t
{
    Performance,
    Mute,
    LoopEdit,
    Menu,
    MenuPage,
};

enum class KnobPage : uint8_t
{
    Volume,
    Pan,
    ReverbSend,
    ChorusSend,
};

enum class MenuPage : uint8_t
{
    Main,
    Fx,
    Song,
    Sf2,
    LoadMidi,
    LoadSf2,
};

struct ChannelState
{
    uint8_t volume      = 100;
    uint8_t pan         = 64;
    uint8_t reverb_send = 0;
    uint8_t chorus_send = 0;
    bool    muted       = false;
};

struct OverlayState
{
    char     text[32]{};
    uint32_t until_ms = 0;
};

struct AppState
{
    UiMode       ui_mode                 = UiMode::Performance;
    KnobPage     knob_page               = KnobPage::Volume;
    MenuPage     menu_page               = MenuPage::Main;
    uint8_t      bank                    = 0;
    bool         mute_all                = false;
    bool         transport_playing       = false;
    int          bpm                     = 120;
    size_t       selected_midi_index     = 0;
    size_t       selected_sf2_index      = 0;
    size_t       menu_root_cursor        = 0;
    size_t       menu_page_cursor        = 0;
    bool         menu_editing            = false;
    uint8_t      sf2_channel             = 0;
    int          loop_start_measure      = 1;
    int          loop_start_beat         = 1;
    int          loop_start_sub          = 1;
    int          loop_end_measure        = 1;
    int          loop_length_beats       = 16;
    uint16_t     song_bpm_override       = 0;
    bool         song_loop_enabled       = false;
    float        fx_reverb_time          = 0.85f;
    float        fx_reverb_lpf_hz        = 8000.0f;
    float        fx_reverb_hpf_hz        = 80.0f;
    float        fx_chorus_depth         = 0.35f;
    float        fx_chorus_speed_hz      = 0.25f;
    uint8_t      sf2_master_volume_max   = 127;
    uint8_t      sf2_expression_max      = 127;
    uint8_t      sf2_reverb_max          = 127;
    uint8_t      sf2_chorus_max          = 127;
    int8_t       sf2_transpose           = 0;
    bool         pending_midi_load       = false;
    bool         pending_sf2_load        = false;
    bool         pending_save_settings   = false;
    bool         settings_dirty          = false;
    OverlayState overlay{};
    ChannelState channels[16]{};
};

inline int VisibleChannelIndex(uint8_t bank, uint8_t slot)
{
    return static_cast<int>(bank) * 4 + static_cast<int>(slot);
}

inline const char* KnobPageName(KnobPage page)
{
    switch(page)
    {
        case KnobPage::Volume: return "Volume";
        case KnobPage::Pan: return "Pan";
        case KnobPage::ReverbSend: return "Reverb";
        case KnobPage::ChorusSend: return "Chorus";
    }
    return "";
}

inline const char* UiModeName(UiMode mode)
{
    switch(mode)
    {
        case UiMode::Performance: return "Perform";
        case UiMode::Mute: return "Mute";
        case UiMode::LoopEdit: return "Loop";
        case UiMode::Menu: return "Menu";
        case UiMode::MenuPage: return "Menu";
    }
    return "";
}

inline const char* MenuPageName(MenuPage page)
{
    switch(page)
    {
        case MenuPage::Main: return "Menu";
        case MenuPage::Fx: return "FX";
        case MenuPage::Song: return "Song";
        case MenuPage::Sf2: return "SF2";
        case MenuPage::LoadMidi: return "Load MIDI";
        case MenuPage::LoadSf2: return "Load SF2";
    }
    return "";
}

inline void SetOverlay(AppState& state,
                       const char* text,
                       uint32_t    now_ms,
                       uint32_t    duration_ms = 1200)
{
    std::strncpy(state.overlay.text, text, sizeof(state.overlay.text) - 1);
    state.overlay.text[sizeof(state.overlay.text) - 1] = '\0';
    state.overlay.until_ms                              = now_ms + duration_ms;
}

} // namespace major_midi
