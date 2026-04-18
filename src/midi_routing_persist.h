#pragma once

#include "app_state.h"

namespace major_midi
{

bool LoadMidiRoutingConfig(const char* path, MidiRoutingConfig& config);
bool SaveMidiRoutingConfig(const char* path,
                           const MidiRoutingConfig& config,
                           PersistWriteStage*       failed_stage = nullptr,
                           int*                     result_code  = nullptr,
                           PersistProgressFn        progress_fn  = nullptr,
                           void*                    progress_ctx = nullptr);

} // namespace major_midi
