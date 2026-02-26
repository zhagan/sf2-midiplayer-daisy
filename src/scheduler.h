#pragma once
#include <cstdint>
#include <cstddef>

enum class EvType : uint8_t
{
    NoteOn,
    NoteOff,
    Program,
    AllNotesOff,
};

struct MidiEv
{
    uint64_t atSample = 0;
    EvType   type     = EvType::NoteOn;
    uint8_t  ch       = 0;
    uint8_t  a        = 0; // note/program
    uint8_t  b        = 0; // velocity
};

template <size_t N>
class EventQueue
{
  public:
    bool Push(const MidiEv& e)
    {
        const size_t next = (head_ + 1) % N;
        if(next == tail_)
            return false; // full
        buf_[head_] = e;
        head_       = next;
        return true;
    }

    bool Peek(MidiEv& out) const
    {
        if(tail_ == head_)
            return false;
        out = buf_[tail_];
        return true;
    }

    bool Pop(MidiEv& out)
    {
        if(tail_ == head_)
            return false;
        out   = buf_[tail_];
        tail_ = (tail_ + 1) % N;
        return true;
    }

    bool Empty() const { return tail_ == head_; }

    void Clear() { head_ = tail_ = 0; }
    bool IsFull() const { return ((head_ + 1) % N) == tail_; }
    size_t Size() const
    {
        if(head_ >= tail_)
            return head_ - tail_;
        return N - (tail_ - head_);
    }

  private:
    MidiEv buf_[N]{};
    size_t head_ = 0;
    size_t tail_ = 0;
};
