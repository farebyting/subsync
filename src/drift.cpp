#include "drift.h"

#include <algorithm>
#include <cmath>

#include "constants.h"
#include "correlate.h"

namespace subsync {

namespace {

// Chunks need at least this many cues before their offset is believed.
constexpr std::size_t MIN_CHUNK_CUES = 4;

int chunk_of(ms t, ms first, ms last, int count) {
    if (last <= first) return 0;
    const double f = static_cast<double>(t - first) / static_cast<double>(last - first);
    int idx = static_cast<int>(f * count);
    if (idx >= count) idx = count - 1;
    if (idx < 0) idx = 0;
    return idx;
}

} // namespace

DriftFit fit_drift(const std::vector<Span>& cues,
                   const std::vector<Span>& reference,
                   const std::vector<float>& weights,
                   ms base_offset,
                   double base_sigma,
                   double base_margin) {
    DriftFit out;
    out.mult = 1.0;
    out.add_ms = static_cast<double>(base_offset);
    out.sigma = base_sigma;
    out.margin = base_margin;
    out.ok = true;

    if (cues.size() < static_cast<std::size_t>(MIN_CUES) || reference.empty()) return out;

    // Score a candidate multiplier by the quality of the match it produces.
    // Margin is used rather than sigma because it is scale-free: it stays
    // comparable across reference sizes, which sigma does not.
    auto score_for = [&](double mult, double* out_off, double* out_sigma,
                         double* out_margin) -> double {
        std::vector<Span> scaled;
        scaled.reserve(reference.size());
        for (const Span& s : reference)
            scaled.emplace_back(static_cast<ms>(std::llround(static_cast<double>(s.first) * mult)),
                                static_cast<ms>(std::llround(static_cast<double>(s.second) * mult)));

        const OffsetResult r = best_offset(cues, scaled, weights, 1.0);
        if (out_off) *out_off = static_cast<double>(r.offset_ms);
        if (out_sigma) *out_sigma = r.sigma;
        if (out_margin) *out_margin = r.margin;
        return r.margin;
    };

    double best_margin = score_for(1.0, nullptr, nullptr, nullptr);
    double best_mult = 1.0;
    double best_off = static_cast<double>(base_offset);
    double best_sigma = base_sigma;

    // A ratio must beat the plain offset by a wide margin AND be confident on its
    // own terms before it is adopted. The absolute floor matters: when the cues are
    // a split, the plain margin is near zero, so a relative-only test would let an
    // arbitrary ratio "win" by beating a bad baseline.
    const double MIN_GAIN = 1.5;

    for (int i = 0; i < FRAMERATE_RATIOS_N; i++) {
        const double r = FRAMERATE_RATIOS[i];
        if (std::abs(r - 1.0) < 1e-9) continue;

        double off = 0.0, sig = 0.0, marg = 0.0;
        score_for(r, &off, &sig, &marg);

        if (marg >= SOLID_MARGIN && marg > best_margin * MIN_GAIN) {
            best_margin = marg;
            best_mult = r;
            best_off = off;
            best_sigma = sig;
        }
    }

    if (std::abs(best_mult - 1.0) > 1e-9) {
        // `best_mult` maps reference time onto cue time: cue ~= ref * best_mult + off.
        // The correction applied to the cues is therefore the inverse mapping.
        out.mult = 1.0 / best_mult;
        out.add_ms = -best_off / best_mult;
        out.sigma = best_sigma;
        out.margin = best_margin;
    }
    return out;
}

std::vector<Chunk> chunk_offsets(const std::vector<Span>& cues,
                                 const std::vector<Span>& reference,
                                 const std::vector<float>& weights,
                                 int count) {
    std::vector<Chunk> out;
    if (cues.empty() || reference.empty() || count < 1) return out;

    const ms first = cues.front().first;
    const ms last = cues.back().second;
    if (last <= first) return out;

    // Never make chunks thinner than MIN_CHUNK_CUES cues.
    const int max_by_cues = static_cast<int>(cues.size() / MIN_CHUNK_CUES);
    if (max_by_cues < 1) return out;
    if (count > max_by_cues) count = max_by_cues;

    std::vector<std::vector<Span>> groups(static_cast<std::size_t>(count));
    for (const Span& c : cues)
        groups[static_cast<std::size_t>(chunk_of(c.first, first, last, count))].push_back(c);

    // The returned vector always has exactly `count` entries, with weight 0 for
    // chunks that could not be trusted. Compacting them away would silently
    // renumber the chunks and desynchronise every index-based consumer.
    out.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; i++) {
        const std::vector<Span>& g = groups[static_cast<std::size_t>(i)];
        Chunk& ch = out[static_cast<std::size_t>(i)];

        const ms mid = g.empty() ? (first + (last - first) * (2 * i + 1) / (2 * count))
                                 : (g.front().first + g.back().second) / 2;
        ch.time_s = static_cast<double>(mid) / 1000.0;
        ch.offset_ms = 0.0;
        ch.weight = 0.0;

        if (g.size() < MIN_CHUNK_CUES) continue;

        const OffsetResult r = best_offset(g, reference, weights, 1.0);
        if (r.sigma <= 0.0) continue;

        ch.offset_ms = static_cast<double>(r.offset_ms);
        ch.weight = r.sigma;
    }
    return out;
}

std::pair<double, double> robust_line(const std::vector<Chunk>& chunks) {
    std::vector<Chunk> usable;
    usable.reserve(chunks.size());
    for (const Chunk& c : chunks)
        if (c.weight > 0.0) usable.push_back(c);

    if (usable.empty()) return {0.0, 0.0};
    if (usable.size() == 1) return {0.0, usable.front().offset_ms};

    double sw = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (const Chunk& c : usable) {
        const double w = c.weight;
        sw += w;
        sx += w * c.time_s;
        sy += w * c.offset_ms;
        sxx += w * c.time_s * c.time_s;
        sxy += w * c.time_s * c.offset_ms;
    }
    if (sw <= 0.0) return {0.0, 0.0};

    const double denom = sw * sxx - sx * sx;
    if (std::abs(denom) < 1e-9) return {0.0, sy / sw};

    const double slope = (sw * sxy - sx * sy) / denom;
    const double intercept = (sy - slope * sx) / sw;
    return {slope, intercept};
}

ms apply_line(ms t, double slope_ms_per_s, double intercept_ms) {
    const double factor = 1.0 + slope_ms_per_s / 1000.0;
    return static_cast<ms>(std::llround(static_cast<double>(t) * factor + intercept_ms));
}

bool worth_splitting(const std::vector<Chunk>& chunks, ms base) {
    ms low = 0, high = 0;
    bool first = true;
    int usable = 0;
    for (const Chunk& c : chunks) {
        if (c.weight <= 0.0) continue;
        usable++;
        const ms o = static_cast<ms>(std::llround(c.offset_ms));
        // A jump this large is a different film, not a re-cut.
        if (abs_ms(o - base) > CONCAT_BACK_MS) return false;
        if (first) { low = high = o; first = false; continue; }
        low = std::min(low, o);
        high = std::max(high, o);
    }
    if (usable < 2) return false;
    return (high - low) >= 120;
}

SplitResult split_alignment(const std::vector<Span>& cues,
                            const std::vector<Span>& reference,
                            const std::vector<float>& weights,
                            double penalty,
                            ms base_offset) {
    SplitResult res;
    if (cues.empty()) return res;

    res.per_cue_offset.assign(cues.size(), base_offset);
    res.splits.clear();
    res.offsets.clear();

    const int want = std::max(2, static_cast<int>(cues.size() / 40));
    const std::vector<Chunk> chunks = chunk_offsets(cues, reference, weights, want);

    if (chunks.size() < 2) {
        res.offsets.push_back(base_offset);
        return res;
    }

    const int C = static_cast<int>(chunks.size());
    // break_cost converts the penalty into the same units as the weighted
    // absolute deviation we are minimising.
    const double break_cost = std::max(0.0, penalty) * 100.0;

    // dp[i] = best cost covering chunks [0, i); prev[i] = start of the last segment.
    std::vector<double> dp(static_cast<std::size_t>(C) + 1, 0.0);
    std::vector<int> prev(static_cast<std::size_t>(C) + 1, 0);
    for (int i = 1; i <= C; i++) dp[static_cast<std::size_t>(i)] = 1e300;

    for (int i = 1; i <= C; i++) {
        for (int j = 0; j < i; j++) {
            // segment covers chunks [j, i)
            double sw = 0.0, sy = 0.0;
            for (int k = j; k < i; k++) {
                const double w = chunks[static_cast<std::size_t>(k)].weight;
                if (w <= 0.0) continue;
                sw += w;
                sy += w * chunks[static_cast<std::size_t>(k)].offset_ms;
            }
            if (sw <= 0.0) {
                // No usable evidence in this range: carry the previous cost forward
                // so the DP does not invent a break just because data is missing.
                if (j > 0 && dp[static_cast<std::size_t>(j)] < dp[static_cast<std::size_t>(i)]) {
                    dp[static_cast<std::size_t>(i)] = dp[static_cast<std::size_t>(j)];
                    prev[static_cast<std::size_t>(i)] = j;
                }
                continue;
            }
            const double seg_off = sy / sw;

            double dev = 0.0;
            for (int k = j; k < i; k++) {
                const double w = chunks[static_cast<std::size_t>(k)].weight;
                if (w <= 0.0) continue;
                dev += w * std::abs(chunks[static_cast<std::size_t>(k)].offset_ms - seg_off);
            }
            const double cost = dp[static_cast<std::size_t>(j)] + dev +
                                (j > 0 ? break_cost : 0.0);
            if (cost < dp[static_cast<std::size_t>(i)]) {
                dp[static_cast<std::size_t>(i)] = cost;
                prev[static_cast<std::size_t>(i)] = j;
            }
        }
    }

    // Walk the DP back to recover the segments.
    std::vector<std::pair<int, int>> segs;  // [j, i)
    for (int i = C; i > 0; i = prev[static_cast<std::size_t>(i)])
        segs.emplace_back(prev[static_cast<std::size_t>(i)], i);
    std::reverse(segs.begin(), segs.end());

    // Adjacent segments whose offsets agree are one segment; keeping them apart
    // would report a break where the data shows none.
    if (segs.size() > 1) {
        auto mean_of = [&](int a, int b) {
            double sw = 0.0, sy = 0.0;
            for (int k = a; k < b; k++) {
                const double w = chunks[static_cast<std::size_t>(k)].weight;
                if (w <= 0.0) continue;
                sw += w;
                sy += w * chunks[static_cast<std::size_t>(k)].offset_ms;
            }
            return sw > 0.0 ? sy / sw : 0.0;
        };

        std::vector<std::pair<int, int>> merged;
        for (const auto& s : segs) {
            if (!merged.empty()) {
                // Compare the incoming range against the range already accumulated,
                // using each range's own extent (an empty range would read as 0.0).
                const double m1 = mean_of(merged.back().first, merged.back().second);
                const double m2 = mean_of(s.first, s.second);
                if (std::abs(m1 - m2) <= AGREE_MS) {
                    merged.back().second = s.second;
                    continue;
                }
            }
            merged.push_back(s);
        }
        segs = merged;
    }

    const ms first_t = cues.front().first;
    const ms last_t = cues.back().second;

    // Refine each segment on its own cue range, then paint the cues.
    for (std::size_t s = 0; s < segs.size(); s++) {
        const int j = segs[s].first;
        const int i = segs[s].second;

        std::vector<Span> seg_cues;
        for (const Span& c : cues) {
            const int idx = chunk_of(c.first, first_t, last_t, C);
            if (idx >= j && idx < i) seg_cues.push_back(c);
        }

        ms off = base_offset;
        double seg_sigma = 0.0, seg_margin = 0.0;
        if (seg_cues.size() >= MIN_CHUNK_CUES) {
            const OffsetResult r = best_offset(seg_cues, reference, weights, 1.0);
            off = r.offset_ms;
            seg_sigma = r.sigma;
            seg_margin = r.margin;
        } else if (j < C) {
            off = static_cast<ms>(std::llround(chunks[static_cast<std::size_t>(j)].offset_ms));
        }

        res.offsets.push_back(off);
        res.sigmas.push_back(seg_sigma);
        res.margins.push_back(seg_margin);
        if (s > 0) {
            // record the cue index where this segment begins
            int cue_idx = 0;
            for (std::size_t k = 0; k < cues.size(); k++) {
                if (chunk_of(cues[k].first, first_t, last_t, C) >= j) { cue_idx = static_cast<int>(k); break; }
            }
            res.splits.push_back(cue_idx);
        }

        for (std::size_t k = 0; k < cues.size(); k++) {
            const int idx = chunk_of(cues[k].first, first_t, last_t, C);
            if (idx >= j && idx < i) res.per_cue_offset[k] = off;
        }
    }

    return res;
}

std::vector<int> backward_jumps(const std::vector<Span>& cues,
                                const std::vector<Span>& reference,
                                const std::vector<float>& weights) {
    std::vector<int> out;
    if (cues.size() < 8 || reference.empty()) return out;

    const int want = std::max(2, static_cast<int>(cues.size() / 40));
    const std::vector<Chunk> chunks = chunk_offsets(cues, reference, weights, want);
    if (chunks.size() < 2) return out;

    for (std::size_t i = 1; i < chunks.size(); i++) {
        const double d = chunks[i].offset_ms - chunks[i - 1].offset_ms;
        if (d <= -static_cast<double>(CONCAT_JUMP_MS)) {
            // find the first cue at or after this chunk boundary
            const double t_boundary = 0.5 * (chunks[i - 1].time_s + chunks[i].time_s) * 1000.0;
            for (std::size_t k = 0; k < cues.size(); k++) {
                if (static_cast<double>(cues[k].first) >= t_boundary) {
                    out.push_back(static_cast<int>(k));
                    break;
                }
            }
        }
    }
    return out;
}

} // namespace subsync
