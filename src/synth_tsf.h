#pragma once
#include <cstdint>
#include <cstddef>

// Initialize synth system (arena, etc.)
bool SynthInit();

// Load SoundFont from SD (example: "0:/soundfonts/microgm.sf2")
bool SynthLoadSf2(const char* path, float sampleRate, int maxVoices);

// Immediately stop all notes
void SynthPanic();

// Basic voice control
void SynthNoteOn(uint8_t preset, uint8_t key, uint8_t velocity);

void SynthNoteOff(uint8_t preset, uint8_t key);

// Render stereo block
void SynthRender(float* outL, float* outR, size_t frames);
