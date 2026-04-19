#pragma once

#include "app_state.h"

namespace major_midi
{

bool LoadSongConfig(const char* path, AppState& state);
bool SaveSongConfig(const char* path,
                    const AppState& state,
                    PersistWriteStage* failed_stage = nullptr,
                    int*               result_code  = nullptr,
                    PersistProgressFn  progress_fn  = nullptr,
                    void*              progress_ctx = nullptr);

} // namespace major_midi
