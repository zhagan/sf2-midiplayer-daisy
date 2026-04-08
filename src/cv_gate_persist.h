#pragma once

#include "app_state.h"

namespace major_midi
{

bool LoadCvGateConfig(const char* path, CvGateConfig& config);
bool SaveCvGateConfig(const char* path, const CvGateConfig& config);

} // namespace major_midi
