#pragma once

#include <cstddef>
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
    Program,
};

enum class MenuPage : uint8_t
{
    Main,
    Fx,
    Song,
    Sf2,
    CvGate,
    LoadMidi,
    LoadSf2,
    SaveAllConfirm,
};

enum class CvGateMenuItem : uint8_t
{
    Back,
    Cv1Mode,
    Cv1Channel,
    Cv1Cc,
    Cv2Mode,
    Cv2Channel,
    Cv2Cc,
    Gate1Mode,
    Gate1Channel,
    Gate1Resolution,
    Gate2Mode,
    Gate2Channel,
    Gate2Resolution,
    CvOut1Mode,
    CvOut1Channel,
    CvOut1Cc,
    CvOut1Priority,
    CvOut2Mode,
    CvOut2Channel,
    CvOut2Cc,
    CvOut2Priority,
};

enum class CvInMode : uint8_t
{
    Off,
    MasterVolume,
    Bpm,
    ChannelCc,
};

enum class GateOutMode : uint8_t
{
    Off,
    SyncOut,
    ResetPulse,
    ChannelGate,
};

enum class CvOutMode : uint8_t
{
    Off,
    ChannelPitch,
    ChannelCc,
};

enum class SyncResolution : uint8_t
{
    Div4,
    Div8,
    Div16,
    Div32,
    Div64,
};

enum class NotePriority : uint8_t
{
    Highest,
    Lowest,
};

struct CvInputConfig
{
    CvInMode mode    = CvInMode::Off;
    uint8_t  channel = 0;
    uint8_t  cc      = 1;
};

struct GateOutputConfig
{
    GateOutMode    mode            = GateOutMode::Off;
    uint8_t        channel         = 0;
    SyncResolution sync_resolution = SyncResolution::Div16;
};

struct CvOutputConfig
{
    CvOutMode    mode     = CvOutMode::Off;
    uint8_t      channel  = 0;
    uint8_t      cc       = 1;
    NotePriority priority = NotePriority::Highest;
};

struct CvGateConfig
{
    CvInputConfig    cv_in[2]{};
    GateOutputConfig gate_out[2]{};
    CvOutputConfig   cv_out[2]{};
};

struct ChannelState
{
    uint8_t volume      = 100;
    uint8_t pan         = 64;
    uint8_t reverb_send = 0;
    uint8_t chorus_send = 0;
    uint8_t current_program = 0;
    int8_t  program_override = -1;
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
    uint8_t      sf2_max_voices          = 16;
    int8_t       sf2_transpose           = 0;
    bool         pending_midi_load       = false;
    bool         pending_sf2_load        = false;
    bool         loading_midi            = false;
    bool         loading_sf2             = false;
    bool         pending_save_settings   = false;
    bool         pending_save_all        = false;
    bool         settings_dirty          = false;
    bool         cv_gate_dirty           = false;
    bool         sync_external           = false;
    bool         sync_locked             = false;
    uint16_t     current_measure         = 1;
    bool         saving_all              = false;
    CvGateConfig cv_gate{};
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
        case KnobPage::Program: return "Program";
    }
    return "";
}

inline const char* KnobPageShortName(KnobPage page)
{
    switch(page)
    {
        case KnobPage::Volume: return "V";
        case KnobPage::Pan: return "P";
        case KnobPage::ReverbSend: return "R";
        case KnobPage::ChorusSend: return "C";
        case KnobPage::Program: return "G";
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
        case MenuPage::CvGate: return "CV/Gate";
        case MenuPage::LoadMidi: return "Load MIDI";
        case MenuPage::LoadSf2: return "Load SF2";
        case MenuPage::SaveAllConfirm: return "Save All";
    }
    return "";
}

inline const char* CvInModeName(CvInMode mode)
{
    switch(mode)
    {
        case CvInMode::Off: return "Off";
        case CvInMode::MasterVolume: return "MasterVol";
        case CvInMode::Bpm: return "BPM";
        case CvInMode::ChannelCc: return "Ch CC";
    }
    return "";
}

inline const char* GateOutModeName(GateOutMode mode)
{
    switch(mode)
    {
        case GateOutMode::Off: return "Off";
        case GateOutMode::SyncOut: return "Sync";
        case GateOutMode::ResetPulse: return "Reset";
        case GateOutMode::ChannelGate: return "Ch Gate";
    }
    return "";
}

inline const char* CvOutModeName(CvOutMode mode)
{
    switch(mode)
    {
        case CvOutMode::Off: return "Off";
        case CvOutMode::ChannelPitch: return "Pitch";
        case CvOutMode::ChannelCc: return "CC";
    }
    return "";
}

inline const char* SyncResolutionName(SyncResolution resolution)
{
    switch(resolution)
    {
        case SyncResolution::Div4: return "1/4";
        case SyncResolution::Div8: return "1/8";
        case SyncResolution::Div16: return "1/16";
        case SyncResolution::Div32: return "1/32";
        case SyncResolution::Div64: return "1/64";
    }
    return "";
}

inline const char* NotePriorityName(NotePriority priority)
{
    switch(priority)
    {
        case NotePriority::Highest: return "High";
        case NotePriority::Lowest: return "Low";
    }
    return "";
}

inline bool CvInModeNeedsChannel(CvInMode mode)
{
    return mode == CvInMode::ChannelCc;
}

inline bool CvInModeNeedsCc(CvInMode mode)
{
    return mode == CvInMode::ChannelCc;
}

inline bool GateOutModeNeedsChannel(GateOutMode mode)
{
    return mode == GateOutMode::ChannelGate;
}

inline bool GateOutModeNeedsResolution(GateOutMode mode)
{
    return mode == GateOutMode::SyncOut;
}

inline bool CvOutModeNeedsChannel(CvOutMode mode)
{
    return mode == CvOutMode::ChannelPitch || mode == CvOutMode::ChannelCc;
}

inline bool CvOutModeNeedsCc(CvOutMode mode)
{
    return mode == CvOutMode::ChannelCc;
}

inline bool CvOutModeNeedsPriority(CvOutMode mode)
{
    return mode == CvOutMode::ChannelPitch;
}

inline size_t CvGateVisibleItemCount(const CvGateConfig& config)
{
    size_t count = 0;
    auto add = [&](bool enabled = true) {
        if(enabled)
            count++;
    };

    add(); // Back
    add(); // CV1 Mode
    add(CvInModeNeedsChannel(config.cv_in[0].mode));
    add(CvInModeNeedsCc(config.cv_in[0].mode));
    add(); // CV2 Mode
    add(CvInModeNeedsChannel(config.cv_in[1].mode));
    add(CvInModeNeedsCc(config.cv_in[1].mode));
    add(); // G1 Mode
    add(GateOutModeNeedsChannel(config.gate_out[0].mode));
    add(GateOutModeNeedsResolution(config.gate_out[0].mode));
    add(); // G2 Mode
    add(GateOutModeNeedsChannel(config.gate_out[1].mode));
    add(GateOutModeNeedsResolution(config.gate_out[1].mode));
    add(); // O1 Mode
    add(CvOutModeNeedsChannel(config.cv_out[0].mode));
    add(CvOutModeNeedsCc(config.cv_out[0].mode));
    add(CvOutModeNeedsPriority(config.cv_out[0].mode));
    add(); // O2 Mode
    add(CvOutModeNeedsChannel(config.cv_out[1].mode));
    add(CvOutModeNeedsCc(config.cv_out[1].mode));
    add(CvOutModeNeedsPriority(config.cv_out[1].mode));
    return count;
}

inline CvGateMenuItem CvGateVisibleItemAt(const CvGateConfig& config, size_t visible_index)
{
    size_t current = 0;
    auto match = [&](CvGateMenuItem item, bool enabled = true) {
        if(!enabled)
            return false;
        if(current == visible_index)
            return true;
        current++;
        return false;
    };

    if(match(CvGateMenuItem::Back))
        return CvGateMenuItem::Back;
    if(match(CvGateMenuItem::Cv1Mode))
        return CvGateMenuItem::Cv1Mode;
    if(match(CvGateMenuItem::Cv1Channel, CvInModeNeedsChannel(config.cv_in[0].mode)))
        return CvGateMenuItem::Cv1Channel;
    if(match(CvGateMenuItem::Cv1Cc, CvInModeNeedsCc(config.cv_in[0].mode)))
        return CvGateMenuItem::Cv1Cc;
    if(match(CvGateMenuItem::Cv2Mode))
        return CvGateMenuItem::Cv2Mode;
    if(match(CvGateMenuItem::Cv2Channel, CvInModeNeedsChannel(config.cv_in[1].mode)))
        return CvGateMenuItem::Cv2Channel;
    if(match(CvGateMenuItem::Cv2Cc, CvInModeNeedsCc(config.cv_in[1].mode)))
        return CvGateMenuItem::Cv2Cc;
    if(match(CvGateMenuItem::Gate1Mode))
        return CvGateMenuItem::Gate1Mode;
    if(match(CvGateMenuItem::Gate1Channel,
             GateOutModeNeedsChannel(config.gate_out[0].mode)))
        return CvGateMenuItem::Gate1Channel;
    if(match(CvGateMenuItem::Gate1Resolution,
             GateOutModeNeedsResolution(config.gate_out[0].mode)))
        return CvGateMenuItem::Gate1Resolution;
    if(match(CvGateMenuItem::Gate2Mode))
        return CvGateMenuItem::Gate2Mode;
    if(match(CvGateMenuItem::Gate2Channel,
             GateOutModeNeedsChannel(config.gate_out[1].mode)))
        return CvGateMenuItem::Gate2Channel;
    if(match(CvGateMenuItem::Gate2Resolution,
             GateOutModeNeedsResolution(config.gate_out[1].mode)))
        return CvGateMenuItem::Gate2Resolution;
    if(match(CvGateMenuItem::CvOut1Mode))
        return CvGateMenuItem::CvOut1Mode;
    if(match(CvGateMenuItem::CvOut1Channel,
             CvOutModeNeedsChannel(config.cv_out[0].mode)))
        return CvGateMenuItem::CvOut1Channel;
    if(match(CvGateMenuItem::CvOut1Cc, CvOutModeNeedsCc(config.cv_out[0].mode)))
        return CvGateMenuItem::CvOut1Cc;
    if(match(CvGateMenuItem::CvOut1Priority,
             CvOutModeNeedsPriority(config.cv_out[0].mode)))
        return CvGateMenuItem::CvOut1Priority;
    if(match(CvGateMenuItem::CvOut2Mode))
        return CvGateMenuItem::CvOut2Mode;
    if(match(CvGateMenuItem::CvOut2Channel,
             CvOutModeNeedsChannel(config.cv_out[1].mode)))
        return CvGateMenuItem::CvOut2Channel;
    if(match(CvGateMenuItem::CvOut2Cc, CvOutModeNeedsCc(config.cv_out[1].mode)))
        return CvGateMenuItem::CvOut2Cc;
    if(match(CvGateMenuItem::CvOut2Priority,
             CvOutModeNeedsPriority(config.cv_out[1].mode)))
        return CvGateMenuItem::CvOut2Priority;
    return CvGateMenuItem::Back;
}

inline bool CvGateConfigEqual(const CvGateConfig& a, const CvGateConfig& b)
{
    for(size_t i = 0; i < 2; i++)
    {
        if(a.cv_in[i].mode != b.cv_in[i].mode || a.cv_in[i].channel != b.cv_in[i].channel
           || a.cv_in[i].cc != b.cv_in[i].cc)
            return false;
        if(a.gate_out[i].mode != b.gate_out[i].mode
           || a.gate_out[i].channel != b.gate_out[i].channel
           || a.gate_out[i].sync_resolution != b.gate_out[i].sync_resolution)
            return false;
        if(a.cv_out[i].mode != b.cv_out[i].mode
           || a.cv_out[i].channel != b.cv_out[i].channel
           || a.cv_out[i].cc != b.cv_out[i].cc
           || a.cv_out[i].priority != b.cv_out[i].priority)
            return false;
    }
    return true;
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
