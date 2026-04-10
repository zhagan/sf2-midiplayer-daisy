#pragma once

#include "app_state.h"

namespace major_midi
{

bool LoadMidiRoutingConfig(const char* path, MidiRoutingConfig& config);
bool SaveMidiRoutingConfig(const char* path, const MidiRoutingConfig& config);

} // namespace major_midi
