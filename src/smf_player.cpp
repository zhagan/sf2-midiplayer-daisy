#include "smf_player.h"

extern "C"
{
#include "ff.h"
}

bool SmfPlayer::Open(const char* path)
{
    Close();

    FIL* f = new FIL;
    if(f_open(f, path, FA_READ) != FR_OK)
    {
        delete f;
        return false;
    }

    file_ = f;
    open_ = true;
    return true;
}

void SmfPlayer::Close()
{
    if(open_)
    {
        FIL* f = (FIL*)file_;
        f_close(f);
        delete f;
    }

    file_    = nullptr;
    open_    = false;
    playing_ = false;
}

void SmfPlayer::SetSampleRate(float sr)
{
    sr_ = sr;
}

void SmfPlayer::SetLookaheadSamples(uint64_t samples)
{
    lookahead_ = samples;
}

void SmfPlayer::Start(uint64_t sampleNow)
{
    if(open_)
    {
        playing_     = true;
        startSample_ = sampleNow;
    }
}

bool SmfPlayer::IsPlaying() const
{
    return playing_;
}

void SmfPlayer::Pump(EventQueue<2048>&, uint64_t)
{
    // TODO: real SMF parsing next
}
