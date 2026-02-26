#pragma once
#include <cstdint>
#include <cstddef>

// Initialize synth system (arena, etc.)
bool SynthInit();

// Load SoundFont from SD (example: "0:/soundfonts/microgm.sf2")
bool SynthLoadSf2(const char* path, float sampleRate, int maxVoices);

// Immediately stop all notes
void SynthPanic();

// Basic voice control (channel-based, supports program changes)
void SynthNoteOn(uint8_t ch, uint8_t key, uint8_t velocity);

void SynthNoteOff(uint8_t ch, uint8_t key);

void SynthProgramChange(uint8_t ch, uint8_t program);

// Render stereo block
void SynthRender(float* outL, float* outR, size_t frames);
