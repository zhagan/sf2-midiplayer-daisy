#include "ui_renderer.h"

#include <cstdio>
#include <cstring>

using namespace daisy;
using namespace patch_sm;

namespace major_midi
{

namespace
{
void CopyTrunc(const char* src, char* dst, size_t dst_sz)
{
    if(dst_sz == 0)
        return;

    if(src == nullptr)
    {
        dst[0] = '\0';
        return;
    }

    const size_t len = std::strlen(src);
    if(len < dst_sz)
    {
        std::memcpy(dst, src, len + 1);
        return;
    }

    if(dst_sz <= 4)
    {
        dst[0] = '\0';
        return;
    }

    const size_t copy_len = dst_sz - 4;
    std::memcpy(dst, src, copy_len);
    std::memcpy(dst + copy_len, "...", 4);
}

const char* VisibleValue(const AppState& state, int ch, char* out, size_t out_sz)
{
    const ChannelState& channel = state.channels[ch];
    switch(state.knob_page)
    {
        case KnobPage::Volume:
            std::snprintf(out, out_sz, "%3d", channel.volume);
            break;
        case KnobPage::Pan:
            std::snprintf(out, out_sz, "%3d", channel.pan);
            break;
        case KnobPage::ReverbSend:
            std::snprintf(out, out_sz, "%3d", channel.reverb_send);
            break;
        case KnobPage::ChorusSend:
            std::snprintf(out, out_sz, "%3d", channel.chorus_send);
            break;
    }
    return out;
}

size_t MenuPageItemCount(const AppState& state, const MediaLibrary& library)
{
    switch(state.menu_page)
    {
        case MenuPage::Main: return 5;
        case MenuPage::Fx: return 6;
        case MenuPage::Song: return 6;
        case MenuPage::Sf2: return 8;
        case MenuPage::LoadMidi: return 1 + library.MidiCount();
        case MenuPage::LoadSf2: return 1 + library.SoundFontCount();
    }
    return 0;
}

size_t MenuPageScrollOffset(const AppState& state, const MediaLibrary& library)
{
    const size_t visible = 4;
    const size_t count   = MenuPageItemCount(state, library);
    if(count <= visible || state.menu_page_cursor < visible)
        return 0;

    const size_t max_offset = count - visible;
    size_t       offset     = state.menu_page_cursor - (visible - 1);
    if(offset > max_offset)
        offset = max_offset;
    return offset;
}
} // namespace

void UiRenderer::Init()
{
    I2CHandle::Config i2c_config;
    i2c_config.periph         = I2CHandle::Config::Peripheral::I2C_1;
    i2c_config.speed          = I2CHandle::Config::Speed::I2C_1MHZ;
    i2c_config.mode           = I2CHandle::Config::Mode::I2C_MASTER;
    i2c_config.pin_config.scl = DaisyPatchSM::B7;
    i2c_config.pin_config.sda = DaisyPatchSM::B8;

    DisplayT::Config display_config;
    display_config.driver_config.transport_config.i2c_config  = i2c_config;
    display_config.driver_config.transport_config.i2c_address = 0x3C;
    display_.Init(display_config);
}

void UiRenderer::Render(const AppState& state,
                        const MediaLibrary& library,
                        uint32_t now_ms)
{
    char line[32];
    char value[8];
    char midi_name[20];
    char sf2_name[20];

    CopyTrunc(library.MidiName(state.selected_midi_index), midi_name, sizeof(midi_name));
    CopyTrunc(library.SoundFontName(state.selected_sf2_index), sf2_name, sizeof(sf2_name));

    display_.Fill(false);

    if(state.ui_mode == UiMode::Menu)
    {
        display_.SetCursor(0, 0);
        display_.WriteString("MAIN MENU", Font_6x8, true);

        const char* rows[] = {
            "Load MIDI",
            "Load SF2",
            "FX Settings",
            "Song Settings",
            "SF2 Settings",
        };

        for(int row = 0; row < 4; row++)
        {
            const size_t idx = state.menu_root_cursor >= 4 ? state.menu_root_cursor - 3 + row
                                                           : static_cast<size_t>(row);
            if(idx >= 5)
                break;
            std::snprintf(line,
                          sizeof(line),
                          "%c%s",
                          idx == state.menu_root_cursor ? '>' : ' ',
                          rows[idx]);
            display_.SetCursor(0, 16 + row * 10);
            display_.WriteString(line, Font_6x8, true);
        }
    }
    else if(state.ui_mode == UiMode::MenuPage)
    {
        std::snprintf(line,
                      sizeof(line),
                      "%s %s",
                      MenuPageName(state.menu_page),
                      state.menu_editing ? "*" : "");
        display_.SetCursor(0, 0);
        display_.WriteString(line, Font_6x8, true);

        const size_t start = MenuPageScrollOffset(state, library);
        for(int row = 0; row < 4; row++)
        {
            const size_t item = start + static_cast<size_t>(row);
            if(item >= MenuPageItemCount(state, library))
                break;

            if(state.menu_page == MenuPage::Fx)
            {
                switch(item)
                {
                    case 0: std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' '); break;
                    case 1: std::snprintf(line, sizeof(line), "%cRev Time %3d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.fx_reverb_time * 100.0f)); break;
                    case 2: std::snprintf(line, sizeof(line), "%cRev LPF %5d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.fx_reverb_lpf_hz)); break;
                    case 3: std::snprintf(line, sizeof(line), "%cRev HPF %4d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.fx_reverb_hpf_hz)); break;
                    case 4: std::snprintf(line, sizeof(line), "%cCh Depth %3d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.fx_chorus_depth * 100.0f)); break;
                    case 5: std::snprintf(line, sizeof(line), "%cCh Speed %3d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.fx_chorus_speed_hz * 100.0f)); break;
                }
            }
            else if(state.menu_page == MenuPage::Song)
            {
                switch(item)
                {
                    case 0: std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' '); break;
                    case 1: std::snprintf(line, sizeof(line), "%cBPM Ovr %3d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.song_bpm_override)); break;
                    case 2: std::snprintf(line, sizeof(line), "%cLoop %s", item == state.menu_page_cursor ? '>' : ' ', state.song_loop_enabled ? "On" : "Off"); break;
                    case 3: std::snprintf(line, sizeof(line), "%cLoop St %03d", item == state.menu_page_cursor ? '>' : ' ', state.loop_start_measure); break;
                    case 4: std::snprintf(line, sizeof(line), "%cLoop End %03d", item == state.menu_page_cursor ? '>' : ' ', state.loop_end_measure); break;
                    case 5: std::snprintf(line, sizeof(line), "%cSave To MIDI", item == state.menu_page_cursor ? '>' : ' '); break;
                }
            }
            else if(state.menu_page == MenuPage::Sf2)
            {
                const ChannelState& channel = state.channels[state.sf2_channel];
                switch(item)
                {
                    case 0: std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' '); break;
                    case 1: std::snprintf(line, sizeof(line), "%cChannel %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.sf2_channel) + 1); break;
                    case 2: std::snprintf(line, sizeof(line), "%cMute %s", item == state.menu_page_cursor ? '>' : ' ', channel.muted ? "On" : "Off"); break;
                    case 3: std::snprintf(line, sizeof(line), "%cVolume %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.volume)); break;
                    case 4: std::snprintf(line, sizeof(line), "%cPan %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.pan)); break;
                    case 5: std::snprintf(line, sizeof(line), "%cRevSend %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.reverb_send)); break;
                    case 6: std::snprintf(line, sizeof(line), "%cChoSend %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.chorus_send)); break;
                    case 7: std::snprintf(line, sizeof(line), "%cTrans %4d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.sf2_transpose)); break;
                }
            }
            else if(state.menu_page == MenuPage::LoadMidi)
            {
                if(item == 0)
                {
                    std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' ');
                }
                else
                {
                    char short_name[24];
                    CopyTrunc(library.MidiName(item - 1), short_name, sizeof(short_name));
                    std::snprintf(line,
                                  sizeof(line),
                                  "%c%s",
                                  item == state.menu_page_cursor ? '>' : ' ',
                                  short_name);
                }
            }
            else if(state.menu_page == MenuPage::LoadSf2)
            {
                if(item == 0)
                {
                    std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' ');
                }
                else
                {
                    char short_name[24];
                    CopyTrunc(library.SoundFontName(item - 1), short_name, sizeof(short_name));
                    std::snprintf(line,
                                  sizeof(line),
                                  "%c%s",
                                  item == state.menu_page_cursor ? '>' : ' ',
                                  short_name);
                }
            }
            else
            {
                line[0] = '\0';
            }

            display_.SetCursor(0, 16 + row * 10);
            display_.WriteString(line, Font_6x8, true);
        }
    }
    else if(state.ui_mode == UiMode::LoopEdit)
    {
        std::snprintf(line, sizeof(line), "LOOP BPM %d", state.bpm);
        display_.SetCursor(0, 0);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "M:%s", midi_name[0] ? midi_name : "None");
        display_.SetCursor(0, 8);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "K1 Start M%02d", state.loop_start_measure);
        display_.SetCursor(0, 24);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "K2 End   M%02d", state.loop_end_measure);
        display_.SetCursor(0, 34);
        display_.WriteString(line, Font_6x8, true);

        display_.SetCursor(0, 46);
        display_.WriteString(
            state.loop_end_measure > state.loop_start_measure ? "Loop Active"
                                                              : "Loop Off",
            Font_6x8,
            true);
    }
    else if(state.ui_mode == UiMode::Mute)
    {
        std::snprintf(line,
                      sizeof(line),
                      "MUTE B%d BPM %d",
                      static_cast<int>(state.bank) + 1,
                      state.bpm);
        display_.SetCursor(0, 0);
        display_.WriteString(line, Font_6x8, true);

        display_.SetCursor(0, 8);
        display_.WriteString("B1-4 mute  Shift+B=bank", Font_6x8, true);

        for(int row = 0; row < 4; row++)
        {
            const int ch = VisibleChannelIndex(state.bank, row);
            std::snprintf(line,
                          sizeof(line),
                          "Ch%02d %s",
                          ch + 1,
                          state.channels[ch].muted ? "MUTED" : "ON");
            display_.SetCursor(0, 22 + row * 9);
            display_.WriteString(line, Font_6x8, true);
        }
    }
    else
    {
        std::snprintf(line,
                      sizeof(line),
                      "%s %3d B%d %s",
                      state.transport_playing ? "PLY" : "STP",
                      state.bpm,
                      static_cast<int>(state.bank) + 1,
                      KnobPageName(state.knob_page));
        display_.SetCursor(0, 0);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "M:%s", midi_name[0] ? midi_name : "None");
        display_.SetCursor(0, 8);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "S:%s", sf2_name[0] ? sf2_name : "None");
        display_.SetCursor(0, 16);
        display_.WriteString(line, Font_6x8, true);

        for(int row = 0; row < 4; row++)
        {
            const int ch = VisibleChannelIndex(state.bank, row);
            std::snprintf(line,
                          sizeof(line),
                          "%2d %s %c%c",
                          ch + 1,
                          VisibleValue(state, ch, value, sizeof(value)),
                          state.channels[ch].muted ? 'M' : '-',
                          state.mute_all ? 'A' : '-');
            display_.SetCursor(0, 24 + row * 8);
            display_.WriteString(line, Font_6x8, true);
        }
    }

    if(state.overlay.until_ms > now_ms
       && state.ui_mode != UiMode::Menu
       && state.ui_mode != UiMode::MenuPage)
    {
        display_.DrawRect(0, 54, 128, 10, true, true);
        display_.SetCursor(2, 56);
        display_.WriteString(state.overlay.text, Font_6x8, false);
    }

    display_.Update();
}

} // namespace major_midi
