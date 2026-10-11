#include "vad.h"

#include <algorithm>
#include <cmath>

#include "constants.h"

namespace subsync {

namespace {

constexpr int FRAME_MS = 10;
constexpr int WIN_MS = 25;
constexpr double FLOOR_WINDOW_S = 5.0;    // window over which the noise floor is tracked
constexpr double FLOOR_PERCENTILE = 0.20; // the floor sits at this percentile of log energy
constexpr double SPEECH_LIFT = 0.5;       // narrow-band (voiced) log-energy lift
constexpr double BROADBAND_LIFT = 0.15;   // broadband lift, scaled by (1 - zcr)
constexpr double LIFT_SCALE = 0.25;       // logistic steepness in log-energy units

} // namespace

std::vector<float> speech_profile(const Audio& a, double* coverage) {
    std::vector<float> prob;
    if (coverage) *coverage = 1.0;
    if (a.samples.empty() || a.sample_rate <= 0) return prob;

    const int hop = a.sample_rate * FRAME_MS / 1000;
    const int win = a.sample_rate * WIN_MS / 1000;
    if (hop <= 0 || win <= 0) return prob;

    const std::size_t nframes = a.samples.size() / static_cast<std::size_t>(hop);
    if (nframes == 0) return prob;

    std::vector<double> log_energy(nframes, 0.0);
    std::vector<double> zcr(nframes, 0.0);

    for (std::size_t f = 0; f < nframes; f++) {
        const std::size_t start = f * static_cast<std::size_t>(hop);
        const std::size_t stop = std::min(a.samples.size(), start + static_cast<std::size_t>(win));

        double e = 0.0;
        int crossings = 0;
        for (std::size_t i = start; i < stop; i++) {
            const double s = a.samples[i];
            e += s * s;
            if (i > start && ((a.samples[i - 1] < 0.0) != (s < 0.0))) crossings++;
        }
        const double span = static_cast<double>(stop > start ? stop - start : 1);
        log_energy[f] = std::log(e / span + 1e-12);
        zcr[f] = static_cast<double>(crossings) / span;
    }

    // Local noise floor: a low percentile of the log-energy in a wide window.
    // A plain running *median* would collapse onto the burst level inside a long
    // burst and score it as average, so a low percentile is used instead.
    const std::size_t half = static_cast<std::size_t>(FLOOR_WINDOW_S * 1000.0 / FRAME_MS / 2.0);
    std::vector<double> floor_at(nframes, 0.0);
    {
        std::vector<double> window;
        for (std::size_t f = 0; f < nframes; f++) {
            const std::size_t a0 = (f > half) ? f - half : 0;
            const std::size_t b0 = std::min(nframes, f + half + 1);
            window.assign(log_energy.begin() + static_cast<std::ptrdiff_t>(a0),
                          log_energy.begin() + static_cast<std::ptrdiff_t>(b0));
            std::sort(window.begin(), window.end());
            const std::size_t idx = static_cast<std::size_t>(
                FLOOR_PERCENTILE * static_cast<double>(window.size() - 1) + 0.5);
            floor_at[f] = window[std::min(idx, window.size() - 1)];
        }
    }

    prob.resize(nframes, 0.0f);
    std::size_t above = 0;
    for (std::size_t f = 0; f < nframes; f++) {
        // How far above the local floor, in natural-log energy units.
        // Narrow-band (voiced) energy counts in full; broadband energy counts in
        // proportion to (1 - zero-crossing rate), so hiss is not read as speech.
        const double lift = log_energy[f] - floor_at[f];
        const double z = std::min(1.0, zcr[f]);
        const double need = SPEECH_LIFT * (1.0 - 0.5 * z) + BROADBAND_LIFT * (1.0 - z);
        double p = 1.0 / (1.0 + std::exp(-(lift - need) / LIFT_SCALE));

        p = std::max(0.0, std::min(1.0, p));
        prob[f] = static_cast<float>(p);
        if (p > SPEECH_THRESHOLD) above++;
    }

    if (coverage) {
        *coverage = nframes ? static_cast<double>(above) / static_cast<double>(nframes) : 1.0;
        if (*coverage < 0.01) *coverage = 0.01;  // never divide by zero downstream
    }
    return prob;
}

} // namespace subsync
