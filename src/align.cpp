#include "align.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>

#include "constants.h"
#include "correlate.h"
#include "fft.h"

namespace subsync {

namespace {

constexpr double PI = 3.14159265358979323846;

// One bin per G ms. Block channel says "speech is happening here"; the onset
// channel says "speech starts here". Removing a long running mean from both
// stops a busy half of the film from outvoting a quiet one.
struct Side {
    std::vector<std::complex<double>> b, o;
    double eb = 0.0, eo = 0.0;
};

struct AlignState {
    bool ready = false;
    int N = 0;
    int nbins = 0;
    Side ref;
    std::vector<int> ref_activity;  // 10 ms bins, for align_refine
    ActivityIndex ref_index;
    std::unique_ptr<FFT> fft;
};

AlignState g;

void smooth_gauss(std::vector<double>& v, double sigma_bins) {
    if (sigma_bins <= 0.0 || v.empty()) return;
    const int rad = std::max(1, static_cast<int>(std::ceil(3.0 * sigma_bins)));
    std::vector<double> k(static_cast<std::size_t>(2 * rad + 1));
    double sum = 0.0;
    for (int i = -rad; i <= rad; i++) {
        k[static_cast<std::size_t>(i + rad)] =
            std::exp(-static_cast<double>(i * i) / (2.0 * sigma_bins * sigma_bins));
        sum += k[static_cast<std::size_t>(i + rad)];
    }
    for (double& x : k) x /= sum;

    std::vector<double> out(v.size(), 0.0);
    const int n = static_cast<int>(v.size());
    for (int i = 0; i < n; i++) {
        double acc = 0.0;
        for (int j = -rad; j <= rad; j++) {
            const int idx = i + j;
            if (idx < 0 || idx >= n) continue;
            acc += v[static_cast<std::size_t>(idx)] * k[static_cast<std::size_t>(j + rad)];
        }
        out[static_cast<std::size_t>(i)] = acc;
    }
    v.swap(out);
}

void highpass(std::vector<double>& v, int win) {
    if (win < 1 || v.empty()) return;
    const int n = static_cast<int>(v.size());
    std::vector<double> pre(static_cast<std::size_t>(n) + 1, 0.0);
    for (int i = 0; i < n; i++)
        pre[static_cast<std::size_t>(i) + 1] = pre[static_cast<std::size_t>(i)] + v[static_cast<std::size_t>(i)];

    std::vector<double> out(static_cast<std::size_t>(n), 0.0);
    const int half = win / 2;
    for (int i = 0; i < n; i++) {
        const int a = std::max(0, i - half);
        const int b = std::min(n, i + half + 1);
        const double mean = (pre[static_cast<std::size_t>(b)] - pre[static_cast<std::size_t>(a)]) /
                            static_cast<double>(b - a);
        out[static_cast<std::size_t>(i)] = v[static_cast<std::size_t>(i)] - mean;
    }
    v.swap(out);
}

double energy_of(const std::vector<double>& v) {
    double e = 0.0;
    for (double x : v) e += x * x;
    return e;
}

void make_side(const std::vector<Span>& spans, int nbins, const FFT& fft, Side& out,
               bool unit_weight) {
    std::vector<double> b(static_cast<std::size_t>(nbins), 0.0);
    std::vector<double> o(static_cast<std::size_t>(nbins), 0.0);

    for (const Span& s : spans) {
        int a = static_cast<int>(s.first / G);
        int z = static_cast<int>(s.second / G);
        if (a < 0) a = 0;
        if (z >= nbins) z = nbins - 1;
        if (z < a) continue;

        // Each span contributes one unit of "coverage", spread over its bins.
        const double h = unit_weight ? (1.0 / static_cast<double>(z - a + 1)) : 1.0;
        for (int k = a; k <= z; k++) b[static_cast<std::size_t>(k)] += h;
        o[static_cast<std::size_t>(a)] += 1.0;
    }

    smooth_gauss(o, ONSET_SM / static_cast<double>(G));
    highpass(b, static_cast<int>(HP_MS / G));
    highpass(o, static_cast<int>(HP_ON_MS / G));

    fft.real_forward(b, out.b);
    out.eb = energy_of(b);
    fft.real_forward(o, out.o);
    out.eo = energy_of(o);
}

std::vector<double> corr_curve(const std::vector<std::complex<double>>& r,
                               const std::vector<std::complex<double>>& s,
                               double er, double es, double blur_ms, const FFT& fft) {
    const int N = fft.size();
    std::vector<std::complex<double>> spec(static_cast<std::size_t>(N));
    for (int k = 0; k < N; k++)
        spec[static_cast<std::size_t>(k)] = r[static_cast<std::size_t>(k)] * std::conj(s[static_cast<std::size_t>(k)]);

    if (blur_ms > 0.0) {
        const double sigma = blur_ms / static_cast<double>(G);
        for (int k = 0; k < N; k++) {
            const double f = static_cast<double>(k) / static_cast<double>(N);
            const double gain = std::exp(-2.0 * PI * PI * sigma * sigma * f * f);
            spec[static_cast<std::size_t>(k)] *= gain;
        }
    }

    fft.inverse(spec);

    std::vector<double> curve(static_cast<std::size_t>(N), 0.0);
    const double norm = (er > 1e-12 && es > 1e-12) ? 1.0 / (std::sqrt(er) * std::sqrt(es)) : 0.0;
    for (int i = 0; i < N; i++)
        curve[static_cast<std::size_t>(i)] = spec[static_cast<std::size_t>(i)].real() /
                                             static_cast<double>(N) * norm;
    return curve;
}

// Lag index -> offset in ms. Index 0 is zero lag; high indices are negative lags.
ms lag_to_offset(int lag, int N) {
    if (lag <= N / 2) return static_cast<ms>(lag) * G;
    return static_cast<ms>(lag - N) * G;
}

int offset_to_lag(ms offset, int N) {
    long long lag = offset / G;
    lag %= N;
    if (lag < 0) lag += N;
    return static_cast<int>(lag);
}

struct PeakStats {
    double median = 0.0;
    double spread = 0.0;
};

PeakStats stats_of(const std::vector<double>& curve) {
    std::vector<double> samp;
    samp.reserve(curve.size() / 3 + 1);
    for (std::size_t i = 0; i < curve.size(); i += 3) samp.push_back(curve[i]);
    if (samp.empty()) return {};

    std::vector<double> sorted = samp;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t n = sorted.size();
    const double med = (n & 1u) ? sorted[n / 2] : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);

    std::vector<double> dev;
    dev.reserve(n);
    for (double x : sorted) dev.push_back(std::abs(x - med));
    std::sort(dev.begin(), dev.end());
    const double mad = (n & 1u) ? dev[n / 2] : 0.5 * (dev[n / 2 - 1] + dev[n / 2]);

    PeakStats st;
    st.median = med;
    st.spread = 1.4826 * mad;
    return st;
}

std::vector<Hit> peaks_from(const std::vector<double>& curve, int N, int want) {
    const PeakStats st = stats_of(curve);
    if (st.spread <= 1e-15) return {};

    const int max_lag = static_cast<int>(MAX_OFFSET_MS / G);
    std::vector<std::pair<double, int>> cand;
    for (int l = 0; l <= max_lag && l < N; l++)
        cand.emplace_back(curve[static_cast<std::size_t>(l)], l);
    for (int l = std::max(0, N - max_lag); l < N; l++) {
        if (l <= max_lag) continue;
        cand.emplace_back(curve[static_cast<std::size_t>(l)], l);
    }
    if (cand.empty()) return {};

    std::sort(cand.begin(), cand.end(),
              [](const std::pair<double, int>& a, const std::pair<double, int>& b) {
                  return a.first > b.first;
              });

    const int guard = std::max(1, 2500 / G);
    std::vector<Hit> hits;
    std::vector<int> taken;
    for (const auto& c : cand) {
        if (static_cast<int>(hits.size()) >= want) break;
        bool clash = false;
        for (int t : taken) {
            if (std::abs(t - c.second) <= guard) { clash = true; break; }
        }
        if (clash) continue;
        taken.push_back(c.second);

        Hit h;
        h.offset_ms = lag_to_offset(c.second, N);
        h.z = (c.first - st.median) / st.spread;
        h.score = c.first;
        hits.push_back(h);
    }

    // runner = how close the next distinct candidate came to the winner
    if (hits.size() >= 2 && std::abs(hits[0].score) > 1e-15)
        hits[0].runner = hits[1].score / hits[0].score;
    return hits;
}

} // namespace

void align_drop() { g = AlignState{}; }

void align_setup(const std::vector<Span>& reference, const std::vector<float>& weights) {
    align_drop();
    if (reference.size() < 4) return;

    ms last = 0;
    for (const Span& s : reference) last = std::max(last, s.second);
    const int nbins = static_cast<int>(last / G) + 2;
    if (nbins < 8) return;

    g.nbins = nbins;
    g.N = next_pow2(2 * nbins + 8);
    g.fft = std::make_unique<FFT>(g.N);

    // Weighted reference: weight spreads across a span's bins.
    std::vector<Span> weighted_spans;
    weighted_spans.reserve(reference.size());
    for (const Span& s : reference) weighted_spans.push_back(s);

    {
        std::vector<double> b(static_cast<std::size_t>(nbins), 0.0);
        std::vector<double> o(static_cast<std::size_t>(nbins), 0.0);
        for (std::size_t i = 0; i < reference.size(); i++) {
            const double w = (i < weights.size()) ? static_cast<double>(weights[i]) : 1.0;
            if (w <= 0.0) continue;
            int a = static_cast<int>(reference[i].first / G);
            int z = static_cast<int>(reference[i].second / G);
            if (a < 0) a = 0;
            if (z >= nbins) z = nbins - 1;
            if (z < a) continue;
            for (int k = a; k <= z; k++) b[static_cast<std::size_t>(k)] += w;
            o[static_cast<std::size_t>(a)] += w;
        }
        smooth_gauss(o, ONSET_SM / static_cast<double>(G));
        highpass(b, static_cast<int>(HP_MS / G));
        highpass(o, static_cast<int>(HP_ON_MS / G));

        g.fft->real_forward(b, g.ref.b);
        g.ref.eb = energy_of(b);
        g.fft->real_forward(o, g.ref.o);
        g.ref.eo = energy_of(o);
    }

    // 10 ms activity index for the exact refine pass.
    ms last_ms = 0;
    for (const Span& s : reference) last_ms = std::max(last_ms, s.second);
    g.ref_activity.assign(static_cast<std::size_t>(last_ms / 10) + 2, 0);
    for (const Span& s : reference) {
        long long a = s.first / 10;
        long long z = s.second / 10;
        if (a < 0) a = 0;
        if (z >= static_cast<long long>(g.ref_activity.size())) z = static_cast<long long>(g.ref_activity.size()) - 1;
        for (long long k = a; k <= z; k++) g.ref_activity[static_cast<std::size_t>(k)] = 1;
    }
    g.ref_index.build(g.ref_activity);

    g.ready = (g.ref.eb > 0.0 || g.ref.eo > 0.0);
}

std::vector<Hit> align_peaks(const std::vector<Span>& cues, int want) {
    std::vector<Hit> all;
    if (!g.ready || cues.empty() || !g.fft) return all;

    Side cue;
    make_side(cues, g.nbins, *g.fft, cue, true);

    struct Pass {
        double blur;
        bool use_block;
        bool use_onset;
    };
    const Pass passes[] = {
        {0.0, true, true},
        {550.0, true, true},
        {450.0, false, true},
        {250.0, true, false},
    };

    for (const Pass& p : passes) {
        std::vector<double> curve;
        if (p.use_block && p.use_onset) {
            const std::vector<double> cb =
                corr_curve(g.ref.b, cue.b, g.ref.eb, cue.eb, p.blur, *g.fft);
            const std::vector<double> co =
                corr_curve(g.ref.o, cue.o, g.ref.eo, cue.eo, p.blur, *g.fft);
            curve.assign(cb.size(), 0.0);
            for (std::size_t i = 0; i < cb.size(); i++) curve[i] = cb[i] + ON_W * co[i];
        } else if (p.use_onset) {
            curve = corr_curve(g.ref.o, cue.o, g.ref.eo, cue.eo, p.blur, *g.fft);
        } else {
            curve = corr_curve(g.ref.b, cue.b, g.ref.eb, cue.eb, p.blur, *g.fft);
        }

        for (Hit& h : peaks_from(curve, g.N, want)) all.push_back(h);
    }

    std::sort(all.begin(), all.end(), [](const Hit& a, const Hit& b) { return a.z > b.z; });

    // Dedup: keep the strongest candidate within 2000 ms of an already-taken one.
    std::vector<Hit> merged;
    for (const Hit& h : all) {
        bool near = false;
        for (const Hit& m : merged) {
            if (abs_ms(m.offset_ms - h.offset_ms) <= 2000) { near = true; break; }
        }
        if (near) continue;
        merged.push_back(h);
        if (static_cast<int>(merged.size()) >= want) break;
    }
    return merged;
}

ms align_refine(const std::vector<Span>& cues, ms offset_ms) {
    if (!g.ready || cues.empty() || g.ref_index.empty()) return offset_ms;

    ms best = offset_ms;
    double best_score = -1.0;
    for (ms d = -REFINE_WINDOW_MS; d <= REFINE_WINDOW_MS; d += REFINE_STEP_MS) {
        const ms cand = offset_ms + d;
        const double s = align_score(cues, g.ref_index, cand);
        if (s > best_score) {
            best_score = s;
            best = cand;
        }
    }
    return best;
}

double onset_z(const std::vector<Span>& cues, ms offset_ms) {
    if (!g.ready || cues.empty() || !g.fft) return 0.0;

    Side cue;
    make_side(cues, g.nbins, *g.fft, cue, true);
    const std::vector<double> curve = corr_curve(g.ref.o, cue.o, g.ref.eo, cue.eo, 0.0, *g.fft);
    const PeakStats st = stats_of(curve);
    if (st.spread <= 1e-15) return 0.0;

    const int lag = offset_to_lag(offset_ms, g.N);
    return (curve[static_cast<std::size_t>(lag)] - st.median) / st.spread;
}

} // namespace subsync
