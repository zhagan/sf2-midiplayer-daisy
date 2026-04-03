#include "synth_tsf.h"
#include <cstdint>
#include <cstddef>
#include <cmath>

#include "daisy_patch_sm.h"
#include "daisysp.h"
#include "daisysp-lgpl.h"

extern "C"
{
#include "ff.h"
}

using namespace daisy;
using namespace daisysp;

// -----------------------------
// Deterministic SDRAM arena for TSF
// -----------------------------
class SdramArena
{
  public:
    void Init(void* mem, size_t bytes)
    {
        base_ = (uint8_t*)mem;
        cap_  = bytes;
        used_ = 0;
    }
    void  Reset() { used_ = 0; }
    void* Alloc(size_t bytes, size_t align = 8)
    {
        if(!base_)
            return nullptr;
        size_t start = (used_ + (align - 1)) & ~(align - 1);
        size_t end   = start + bytes;
        if(end > cap_)
            return nullptr;
        used_ = end;
        return base_ + start;
    }
    size_t Used() const { return used_; }
    size_t Cap() const { return cap_; }

  private:
    uint8_t* base_ = nullptr;
    size_t   cap_  = 0;
    size_t   used_ = 0;
};

static SdramArena g_arena;
static bool       g_arena_oom = false;

// Put arena in SDRAM
static uint8_t DSY_SDRAM_BSS sdram_arena_buf[56 * 1024 * 1024];

// -----------------------------
// TinySoundFont config (allocator + no stdio)
// -----------------------------
#define TSF_NO_STDIO

struct ArenaHdr
{
    uint32_t sz;
};

static inline void* ArenaMalloc(size_t bytes)
{
    const size_t total = sizeof(ArenaHdr) + bytes;
    void*        raw   = g_arena.Alloc(total, 8);
    if(!raw)
    {
        g_arena_oom = true;
        return nullptr;
    }
    auto* h = (ArenaHdr*)raw;
    h->sz   = (uint32_t)bytes;
    return (void*)(h + 1);
}

static inline void* ArenaRealloc(void* ptr, size_t newBytes)
{
    if(!ptr)
        return ArenaMalloc(newBytes);
    auto*        h        = ((ArenaHdr*)ptr) - 1;
    const size_t oldBytes = h->sz;

    void* np = ArenaMalloc(newBytes);
    if(!np)
        return nullptr;

    const size_t ncopy = (oldBytes < newBytes) ? oldBytes : newBytes;
    if(ncopy)
        __builtin_memcpy(np, ptr, ncopy);
    return np;
}

static inline void ArenaFree(void*) {}

#define TSF_MALLOC(sz) ArenaMalloc(sz)
#define TSF_REALLOC(p, sz) ArenaRealloc(p, sz)
#define TSF_FREE(p) ArenaFree(p)

#define TSF_IMPLEMENTATION
#include "tsf.h"

// -----------------------------
// TSF stream backed by FatFS FIL
// -----------------------------
static int TsfRead(void* data, void* ptr, unsigned int size)
{
    FIL* f  = (FIL*)data;
    UINT br = 0;
    if(f_read(f, ptr, size, &br) != FR_OK)
        return 0;
    return (int)br;
}
static int TsfSkip(void* data, unsigned int count)
{
    FIL*          f   = (FIL*)data;
    const FSIZE_t pos = f_tell(f);
    if(f_lseek(f, pos + count) != FR_OK)
        return 0;
    return 1;
}

// -----------------------------
// Public-ish synth API (kept simple)
// -----------------------------
static tsf* g_tsf = nullptr;
static FIL  g_sf2file;
static float g_sample_rate = 48000.0f;
static Chorus   DSY_SDRAM_BSS g_chorus;
static ReverbSc DSY_SDRAM_BSS g_reverb;
static bool    g_fx_init = false;
static float   g_chorus_wet = 1.0f;
static float   g_chorus_dry = 0.0f;
static float   g_chorus_gain = 1.5f;
static float   g_reverb_wet = 1.0f;
static float   g_reverb_dry = 0.0f;
static float   g_reverb_hp_a = 0.0f;
static float   g_reverb_hp_zl = 0.0f;
static float   g_reverb_hp_zr = 0.0f;
static float   g_reverb_hp_xl = 0.0f;
static float   g_reverb_hp_xr = 0.0f;
static float   g_reverb_time = 0.85f;
static float   g_reverb_lpf_hz = 8000.0f;
static float   g_reverb_hpf_hz = 80.0f;
static float   g_chorus_depth = 0.35f;
static float   g_chorus_speed_hz = 0.25f;

bool SynthInit()
{
    g_arena.Init(sdram_arena_buf, sizeof(sdram_arena_buf));
    return true;
}

bool SynthLoadSf2(const char* path, float sampleRate, int voices)
{
    g_sample_rate = sampleRate;
    g_arena.Reset();
    g_arena_oom = false;
    __builtin_memset(sdram_arena_buf, 0, sizeof(sdram_arena_buf));

    if(f_open(&g_sf2file, path, FA_READ) != FR_OK)
        return false;

    tsf_stream s{};
    s.data = &g_sf2file;
    s.read = &TsfRead;
    s.skip = &TsfSkip;

    g_tsf = tsf_load(&s);
    if(!g_tsf)
    {
        f_close(&g_sf2file);
        return false;
    }

    tsf_set_output(g_tsf, TSF_STEREO_INTERLEAVED, sampleRate, 0.0f);
    tsf_set_max_voices(g_tsf, voices);
    if(!g_fx_init)
    {
        g_chorus.Init(sampleRate);
        g_chorus.SetLfoFreq(0.25f);
        g_chorus.SetLfoDepth(0.35f);
        g_chorus.SetDelay(0.2f);
        g_chorus.SetFeedback(0.05f);
        g_chorus.SetPan(0.25f, 0.75f);

        g_reverb.Init(sampleRate);
        g_reverb.SetFeedback(0.85f);
        g_reverb.SetLpFreq(8000.0f);
        SynthSetReverbHpFreq(80.0f);

        g_fx_init = true;
    }
    // Initialize default preset for channels (0-15), with drums on channel 10.
    for(int ch = 0; ch < 16; ch++)
        tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9 ? 1 : 0);
    return true;
}

void SynthUnloadSf2()
{
    if(g_tsf)
    {
        tsf_close(g_tsf);
        g_tsf = nullptr;
    }
    f_close(&g_sf2file);
}

size_t SynthArenaUsed()
{
    return g_arena.Used();
}

size_t SynthArenaCap()
{
    return g_arena.Cap();
}

bool SynthArenaOom()
{
    return g_arena_oom;
}

void SynthPanic()
{
    if(g_tsf)
        tsf_reset(g_tsf);
}

bool SynthNoteOn(uint8_t ch, uint8_t key, uint8_t vel)
{
    if(!g_tsf)
        return false;
    const float v = (vel <= 1) ? 0.0f : (float)vel / 127.0f;
    return tsf_channel_note_on(g_tsf, (int)ch, (int)key, v) != 0;
}

void SynthNoteOff(uint8_t ch, uint8_t key)
{
    if(!g_tsf)
        return;
    tsf_channel_note_off(g_tsf, (int)ch, (int)key);
}

void SynthAllNotesOff(uint8_t ch)
{
    if(!g_tsf)
        return;
    tsf_channel_note_off_all(g_tsf, (int)ch);
}

void SynthAllSoundOff(uint8_t ch)
{
    if(!g_tsf)
        return;
    tsf_channel_sounds_off_all(g_tsf, (int)ch);
}

void SynthProgramChange(uint8_t ch, uint8_t program)
{
    if(!g_tsf)
        return;
    tsf_channel_set_presetnumber(g_tsf, (int)ch, (int)program, ch == 9 ? 1 : 0);
}

void SynthControlChange(uint8_t ch, uint8_t cc, uint8_t value)
{
    if(!g_tsf)
        return;
    tsf_channel_midi_control(g_tsf, (int)ch, (int)cc, (int)value);
}

void SynthPitchBend(uint8_t ch, uint16_t value)
{
    if(!g_tsf)
        return;
    tsf_channel_set_pitchwheel(g_tsf, (int)ch, (int)value);
}

void SynthResetChannels()
{
    if(!g_tsf)
        return;
    for(int ch = 0; ch < 16; ch++)
    {
        tsf_channel_midi_control(g_tsf, ch, 121, 0);  // Reset All Controllers
        tsf_channel_midi_control(g_tsf, ch, 7, 100);  // Volume
        tsf_channel_midi_control(g_tsf, ch, 11, 127); // Expression
        tsf_channel_midi_control(g_tsf, ch, 10, 64);  // Pan
        tsf_channel_set_pitchwheel(g_tsf, ch, 8192);
        tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9 ? 1 : 0);
    }
}

void SynthRender(float* outL, float* outR, size_t frames)
{
    if(!g_tsf)
    {
        for(size_t i = 0; i < frames; i++)
            outL[i] = outR[i] = 0.0f;
        return;
    }

    static float tmp[2 * 256];
    static float tmpChorus[2 * 256];
    static float tmpReverb[2 * 256];
    if(frames > 256)
        frames = 256;

    tsf_render_float_fx(g_tsf, tmp, tmpChorus, tmpReverb, (int)frames, 0);
    for(size_t i = 0; i < frames; i++)
    {
        const float dryL = tmp[2 * i + 0];
        const float dryR = tmp[2 * i + 1];

        const float chInL = tmpChorus[2 * i + 0];
        const float chInR = tmpChorus[2 * i + 1];
        const float chIn = (chInL + chInR);
        g_chorus.Process(chIn);
        const float chL = g_chorus.GetLeft() * g_chorus_gain * g_chorus_wet;
        const float chR = g_chorus.GetRight() * g_chorus_gain * g_chorus_wet;

        float rvL = 0.0f;
        float rvR = 0.0f;
        g_reverb.Process(tmpReverb[2 * i + 0], tmpReverb[2 * i + 1], &rvL, &rvR);
        // Simple one-pole HPF on reverb return
        const float yL = g_reverb_hp_a * (g_reverb_hp_zl + rvL - g_reverb_hp_xl);
        const float yR = g_reverb_hp_a * (g_reverb_hp_zr + rvR - g_reverb_hp_xr);
        g_reverb_hp_zl = yL;
        g_reverb_hp_zr = yR;
        g_reverb_hp_xl = rvL;
        g_reverb_hp_xr = rvR;

        // Send amounts already scale per-voice contributions.
        outL[i] = dryL + chInL * g_chorus_dry + chL + tmpReverb[2 * i + 0] * g_reverb_dry + yL * g_reverb_wet;
        outR[i] = dryR + chInR * g_chorus_dry + chR + tmpReverb[2 * i + 1] * g_reverb_dry + yR * g_reverb_wet;
    }
}

void SynthSetReverbTime(float t01)
{
    if(t01 < 0.0f)
        t01 = 0.0f;
    if(t01 > 1.0f)
        t01 = 1.0f;
    g_reverb_time = t01;
    g_reverb.SetFeedback(t01);
}

void SynthSetReverbLpFreq(float hz)
{
    if(hz < 20.0f)
        hz = 20.0f;
    g_reverb_lpf_hz = hz;
    g_reverb.SetLpFreq(hz);
}

void SynthSetReverbHpFreq(float hz)
{
    if(hz < 20.0f)
        hz = 20.0f;
    if(hz > 1000.0f)
        hz = 1000.0f;
    g_reverb_hpf_hz = hz;
    // one-pole HPF coefficient
    const float x  = expf(-2.0f * 3.14159265f * hz / g_sample_rate);
    g_reverb_hp_a = x;
}
void SynthSetChorusDepth(float d01)
{
    if(d01 < 0.0f)
        d01 = 0.0f;
    if(d01 > 1.0f)
        d01 = 1.0f;
    g_chorus_depth = d01;
    g_chorus.SetLfoDepth(d01);
}


void SynthSetChorusSpeed(float hz)
{
    if(hz < 0.05f)
        hz = 0.05f;
    if(hz > 5.0f)
        hz = 5.0f;
    g_chorus_speed_hz = hz;
    g_chorus.SetLfoFreq(hz);
}

float SynthGetReverbTime()
{
    return g_reverb_time;
}

float SynthGetReverbLpFreq()
{
    return g_reverb_lpf_hz;
}

float SynthGetReverbHpFreq()
{
    return g_reverb_hpf_hz;
}

float SynthGetChorusDepth()
{
    return g_chorus_depth;
}

float SynthGetChorusSpeed()
{
    return g_chorus_speed_hz;
}
