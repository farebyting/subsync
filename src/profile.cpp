#include "profile.h"

#include <algorithm>
#include <cmath>

#include "constants.h"
#include "subtitle.h"

namespace subsync {

std::vector<int> activity(const std::vector<Span>& spans) {
    long long last = 0;
    for (const Span& s : spans) last = std::max(last, s.second);
    if (last <= 0) return {};

    const long long cap = MAX_TIME_MS / 10;
    long long nbin = last / 10 + 1;
    if (nbin > cap) nbin = cap;

    std::vector<int> out(static_cast<std::size_t>(nbin), 0);
    for (const Span& s : spans) {
        long long a = s.first / 10;
        long long z = s.second / 10;
        if (a < 0) a = 0;
        if (z > nbin) z = nbin;
        for (long long k = a; k < z; k++) out[static_cast<std::size_t>(k)] = 1;
    }
    return out;
}

std::pair<std::vector<Span>, std::vector<float>> reference_spans(const std::vector<float>& prob) {
    std::vector<Span> spans;
    std::vector<float> weights;
    if (prob.empty()) return {spans, weights};

    const ms frame = 10;
    const ms min_len = MIN_SPEECH_MS;
    const ms max_gap = MIN_SPEECH_MS;

    long long run_start = -1;
    double run_sum = 0.0;
    long long run_frames = 0;

    auto flush = [&](long long end_frame) {
        if (run_start < 0) return;
        const ms a = run_start * frame;
        const ms b = end_frame * frame;
        if (b - a >= min_len && run_frames > 0) {
            spans.emplace_back(a, b);
            weights.push_back(static_cast<float>(run_sum / static_cast<double>(run_frames)));
        }
        run_start = -1;
        run_sum = 0.0;
        run_frames = 0;
    };

    for (std::size_t i = 0; i < prob.size(); i++) {
        if (prob[i] > SPEECH_THRESHOLD) {
            if (run_start < 0) run_start = static_cast<long long>(i);
            run_sum += prob[i];
            run_frames++;
        } else {
            // a short dip does not end the run
            if (run_start >= 0) {
                std::size_t j = i;
                while (j < prob.size() && prob[j] <= SPEECH_THRESHOLD &&
                       static_cast<ms>(j - i) * frame < max_gap)
                    j++;
                if (j < prob.size() && prob[j] > SPEECH_THRESHOLD) {
                    run_sum += 0.0;
                    i = j - 1;
                    continue;
                }
            }
            flush(static_cast<long long>(i));
        }
    }
    flush(static_cast<long long>(prob.size()));
    return {spans, weights};
}

std::pair<std::vector<Span>, std::vector<int>> process_spans(const std::vector<Cue>& cues,
                                                             bool merge,
                                                             bool sort_by_time) {
    std::vector<Span> spans;
    std::vector<int> keep;

    std::vector<std::pair<Span, int>> tmp;
    tmp.reserve(cues.size());
    for (std::size_t i = 0; i < cues.size(); i++) {
        const Cue& c = cues[i];
        if (is_junk_cue(c.text)) continue;
        if (c.end <= c.start) continue;
        if (c.end - c.start > MAX_CUE_MS) continue;
        if (c.start < 0) continue;
        if (c.end > MAX_TIME_MS) continue;
        tmp.emplace_back(Span{c.start, c.end}, static_cast<int>(i));
    }

    if (sort_by_time)
        std::sort(tmp.begin(), tmp.end(),
                  [](const std::pair<Span, int>& a, const std::pair<Span, int>& b) {
                      if (a.first.first != b.first.first) return a.first.first < b.first.first;
                      return a.first.second < b.first.second;
                  });

    for (const auto& e : tmp) {
        if (merge && !spans.empty() && e.first.first <= spans.back().second) {
            spans.back().second = std::max(spans.back().second, e.first.second);
            continue;
        }
        spans.push_back(e.first);
        keep.push_back(e.second);
    }

    return {spans, keep};
}

} // namespace subsync
