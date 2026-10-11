#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include "fft.h"
#include "harness.h"

using namespace subsync;

int main() {
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> dist(-1.0, 1.0);

    for (int n : {256, 1024, 4096}) {
        std::vector<double> x(static_cast<std::size_t>(n));
        for (double& v : x) v = dist(rng);

        FFT fft(n);
        std::vector<std::complex<double>> spec;
        fft.real_forward(x, spec);
        t::eq_i(static_cast<long long>(spec.size()), n, "spectrum size");

        // DC bin must equal the sum of the input.
        double sum = 0.0;
        for (double v : x) sum += v;
        t::near(spec[0].real(), sum, 1e-6 * std::fabs(sum) + 1e-6, "DC bin == sum");

        // Round trip through the inverse.
        std::vector<std::complex<double>> work = spec;
        fft.inverse(work);
        double maxerr = 0.0;
        for (int i = 0; i < n; i++) {
            const double got = work[static_cast<std::size_t>(i)].real() / n;
            maxerr = std::max(maxerr, std::fabs(got - x[static_cast<std::size_t>(i)]));
        }
        t::near(maxerr, 0.0, 1e-9, "round-trip error");
    }

    // A pure tone must put its energy in one bin.
    {
        const int n = 1024;
        const int k = 37;
        std::vector<double> x(static_cast<std::size_t>(n));
        for (int i = 0; i < n; i++)
            x[static_cast<std::size_t>(i)] = std::cos(2.0 * 3.14159265358979 * k * i / n);
        FFT fft(n);
        std::vector<std::complex<double>> spec;
        fft.real_forward(x, spec);

        double peak = 0.0;
        int peak_bin = -1;
        for (int i = 0; i < n / 2; i++) {
            const double m = std::abs(spec[static_cast<std::size_t>(i)]);
            if (m > peak) { peak = m; peak_bin = i; }
        }
        t::eq_i(peak_bin, k, "tone lands in the expected bin");
    }

    // next_pow2 floor and rounding.
    t::eq_i(next_pow2(1), 256, "next_pow2 floor");
    t::eq_i(next_pow2(300), 512, "next_pow2 rounds up");
    t::eq_i(next_pow2(1024), 1024, "next_pow2 exact");

    return t::done("test_fft");
}
