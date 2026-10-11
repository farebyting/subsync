// subsync — self-contained RIFF/WAV reader (PCM), mono float output at 16 kHz
#pragma once
#include <string>
#include <vector>

namespace subsync {

struct Audio {
    std::vector<float> samples;  // mono, [-1, 1]
    int sample_rate = 0;
};

// Reads PCM WAV (8/16/24/32-bit int, 32/64-bit float), downmixes to mono and
// resamples to 16 kHz. Returns false with a message on failure.
bool read_wav(const std::string& path, Audio& out, std::string* err);

} // namespace subsync
