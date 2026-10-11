// subsync — self-contained radix-2 Cooley-Tukey FFT
#pragma once
#include <complex>
#include <vector>

namespace subsync {

// Smallest power of two >= n, with a floor of 256.
int next_pow2(int n);

class FFT {
public:
    explicit FFT(int n);  // n must be a power of two, >= 2

    // Forward DFT of a real signal of length n (shorter input is zero-padded).
    // out is filled with the full n-point complex spectrum.
    void real_forward(const std::vector<double>& re, std::vector<std::complex<double>>& out) const;

    // In-place inverse DFT. Unnormalized: caller divides by size().
    void inverse(std::vector<std::complex<double>>& a) const;

    int size() const { return n_; }

private:
    int n_ = 0;
    int levels_ = 0;
    std::vector<int> rev_;
    std::vector<std::complex<double>> tw_;  // forward twiddles, exp(-2*pi*i*k/n), k < n/2
};

} // namespace subsync
