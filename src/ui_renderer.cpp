#include "ui_renderer.h"

#include <cstdio>
#include <cstring>

using namespace daisy;
using namespace patch_sm;

namespace major_midi
{

namespace
{
void FormatPerfValue(char* out, size_t out_sz, uint8_t value)
{
    if(value == 0)
        std::snprintf(out, out_sz, "-");
    else
        std::snprintf(out, out_sz, "%03d", static_cast<int>(value));
}

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

size_t MenuPageItemCount(const AppState& state, const MediaLibrary& library)
{
    switch(state.menu_page)
    {
        case MenuPage::Main: return 7;
        case MenuPage::Fx: return 6;
        case MenuPage::Song: return 6;
        case MenuPage::Sf2: return 10;
        case MenuPage::Midi: return 13;
        case MenuPage::CvGate: return CvGateVisibleItemCount(state.cv_gate);
        case MenuPage::LoadMidi: return 1 + library.MidiCount();
        case MenuPage::LoadSf2: return 1 + library.SoundFontCount();
        case MenuPage::SaveAllConfirm: return 2;
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

void UiRenderer::ShowSplash()
{
    display_.Fill(false);
    display_.SetCursor(24, 22);
    display_.WriteString("Major", Font_11x18, true);
    display_.SetCursor(24, 44);
    display_.WriteString("MIDI", Font_11x18, true);
    display_.Update();
}

void UiRenderer::Render(const AppState& state,
                        const MediaLibrary& library,
                        uint32_t now_ms)
{
    char line[32];
    char midi_name[20];
    char sf2_name[20];

    CopyTrunc(library.MidiName(state.selected_midi_index), midi_name, sizeof(midi_name));
    CopyTrunc(library.SoundFontName(state.selected_sf2_index), sf2_name, sizeof(sf2_name));

    display_.Fill(false);

    if(state.saving_all)
    {
        display_.SetCursor(0, 8);
        display_.WriteString("Saving Settings", Font_7x10, true);
        display_.SetCursor(0, 24);
        display_.WriteString(midi_name[0] ? midi_name : "No MIDI", Font_6x8, true);
        display_.SetCursor(0, 34);
        display_.WriteString(sf2_name[0] ? sf2_name : "No SF2", Font_6x8, true);
        display_.SetCursor(0, 48);
        display_.WriteString("Please wait...", Font_6x8, true);
    }
    else if(state.loading_midi || state.loading_sf2)
    {
        display_.SetCursor(0, 8);
        display_.WriteString(state.loading_midi ? "Loading MIDI" : "Loading SF2",
                             Font_7x10,
                             true);
        display_.SetCursor(0, 24);
        display_.WriteString(state.loading_midi ? (midi_name[0] ? midi_name : "None")
                                                : (sf2_name[0] ? sf2_name : "None"),
                             Font_6x8,
                             true);
        display_.SetCursor(0, 40);
        display_.WriteString("Please wait...", Font_6x8, true);
    }
    else if(state.ui_mode == UiMode::Menu)
    {
        display_.SetCursor(0, 0);
        display_.WriteString("MAIN MENU", Font_6x8, true);

        const char* rows[] = {
            "Load MIDI",
            "Load SF2",
            "FX Settings",
            "Song Settings",
            "SF2 Settings",
            "MIDI Settings",
            "CV/Gate",
            "Save All",
        };

        for(int row = 0; row < 4; row++)
        {
            const size_t idx = state.menu_root_cursor >= 4 ? state.menu_root_cursor - 3 + row
                                                           : static_cast<size_t>(row);
            if(idx >= 8)
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
                    case 1: std::snprintf(line, sizeof(line), "%cVoices %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.sf2_max_voices)); break;
                    case 2: std::snprintf(line, sizeof(line), "%cChannel %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.sf2_channel) + 1); break;
                    case 3: std::snprintf(line, sizeof(line), "%cMute %s", item == state.menu_page_cursor ? '>' : ' ', channel.muted ? "On" : "Off"); break;
                    case 4: std::snprintf(line, sizeof(line), "%cVolume %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.volume)); break;
                    case 5: std::snprintf(line, sizeof(line), "%cPan %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.pan)); break;
                    case 6: std::snprintf(line, sizeof(line), "%cRevSend %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.reverb_send)); break;
                    case 7: std::snprintf(line, sizeof(line), "%cChoSend %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(channel.chorus_send)); break;
                    case 8:
                        if(channel.program_override >= 0)
                            std::snprintf(line,
                                          sizeof(line),
                                          "%cProgram %03d",
                                          item == state.menu_page_cursor ? '>' : ' ',
                                          static_cast<int>(channel.program_override));
                        else
                            std::snprintf(line,
                                          sizeof(line),
                                          "%cProgram File",
                                          item == state.menu_page_cursor ? '>' : ' ');
                        break;
                    case 9: std::snprintf(line, sizeof(line), "%cTrans %4d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.sf2_transpose)); break;
                }
            }
            else if(state.menu_page == MenuPage::CvGate)
            {
                switch(CvGateVisibleItemAt(state.cv_gate, item))
                {
                    case CvGateMenuItem::Back: std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' '); break;
                    case CvGateMenuItem::Cv1Mode: std::snprintf(line, sizeof(line), "%cCV1 Mode %s", item == state.menu_page_cursor ? '>' : ' ', CvInModeName(state.cv_gate.cv_in[0].mode)); break;
                    case CvGateMenuItem::Cv1Channel: std::snprintf(line, sizeof(line), "%cCV1 Ch %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.cv_gate.cv_in[0].channel) + 1); break;
                    case CvGateMenuItem::Cv1Cc: std::snprintf(line, sizeof(line), "%cCV1 CC %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.cv_gate.cv_in[0].cc)); break;
                    case CvGateMenuItem::Gate1Mode: std::snprintf(line, sizeof(line), "%cG1 Mode %s", item == state.menu_page_cursor ? '>' : ' ', GateOutModeName(state.cv_gate.gate_out[0].mode)); break;
                    case CvGateMenuItem::Gate1Channel: std::snprintf(line, sizeof(line), "%cG1 Ch %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.cv_gate.gate_out[0].channel) + 1); break;
                    case CvGateMenuItem::Gate1Resolution: std::snprintf(line, sizeof(line), "%cG1 Res %s", item == state.menu_page_cursor ? '>' : ' ', SyncResolutionName(state.cv_gate.gate_out[0].sync_resolution)); break;
                    case CvGateMenuItem::Gate2Mode: std::snprintf(line, sizeof(line), "%cG2 Mode %s", item == state.menu_page_cursor ? '>' : ' ', GateOutModeName(state.cv_gate.gate_out[1].mode)); break;
                    case CvGateMenuItem::Gate2Channel: std::snprintf(line, sizeof(line), "%cG2 Ch %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.cv_gate.gate_out[1].channel) + 1); break;
                    case CvGateMenuItem::Gate2Resolution: std::snprintf(line, sizeof(line), "%cG2 Res %s", item == state.menu_page_cursor ? '>' : ' ', SyncResolutionName(state.cv_gate.gate_out[1].sync_resolution)); break;
                    case CvGateMenuItem::CvOut1Mode: std::snprintf(line, sizeof(line), "%cO1 Mode %s", item == state.menu_page_cursor ? '>' : ' ', CvOutModeName(state.cv_gate.cv_out[0].mode)); break;
                    case CvGateMenuItem::CvOut1Channel: std::snprintf(line, sizeof(line), "%cO1 Ch %02d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.cv_gate.cv_out[0].channel) + 1); break;
                    case CvGateMenuItem::CvOut1Cc: std::snprintf(line, sizeof(line), "%cO1 CC %03d", item == state.menu_page_cursor ? '>' : ' ', static_cast<int>(state.cv_gate.cv_out[0].cc)); break;
                    case CvGateMenuItem::CvOut1Priority: std::snprintf(line, sizeof(line), "%cO1 Pri %s", item == state.menu_page_cursor ? '>' : ' ', NotePriorityName(state.cv_gate.cv_out[0].priority)); break;
                    case CvGateMenuItem::Cv2Mode:
                    case CvGateMenuItem::Cv2Channel:
                    case CvGateMenuItem::Cv2Cc:
                    case CvGateMenuItem::CvOut2Mode:
                    case CvGateMenuItem::CvOut2Channel:
                    case CvGateMenuItem::CvOut2Cc:
                    case CvGateMenuItem::CvOut2Priority:
                        line[0] = '\0';
                        break;
                }
            }
            else if(state.menu_page == MenuPage::Midi)
            {
                switch(static_cast<MidiSettingsMenuItem>(item))
                {
                    case MidiSettingsMenuItem::Back: std::snprintf(line, sizeof(line), "%cBack to Menu", item == state.menu_page_cursor ? '>' : ' '); break;
                    case MidiSettingsMenuItem::UsbNotes: std::snprintf(line, sizeof(line), "%cUSB Notes %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.usb.notes ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UsbCcs: std::snprintf(line, sizeof(line), "%cUSB CCs %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.usb.ccs ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UsbPrograms: std::snprintf(line, sizeof(line), "%cUSB Prog %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.usb.programs ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UsbTransport: std::snprintf(line, sizeof(line), "%cUSB Trn %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.usb.transport ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UsbClock: std::snprintf(line, sizeof(line), "%cUSB Clk %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.usb.clock ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UartNotes: std::snprintf(line, sizeof(line), "%cUART Notes %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.uart.notes ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UartCcs: std::snprintf(line, sizeof(line), "%cUART CCs %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.uart.ccs ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UartPrograms: std::snprintf(line, sizeof(line), "%cUART Prog %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.uart.programs ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UartTransport: std::snprintf(line, sizeof(line), "%cUART Trn %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.uart.transport ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UartClock: std::snprintf(line, sizeof(line), "%cUART Clk %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.uart.clock ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UsbInToUart: std::snprintf(line, sizeof(line), "%cUSB>UART %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.usb_in_to_uart ? "On" : "Off"); break;
                    case MidiSettingsMenuItem::UartInToUsb: std::snprintf(line, sizeof(line), "%cUART>USB %s", item == state.menu_page_cursor ? '>' : ' ', state.midi_routing.uart_in_to_usb ? "On" : "Off"); break;
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
            else if(state.menu_page == MenuPage::SaveAllConfirm)
            {
                std::snprintf(line,
                              sizeof(line),
                              "%c%s",
                              item == state.menu_page_cursor ? '>' : ' ',
                              item == 0 ? "Confirm Save" : "Cancel");
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
    else
    {
        const int ch0 = VisibleChannelIndex(state.bank, 0);
        const int ch1 = VisibleChannelIndex(state.bank, 1);
        const int ch2 = VisibleChannelIndex(state.bank, 2);
        const int ch3 = VisibleChannelIndex(state.bank, 3);
        const int col_x[4] = {20, 47, 74, 101};
        const int row_y[5] = {24, 32, 40, 48, 56};

        std::snprintf(line,
                      sizeof(line),
                      "%s %3d M%03d B%d %s",
                      state.transport_playing ? "PLY" : "STP",
                      state.bpm,
                      static_cast<int>(state.current_measure),
                      static_cast<int>(state.bank) + 1,
                      KnobPageShortName(state.knob_page));
        display_.SetCursor(0, 0);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "M:%s", midi_name[0] ? midi_name : "None");
        display_.SetCursor(0, 8);
        display_.WriteString(line, Font_6x8, true);

        std::snprintf(line, sizeof(line), "S:%s", sf2_name[0] ? sf2_name : "None");
        display_.SetCursor(0, 16);
        display_.WriteString(line, Font_6x8, true);

        display_.SetCursor(0, row_y[0]);
        display_.WriteString("Ch", Font_6x8, true);
        display_.SetCursor(0, row_y[1]);
        display_.WriteString("V", Font_6x8, true);
        display_.SetCursor(0, row_y[2]);
        display_.WriteString("P", Font_6x8, true);
        display_.SetCursor(0, row_y[3]);
        display_.WriteString("R", Font_6x8, true);
        display_.SetCursor(0, row_y[4]);
        display_.WriteString(state.knob_page == KnobPage::Program ? "G" : "C",
                             Font_6x8,
                             true);

        const int channels[4] = {ch0, ch1, ch2, ch3};
        for(int i = 0; i < 4; i++)
        {
            const ChannelState& channel = state.channels[channels[i]];

            std::snprintf(line,
                          sizeof(line),
                          "%c%02d",
                          channel.muted ? '*' : ' ',
                          channels[i] + 1);
            display_.SetCursor(col_x[i], row_y[0]);
            display_.WriteString(line, Font_6x8, true);

            FormatPerfValue(line, sizeof(line), channel.volume);
            display_.SetCursor(col_x[i], row_y[1]);
            display_.WriteString(line, Font_6x8, true);

            FormatPerfValue(line, sizeof(line), channel.pan);
            display_.SetCursor(col_x[i], row_y[2]);
            display_.WriteString(line, Font_6x8, true);

            FormatPerfValue(line, sizeof(line), channel.reverb_send);
            display_.SetCursor(col_x[i], row_y[3]);
            display_.WriteString(line, Font_6x8, true);

            const uint8_t bottom_value
                = state.knob_page == KnobPage::Program
                      ? static_cast<uint8_t>(channel.program_override >= 0
                                                 ? channel.program_override
                                                 : channel.current_program)
                      : channel.chorus_send;
            FormatPerfValue(line, sizeof(line), bottom_value);
            display_.SetCursor(col_x[i], row_y[4]);
            display_.WriteString(line, Font_6x8, true);
        }

        if(state.ui_mode == UiMode::Mute)
        {
            display_.DrawRect(0, 54, 128, 10, true, true);
            display_.SetCursor(2, 56);
            display_.WriteString("MUTE MODE ENC=EXIT", Font_6x8, false);
        }
    }

    if(state.overlay.until_ms > now_ms && state.ui_mode != UiMode::Mute)
    {
        display_.DrawRect(0, 54, 128, 10, true, true);
        display_.SetCursor(2, 56);
        display_.WriteString(state.overlay.text, Font_6x8, false);
    }

    display_.Update();
}

} // namespace major_midi
