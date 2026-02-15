# SF2MidiPlayer

This is a starter for a general midi enabled eurorack module

The module will load and play SMF thru a fluidsytnh inspired SF2 loader via tinysoundfount

This repo would need to be installed withion the DaisyExamples/patch_sm/SF2MidiPlayer context so that all libraries are available to build and load the patch. 

## Controls

This example automatically loads `microgm.sf2` from the SD card and triggers Middle C on startup, so none of the front-panel knobs, CV inputs, or gate jacks are read by the firmware yet. Plug in the audio outputs and insert a soundfont-equipped SD card to hear the patch. Add CV/gate handling in `src` if you would like to control the synth parameters in real time.

| Pin Name | Pin Location | Function | Comment |
| --- | --- | --- | --- |
| CV_1 | C5 | Not used | No knob or CV value is read by this example. |
| CV_2 | C4 | Not used | Reserved for future modulation. |
| CV_3 | C3 | Not used | Reserved for future modulation. |
| CV_4 | C2 | Not used | Reserved for future modulation. |
| CV_5 | C6 | 1V/Oct input (unused) | Patch SM provides a 1V/oct input here; you can wire it up and call `GetAdcValue(CV_5)` if you want pitch control. |
| OUT_R | B1 | Audio output (right) | Feed into a mixer/line input or module. |
| OUT_L | B2 | Audio output (left) | Feed into a mixer/line input or module. |
| Micro SD slot | On board | SoundFont + MIDI storage | Drop `soundfonts/microgm.sf2` (and any SMF you add later) inside `0:/soundfonts/`. |
| USB-C | USB connector | Power & firmware loading | Power the Patch SM and flash via `make program`. |
