# Major MIDI

Major MIDI is Daisy Patch SM (STM32H750, Cortex-M7) firmware for playing Standard MIDI Files from SD card through a SoundFont 2 synth engine. The firmware also exposes live MIDI input, per-channel mixing, saved song state, MIDI routing, CV/gate integration, and a browser-based remote control / MIDI file transfer over USB SysEx.

Docs site: [zackhagan.com/major-midi/](https://zackhagan.com/major-midi/)

The user-facing guide lives in `site/USER.md`. The generated docs site in `docs/` is built from that file (and three others — see Docs Workflow below).

---

## Updating firmware

Major MIDI ships as a complete Eurorack module with firmware already installed, an SD card, and a power cable. **You do not need this section to start using one** — it covers updating an existing module, and bringing up a bare Daisy Patch SM from scratch.

Major MIDI is a `BOOT_QSPI` application: it lives in QSPI flash at `0x90040000` and is launched by the Daisy bootloader in internal flash. Shipped modules already have that bootloader.

### Updating a module

No toolchain required. Download `MajorMIDI.bin` from the [latest release](../../releases/latest), then tap **RESET** — the bootloader opens a 2-second DFU window on power-up, and you flash within it.

**Option A — browser (no install).** Open the [Electro-Smith Web Programmer](https://electro-smith.github.io/Programmer/) in Chrome or Edge, connect the module over USB while it is in that DFU window, select `MajorMIDI.bin`, and flash to the **QSPI** target.

**Option B — `dfu-util`.**

```sh
dfu-util -a 0 -s 0x90040000:leave -D MajorMIDI.bin -d ,0483:df11
```

Power-cycle when it completes. The module enumerates over USB as **Major MIDI**.

> If the board does not appear, it is almost always the DFU window having closed — tap **RESET** and retry immediately. `dfu-util -l` is the quickest way to confirm the host sees the device before committing to a flash.

### First-time bring-up on a bare Patch SM

Only needed if you are building your own module from a stock Daisy Patch SM. A stock board does **not** ship with the Daisy bootloader, and without it flashing the app appears to succeed while the module does nothing on power-up.

Put the board into the STM32 system bootloader: hold **BOOT**, tap **RESET**, then release **BOOT**. Confirm the host sees it:

```sh
dfu-util -l          # expect a device with PID 0483:df11
```

Then flash the bootloader to internal flash. From a source checkout:

```sh
make program-boot
```

Or directly, using `dsy_bootloader_v6_2-intdfu-2000ms.bin` from a libDaisy checkout or the release assets:

```sh
dfu-util -a 0 -s 0x08000000:leave -D dsy_bootloader_v6_2-intdfu-2000ms.bin -d ,0483:df11
```

Then flash the app as described under **Updating a module** above. This is a one-time step per board.

---

## Building from source

### Prerequisites

| Tool | Purpose | Install (macOS) |
| --- | --- | --- |
| `arm-none-eabi-gcc` | Cross-compiler | `brew install --cask gcc-arm-embedded` |
| `dfu-util` | USB flashing | `brew install dfu-util` |
| `make`, `git` | Build / checkout | Xcode Command Line Tools |
| `python3` | Docs generation (optional) | Preinstalled |

### Clone

The build depends on two submodules, one of which has a nested submodule of its own — `--recursive` is required:

```sh
git clone --recursive git@github.com:zhagan/major-midi.git
cd major-midi
```

Already cloned without it?

```sh
git submodule update --init --recursive
```

### Build

The dependencies are static libraries and only need rebuilding when they change:

```sh
make -C lib/libDaisy      # builds libdaisy.a
make -C lib/DaisySP       # builds libdaisysp.a and libdaisysp-lgpl.a
make                      # builds build/MajorMIDI.bin
```

`make clean` clears the firmware build only, not the libraries.

### Flash

With the bootloader already installed (see step 1 above), tap **RESET** and run:

```sh
make program-dfu
```

`make program` is **not** usable here — it errors out for `BOOT_QSPI` app types by design.

---

## Dependencies

| Path | Upstream | Pinned at | Why |
| --- | --- | --- | --- |
| `lib/libDaisy` | Fork of [electro-smith/libDaisy](https://github.com/electro-smith/libDaisy), branch `major-midi` | `85172e2b` (v5.4.0-22) + 3 commits | **Modified.** See below. |
| `lib/DaisySP` | [electro-smith/DaisySP](https://github.com/electro-smith/DaisySP) | `a0494a3` (V1.0.0) | Unmodified, pinned only |
| `lib/DaisySP/DaisySP-LGPL` | [electro-smith/DaisySP-LGPL](https://github.com/electro-smith/DaisySP-LGPL) | `c89d380` | Unmodified. Required — the Makefile sets `USE_DAISYSP_LGPL = 1` |
| `src/synth/tsf.h` | [schellingb/TinySoundFont](https://github.com/schellingb/TinySoundFont) | vendored copy | Modified in-tree; not a submodule |

The libDaisy fork carries three changes, kept as three reviewable commits on top of the pinned upstream commit so it can be rebased onto future releases:

1. **`SSD130x`: batched I2C writes, column offset, chunked update.** Upstream issues one 2-byte I2C transaction per pixel byte, which cannot refresh a 128x64 panel at UI rates. Adds 32-byte batching, `SetColumnOffset()` for panels whose column 0 is offset, and a chunked-update API that spreads a frame across several main-loop passes so the display never blocks audio or MIDI.
2. **MIDI: `StopReceive`/`StopRx`, `ServiceTx` hook, UART Tx size guard.** Needed to tear down and re-establish UART MIDI when routing changes at runtime.
3. **USB descriptor: identify as "Major MIDI".** Product/config/interface strings plus a distinct PID (22337) on the external HS port.

`lib/libDaisy/tests/googletest` is a nested submodule used only by libDaisy's own unit tests. It is not needed to build firmware.

---

## Repo Layout

| Path | Purpose |
| --- | --- |
| `src/main.cpp` | App bootstrap, boot sequence, media loading, transport loop, save/load flow, USB/UART MIDI wiring |
| `src/app_state.h` | `AppState` — the shared state struct everything else reads and writes |
| `src/ui/` | Input scanning, UI event translation, controller logic, OLED rendering |
| `src/midi/` | SMF playback, media browser, scheduling, routing, transport, SysEx remote control and file transfer |
| `src/synth/` | SoundFont synth integration |
| `src/cv/` | CV/gate engine |
| `src/persist/` | Boot state and per-song persistence |
| `lib/` | Pinned dependencies (submodules) — see Dependencies above |
| `docs/` | Generated site output plus the generator script, and two hand-written pages (`transfer.html`, `remote.html`) that are not generated |
| `site/` | Markdown source for the generated pages (`SPLASH.md`, `USER.md`, `ORDER.md`) |
| `DEV.md` | Developer-facing build notes, source map, and protocol reference |
| `alt_sram.lds` | Custom linker script used instead of libDaisy's default QSPI script |

## Docs Workflow

The published docs site is generated from four Markdown sources:

```sh
python3 docs/generate_docs.py
```

| Source | Output |
| --- | --- |
| `site/SPLASH.md` | `docs/index.html` |
| `site/USER.md` | `docs/user.html` |
| `DEV.md` | `docs/dev.html` |
| `site/ORDER.md` | `docs/order.html` |

`docs/transfer.html` and `docs/remote.html` are separate hand-written pages (static HTML + embedded JS Web MIDI clients) that the generator does not touch — edit them directly.

## Persistence Model

Major MIDI persists two different layers of state, both handled in `src/persist/song_config_persist.cpp` and `src/persist/boot_state_persist.cpp`:

| File | Purpose |
| --- | --- |
| `0:/major_midi_boot.cfg` | Last boot MIDI selection plus general UI preferences |
| `<selected-midi>.cfg` | Song-scoped settings, SF2 selection, MIDI routing, CV/gate, channel mix state |

`Song > Save Song CFG` writes both files immediately, with no confirmation and without requiring playback to be stopped. `Save All` writes the same files behind a confirmation prompt and refuses to run while the transport is playing or a channel gate is active. See `DEV.md` for the exact field layout, and note that `src/persist/midi_routing_persist.*`, `src/persist/cv_gate_persist.*`, and `src/persist/performance_persist.*` are unused/orphaned modules — the real serialization lives in `song_config_persist.cpp`.

## Development Notes

- The repo may already contain local work; avoid broad reverts.
- The UI behavior is driven primarily by `src/ui/ui_controller.cpp` and `src/ui/ui_renderer.cpp`.
- The docs generator is intentionally simple Markdown-to-HTML code, not MkDocs.
- Changing a dependency means committing in `lib/<dep>` first, then committing the updated submodule pointer here.

## Licensing

libDaisy is MIT. DaisySP is MIT, but this firmware links `DaisySP-LGPL` (`USE_DAISYSP_LGPL = 1`), which is LGPL — distributing binaries carries the LGPL obligation to let recipients relink against a modified version of that library. Publishing full source and build instructions, as this repo does, satisfies that. `tsf.h` is MIT.
