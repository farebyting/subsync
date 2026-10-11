// subsync — drift, framerate and split-piece recovery
#pragma once
#include <vector>

#include "types.h"

namespace subsync {

struct SplitResult {
    std::vector<ms> per_cue_offset;  // size == cues.size()
    std::vector<int> splits;         // cue indices where a new segment starts (0 excluded)
    std::vector<ms> offsets;         // one offset per segment
    std::vector<double> sigmas;      // per-segment peak sigma
    std::vector<double> margins;     // per-segment peak margin
};

// A full timing model: cue times are first multiplied by `mult`, then `add_ms` is
// added. A pure offset is mult == 1.0; a framerate mismatch is mult != 1.0.
struct DriftFit {
    double mult = 1.0;
    double add_ms = 0.0;
    double sigma = 0.0;
    double margin = 0.0;
    bool ok = false;
};

// Search the framerate grid jointly with the residual offset. For each candidate
// ratio the reference is rescaled and the spread of per-chunk offsets is measured;
// the ratio that leaves the least spread is the one that explains the cues.
// Falls back to the supplied constant-offset answer when nothing beats it.
DriftFit fit_drift(const std::vector<Span>& cues,
                   const std::vector<Span>& reference,
                   const std::vector<float>& weights,
                   ms base_offset,
                   double base_sigma,
                   double base_margin);

// Per-chunk offset samples across the timeline (chunks too thin to trust are dropped).
std::vector<Chunk> chunk_offsets(const std::vector<Span>& cues,
                                 const std::vector<Span>& reference,
                                 const std::vector<float>& weights,
                                 int count);

// Weighted least squares over the chunk samples.
// Returns {slope_ms_per_second, intercept_ms}.
std::pair<double, double> robust_line(const std::vector<Chunk>& chunks);

// new_time = t * (1 + slope/1000) + intercept
ms apply_line(ms t, double slope_ms_per_s, double intercept_ms);

// Does the spread of per-chunk offsets justify a split rather than one line?
bool worth_splitting(const std::vector<Chunk>& chunks, ms base);

// Piecewise-constant offsets, chosen by dynamic programming over chunks with a
// break penalty. Segments whose offsets differ by less than the penalty's
// tolerance are merged.
SplitResult split_alignment(const std::vector<Span>& cues,
                            const std::vector<Span>& reference,
                            const std::vector<float>& weights,
                            double penalty,
                            ms base_offset);

// Cue indices where the local offset jumps backwards by more than CONCAT_JUMP_MS
// (content was re-ordered or joined onto the front).
std::vector<int> backward_jumps(const std::vector<Span>& cues,
                                const std::vector<Span>& reference,
                                const std::vector<float>& weights);

} // namespace subsync
