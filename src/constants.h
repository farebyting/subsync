// subsync — constants (verified from the lapse engine; do not invent new values)
#pragma once
#include <cstddef>

namespace subsync {

using ms = long long;

inline constexpr ms MAX_OFFSET_MS   = 15LL * 60 * 1000;   // beyond this = wrong film, stop
inline constexpr ms SPLIT_WINDOW_MS = 600000;
inline constexpr ms SPLIT_COARSE_MS = 100;
inline constexpr ms REFINE_WINDOW_MS= 45000;
inline constexpr ms REFINE_STEP_MS  = 25;
inline constexpr ms CONCAT_SEARCH_MS= 90LL * 60 * 1000;
inline constexpr ms CONCAT_JUMP_MS  = 300000;
inline constexpr ms CONCAT_BACK_MS  = 60000;

inline constexpr int  G               = 40;      // FFT align bin size, ms
inline constexpr ms   HP_MS           = 30000;   // running-mean window removed from block channel
inline constexpr ms   HP_ON_MS        = 8000;    // running-mean window removed from onset channel
inline constexpr double ON_W          = 1.15;    // onset channel weight vs block
inline constexpr double ONSET_SM      = 120;     // onset gaussian smooth, ms
inline constexpr double TAU           = 450;     // (reserved) onset decay, ms
inline constexpr int  SWEEP_BUCKET_MS = 1000;    // margin/sigma bucket width

inline constexpr double SPEECH_THRESHOLD = 0.25;
inline constexpr int  MIN_CUES        = 10;
inline constexpr ms   MIN_SPEECH_MS   = 200;
inline constexpr ms   MAX_CUE_MS      = 10000;
inline constexpr ms   MAX_TIME_MS     = 24LL * 3600 * 1000;
inline constexpr int  MAX_CUES        = 100000;
inline constexpr std::size_t MAX_SUBTITLE_BYTES = 64u * 1024u * 1024u;

inline constexpr double SURE_SIGMA = 8.0;
inline constexpr double SOME_SIGMA = 6.0;

// Peak-significance tuning (values chosen from measured sweep distributions).
inline constexpr int SIGMA_EXCLUDE_BUCKETS = 5;   // buckets either side of the peak
inline constexpr double SIGMA_NO_RIVAL = 12.0;    // nothing competes with the peak at all

// The verdict rides on the *margin* (peak vs best rival), which is scale-free and
// therefore stable across cue counts; sigma is reported as a diagnostic.
inline constexpr double SOLID_MARGIN = 0.25;  // a clear winner
inline constexpr double SOME_MARGIN = 0.15;   // at least a discernible winner

inline constexpr ms SNAP_WINDOW_MS = 120;
inline constexpr ms AGREE_MS       = 400;
inline constexpr int MIN_AGREEING  = 3;

// framerate ratios actually encountered (subtitle made for one fps, played at another)
inline constexpr double FRAMERATE_RATIOS[] = {
    1.0, 24.0/23.976, 23.976/24.0, 25.0/24.0, 24.0/25.0,
    25.0/23.976, 23.976/25.0, 30.0/29.97, 29.97/30.0, 30.0/25.0, 25.0/30.0
};
inline constexpr int FRAMERATE_RATIOS_N =
    static_cast<int>(sizeof(FRAMERATE_RATIOS) / sizeof(FRAMERATE_RATIOS[0]));

} // namespace subsync
