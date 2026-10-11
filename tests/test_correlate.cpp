#include <cmath>
#include <random>
#include <vector>

#include "align.h"
#include "correlate.h"
#include "drift.h"
#include "harness.h"
#include "profile.h"

using namespace subsync;

namespace {

// A plausible subtitle: cues of 1-4 s, spread over ~20 minutes with gaps.
std::vector<Span> make_cues(int count, unsigned seed, ms start = 0) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> gap(500, 6000);
    std::uniform_int_distribution<int> len(1000, 4000);
    std::vector<Span> out;
    ms t = start;
    for (int i = 0; i < count; i++) {
        const ms l = len(rng);
        out.emplace_back(t, t + l);
        t += l + gap(rng);
    }
    return out;
}

std::vector<Span> shift(const std::vector<Span>& v, ms d) {
    std::vector<Span> out;
    out.reserve(v.size());
    for (const Span& s : v) out.emplace_back(s.first + d, s.second + d);
    return out;
}

std::vector<float> unit_weights(std::size_t n) { return std::vector<float>(n, 1.0f); }

} // namespace

int main() {
    // ---- constant offset, exact sweep ----
    {
        const std::vector<Span> ref = make_cues(120, 1);
        const std::vector<Span> cues = shift(ref, 2500);
        const OffsetResult r = best_offset(cues, ref, unit_weights(ref.size()), 1.0);
        t::eq_i(r.offset_ms, -2500, "constant offset recovered (sign)");
        t::ok(r.sigma > 5.0, "constant offset sigma is high");
        t::ok(r.margin > 0.5, "constant offset margin is high");
        t::ok(r.confidence > 0.9, "constant offset confidence is high");
    }
    // Negative shift
    {
        const std::vector<Span> ref = make_cues(120, 2);
        const std::vector<Span> cues = shift(ref, -7000);
        const OffsetResult r = best_offset(cues, ref, unit_weights(ref.size()), 1.0);
        t::eq_i(r.offset_ms, 7000, "negative offset recovered");
        t::ok(r.sigma > 5.0, "negative offset sigma is high");
        t::ok(r.margin > 0.5, "negative offset margin is high");
    }

    // ---- unrelated content must not look confident ----
    {
        const std::vector<Span> ref = make_cues(120, 3);
        const std::vector<Span> other = make_cues(120, 99);
        const OffsetResult r = best_offset(other, ref, unit_weights(ref.size()), 1.0);
        t::ok(r.margin < SOME_MARGIN, "unrelated cues have no clear winner");
        t::ok(r.sigma < 5.0, "unrelated cues are not confident");
    }

    // ---- framerate grid ----
    {
        const std::vector<Span> ref = make_cues(200, 4);
        // cues are the reference played 25/24 too fast -> scaling back recovers it
        std::vector<Span> cues;
        for (const Span& s : ref)
            cues.emplace_back(static_cast<ms>(std::llround(s.first * 24.0 / 25.0)),
                              static_cast<ms>(std::llround(s.second * 24.0 / 25.0)));

        ms off = 0;
        double sig = 0;
        const double ratio = best_framerate(cues, ref, unit_weights(ref.size()), &off, &sig);
        t::near(ratio, 25.0 / 24.0, 0.01, "framerate ratio detected");
        t::ok(sig > 6.0, "framerate pick is confident");
    }

    // ---- drift via robust_line ----
    {
        const std::vector<Span> ref = make_cues(400, 5);
        // cues drift: t' = t * (1 + 0.0004) + 3000   (0.4 ms per second)
        const double slope = 0.0004;
        const ms intercept = 3000;
        std::vector<Span> cues;
        for (const Span& s : ref)
            cues.emplace_back(static_cast<ms>(std::llround(s.first * (1.0 + slope))) + intercept,
                              static_cast<ms>(std::llround(s.second * (1.0 + slope))) + intercept);

        const std::vector<Chunk> chunks =
            chunk_offsets(cues, ref, unit_weights(ref.size()), std::max(2, static_cast<int>(cues.size() / 40)));
        t::ok(chunks.size() >= 4, "drift produced enough chunks");

        const std::pair<double, double> line = robust_line(chunks);
        // The line predicts the shift that aligns cues to the reference.
        // Cues sit at ref*(1+slope)+intercept, so the needed shift is negative.
        t::near(line.first, -slope * 1000.0, 0.15, "drift slope recovered");
        t::near(line.second, -static_cast<double>(intercept), 1500.0, "drift intercept recovered");

        // Applying the line should land cues back on the reference.
        double worst = 0.0;
        for (std::size_t i = 0; i < cues.size(); i++) {
            const ms back = apply_line(cues[i].first, line.first, line.second);
            worst = std::max(worst, std::fabs(static_cast<double>(back - ref[i].first)));
        }
        t::ok(worst < 250.0, "drift line restores alignment");
    }

    // ---- split: two halves with different offsets ----
    {
        const std::vector<Span> ref = make_cues(240, 6);
        const std::size_t half = ref.size() / 2;
        std::vector<Span> cues;
        for (std::size_t i = 0; i < ref.size(); i++) {
            const ms d = (i < half) ? 1500 : -4500;
            cues.emplace_back(ref[i].first + d, ref[i].second + d);
        }

        const std::vector<Chunk> chunks =
            chunk_offsets(cues, ref, unit_weights(ref.size()), std::max(2, static_cast<int>(cues.size() / 40)));
        t::ok(worth_splitting(chunks, 0), "split is judged worthwhile");

        const SplitResult sr = split_alignment(cues, ref, unit_weights(ref.size()), 1.0, 0);
        t::ok(sr.offsets.size() >= 2, "split found at least two segments");
        if (sr.offsets.size() >= 2) {
            t::eq_i(sr.offsets.front(), -1500, "split first segment offset");
            t::eq_i(sr.offsets.back(), 4500, "split last segment offset");
        }
    }

    // ---- a single offset must NOT be judged splittable ----
    {
        const std::vector<Span> ref = make_cues(240, 7);
        const std::vector<Span> cues = shift(ref, 2000);
        const std::vector<Chunk> chunks =
            chunk_offsets(cues, ref, unit_weights(ref.size()), std::max(2, static_cast<int>(cues.size() / 40)));
        t::ok(!worth_splitting(chunks, -2000), "uniform offset is not a split");
    }

    // ---- FFT align finds the same offset ----
    {
        const std::vector<Span> ref = make_cues(150, 8);
        const std::vector<Span> cues = shift(ref, -3000);
        align_setup(ref, unit_weights(ref.size()));
        const std::vector<Hit> hits = align_peaks(cues, 4);
        t::ok(!hits.empty(), "fft align produced candidates");
        if (!hits.empty()) {
            t::ok(abs_ms(hits[0].offset_ms - 3000) <= 120, "fft align best offset is close");
            t::ok(hits[0].z > 3.0, "fft align best z is meaningful");
        }
        const ms refined = align_refine(cues, hits.empty() ? 0 : hits[0].offset_ms);
        t::eq_i(refined, 3000, "fft align refine lands exactly");
        t::ok(onset_z(cues, 3000) > 2.0, "onset z at the true offset is positive");
        align_drop();
    }

    // ---- activity / reference_spans ----
    {
        const std::vector<Span> ref = make_cues(20, 9);
        const std::vector<int> act = activity(ref);
        t::ok(!act.empty(), "activity is non-empty");
        t::eq_i(static_cast<long long>(act.size()), ref.back().second / 10 + 1, "activity size");

        // VAD-like probabilities: bursts at 1-2 s and 5-6 s
        std::vector<float> prob(1000, 0.02f);
        for (int i = 100; i < 200; i++) prob[static_cast<std::size_t>(i)] = 0.9f;
        for (int i = 500; i < 600; i++) prob[static_cast<std::size_t>(i)] = 0.85f;
        const auto rs = reference_spans(prob);
        t::eq_i(static_cast<long long>(rs.first.size()), 2, "reference_spans found two bursts");
        if (rs.first.size() == 2) {
            t::eq_i(rs.first[0].first, 1000, "first burst start");
            t::eq_i(rs.first[0].second, 2000, "first burst end");
            t::ok(rs.second[0] > 0.8f, "first burst weight");
        }
    }

    // ---- process_spans drops junk ----
    {
        std::vector<Cue> cues;
        cues.push_back({0, 1000, "Hello there.", ""});
        cues.push_back({1000, 2000, "[door slams]", ""});
        cues.push_back({2000, 3000, "\xE2\x99\xAA", ""});
        cues.push_back({3000, 4000, "General Kenobi!", ""});
        const auto ps = process_spans(cues);
        t::eq_i(static_cast<long long>(ps.first.size()), 2, "junk cues dropped");
        t::eq_i(ps.second[0], 0, "first survivor maps to cue 0");
        t::eq_i(ps.second[1], 3, "second survivor maps to cue 3");
    }

    return t::done("test_correlate");
}
