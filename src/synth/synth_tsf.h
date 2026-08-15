#pragma once
#include <cstdint>
#include <cstddef>

enum class SynthLoadResult : uint8_t
{
    Ok,
    FileOpenFailed,
    FileTooLarge,
    ParseFailed,
};

// Initialize synth system (arena, etc.)
bool SynthInit();

// Load SoundFont from SD (example: "0:/soundfonts/microgm.sf2")
bool SynthLoadSf2(const char* path, float sampleRate, int maxVoices);
SynthLoadResult SynthLastLoadResult();
// Unload current SoundFont (clears tsf + closes file)
void SynthUnloadSf2();
int  SynthActiveVoiceCount();
void SynthSetMaxVoices(int maxVoices);
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
const char* SynthProgramName(uint8_t ch, uint8_t program);

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
void SynthSetReverbEnabled(bool enabled);
void SynthSetChorusDepth(float d01);
void SynthSetChorusSpeed(float hz);
void SynthSetChorusEnabled(bool enabled);
void SynthSetExternalGain(float gain);
float SynthGetReverbTime();
float SynthGetReverbLpFreq();
float SynthGetReverbHpFreq();
bool  SynthGetReverbEnabled();
float SynthGetChorusDepth();
float SynthGetChorusSpeed();
bool  SynthGetChorusEnabled();
