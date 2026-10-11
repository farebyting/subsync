#include "fft.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace subsync {

namespace {
constexpr double PI = 3.14159265358979323846;

void transform(std::vector<std::complex<double>>& a,
               const std::vector<int>& rev,
               const std::vector<std::complex<double>>& tw,
               int n, bool inverse) {
    for (int i = 0; i < n; i++)
        if (i < rev[i]) std::swap(a[i], a[rev[i]]);

    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1;
        const int step = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0; j < half; j++) {
                std::complex<double> w = tw[j * step];
                if (inverse) w = std::conj(w);
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
        }
    }
}
} // namespace

int next_pow2(int n) {
    int p = 256;
    while (p < n) p <<= 1;
    return p;
}

FFT::FFT(int n) : n_(n) {
    if (n < 2 || (n & (n - 1)) != 0)
        throw std::invalid_argument("FFT size must be a power of two >= 2");
    levels_ = 0;
    while ((1 << levels_) < n) levels_++;

    rev_.resize(n);
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < levels_; b++)
            if (i & (1 << b)) r |= 1 << (levels_ - 1 - b);
        rev_[i] = r;
    }

    tw_.resize(n / 2);
    for (int k = 0; k < n / 2; k++)
        tw_[k] = std::polar(1.0, -2.0 * PI * k / n);
}

void FFT::real_forward(const std::vector<double>& re,
                       std::vector<std::complex<double>>& out) const {
    out.assign(static_cast<std::size_t>(n_), std::complex<double>(0.0, 0.0));
    const int m = std::min<int>(static_cast<int>(re.size()), n_);
    for (int i = 0; i < m; i++) out[i] = std::complex<double>(re[i], 0.0);
    transform(out, rev_, tw_, n_, false);
}

void FFT::inverse(std::vector<std::complex<double>>& a) const {
    if (static_cast<int>(a.size()) != n_)
        throw std::invalid_argument("FFT::inverse size mismatch");
    transform(a, rev_, tw_, n_, true);
}

} // namespace subsync
