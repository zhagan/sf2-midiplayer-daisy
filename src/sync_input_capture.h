#pragma once

#include <cstddef>
#include <cstdint>

#include "daisy_patch_sm.h"
#include "util/scopedirqblocker.h"

class ClockSync;

class SyncInputCapture
{
  public:
    void Init(daisy::patch_sm::DaisyPatchSM& hw);

    void PushMidiClockTimestampUs(uint32_t timestampUs);
    void PushGateTimestampUs(uint32_t timestampUs);
    void ProcessGateState(bool gateHigh, uint32_t timestampUs);
    void PollGateInput(uint32_t timestampUs);
    void ResetGateState(bool gateHigh);
    void ClearGateClock();

    void DrainMidiClock(ClockSync& sync);
    void DrainGateClock(ClockSync& sync);

  private:
    template <size_t Capacity>
    class TimestampQueue
    {
      public:
        bool Push(uint32_t timestampUs)
        {
            daisy::ScopedIrqBlocker lock;
            const size_t           next = (head_ + 1u) % Capacity;
            if(next == tail_)
                return false;
            data_[head_] = timestampUs;
            head_        = next;
            return true;
        }

        bool Pop(uint32_t& timestampUs)
        {
            daisy::ScopedIrqBlocker lock;
            if(head_ == tail_)
                return false;
            timestampUs = data_[tail_];
            tail_       = (tail_ + 1u) % Capacity;
            return true;
        }

        void Clear()
        {
            daisy::ScopedIrqBlocker lock;
            head_ = 0;
            tail_ = 0;
        }

      private:
        uint32_t data_[Capacity]{};
        size_t   head_ = 0;
        size_t   tail_ = 0;
    };

    static constexpr size_t kQueueCapacity = 128;

    daisy::patch_sm::DaisyPatchSM* hw_ = nullptr;
    bool                           gate_prev_high_ = false;
    TimestampQueue<kQueueCapacity> midi_clock_edges_{};
    TimestampQueue<kQueueCapacity> gate_edges_{};
};
