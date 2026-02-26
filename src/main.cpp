static const char* midiPath = "0:/midi/825.mid";
#include "daisy_patch_sm.h"
#include "daisysp.h"
#include "hid/midi.h"
#include "synth_tsf.h"
#include "sd_mount.h"
#include "scheduler.h"
#include "clock_sync.h"
#include "smf_player.h"

using namespace daisy;
using namespace patch_sm;
using namespace daisysp;

static DaisyPatchSM   hw;
static volatile uint64_t sampleClock = 0;
static EventQueue<2048> g_queue;
static MidiUartHandler   g_midi;
static SmfPlayer         g_smfPlayer;
static ClockSync         g_clock;

void AudioCallback(AudioHandle::InputBuffer  in,
                   AudioHandle::OutputBuffer out,
                   size_t                    size)
{
    (void)in;

    static float lbuf[256];
    static float rbuf[256];

    size_t frames = size;
    if(frames > 256)
        frames = 256;

    SynthRender(lbuf, rbuf, frames);

    for(size_t i = 0; i < frames; i++)
    {
        out[0][i] = lbuf[i];
        out[1][i] = rbuf[i];
    }

    sampleClock += size;
}

static void DispatchMidiMessage(MidiEvent msg)
{
    switch(msg.type)
    {
        case MidiMessageType::NoteOn:
        {
            auto note = msg.AsNoteOn();
            if(note.velocity == 0)
                SynthNoteOff(note.channel, note.note);
            else
                SynthNoteOn(note.channel, note.note, note.velocity);
        }
        break;

        case MidiMessageType::NoteOff:
        {
            auto note = msg.AsNoteOff();
            SynthNoteOff(note.channel, note.note);
        }
        break;

        case MidiMessageType::ChannelMode:
        {
            auto mode = msg.AsChannelMode();
            if(mode.event_type == ChannelModeType::AllNotesOff
               || mode.event_type == ChannelModeType::AllSoundOff)
            {
                SynthPanic();
            }
        }
        break;

        default: break;
    }
}

static void ProcessScheduledEvents(uint64_t sampleNow)
{
    MidiEv ev;
    while(g_queue.Peek(ev) && ev.atSample <= sampleNow)
    {
        g_queue.Pop(ev);
        const char* name = "Unknown";
        switch(ev.type)
        {
            case EvType::NoteOn: name = "NoteOn"; break;
            case EvType::NoteOff: name = "NoteOff"; break;
            case EvType::Program: name = "Program"; break;
            case EvType::AllNotesOff: name = "AllNotesOff"; break;
        }
        hw.PrintLine("Scheduled %s ch:%u note:%u vel:%u sample:%lu", name,
                     ev.ch,
                     ev.a,
                     ev.b,
                     (unsigned long)ev.atSample);
        switch(ev.type)
        {
            case EvType::NoteOn:
                SynthNoteOn(ev.ch, ev.a, ev.b);
                break;
            case EvType::NoteOff:
                SynthNoteOff(ev.ch, ev.a);
                break;
            case EvType::Program:
                SynthProgramChange(ev.ch, ev.a);
                break;
            case EvType::AllNotesOff:
                SynthPanic();
                break;
        }
    }
}

int main(void)
{
    hw.Init();
    hw.StartLog(true);
    hw.PrintLine("SF2 test: init");

    SynthInit();

    const bool sdOk = SdMount();
    hw.PrintLine("SdMount: %s", sdOk ? "PASS" : "FAIL");

    const float sr = 48000.0f;
    const bool  sfOk = sdOk && SynthLoadSf2("0:/soundfonts/microgm.sf2", sr, 16);
    hw.PrintLine("SoundFont: %s", sfOk ? "PASS" : "FAIL");

    if(sfOk)
    {
        g_clock.Init(sr);
        g_smfPlayer.SetSampleRate(sr);
        g_smfPlayer.SetLookaheadSamples(0);
        const bool midiOk = g_smfPlayer.Open(midiPath);
        hw.PrintLine("SMF open %s (%s)", midiOk ? "PASS" : "FAIL", midiPath);
        if(midiOk)
            g_smfPlayer.Start(sampleClock);
        if(midiOk)
            hw.PrintLine("SMF start requested");
    }

    MidiUartHandler::Config midi_cfg{};
    midi_cfg.transport_config = MidiUartTransport::Config();
    g_midi.Init(midi_cfg);
    g_midi.StartReceive();
    hw.PrintLine("Event queue capacity: %d", (int)2048);

    bool midiLogged = false;

    if(sfOk)
        hw.StartAudio(AudioCallback);

    uint32_t lastStatusLog = System::GetNow();
    while(1)
    {
        if(!sfOk)
        {
            System::Delay(500);
            continue;
        }

        g_midi.Listen();
        while(g_midi.HasEvents())
        {
            if(!midiLogged)
            {
                hw.PrintLine("MIDI events processing");
                midiLogged = true;
            }
            DispatchMidiMessage(g_midi.PopEvent());
        }

        g_smfPlayer.Pump(g_queue, sampleClock);
        ProcessScheduledEvents(sampleClock);

        uint32_t now = System::GetNow();
        if(now - lastStatusLog >= 1000)
        {
            lastStatusLog = now;
            hw.PrintLine("Clock %lu, queue %d",
                         (unsigned long)sampleClock,
                         (int)g_queue.Size());
            hw.PrintLine("SMF playing %d, remaining %lu",
                         (int)g_smfPlayer.IsPlaying(),
                         (unsigned long)g_smfPlayer.RemainingBytes());
            if(!g_smfPlayer.IsPlaying() && g_smfPlayer.RemainingBytes() > 0)
            {
                g_smfPlayer.Start(sampleClock);
                hw.PrintLine("SMF auto-start");
            }
        }
    }
}
