// subsync — exact sweep-line overlap correlation (lapse-faithful)
#pragma once
#include <vector>

#include "constants.h"
#include "types.h"

namespace subsync {

// 10 ms-bin activity profile with prefix sums, for O(1) span overlap queries.
struct ActivityIndex {
    std::vector<long long> prefix;  // prefix[i] = sum(activity[0..i-1])
    int nbin = 0;

    void build(const std::vector<int>& activity);
    // Fraction (0..1) of span s that is covered after shifting it by offset_ms.
    double covered_fraction(Span s, ms offset_ms) const;
    bool empty() const { return nbin == 0; }
};

// Exact overlap search over all offsets, via a slope-change sweep.
// Overlap length is piecewise-linear in the offset, so four events per
// (reference span, cue) pair capture it exactly.
OffsetResult best_offset(const std::vector<Span>& cues,
                         const std::vector<Span>& reference,
                         const std::vector<float>& weights,
                         double coverage = 1.0);

// Mean covered fraction of cues against a reference activity index at an offset.
double align_score(const std::vector<Span>& cues, const ActivityIndex& ref, ms offset_ms);

// MAD-based sigma of the winning bucket against the rest of the sweep.
double peak_sigma(const std::vector<double>& buckets, int best_bucket, double fmax);

// Try every known framerate ratio; returns the ratio whose peak is strongest.
double best_framerate(const std::vector<Span>& cues,
                      const std::vector<Span>& reference,
                      const std::vector<float>& weights,
                      ms* out_offset,
                      double* out_sigma);

} // namespace subsync
