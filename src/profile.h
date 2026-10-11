// subsync — activity profiles and span processing
#pragma once
#include <utility>
#include <vector>

#include "types.h"

namespace subsync {

// 10 ms bins, 1 where any span covers the bin.
std::vector<int> activity(const std::vector<Span>& spans);

// Turn per-frame speech probabilities (10 ms frames) into weighted spans.
std::pair<std::vector<Span>, std::vector<float>> reference_spans(const std::vector<float>& prob);

// Clean up cues for matching: drop junk and zero-length, optionally sort by start
// and merge overlaps. Returns the surviving spans plus the indices of the cues
// that produced them (so the caller can map back to the original cue list).
std::pair<std::vector<Span>, std::vector<int>> process_spans(const std::vector<Cue>& cues,
                                                             bool merge = true,
                                                             bool sort_by_time = true);

} // namespace subsync
