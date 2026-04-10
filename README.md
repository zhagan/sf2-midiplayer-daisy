# Major MIDI

`Major MIDI` is a Daisy Patch SM firmware for playing Standard MIDI Files from SD card through a SoundFont 2 synth engine, with a compact front-panel UI, live MIDI input, loop playback, internal or external sync, and assignable CV/gate I/O.

This README is written as an operator guide first and a developer guide second, so it can be reused as source material for a website or user manual.

## What Major MIDI Does

Major MIDI turns a Daisy Patch SM-based module into a multitimbral MIDI file player and live MIDI SoundFont instrument.

At a high level it gives you:

| Capability | Notes |
| --- | --- |
| `.mid` playback from SD card | Standard MIDI Files loaded from `0:/midi` |
| `.sf2` SoundFont loading | SoundFonts loaded from `0:/soundfonts` |
| 16-channel mixer | Per-channel mix control from the front panel |
| Per-channel program selection | File-driven or manually overridden |
| Loop playback with saved metadata | Song settings can be written into MIDI metadata |
| Live UART MIDI input | Hardware MIDI input on the Patch SM build |
| Live external USB MIDI input | Additional live performance input path |
| Internal tempo or external sync | Switchable transport clock source |
| Assignable CV/gate I/O | CV inputs plus configurable gate/CV outputs |

The system is designed around a small OLED and a minimal set of controls, so the workflow is fast once the front-panel logic is learned.

## Quick Start

If you just want to get sound quickly:

1. Put at least one `.mid` file into `0:/midi`.
2. Put at least one `.sf2` file into `0:/soundfonts`.
3. Insert the SD card and boot the module.
4. Open the main menu with a long encoder press.
5. Load a MIDI file.
6. Load an SF2.
7. Return to the performance screen.
8. Press `Play`.

If sync is set to external and no valid external clock is present, playback will not run. In that case either:

| Option | Action |
| --- | --- |
| Use internal sync | Set the sync source switch to internal |
| Keep external sync | Provide MIDI clock or gate clock |

## SD Card Layout

Major MIDI expects these folders:

```text
0:/midi
0:/soundfonts
```

Example:

```text
0:/midi/825.mid
0:/midi/beat_loop.mid
0:/soundfonts/microgm.sf2
0:/soundfonts/another.sf2
```

The browser ignores hidden AppleDouble files such as:

| Ignored file examples |
| --- |
| `._581.mid` |
| `._microgm.sf2` |

## Boot Behavior

On boot:

| Step | Behavior |
| --- | --- |
| Display | OLED shows a splash screen |
| Media scan | SD card is scanned |
| Content | The first available MIDI and SF2 can be loaded |
| Config restore | Saved CV/gate config is read from `0:/major_midi_cv_gate.bin` if present |

The firmware does not background-save CV/gate changes while running, because SD writes during active streaming caused freezes. The safe save path is `Save All`.

## Front Panel Overview

Major MIDI uses:

| Control | Role |
| --- | --- |
| `B1..B4` | Channel / bank buttons |
| `K1..K4` | Parameter knobs for the visible channels |
| `Play` | Transport control |
| Encoder with push switch | Navigation, editing, and shift actions |
| OLED display | Main feedback display |

These controls do different things depending on the current mode:

| Mode | Purpose |
| --- | --- |
| `performance` | Playback and quick mixing |
| `mute` | Fast per-channel muting |
| `loop edit` | Quick loop boundary editing |
| `main menu` | Top-level configuration pages |
| `submenu page` | Parameter editing inside a selected page |

## Performance Screen

The performance screen is the default operating view.

Top line example:

```text
STP 120 M001 B1 V
```

This means:

| Field | Meaning |
| --- | --- |
| `STP` or `PLY` | Stop or play state |
| `120` | Current BPM |
| `M001` | Current measure |
| `B1` | Visible bank |
| `V` | Current quick page |

| Line | Shows |
| --- | --- |
| Second line | Currently loaded MIDI file |
| Third line | Currently loaded SF2 |

The bottom of the screen shows the visible 4 channels as columns.

Muted channels are shown with a leading `*`, for example:

```text
*01
```

Zero values are shown as:

```text
-
```

## Performance Workflow

### Banks

The 16 MIDI channels are grouped in banks of 4:

| Bank | Channels |
| --- | --- |
| `B1` | 1-4 |
| `B2` | 5-8 |
| `B3` | 9-12 |
| `B4` | 13-16 |

Press a bank button once to select that bank.

### Quick Pages

The performance screen has 5 quick pages:

| Page | Function |
| --- | --- |
| `V` | Volume |
| `P` | Pan |
| `R` | Reverb send |
| `C` | Chorus send |
| `G` | Program |

There are two ways to move between pages:

| Method | Action |
| --- | --- |
| `Encoder + B1` | Cycle quick page |
| Repeated quick bank press | Advance the current page for that bank |

Example:

- press `B1` once: bank 1, page stays as-is
- press `B1` again quickly: page advances
- press `B1` again quickly: page advances again

So if you are on bank 1:

- `B1` once may show `B1 V`
- `B1` twice more quickly may land on `B1 R`

If you then press `B2` once:

- bank changes to 2
- page stays `R`

If you then press `B3` quickly three times:

- bank changes to 3
- page cycles three times
- for example `R -> C -> G -> V`

### Knobs

`K1..K4` always control the 4 visible channels for the active quick page.

Examples:

| Page | `K1..K4` control |
| --- | --- |
| `V` | Volume for the visible channels |
| `P` | Pan |
| `R` | Reverb send |
| `C` | Chorus send |
| `G` | Program override |

The knob system uses pickup/hysteresis behavior, so a knob usually needs to cross the current stored value before it starts changing that parameter. This prevents abrupt jumps after bank/page changes.

### Play Button

`Play` toggles:

| Button | Result |
| --- | --- |
| `Play` | Toggle play / stop |

If external sync is selected and there is no valid external clock, the transport will not free-run.

### Encoder Turn

In performance mode, turning the encoder changes BPM.

BPM range:

| Parameter | Range |
| --- | --- |
| BPM | `20` to `300` |

If a song BPM override or CV BPM control is active, that affects the effective playback tempo.

## Shift Actions

The encoder press acts as shift in performance mode.

Available combos:

| Combo | Action |
| --- | --- |
| `Encoder + B1` | Cycle quick page |
| `Encoder + B2` | Enter or exit mute mode |
| `Encoder + B3` | Mute all channels or unmute all channels |
| `Encoder + B4` | Enter loop edit |
| `Encoder + Play` | Enters menu behavior in the current UI model via long-press / menu system |

### Mute All Behavior

`Mute All` is not a separate global mute engine anymore.

Instead, it directly toggles the `muted` flag on all 16 channels:

| State | Result |
| --- | --- |
| Not all channels muted | Mute them all |
| All channels already muted | Unmute them all |

This means mute state remains per-channel and visible everywhere.

## Mute Mode

Mute mode is meant for fast channel muting without touching the mix values.

While in mute mode:

| Control | Action |
| --- | --- |
| `B1..B4` | Toggle mute on the visible 4 channels |
| `Encoder + B1..B4` | Switch to another bank while staying in mute mode |
| `Encoder press` | Return to performance |

Important:

| Behavior | Result |
| --- | --- |
| Muting | Does not overwrite volume |
| Unmuting | Returns to the previous channel volume value |

## Loop Edit

Loop edit is a quick front-panel way to set loop boundaries.

In loop edit:

| Control | Action |
| --- | --- |
| `K1` | Loop start |
| `K2` | Loop end |
| `Encoder press` | Exit loop edit |

Under the hood, Major MIDI keeps more precise loop metadata than the coarse performance controls expose. Loop playback was reworked to avoid seam restarts and instead pre-schedule the next loop cycle, which is why loop timing is now much more stable than earlier versions.

## Main Menu

Open the main menu with a long encoder press.

Main menu items:

| Menu Item | Purpose |
| --- | --- |
| `Load MIDI` | Load a MIDI file from SD |
| `Load SF2` | Load a SoundFont from SD |
| `FX Settings` | Edit global synth FX parameters |
| `Song Settings` | Edit and save per-song metadata |
| `SF2 Settings` | Edit deeper synth and channel settings |
| `CV/Gate` | Configure CV and gate routing |
| `Save All` | Persist current song and CV/gate settings |

Menu controls:

| Control | Action |
| --- | --- |
| `Encoder turn` | Move cursor |
| `Encoder press` | Select item |
| `Encoder press while editing` | Finish editing |
| `Play` | Back / exit current submenu |
| Long encoder press | Leave the menu entirely |

## Load MIDI

This page shows the available `.mid` files.

Behavior:

| Step | Behavior |
| --- | --- |
| Selection | Choose a file with the encoder |
| Confirm | Press encoder to load it |
| Loading | A loading screen appears |
| Success | Returns to performance mode |
| Failure | Stays in the menu and shows an error message |

## Load SF2

This page works the same way as `Load MIDI`, but for SoundFonts.

| Outcome | Result |
| --- | --- |
| Success | The SF2 is loaded and the UI returns to performance mode |
| Failure | The UI stays in the menu and an error overlay is shown |

## FX Settings

This page adjusts the global FX parameters used by the synth engine.

Parameters:

| Parameter |
| --- |
| `Rev Time` |
| `Rev LPF` |
| `Rev HPF` |
| `Ch Depth` |
| `Ch Speed` |

These are global synth settings, not per-channel send levels.

Per-channel reverb and chorus amounts are on:

| Location |
| --- |
| Performance quick pages |
| SF2 settings channel page |

## Song Settings

This page stores song-level metadata in the current MIDI file.

Parameters:

| Parameter | Purpose |
| --- | --- |
| `BPM Ovr` | Per-song BPM override |
| `Loop` | Loop enabled or disabled |
| `Loop St` | Loop start measure |
| `Loop End` | Loop end measure / derived loop length |
| `Save To MIDI` | Write the current song metadata into the MIDI file |

This data is stored in the custom `MMID` meta-event block.

### What Song Settings Persist

Current song save data includes:

| Saved song data |
| --- |
| BPM override |
| Loop enabled |
| Loop start |
| Loop beat/sub data |
| Loop length |
| Other Major MIDI song settings already supported by the metadata block |

## SF2 Settings

This page is the deeper per-channel and synth configuration page.

Parameters:

| Parameter |
| --- |
| `Voices` |
| `Channel` |
| `Mute` |
| `Volume` |
| `Pan` |
| `RevSend` |
| `ChoSend` |
| `Program` |
| `Trans` |

### Voices

`Voices` sets the maximum synth voice count.

Range:

| Parameter | Range |
| --- | --- |
| `Voices` | `4` to `32` |

This directly affects CPU load. If you hear crunching at high polyphony:

| Recommendation | Why |
| --- | --- |
| Lower the voice count | Reduces CPU load |
| Be extra conservative with dense MIDI files and heavy FX | Those combinations increase load fastest |

### Channel

Selects which MIDI channel the rest of the SF2 page edits.

### Program

Program behavior is important:

| Mode | Behavior |
| --- | --- |
| `Program File` | Follow the MIDI file's program changes |
| `Program 000..127` | Force a program override on that channel |

Program overrides are also available from the live performance `G` page.

### Transpose

Global transpose for melodic channels.

Drum channel handling remains separate so channel 10 behaves as expected for kits.

## CV/Gate

Phase 1 CV/gate routing is implemented.

The page dynamically hides irrelevant options. For example:

| Field | When it appears |
| --- | --- |
| Channel | Only when the selected mode needs a channel |
| CC | Only when the selected mode needs a CC number |
| Sync resolution | Only for sync gate output |

### CV Inputs

Available inputs:

| Input |
| --- |
| `CV In 1` |

Modes:

| Mode | Function |
| --- | --- |
| `Off` | Disabled |
| `MasterVol` | Map CV to overall output gain |
| `BPM` | Map CV to tempo |
| `Ch CC` | Send a continuous CC to the selected channel |

#### CV Input: MasterVol

Maps CV to overall output gain.

#### CV Input: BPM

Maps CV to tempo.

Current range:

| Parameter | Range |
| --- | --- |
| CV BPM control | `20` to `300` BPM |

#### CV Input: Ch CC

Sends a continuous CC to the selected channel.

### Gate Outputs

Available outputs:

| Output |
| --- |
| `Gate Out 1` |
| `Gate Out 2` |

Modes:

| Mode | Function |
| --- | --- |
| `Off` | Disabled |
| `Sync` | Output a transport clock pulse |
| `Reset` | Output a short pulse every measure |
| `Ch Gate` | Output a gate while the selected channel has active notes |

#### Sync

Outputs regular pulses at:

| Sync resolution |
| --- |
| `1/4` |
| `1/8` |
| `1/16` |
| `1/32` |
| `1/64` |

#### Reset

Outputs a short pulse every measure.

#### Ch Gate

Outputs a gate while the selected channel has active notes.

### CV Outputs

Available outputs:

| Output |
| --- |
| `CV Out 1` |

Modes:

| Mode | Function |
| --- | --- |
| `Off` | Disabled |
| `Pitch` | Monophonic pitch CV for the selected channel |
| `CC` | Output the current CC value for the selected channel and CC number |

#### Pitch

Pitch mode is monophonic per selected channel.

Priority options:

| Pitch Priority |
| --- |
| `Highest` |
| `Lowest` |

#### CC

Outputs the current CC value for the selected channel and CC number.

## Save All

`Save All` is available from the main menu and includes a confirm/cancel screen.

Saved data:

| Saved item | Destination |
| --- | --- |
| Current MIDI song settings | Current MIDI file |
| Current CV/gate routing | `0:/major_midi_cv_gate.bin` |

Safety behavior:

| Condition | Result |
| --- | --- |
| Playback stopped | Save can proceed |
| Playback active | UI shows `Stop Playback First` |

This is currently the safe persistence path for CV/gate settings.

## Sync

Major MIDI supports:

| Sync Mode | Source |
| --- | --- |
| Internal | Internal BPM engine |
| External | MIDI clock or gate pulse sync |

The sync source switch is on MCP23017 `GPB1`.

### Internal Sync

When the switch is up:

| Switch State | Behavior |
| --- | --- |
| Up | The transport uses the internal BPM engine and encoder BPM changes apply directly |

### External Sync

When the switch is down:

| Switch State | Behavior |
| --- | --- |
| Down | Transport follows external timing |

Current external sync sources:

| External Source | Input |
| --- | --- |
| MIDI clock | MIDI input |
| Gate pulse sync | `gate_in_1` |

Priority:

| Priority Order |
| --- |
| MIDI clock if locked |
| Otherwise gate clock if locked |

If external sync is selected and no valid external clock is present:

| Condition | Result |
| --- | --- |
| External sync selected with no valid clock | Playback does not free-run |

### External Sync Notes

External sync behavior has improved substantially, but this is still one of the more complex areas of the firmware. In particular:

- clock drop/reacquire behavior is still something to keep testing
- gate pulse expectations matter
- MIDI start/continue/stop and gate clock behavior do not map perfectly to every external setup

## MIDI Input

Supported live MIDI inputs:

| Input | Mapping |
| --- | --- |
| UART MIDI | `A2/A3` |
| External USB MIDI | `A8/A9` |

Live MIDI is serviced on the fixed control/audio path for better timing than simple main-loop polling.

Handled message types:

| Message Type |
| --- |
| Note on |
| Note off |
| Control change |
| Program change |
| Pitch bend |
| All notes off |
| All sound off |
| MIDI clock |
| MIDI start |
| MIDI continue |
| MIDI stop |

## Program Handling

Program handling now works in three layers:

1. file program changes from the MIDI file
2. restored file program state on seek / loop start
3. user program override on top

This matters because Major MIDI can:

| Scenario |
| --- |
| Start in the middle of a song |
| Start from a loop point |
| Wrap loops continuously |

The firmware now restores the latest file program state before a seek point, then applies the user override if one exists.

### Performance Program Page

The `G` quick page is the fast live program page.

Behavior:

| Behavior | Result |
| --- | --- |
| Bank selection | Chooses which 4 channels you are editing |
| Knobs | Set the program override for those 4 channels |
| Bottom row on the `G` page | Shows the current program values |

## Looping

Looping in Major MIDI is not a naive stop-and-restart seam anymore.

Current loop design:

| Loop Behavior | Effect |
| --- | --- |
| Loop playback is scheduled ahead | Avoids seam restarts |
| The next loop cycle is appended before the seam | Keeps timing tight |
| Loop timing uses the active BPM | Tempo remains consistent |
| Loop starts can begin immediately from the chosen loop point | Faster operator workflow |
| Stuck notes at the loop boundary are explicitly terminated on wrap | Prevents hanging notes |

This is why loop playback is now much tighter than earlier revisions.

## Performance Tuning Notes

If you are hearing crunching or overload:

| Action | Why |
| --- | --- |
| Reduce `Voices` in `SF2 Settings` | Lowers synth CPU load |
| Use lighter SF2 files | Reduces sample and voice overhead |
| Reduce extreme polyphony in the MIDI file | Prevents dense note bursts from overloading playback |

The synth engine also uses:

| Built-in mitigation |
| --- |
| Output limiting |
| Some FX load shedding at high voice counts |

But max voices is still the main operator control for CPU load.

## File Saving and Persistence

### What Saves Per MIDI File

Song metadata in the MIDI file can include:

| Per-file saved data |
| --- |
| BPM override |
| Loop metadata |
| Program overrides already supported by the `MMID` metadata block |

### What Saves Globally

Currently:

| Global saved data | Location |
| --- | --- |
| CV/gate config | `0:/major_midi_cv_gate.bin` |

### What Does Not Background-Save

CV/gate config is not auto-saved during runtime because SD writes collided with active SF2 and MIDI streaming and caused freezes. `Save All` is the safe user-facing save flow.

## Hardware Mapping

Current firmware assumptions match the tested hardware used during development.

| Item | Mapping |
| --- | --- |
| OLED | `I2C1` on `B7/B8`, address `0x3C` |
| MCP23017 | `I2C1` on `B7/B8`, address `0x20` |
| Encoder A/B | `D9/D10` |
| Encoder switch | MCP `GPB2` |
| Sync source switch | MCP `GPB1` |
| Bank buttons / play | MCP inputs |
| Knobs | `CV_1..CV_4` |
| CV inputs | `CV_5`, `CV_6` |
| Gate input sync | `gate_in_1` |
| Gate outputs | `gate_out_1`, `gate_out_2` |
| UART MIDI | `UART_4` on `A2/A3` |
| External USB MIDI | `A8/A9` |

## Build

From the repo root:

```sh
make -C DaisyExamples/patch_sm/SF2MidiPlayer
```

Artifacts:

| Artifact |
| --- |
| `build/SF2MidiPlayer.elf` |
| `build/SF2MidiPlayer.hex` |
| `build/SF2MidiPlayer.bin` |

## Typical Workflows

### Play a Song

1. Long-press encoder to open the menu.
2. Load a MIDI file.
3. Load an SF2.
4. Return to performance.
5. Press `Play`.

### Mix the Visible 4 Channels

1. Choose the bank with `B1..B4`.
2. Cycle to the page you want:
   - `V`
   - `P`
   - `R`
   - `C`
3. Turn `K1..K4`.

### Change Instruments for a Bank

1. Go to the desired bank.
2. Cycle to page `G`.
3. Turn `K1..K4` to set programs for the 4 visible channels.

### Set a Loop and Save It

1. Enter `Loop Edit` or `Song Settings`.
2. Set the loop start and loop length.
3. Save the song settings to the MIDI file.

### Configure a Clock Output

1. Open `CV/Gate`.
2. Set `Gate Out 1` or `Gate Out 2` to `Sync`.
3. Choose a resolution.

### Save Your Setup

1. Stop playback.
2. Open main menu.
3. Choose `Save All`.
4. Confirm.

## Troubleshooting

### The song will not start in external sync mode

Possible causes:

| Possible Cause |
| --- |
| No MIDI clock is present |
| No gate clock is present |
| Sync source switch is in external mode with no valid clock |

### I changed a program and other channels changed unexpectedly

This was previously caused by a synth-wide reset path tied to voice-count reapplication. The current firmware only reapplies max voices when the value actually changes.

### CV/Gate menu changes used to freeze the module

That was caused by background SD writes during active streaming. Runtime autosave was removed and replaced with explicit save flow.

### The loop used to hang notes

Loop wrap now flushes still-active notes at the seam before continuing the next cycle.

### Playback crunches at high polyphony

Reduce:

| Reduce |
| --- |
| Voice count |
| File density |
| SF2 complexity |

### External sync is still not perfect for my setup

That area is still under active iteration. Test with a known-good clock source first and verify whether your external gate clock is behaving as expected.

## Developer Notes

Major MIDI evolved from a simpler SF2 MIDI player into a more structured UI-driven firmware. The current architecture separates:

| Layer |
| --- |
| Hardware input |
| UI event translation |
| App state |
| Controller logic |
| Renderer |
| Transport / mixer behavior |
| CV/gate engine |

Important implementation notes:

| Note |
| --- |
| SF2 and MIDI playback both stream from SD |
| Avoid runtime SD writes during active playback unless explicitly managed |
| Live MIDI timing was moved off naive main-loop polling |
| Loop playback now uses continuous ahead-of-time scheduling |

## License / Project Context

This project expects to live at:

```text
DaisyExamples/patch_sm/SF2MidiPlayer
```

so it can build against the local `libDaisy` and `DaisySP` trees in this repository layout.
