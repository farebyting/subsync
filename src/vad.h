// subsync — energy/zero-crossing VAD (self-contained; no model, no runtime)
#pragma once
#include <vector>

#include "wav.h"

namespace subsync {

// One speech probability per 10 ms frame.
// coverage receives the fraction of frames above SPEECH_THRESHOLD.
std::vector<float> speech_profile(const Audio& a, double* coverage = nullptr);

} // namespace subsync
