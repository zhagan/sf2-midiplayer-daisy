#include "sync_input_capture.h"

#include "clock_sync.h"

void SyncInputCapture::Init(daisy::patch_sm::DaisyPatchSM& hw)
{
    hw_             = &hw;
    gate_prev_high_ = hw.gate_in_1.State();
}

void SyncInputCapture::PushMidiClockTimestampUs(uint32_t timestampUs)
{
    midi_clock_edges_.Push(timestampUs);
}

void SyncInputCapture::PushGateTimestampUs(uint32_t timestampUs)
{
    gate_edges_.Push(timestampUs);
}

void SyncInputCapture::ProcessGateState(bool gateHigh, uint32_t timestampUs)
{
    const bool rising = gateHigh && !gate_prev_high_;
    gate_prev_high_   = gateHigh;

    if(rising)
        PushGateTimestampUs(timestampUs);
}

void SyncInputCapture::PollGateInput(uint32_t timestampUs)
{
    if(hw_ == nullptr)
        return;

    ProcessGateState(hw_->gate_in_1.State(), timestampUs);
}

void SyncInputCapture::ResetGateState(bool gateHigh)
{
    gate_prev_high_ = gateHigh;
}

void SyncInputCapture::ClearGateClock()
{
    gate_edges_.Clear();
}

void SyncInputCapture::DrainMidiClock(ClockSync& sync)
{
    uint32_t timestampUs = 0;
    while(midi_clock_edges_.Pop(timestampUs))
        sync.ProcessEdgeTimestampUs(timestampUs);
}

void SyncInputCapture::DrainGateClock(ClockSync& sync)
{
    uint32_t timestampUs = 0;
    while(gate_edges_.Pop(timestampUs))
        sync.ProcessEdgeTimestampUs(timestampUs);
}
