#include "correlate.h"

#include <algorithm>
#include <cmath>

namespace subsync {

namespace {

struct Event {
    ms offset;
    double slope;
};

// Safety valve: a pathological pair count must not exhaust memory.
constexpr std::size_t MAX_EVENTS = 40u * 1000u * 1000u;

} // namespace

void ActivityIndex::build(const std::vector<int>& activity) {
    nbin = static_cast<int>(activity.size());
    prefix.assign(static_cast<std::size_t>(nbin) + 1, 0);
    for (int i = 0; i < nbin; i++)
        prefix[static_cast<std::size_t>(i) + 1] = prefix[static_cast<std::size_t>(i)] + (activity[i] ? 1 : 0);
}

double ActivityIndex::covered_fraction(Span s, ms offset_ms) const {
    if (nbin <= 0) return 0.0;
    const ms a = s.first + offset_ms;
    const ms b = s.second + offset_ms;
    if (b <= a) return 0.0;

    long long i0 = a / 10;
    long long i1 = b / 10;
    if (i1 <= i0) i1 = i0 + 1;
    if (i0 < 0) i0 = 0;
    if (i1 > nbin) i1 = nbin;
    if (i1 <= i0) return 0.0;

    const long long cov = prefix[static_cast<std::size_t>(i1)] - prefix[static_cast<std::size_t>(i0)];
    return static_cast<double>(cov) / static_cast<double>(i1 - i0);
}

double align_score(const std::vector<Span>& cues, const ActivityIndex& ref, ms offset_ms) {
    if (cues.empty() || ref.empty()) return 0.0;
    double sum = 0.0;
    for (const Span& c : cues) sum += ref.covered_fraction(c, offset_ms);
    return sum / static_cast<double>(cues.size());
}

namespace {

// Percentile of a value list (linear interpolation between order statistics).
double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double idx = p * static_cast<double>(v.size() - 1);
    const std::size_t i = static_cast<std::size_t>(idx);
    const double f = idx - static_cast<double>(i);
    if (i + 1 >= v.size()) return v.back();
    return v[i] * (1.0 - f) + v[i + 1] * f;
}

} // namespace

double peak_sigma(const std::vector<double>& buckets, int best_bucket, double fmax) {
    std::vector<double> noise;
    noise.reserve(buckets.size());
    for (int i = 0; i < static_cast<int>(buckets.size()); i++) {
        if (std::abs(i - best_bucket) <= SIGMA_EXCLUDE_BUCKETS) continue;
        noise.push_back(buckets[static_cast<std::size_t>(i)]);
    }
    if (noise.size() < 8) return 0.0;

    // Overlap is zero across most offsets, so the noise distribution is a spike at
    // zero plus a thin tail of coincidental matches. A MAD collapses to zero on
    // that spike; the interquartile gap (p75 - p50) measures the tail instead and
    // stays stable as the cue count grows.
    const double p50 = percentile(noise, 0.50);
    const double spread = percentile(noise, 0.75) - p50;

    if (spread <= 1e-12) {
        // Every rival bucket is zero: the only noise mass is the best rival.
        double rival = 0.0;
        for (int i = 0; i < static_cast<int>(buckets.size()); i++) {
            if (std::abs(i - best_bucket) <= SIGMA_EXCLUDE_BUCKETS) continue;
            rival = std::max(rival, buckets[static_cast<std::size_t>(i)]);
        }
        if (rival <= 1e-12) return fmax > 1e-12 ? SIGMA_NO_RIVAL : 0.0;
        return (fmax - rival) / rival;
    }
    return (fmax - p50) / spread;
}

OffsetResult best_offset(const std::vector<Span>& cues,
                         const std::vector<Span>& reference,
                         const std::vector<float>& weights,
                         double coverage) {
    OffsetResult out;
    if (cues.empty() || reference.empty()) return out;

    std::vector<Event> events;
    events.reserve(cues.size() * 4 + 16);

    for (std::size_t k = 0; k < reference.size(); k++) {
        const Span r = reference[k];
        const double Lr = static_cast<double>(r.second - r.first);
        if (Lr <= 0) continue;
        const double wk = (k < weights.size()) ? static_cast<double>(weights[k]) : 1.0;
        if (wk <= 0) continue;

        for (const Span& c : cues) {
            const double Lc = static_cast<double>(c.second - c.first);
            if (Lc <= 0) continue;

            const double Lmin = std::min(Lc, Lr);
            const double Lmax = std::max(Lc, Lr);
            const double w = (Lmin / Lmax) * wk;
            const double s = w / Lmin;

            const ms o1 = r.first - c.second;                                   // overlap begins
            const ms o2 = std::min(r.first - c.first, r.second - c.second);     // plateau begins
            const ms o3 = std::max(r.first - c.first, r.second - c.second);     // plateau ends
            const ms o4 = r.second - c.first;                                   // overlap ends

            if (o4 < -MAX_OFFSET_MS || o1 > MAX_OFFSET_MS) continue;

            events.push_back({o1, +s});
            events.push_back({o2, -s});
            events.push_back({o3, -s});
            events.push_back({o4, +s});
            if (events.size() > MAX_EVENTS) return out;
        }
    }
    if (events.empty()) return out;

    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.offset < b.offset; });

    const int nbuckets = static_cast<int>((2 * MAX_OFFSET_MS) / SWEEP_BUCKET_MS) + 2;
    std::vector<double> peak(static_cast<std::size_t>(nbuckets), 0.0);
    auto bucket_of = [&](ms o) -> int {
        long long b = (o + MAX_OFFSET_MS) / SWEEP_BUCKET_MS;
        if (b < 0) b = 0;
        if (b >= nbuckets) b = nbuckets - 1;
        return static_cast<int>(b);
    };

    double slope = 0.0;
    double val = 0.0;
    double fmax = 0.0;
    ms best = 0;

    std::size_t i = 0;
    while (i < events.size()) {
        const ms o = events[i].offset;
        const double v = val;

        if (v > fmax) { fmax = v; best = o; }
        {
            const int b = bucket_of(o);
            if (v > peak[static_cast<std::size_t>(b)]) peak[static_cast<std::size_t>(b)] = v;
        }

        while (i < events.size() && events[i].offset == o) {
            slope += events[i].slope;
            i++;
        }

        const ms nxt = (i < events.size()) ? events[i].offset : o;
        if (nxt > o) {
            const double vn = v + slope * static_cast<double>(nxt - o);
            if (vn > fmax) { fmax = vn; best = nxt; }

            const int b0 = bucket_of(o);
            const int b1 = bucket_of(nxt);
            for (int b = b0; b <= b1; b++) {
                const ms bs = static_cast<ms>(b) * SWEEP_BUCKET_MS - MAX_OFFSET_MS;
                const ms be = bs + SWEEP_BUCKET_MS;
                const ms cs = std::max(o, bs);
                const ms ce = std::min(nxt, be);
                if (ce < cs) continue;
                const double vs = v + slope * static_cast<double>(cs - o);
                const double ve = v + slope * static_cast<double>(ce - o);
                const double m = std::max(vs, ve);
                if (m > peak[static_cast<std::size_t>(b)]) peak[static_cast<std::size_t>(b)] = m;
            }
            val = vn;
        }
    }

    const int bb = bucket_of(best);
    double rival = 0.0;
    for (int b = 0; b < nbuckets; b++) {
        if (std::abs(b - bb) <= SIGMA_EXCLUDE_BUCKETS) continue;
        rival = std::max(rival, peak[static_cast<std::size_t>(b)]);
    }

    out.offset_ms = best;
    const double denom = static_cast<double>(cues.size()) * (coverage > 0.0 ? coverage : 1.0);
    out.confidence = denom > 0.0 ? std::min(1.0, fmax / denom) : 0.0;
    out.margin = fmax > 1e-12 ? std::max(0.0, std::min(1.0, (fmax - rival) / fmax)) : 0.0;
    out.sigma = peak_sigma(peak, bb, fmax);
    return out;
}

double best_framerate(const std::vector<Span>& cues,
                      const std::vector<Span>& reference,
                      const std::vector<float>& weights,
                      ms* out_offset,
                      double* out_sigma) {
    double best_ratio = 1.0;
    double best_sigma = -1.0;
    ms best_off = 0;

    // `cues` are the reference timeline rescaled by some factor. We search for the
    // factor r that maps the reference onto the cues, then report the correction
    // the caller must apply to the cues, which is the inverse.
    for (int i = 0; i < FRAMERATE_RATIOS_N; i++) {
        const double r = FRAMERATE_RATIOS[i];
        std::vector<Span> scaled;
        scaled.reserve(reference.size());
        for (const Span& s : reference) {
            scaled.emplace_back(static_cast<ms>(std::llround(static_cast<double>(s.first) * r)),
                                static_cast<ms>(std::llround(static_cast<double>(s.second) * r)));
        }
        const OffsetResult res = best_offset(cues, scaled, weights, 1.0);
        if (res.sigma > best_sigma) {
            best_sigma = res.sigma;
            best_ratio = r;
            best_off = res.offset_ms;
        }
    }

    if (out_offset) *out_offset = best_off;
    if (out_sigma) *out_sigma = best_sigma < 0.0 ? 0.0 : best_sigma;
    return best_ratio > 0.0 ? 1.0 / best_ratio : 1.0;
}

} // namespace subsync
