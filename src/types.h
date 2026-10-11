// subsync — shared data types
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace subsync {

using ms = long long;  // milliseconds, signed

// std::llabs is not available on every MinGW/libstdc++ combination; keep one
// portable definition and use it everywhere.
inline ms abs_ms(ms v) { return v < 0 ? -v : v; }

struct Cue {
    ms start = 0;
    ms end = 0;
    std::string text;  // markup-preserved cue text (used for matching)
    std::string raw;   // original block text, used for verbatim write-back
};

using Span = std::pair<ms, ms>;  // {start_ms, end_ms}, start < end

struct SpanW {
    Span span;
    float weight = 1.0f;
};

struct OffsetResult {  // sweep-correlate answer
    ms offset_ms = 0;
    double confidence = 0;  // 0..1, overlap / (cues * coverage)
    double margin = 0;      // 0..1, (peak - rival)/peak
    double sigma = 0;       // how far peak stands out of noise (MAD z)
};

struct Hit {  // FFT-align candidate
    ms offset_ms = 0;
    double z = 0;       // (peak - median)/spread
    double runner = 0;  // best rival / winner, 0..1
    double score = 0;   // overlap score
};

struct Chunk {  // drift sample
    double time_s = 0;
    double offset_ms = 0;
    double weight = 0;
};

} // namespace subsync
