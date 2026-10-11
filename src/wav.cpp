#include "wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

#include "constants.h"

namespace subsync {

namespace {

constexpr int TARGET_RATE = 16000;

std::uint32_t rd32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t rd16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// Linear-interpolation resampler; good enough for speech-region detection.
std::vector<float> resample(const std::vector<float>& in, int from, int to) {
    if (from == to || in.empty()) return in;
    const double ratio = static_cast<double>(to) / static_cast<double>(from);
    const std::size_t n = static_cast<std::size_t>(static_cast<double>(in.size()) * ratio);
    std::vector<float> out(n, 0.0f);
    for (std::size_t i = 0; i < n; i++) {
        const double src = static_cast<double>(i) / ratio;
        const std::size_t i0 = static_cast<std::size_t>(src);
        const std::size_t i1 = std::min(i0 + 1, in.size() - 1);
        const double f = src - static_cast<double>(i0);
        out[i] = static_cast<float>(in[i0] * (1.0 - f) + in[i1] * f);
    }
    return out;
}

} // namespace

bool read_wav(const std::string& path, Audio& out, std::string* err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (err) *err = "cannot open " + path;
        return false;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 44) {
        if (err) *err = "file too small to be a WAV";
        return false;
    }

    const unsigned char* p = reinterpret_cast<const unsigned char*>(bytes.data());
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) {
        if (err) *err = "not a RIFF/WAVE file (only PCM WAV is supported)";
        return false;
    }

    int channels = 0, bits = 0, rate = 0;
    int fmt_tag = 0;
    std::size_t data_off = 0, data_len = 0;
    bool have_fmt = false;

    std::size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const unsigned char* ck = p + pos;
        const std::uint32_t sz = rd32(ck + 4);
        const std::size_t body = pos + 8;
        if (std::memcmp(ck, "fmt ", 4) == 0 && sz >= 16 && body + 16 <= bytes.size()) {
            fmt_tag = rd16(p + body);
            channels = rd16(p + body + 2);
            rate = static_cast<int>(rd32(p + body + 4));
            bits = rd16(p + body + 14);
            have_fmt = true;
        } else if (std::memcmp(ck, "data", 4) == 0) {
            data_off = body;
            data_len = std::min<std::size_t>(sz, bytes.size() - body);
        }
        pos = body + sz + (sz & 1u);
    }

    if (!have_fmt || data_len == 0 || channels < 1 || rate <= 0) {
        if (err) *err = "WAV is missing fmt/data chunks";
        return false;
    }
    const bool is_float = (fmt_tag == 3);
    const bool is_pcm = (fmt_tag == 1);
    if (!is_pcm && !is_float) {
        if (err) *err = "compressed WAV (format " + std::to_string(fmt_tag) +
                        ") is not supported; only PCM";
        return false;
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32 && !(is_float && bits == 64)) {
        if (err) *err = "unsupported PCM bit depth";
        return false;
    }

    const int bytes_per_sample = bits / 8;
    const std::size_t frame_bytes = static_cast<std::size_t>(bytes_per_sample * channels);
    if (frame_bytes == 0) {
        if (err) *err = "bad frame size";
        return false;
    }
    const std::size_t frames = data_len / frame_bytes;

    const long long cap = MAX_TIME_MS * rate / 1000;
    const std::size_t use_frames =
        (cap > 0 && static_cast<long long>(frames) > cap) ? static_cast<std::size_t>(cap) : frames;

    std::vector<float> mono(use_frames, 0.0f);
    for (std::size_t f = 0; f < use_frames; f++) {
        double acc = 0.0;
        const unsigned char* base = p + data_off + f * frame_bytes;
        for (int c = 0; c < channels; c++) {
            const unsigned char* s = base + static_cast<std::size_t>(c * bytes_per_sample);
            double v = 0.0;
            if (is_float && bits == 32) {
                float fv = 0.0f;
                std::memcpy(&fv, s, 4);
                v = fv;
            } else if (is_float && bits == 64) {
                double dv = 0.0;
                std::memcpy(&dv, s, 8);
                v = dv;
            } else if (bits == 8) {
                v = (static_cast<double>(s[0]) - 128.0) / 128.0;
            } else if (bits == 16) {
                const std::int16_t sv = static_cast<std::int16_t>(s[0] | (s[1] << 8));
                v = static_cast<double>(sv) / 32768.0;
            } else if (bits == 24) {
                std::int32_t sv = s[0] | (s[1] << 8) | (s[2] << 16);
                if (sv & 0x800000) sv |= ~0xFFFFFF;
                v = static_cast<double>(sv) / 8388608.0;
            } else if (bits == 32) {
                const std::int32_t sv = static_cast<std::int32_t>(rd32(s));
                v = static_cast<double>(sv) / 2147483648.0;
            }
            acc += v;
        }
        mono[f] = static_cast<float>(acc / channels);
    }

    out.samples = resample(mono, rate, TARGET_RATE);
    out.sample_rate = TARGET_RATE;
    return true;
}

} // namespace subsync
