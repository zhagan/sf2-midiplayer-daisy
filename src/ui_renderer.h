#pragma once

#include "app_state.h"
#include "daisy_patch_sm.h"
#include "dev/oled_ssd130x.h"
#include "media_library.h"

namespace major_midi
{

class UiRenderer
{
  public:
    using DisplayT = daisy::OledDisplay<daisy::SSD130xI2c128x64Driver>;

    void Init();
    void ShowSplash();
    void Render(const AppState& state, const MediaLibrary& library, uint32_t now_ms);

  private:
    DisplayT display_;
};

} // namespace major_midi
