# Major MIDI

Major MIDI is Daisy Patch SM firmware (STM32H750, Cortex-M7) for playing Standard MIDI Files from an SD card through a SoundFont 2 synth engine, with front-panel mixing, live MIDI input, internal or external sync, saved song settings, MIDI routing, and assignable CV/gate I/O.

## Quick Start

1. Copy one or more `.mid` files into `0:/midi`.
2. Copy one or more `.sf2` files into `0:/soundfonts`.
3. Insert the SD card and power the module.
4. Open the menu with a long encoder press.
5. Load a MIDI file from `Load MIDI`.
6. Load a SoundFont from `Load SF2`.
7. Return to the performance screen.
8. Press `Play`.

If the sync switch is set to external, playback will wait for external clock instead of free-running.

Prefer to read offline? Download this manual as a PDF: [user_manual.pdf](user_manual.pdf).

## SD Card Layout

Major MIDI scans these folders recursively:

```text
0:/midi
0:/soundfonts
```

Example:

```text
0:/midi/set1/track01.mid
0:/midi/loops/arp.mid
0:/soundfonts/general/microgm.sf2
0:/soundfonts/drums/clubkit.sf2
```

Hidden files and AppleDouble files are ignored.

There's no limit on how many `.mid` or `.sf2` files the card can hold in total — folders are browsed live, not pre-scanned into memory. The only cap is per folder: browsing a single folder in `Load MIDI` or `Load SF2` shows up to 128 entries for a MIDI folder and 32 entries for a SoundFont folder — if one folder holds more files than that, split them across subfolders so everything is reachable.

## Boot And Loading

On boot, Major MIDI:

| Step | Behavior |
| --- | --- |
| SD scan | Scans `0:/midi` and `0:/soundfonts` |
| MIDI restore | Reloads the last saved MIDI selection when available |
| SF2 selection | Starts from the first SF2, then may switch to the song's saved SF2 |
| Song config | Loads `<midi-file>.cfg` if present |
| UI prefs | Loads general UI settings from `0:/major_midi_boot.cfg` |

If both a MIDI file and an SF2 are available, the unit loads them automatically at startup.

## Front Panel

<figure class="image-card image-card-wide">
  <div class="image-frame panel-frame">
    <img src="./assets/images/panel-major-midi.png" alt="Major MIDI front panel layout" loading="lazy" />
  </div>
  <figcaption>Front panel: display, transport and bank buttons, four performance knobs, and the CV, gate, and MIDI patch points along the bottom edge.</figcaption>
</figure>

| Control | Role |
| --- | --- |
| `B1..B4` | Bank buttons and view combos |
| `K1..K4` | Per-channel controls for the visible bank |
| `Play` | Transport and back/cancel in menus |
| Encoder turn | Page selection, menu navigation, loop edits, BPM edit |
| Encoder press | Shift, confirm, enter/exit edit modes |
| OLED | Current mode, transport, file names, and parameter values |

`B1..B4` each have an LED that flashes briefly whenever incoming MIDI activity hits a channel in that button's bank, independent of which bank is currently selected on screen.

The sync source is a hardware switch. Internal sync free-runs from the current BPM. External sync follows incoming MIDI clock or configured gate sync.

## Performance View

Performance mode is the default screen.

The top line shows:

| Field | Meaning |
| --- | --- |
| `STP` or `PLY` | Stopped or playing |
| Active page name | The current knob page, spelled out (`Volume`, `Pan`, `Reverb`, `Chorus`, `Program`, `MUTE PAGE`, `BPM`) |
| BPM | Current transport BPM |
| Measure and beat | Current musical position |
| `B1..B4` | Active bank |

The screen also shows the current MIDI file, the current SoundFont, and four visible channels from the selected bank. Each channel row is labeled with a single-letter code for the value it's showing (`V`, `P`, `R`, `C`, `G`, `M`) — those letters only appear in the per-channel grid, not on the top status line.

## Banks And Knob Pages

The 16 channels are shown in four banks:

| Bank | Channels |
| --- | --- |
| `B1` | 1-4 |
| `B2` | 5-8 |
| `B3` | 9-12 |
| `B4` | 13-16 |

Press `B1..B4` to select a bank.

Turn the encoder in performance mode to cycle knob pages:

| Page | Row letter | Function |
| --- | --- | --- |
| Volume | `V` | Volume |
| Pan | `P` | Pan |
| Reverb | `R` | Reverb send |
| Chorus | `C` | Chorus send |
| Program | `G` | Program override |
| Mute | `M` | Mute |
| BPM | `BPM` | Tempo edit page |

`K1..K4` always control the four visible channels for the active page.

With `Knobs` set to `Pickup`, a knob must cross the stored value before it takes control. With `Knobs` set to `Instant`, the value jumps immediately.

## BPM Editing

The encoder does not always change tempo directly.

To edit tempo:

1. Turn the encoder until the active page is `BPM`.
2. Tap the encoder to unlock BPM editing.
3. Turn the encoder to set BPM from `20` to `300`.
4. Tap the encoder again to lock BPM.

BPM is hard-clamped to the 20-300 range everywhere it can be set (performance page, Song BPM override, and the web remote). If a song BPM override is saved for the current MIDI file, that override is applied when the song loads.

When you change tempo during playback, Major MIDI glides to the new BPM instead of snapping to it — the transport ramps at up to 240 BPM per second so a large jump takes a moment to settle, keeping timing musical rather than jarring. This smoothing applies to tempo changes you make by hand (encoder or web remote); locking to external MIDI/gate clock tracks the incoming clock directly, and a tempo change made while looping re-seeks the loop instead of ramping.

## Channel Focus

Long-press one of `B1..B4` in performance mode to focus that visible channel. This is disabled while the `Mute` knob page is active — on that page, a plain press of `B1..B4` toggles mute directly instead (see Mute Page below).

Channel focus shows:

| Item | Meaning |
| --- | --- |
| Channel and bank | Which channel is selected |
| Program | Current program or override, marked `OVR` for a manual override or `MID` when following the file |
| Program name | Current GM/SF2 program label when available |
| Volume and pan | Current mix values. Volume is a scaler on top of the MIDI file's own CC7 (channel volume) automation, not a replacement for it — at `127` the file's CC7 passes through unchanged, and lower settings quiet that channel proportionally while preserving the file's original mix and any volume automation baked into the track. |
| Reverb and chorus | Current send values |
| Mute state | Per-channel mute status |

Turn the encoder to move between fields, cycling `Mute` → `Volume` → `Pan` → `Reverb` → `Chorus` → `Program`. Tap the encoder to enter edit mode for the selected field, turn to adjust it, then tap again to lock it back. Long-press the encoder to leave channel focus entirely and return to the normal bank view — a plain tap only toggles edit mode, it does not exit.

## Mute Page

On the `M` knob page:

| Control | Result |
| --- | --- |
| `K1..K4` | Set mute state for the visible channels |
| `B1..B4` | Toggle mute for the visible channels directly |

To switch banks while on the mute page, hold the encoder and press a bank button.

## Extra Views

Press the button combos below to open alternate views:

| Combo | View |
| --- | --- |
| `B1 + B2` | MIDI monitor |
| `B1 + B3` | Transport/song info |
| `B1 + B4` | Loop edit |

You can jump directly between MIDI monitor, transport view, and loop edit using these same combos without returning to performance mode first. The combos are only ignored while a menu is open.

### MIDI Monitor

The MIDI monitor shows the most recent note, pitch bend, and CC activity for each channel, five channels at a time. Scroll to reach the rest.

Controls:

| Control | Result |
| --- | --- |
| Encoder turn | Scroll channel list |
| `Play` | Clear monitor data |
| Encoder tap or long press | Exit |

### Transport View

The transport view shows the current MIDI file, measure and beat, loop range, current tick, active voice count, and time signature.

Press `Play`, tap the encoder, or long-press the encoder to exit back to performance mode.

## Loop Edit

Loop edit gives direct access to the current loop range.

Editable fields:

| Field | Meaning |
| --- | --- |
| `Active` | Enable or disable looping |
| `St M` | Start measure |
| `St B` | Start beat |
| `St T` | Start tick |
| `Ln M` | Loop length in measures |
| `Ln B` | Additional beats |
| `Ln T` | Absolute length in ticks (up to 2,000,000,000) |

Controls:

| Control | Result |
| --- | --- |
| Encoder turn | Move between fields |
| Encoder press | Toggle edit mode for the selected field |
| `Play` | Exit loop edit |
| Encoder long press | Exit loop edit |

## Menu Basics

Open or close the main menu with either:

| Action | Result |
| --- | --- |
| Long encoder press | Toggle menu |
| Hold encoder and press `Play` | Toggle menu |

Inside menus:

| Control | Result |
| --- | --- |
| Encoder turn | Move cursor |
| Encoder press | Enter page, confirm selection, or toggle page edit mode |
| `Play` | Back out one level or leave the menu |

Main menu pages:

| Page | Purpose |
| --- | --- |
| `Load MIDI` | Browse and load `.mid` files |
| `Load SF2` | Browse and load `.sf2` files |
| `General` | UI preferences |
| `FX` | Global synth FX tuning |
| `Song` | Song-level loop, tempo, and quick-save settings |
| `SF2` | Synth and per-channel settings |
| `MIDI` | USB/UART output routing |
| `CV/Gate` | CV and gate assignment |
| `Save All` | Write config files safely |

`Save All` opens a confirmation page (`Confirm Save` / `Cancel`) rather than saving immediately, and it is refused with a `Stop Playback First` message if the transport is playing or any channel gate is currently active.

## Load MIDI And Load SF2

The file browsers support subdirectories.

Behavior:

| Item | Behavior |
| --- | --- |
| Directory | Enter it |
| `[..]` | Go up one level |
| File | Load it |

Loading a MIDI file also reloads the song-scoped config file when `<song>.cfg` exists next to that MIDI file.

## General

General settings:

| Item | Meaning |
| --- | --- |
| `Saver` | Screen saver timeout: `Off`, `10s`, `30s`, `1m`, `2m`, `5m`, `10m`, `30m`, or `1h` (default `1h`) |
| `Knobs` | `Pickup` or `Instant` |
| `Enc` | Encoder direction |
| `OLED X` | Horizontal OLED column offset, `0`-`8` |
| `CV1 Scl` | CV out 1 pitch-scale calibration, `90.0` - `110.0`% (default `102.8`%) |
| `CV2 Scl` | CV out 2 pitch-scale calibration, `90.0` - `110.0`% (default `102.8`%) |
| `Master Vol` | Master volume, `0`-`200`% in `5`% steps (default `100`%) |

`CV1 Scl` and `CV2 Scl` trim the 1V/octave scaling of each pitch CV output in `0.1`% steps. This calibration is global and shared across every song — it lives in the boot config, not the per-song `.cfg` — so you set it once for your rack. Raise the scale if each octave measures slightly flat, and lower it if each octave measures sharp.

`Master Vol` scales the final post-synthesis audio output. It's a device-global setting (stored in the boot config, not per-song) and is separate from the per-channel `Volume` scaler described in [Channel Focus](#channel-focus) and the `SF2` menu, which scales each channel's own CC7 automation rather than the overall output. At `100`% the output is unchanged; raising it above `100`% boosts overall level (up to `200`%), and the downstream limiter still protects against clipping if you push it that high. If a CV input is patched to `MasterVol` (see [CV/Gate](#cvgate)), the CV signal modulates within the range set by `Master Vol` rather than driving gain on its own.

The screen saver only engages in performance mode, and only after the timeout has passed with no button/encoder/knob activity and no overlay message showing.

## FX

FX settings are global, not per-channel.

| Item | Meaning | Range |
| --- | --- | --- |
| `Rev Time` | Reverb time | `0.0` - `1.0` |
| `Rev LPF` | Reverb low-pass filter | `200 Hz` - `18000 Hz` |
| `Rev HPF` | Reverb high-pass filter | `20 Hz` - `1000 Hz` |
| `Ch Depth` | Chorus depth | `0.0` - `1.0` |
| `Ch Speed` | Chorus speed | `0.05 Hz` - `5.0 Hz` |

Per-channel reverb and chorus amounts stay on the performance pages and in the `SF2` menu.

To protect CPU headroom, reverb and chorus are automatically bypassed (dry signal only) once active voice count reaches 16, and are restored once it drops back to 12 or fewer. This happens automatically and isn't a setting you control directly — if you hear FX cut out under a dense arrangement, that's why.

## Song

Song settings:

| Item | Meaning |
| --- | --- |
| `BPM Ovr` | Saved BPM override |
| `Loop` | Loop enable |
| `St M`, `St B`, `St T` | Loop start |
| `Ln M`, `Ln B`, `Ln T` | Loop length |
| `Save Song CFG` | Quick-save the current song's settings without opening the `Save All` confirmation |

`Save Song CFG` writes the same per-song `.cfg` file (plus the boot-state file) that `Save All` writes, but does it immediately with no confirmation step and without requiring playback to be stopped first. Use `Save All` instead when you want the safety of a confirmation prompt and a guaranteed-stopped transport during the write.

## SF2

SF2 settings:

| Item | Meaning |
| --- | --- |
| `Voices` | Max synth voices, `0`-`32` (default `16`); `0` silences the synth entirely |
| `Channel` | Channel being edited |
| `Mute` | Mute for that channel |
| `Volume` | Channel volume. Scales the channel's own MIDI CC7 automation from the file (`127` = pass the file's CC7 through unchanged; lower values quiet it proportionally) rather than overriding it, so a song's original track balance and any volume automation in the file are preserved. A live CC7 message from an external MIDI controller on that channel still takes precedence in real time. |
| `Pan` | Channel pan |
| `RevSend` | Channel reverb send |
| `ChoSend` | Channel chorus send |
| `Program` | Program override or file-follow mode |
| `Trans` | Global transpose, `-24` to `+24` semitones |

Program behavior:

| Value | Result |
| --- | --- |
| `Program File` | Follow program changes from the MIDI file |
| `Program 000..127` | Force a manual program override |

`Trans` shifts every channel except the drum channel (MIDI channel 10) — drum kits stay at their original pitch under transpose.

Higher voice counts increase CPU load. If playback becomes unstable, reduce `Voices`.

## MIDI

The `MIDI` page controls USB and UART output routing.

Available output modes:

| Mode | Meaning |
| --- | --- |
| `Off` | No channel output |
| `Notes` | Forward notes only |
| `Nt+CC` | Forward notes and CCs |
| `N+C+P` | Forward notes, CCs, and programs |
| `Matrix` | Per-channel routing matrix |

Matrix controls let you choose:

| Item | Meaning |
| --- | --- |
| `Mtx Port` | USB or UART |
| `Mtx Src` | Source channel |
| `Mtx Dst` | Destination channel |
| `Mtx Nt` | Forward notes |
| `Mtx CC` | Forward CCs |
| `Mtx Prg` | Forward program changes |

There are also dedicated toggles for:

| Item | Meaning |
| --- | --- |
| `USB Trn` / `UART Trn` | Forward transport messages |
| `USB Clk` / `UART Clk` | Forward clock |
| `USB>UART` | Pass USB input to UART output |
| `UART>USB` | Pass UART input to USB output |

## CV/Gate

Major MIDI exposes exactly two CV inputs, two gate inputs, two gate outputs, and two CV outputs, fully configurable per song.

Input CV modes:

| Mode | Meaning |
| --- | --- |
| `Off` | Disabled |
| `MasterVol` | Master volume control. Modulates within the range set by `Master Vol` in the `General` menu (see [General](#general)) rather than setting gain on its own — the UI setting is the base/ceiling, and the CV value scales within it. |
| `BPM` | Tempo control, `20`-`300` BPM linear across the CV input's `0V`-`5V` range |
| `Ch Pitch` | Channel pitch control |
| `Ch CC` | Channel CC control |
| `NotePitch` | Note pitch control |

Gate input modes:

| Mode | Meaning |
| --- | --- |
| `Off` | Disabled |
| `Sync` | External gate sync input — pulses directly step playback (one pulse advances the loaded MIDI file by one 16th note) and also drive the displayed/effective BPM |
| `NoteTrig` | Trigger notes on a channel |

`NoteTrig` reads its paired CV input as a pitch across a 5-octave span (from `C1` to `C6`) and fires the note at a fixed velocity.

Gate output modes:

| Mode | Meaning |
| --- | --- |
| `Off` | Disabled |
| `Sync` | Clock output |
| `Reset` | Reset pulse output |
| `Ch Gate` | Gate output from a channel |

Gate outputs pulse for about 10 ms.

CV output modes:

| Mode | Meaning |
| --- | --- |
| `Off` | Disabled |
| `Pitch` | Output channel pitch |
| `CC` | Output a channel CC value |

Each CV/gate page also exposes the related channel, CC number, sync resolution, trigger style, or note priority when that mode needs it.

Pitch CV out runs 1V/octave from `C1` (0V) up to `5V` of headroom (about 10 octaves). The 1V/oct scaling is trimmed with `CV1 Scl` / `CV2 Scl` in the `General` menu, not here — that calibration is global (shared across all songs), so it lives with the general settings rather than the per-song CV/Gate config. See [General](#general) for the calibration procedure.

## Sync

Major MIDI has two sync behaviors:

| Sync source | Behavior |
| --- | --- |
| Internal | Plays at the current BPM |
| External | Waits for incoming clock before advancing |

External sync can follow:

| Source | Notes |
| --- | --- |
| MIDI clock | Via incoming MIDI transport clock |
| Gate sync | Via configured gate input sync pulses |

If a gate input is configured in `Sync` mode, its pulses directly drive playback — each pulse advances the loaded MIDI file by one 16th note — and also set the displayed/effective BPM and lock status. MIDI clock only drives playback when no gate input is configured for `Sync`.

If both a MIDI clock source and a gate sync input are active at once, the configured gate sync input takes priority: its pulses step playback directly and MIDI clock is ignored for advancing the file (though it may still be present on the bus).

If external sync is selected and no valid clock arrives, `Play` will arm transport but the song will not move.

## Saving And Recall

There are two ways to write settings to disk, both scoped to the currently loaded song and both writing the per-song `.cfg` plus the boot-state file:

| Action | Behavior |
| --- | --- |
| `Song > Save Song CFG` | Immediate save, no confirmation, works even while playing |
| `Save All` (main menu) | Confirmation prompt, forces the transport to stop first, refuses to run if anything is still playing or gating |

`Save All` stores:

| Saved state |
| --- |
| Selected SF2 for the current song |
| Channel mix, mute, and program override state |
| Song loop data |
| MIDI routing |
| CV/gate assignments |
| General UI settings and last boot MIDI selection |

For reliability, prefer `Save All` when you have time for the confirmation step; use `Save Song CFG` for a fast save between takes.

## Firmware Updates

Your module ships with firmware already installed, so there is nothing to flash to get started. This section is only for moving to a newer release.

Updating needs no toolchain — just a USB cable and a browser:

1. Download `MajorMIDI.bin` from the [latest release](https://github.com/zhagan/major-midi/releases/latest).
2. Connect the module to your computer over USB.
3. Tap the `RESET` button on the Daisy board. This opens a 2-second window where the module accepts a firmware update.
4. Within that window, open the [Electro-Smith Web Programmer](https://electro-smith.github.io/Programmer/) in Chrome or Edge, connect to the module, select `MajorMIDI.bin`, and flash it to the **QSPI** target.
5. Power-cycle the module when it finishes.

If the programmer does not see the module, the 2-second window has almost certainly closed — tap `RESET` again and reconnect straight away.

Updating firmware does not erase your SD card, so your `.mid` files, SoundFonts, and saved `.cfg` song settings all carry over. If you prefer the command line, or you are building your own module from a bare Daisy Patch SM, the repository README covers `dfu-util` and first-time bootloader setup.

## Troubleshooting

| Problem | Check |
| --- | --- |
| No playback when pressing `Play` | Sync switch may be set to external with no incoming clock |
| No files in browser | Confirm files are under `0:/midi` or `0:/soundfonts`, and that the folder doesn't exceed the browser's visible-entry limit |
| Wrong instrument loads with a song | Check the song's saved SF2 and per-channel program overrides |
| Knobs do not respond immediately | `Knobs` may be set to `Pickup` |
| No sound at all | `Voices` may be set to `0`, which silences the synth |
| Audio overload or glitches | Lower `Voices`, reduce dense arrangements, or use a lighter SF2 |
| Reverb/chorus seems to disappear under dense passages | Expected: FX auto-bypasses above 16 active voices and returns at 12 or fewer |

## Changelog

- Performance mode no longer shows a pop-up message in the bottom row of the display when switching knob pages, banks, muting channels, or navigating instrument focus — the display stays clean while you play.
- Instrument focus view no longer shows the small channel-position indicator (e.g. ">01 02 03 04") in the bottom-right corner.
- Gate Sync In now drives MIDI file playback directly (one pulse = one 16th note) and takes priority over MIDI clock when configured, instead of only affecting the BPM estimate.
- Fixed: pressing `Play` under external sync (MIDI clock or gate) could resume mid-song instead of from the start, because pulses received while stopped kept queuing up and were dumped into the playback position the instant playback started. `Play` now always rewinds to the beginning (or the active loop's start point) and discards any backlog from while it was stopped.
- Fixed: long-pressing a bank button now correctly opens that channel's instrument edit instead of jumping to the wrong channel.
