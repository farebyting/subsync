// subsync — FFT two-channel (block + onset) coarse aligner
#pragma once
#include <vector>

#include "types.h"

namespace subsync {

// Build the reference spectra once; reused for every align_peaks call.
// No-op when the reference has fewer than 4 spans.
void align_setup(const std::vector<Span>& reference, const std::vector<float>& weights);

// Top candidate offsets for these cues, best z first.
std::vector<Hit> align_peaks(const std::vector<Span>& cues, int want);

// Snap an offset to the best overlap on a REFINE_STEP_MS grid.
ms align_refine(const std::vector<Span>& cues, ms offset_ms);

// MAD z-score of the onset correlation at this offset (used as the verdict sigma).
double onset_z(const std::vector<Span>& cues, ms offset_ms);

void align_drop();

} // namespace subsync
