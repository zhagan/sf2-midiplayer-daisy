#pragma once

#include "app_state.h"
#include "media_library.h"
#include "ui_events.h"

namespace major_midi
{

class UiController
{
  public:
    void Init(AppState& state);
    bool HandleEvent(const UiEvent&    event,
                     uint32_t          now_ms,
                     const MediaLibrary& library);

  private:
    void EnterMenu(uint32_t now_ms);
    void ExitMenu(uint32_t now_ms);
    void EnterMenuPage(MenuPage page, uint32_t now_ms);
    void ExitMenuPage(uint32_t now_ms);
    void HandlePerformanceBankButton(uint8_t bank, uint32_t now_ms);
    void ResetKnobPickup();
    void SetMode(UiMode mode, uint32_t now_ms, const char* overlay);
    void CycleKnobPage(uint32_t now_ms);
    void SelectBank(uint8_t bank, uint32_t now_ms);
    void ToggleVisibleMute(uint8_t slot, uint32_t now_ms);
    void ToggleMenu(uint32_t now_ms);
    void MoveMenuRootCursor(int32_t delta, uint32_t now_ms);
    void MoveMenuPageCursor(int32_t delta, const MediaLibrary& library, uint32_t now_ms);
    void ActivateMenuRoot(const MediaLibrary& library, uint32_t now_ms);
    void ActivateMenuPage(const MediaLibrary& library, uint32_t now_ms);
    void AdjustMenuValue(int32_t delta, uint32_t now_ms);
    bool HandleKnob(uint8_t index, float value, uint32_t now_ms);
    static uint8_t NormToMidi(float value);

    AppState* state_ = nullptr;
    bool      knob_caught_[4]{};
    uint8_t   last_bank_button_      = 0xFF;
    uint32_t  last_bank_button_ms_   = 0;
};

} // namespace major_midi
