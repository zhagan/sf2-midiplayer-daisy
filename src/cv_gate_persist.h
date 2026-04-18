#pragma once

#include "app_state.h"

namespace major_midi
{

bool LoadCvGateConfig(const char* path, CvGateConfig& config);
bool SaveCvGateConfig(const char* path,
                      const CvGateConfig& config,
                      PersistWriteStage*  failed_stage = nullptr,
                      int*                result_code  = nullptr,
                      PersistProgressFn   progress_fn  = nullptr,
                      void*               progress_ctx = nullptr);

} // namespace major_midi
