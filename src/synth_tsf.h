#pragma once
#include <cstdint>
#include <cstddef>

// Initialize synth system (arena, etc.)
bool SynthInit();

// Load SoundFont from SD (example: "0:/soundfonts/microgm.sf2")
bool SynthLoadSf2(const char* path, float sampleRate, int maxVoices);
// Unload current SoundFont (clears tsf + closes file)
void SynthUnloadSf2();
// Arena diagnostics
size_t SynthArenaUsed();
size_t SynthArenaCap();
bool   SynthArenaOom();

// Immediately stop all notes
void SynthPanic();

// Basic voice control (channel-based, supports program changes)
bool SynthNoteOn(uint8_t ch, uint8_t key, uint8_t velocity);

void SynthNoteOff(uint8_t ch, uint8_t key);
void SynthAllNotesOff(uint8_t ch);
void SynthAllSoundOff(uint8_t ch);

void SynthProgramChange(uint8_t ch, uint8_t program);

void SynthControlChange(uint8_t ch, uint8_t cc, uint8_t value);

void SynthPitchBend(uint8_t ch, uint16_t value);

// Reset channel controllers and default presets (program 0, drums on ch10)
void SynthResetChannels();

// Render stereo block
void SynthRender(float* outL, float* outR, size_t frames);

// Global FX controls
void SynthSetReverbTime(float t01);
void SynthSetReverbLpFreq(float hz);
void SynthSetReverbHpFreq(float hz);
void SynthSetChorusDepth(float d01);
void SynthSetChorusSpeed(float hz);
float SynthGetReverbTime();
float SynthGetReverbLpFreq();
float SynthGetReverbHpFreq();
float SynthGetChorusDepth();
float SynthGetChorusSpeed();
