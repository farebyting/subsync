#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "profile.h"
#include "vad.h"
#include "wav.h"

using namespace subsync;

namespace {

constexpr int RATE = 16000;

void put32(std::string& s, unsigned int v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
}
void put16(std::string& s, unsigned int v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}

// 16-bit mono PCM WAV with the given samples.
std::string make_wav(const std::vector<short>& pcm, int rate) {
    std::string data;
    for (short v : pcm) put16(data, static_cast<unsigned short>(v));

    std::string s;
    s += "RIFF";
    put32(s, static_cast<unsigned int>(36 + data.size()));
    s += "WAVE";
    s += "fmt ";
    put32(s, 16);
    put16(s, 1);              // PCM
    put16(s, 1);              // mono
    put32(s, static_cast<unsigned int>(rate));
    put32(s, static_cast<unsigned int>(rate * 2));
    put16(s, 2);
    put16(s, 16);
    s += "data";
    put32(s, static_cast<unsigned int>(data.size()));
    s += data;
    return s;
}

void write_file(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

int main() {
    // ---- WAV reading ----
    {
        std::vector<short> pcm;
        for (int i = 0; i < RATE; i++) pcm.push_back(static_cast<short>(10000 * std::sin(i * 0.05)));
        const std::string wav = make_wav(pcm, RATE);
        const std::string path = "test_wav_basic.wav";
        write_file(path, wav);

        Audio a;
        std::string err;
        t::ok(read_wav(path, a, &err), "wav reads: " + err);
        t::eq_i(a.sample_rate, 16000, "wav resampled to 16k");
        t::eq_i(static_cast<long long>(a.samples.size()), RATE, "wav sample count");
        t::ok(!a.samples.empty() && std::fabs(a.samples[100]) > 0.05f, "wav samples are non-trivial");
        std::remove(path.c_str());
    }
    // Non-WAV input is rejected cleanly.
    {
        const std::string path = "test_wav_bad.wav";
        write_file(path, "this is not a wav file at all, definitely not");
        Audio a;
        std::string err;
        t::ok(!read_wav(path, a, &err), "non-wav rejected");
        t::ok(!err.empty(), "non-wav gives a message");
        std::remove(path.c_str());
    }

    // ---- VAD finds bursts and only bursts ----
    {
        std::vector<short> pcm(static_cast<std::size_t>(RATE) * 8, 0);
        // two speech-like bursts: 1-2 s and 5-6 s (amplitude-modulated noise)
        unsigned seed = 12345;
        auto noise = [&]() {
            seed = seed * 1103515245u + 12345u;
            return static_cast<double>((seed >> 16) & 0x7FFF) / 32767.0 * 2.0 - 1.0;
        };
        for (int i = RATE; i < RATE * 2; i++)
            pcm[static_cast<std::size_t>(i)] = static_cast<short>(9000 * noise());
        for (int i = RATE * 5; i < RATE * 6; i++)
            pcm[static_cast<std::size_t>(i)] = static_cast<short>(9000 * noise());

        const std::string path = "test_vad_bursts.wav";
        write_file(path, make_wav(pcm, RATE));

        Audio a;
        std::string err;
        t::ok(read_wav(path, a, &err), "vad wav reads");

        double coverage = 0;
        const std::vector<float> prob = speech_profile(a, &coverage);
        t::eq_i(static_cast<long long>(prob.size()), 800, "vad frame count");

        auto mean_over = [&](int a_s, int b_s) {
            double sum = 0;
            int n = 0;
            for (int i = a_s * 100; i < b_s * 100 && i < static_cast<int>(prob.size()); i++) {
                sum += prob[static_cast<std::size_t>(i)];
                n++;
            }
            return n ? sum / n : 0.0;
        };

        const double quiet = mean_over(3, 4);
        const double burst1 = mean_over(1, 2);
        const double burst2 = mean_over(5, 6);
        t::ok(burst1 > quiet + 0.3, "vad separates burst 1 from silence");
        t::ok(burst2 > quiet + 0.3, "vad separates burst 2 from silence");
        t::ok(coverage > 0.0 && coverage < 1.0, "vad coverage is partial");

        // reference_spans over the VAD output should find exactly two regions
        const auto rs = reference_spans(prob);
        t::ok(rs.first.size() >= 2, "vad spans found");
        std::remove(path.c_str());
    }

    return t::done("test_audio");
}
