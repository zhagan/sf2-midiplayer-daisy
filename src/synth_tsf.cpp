#include "synth_tsf.h"
#include <cstdint>
#include <cstddef>

#include "daisy_patch_sm.h"

extern "C"
{
#include "ff.h"
}

using namespace daisy;

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

// Put arena in SDRAM
static uint8_t DSY_SDRAM_BSS sdram_arena_buf[16 * 1024 * 1024];

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
        return nullptr;
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

bool SynthInit()
{
    g_arena.Init(sdram_arena_buf, sizeof(sdram_arena_buf));
    return true;
}

bool SynthLoadSf2(const char* path, float sampleRate, int voices)
{
    g_arena.Reset();

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
    // Initialize default preset for channels (0-15), with drums on channel 10.
    for(int ch = 0; ch < 16; ch++)
        tsf_channel_set_presetnumber(g_tsf, ch, 0, ch == 9 ? 1 : 0);
    return true;
}

void SynthPanic()
{
    if(g_tsf)
        tsf_reset(g_tsf);
}

void SynthNoteOn(uint8_t ch, uint8_t key, uint8_t vel)
{
    if(!g_tsf)
        return;
    const float v = (vel <= 1) ? 0.0f : (float)vel / 127.0f;
    tsf_channel_note_on(g_tsf, (int)ch, (int)key, v);
}

void SynthNoteOff(uint8_t ch, uint8_t key)
{
    if(!g_tsf)
        return;
    tsf_channel_note_off(g_tsf, (int)ch, (int)key);
}

void SynthProgramChange(uint8_t ch, uint8_t program)
{
    if(!g_tsf)
        return;
    tsf_channel_set_presetnumber(g_tsf, (int)ch, (int)program, ch == 9 ? 1 : 0);
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
    if(frames > 256)
        frames = 256;

    tsf_render_float(g_tsf, tmp, (int)frames, 0);
    for(size_t i = 0; i < frames; i++)
    {
        outL[i] = tmp[2 * i + 0];
        outR[i] = tmp[2 * i + 1];
    }
}
