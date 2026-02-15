#include "daisy_patch_sm.h"
using namespace daisy;

#include "sd_mount.h"
#include "synth_tsf.h"
#include "scheduler.h"

static patch_sm::DaisyPatchSM hw;
static volatile uint64_t      sampleClock = 0;

static EventQueue<2048> g_queue; // not used yet, but ready

static void AudioCallback(AudioHandle::InputBuffer  in,
                          AudioHandle::OutputBuffer out,
                          size_t                    size)
{
    (void)in;

    static float lbuf[256];
    static float rbuf[256];

    size_t frames = size;
    if(frames > 256)
        frames = 256;

    // TODO later: pop g_queue events whose atSample <= sampleClock
    // and dispatch to SynthNoteOn/Off/Program, etc.

    SynthRender(lbuf, rbuf, frames);

    for(size_t i = 0; i < frames; i++)
    {
        out[0][i] = lbuf[i];
        out[1][i] = rbuf[i];
    }

    sampleClock += size;
}

int main(void)
{
    hw.Init();
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    hw.SetAudioBlockSize(48);

    SynthInit();
    hw.StartAudio(AudioCallback);

    const bool  sdOk = SdMount();
    const float sr   = 48000.0f;

    const bool sfOk = sdOk && SynthLoadSf2("0:/soundfonts/microgm.sf2", sr, 16);

    if(sfOk)
        SynthNoteOn(0, 60, 100);

    const uint64_t noteOffAt = sampleClock + (uint64_t)(1.0f * sr);

    while(1)
    {
        if(sfOk && sampleClock >= noteOffAt)
        {
            SynthPanic(); // stop notes, keep font loaded
            break;
        }
    }

    while(1) { /* idle */ }
}
