#include "subtitle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "constants.h"
#include "textutil.h"

namespace subsync {

namespace {

std::string extension_of(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos) return {};
    if (slash != std::string::npos && dot < slash) return {};
    return lower(path.substr(dot));
}

// Bracket / note / credit markers used to spot non-dialogue cues.
const char* kOpenMarks[] = {"[", "(", "\xEF\xBC\x88", "\xE3\x80\x90", "\xE3\x80\x8C", "<<"};
const char* kNoteMarks[] = {"\xE2\x99\xAA", "\xE2\x99\xAB", "\xE2\x99\xAC", "\xE2\x99\xA9", "#", "*", "~"};

bool contains_any(const std::string& s, const char* const* list, std::size_t n) {
    for (std::size_t i = 0; i < n; i++)
        if (s.find(list[i]) != std::string::npos) return true;
    return false;
}

bool is_wrapped_in_marks(const std::string& t) {
    static const std::pair<const char*, const char*> pairs[] = {
        {"[", "]"}, {"(", ")"}, {"\xEF\xBC\x88", "\xEF\xBC\x89"},
        {"\xE3\x80\x90", "\xE3\x80\x91"}, {"\xE3\x80\x8C", "\xE3\x80\x8D"},
        {"<<", ">>"}, {"{", "}"},
    };
    for (const auto& p : pairs) {
        if (starts_with(t, p.first) && ends_with(t, p.second)) return true;
    }
    return false;
}

const char* kCreditMarks[] = {
    "www.", "http", ".com", ".net", ".org", "subtitle", "subtitles",
    "synced by", "sync by", "sync and corrections", "opensubtitles", "addic7ed",
    "subscene", "tekstet af", "oversat af", "legendas", "subtitulado",
    "subs by", "corrected by", "resync", "ripped by", "encoding by",
};

} // namespace

std::string subtitle_kind(const std::string& path) {
    const std::string ext = extension_of(path);
    if (ext == ".srt") return ".srt";
    if (ext == ".vtt") return ".vtt";
    if (ext == ".ass") return ".ass";
    if (ext == ".ssa") return ".ssa";
    if (ext == ".sub") return ".sub";
    return {};
}

std::vector<Cue> read_subtitle(const std::string& path, double fps) {
    const std::string kind = subtitle_kind(path);
    std::vector<Cue> cues;
    if (kind.empty()) return cues;

    const std::string text = read_text(path);
    if (text.empty()) return cues;

    if (kind == ".srt") {
        parse_srt(text, cues);
    } else if (kind == ".vtt") {
        parse_vtt(text, cues);
    } else if (kind == ".ass" || kind == ".ssa") {
        parse_ass(text, cues);
    } else if (kind == ".sub") {
        parse_sub(text, cues, fps);
    }

    std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) { return a.start < b.start; });
    if (cues.size() > static_cast<std::size_t>(MAX_CUES))
        cues.resize(static_cast<std::size_t>(MAX_CUES));
    return cues;
}

bool write_subtitle(const std::string& in_path, const std::string& out_path,
                    Charset out_charset, bool keep_charset,
                    const std::vector<ms>& new_starts, const std::vector<ms>& new_ends,
                    double fps, std::string* err) {
    const std::string kind = subtitle_kind(in_path);
    if (kind.empty()) {
        if (err) *err = "unsupported subtitle extension";
        return false;
    }

    Charset was = Charset::Utf8;
    const std::string text = read_text(in_path, &was);
    if (text.empty()) {
        if (err) *err = "could not read input subtitle";
        return false;
    }

    std::string out;
    if (kind == ".srt") {
        out = write_srt(text, new_starts, new_ends);
    } else if (kind == ".vtt") {
        out = write_vtt(text, new_starts, new_ends);
    } else if (kind == ".ass" || kind == ".ssa") {
        out = write_ass(text, new_starts, new_ends);
    } else if (kind == ".sub") {
        if (fps <= 0.0) {
            if (err) *err = "MicroDVD (.sub) needs a framerate; pass --fps";
            return false;
        }
        out = write_sub(text, new_starts, new_ends, fps);
    }

    const Charset target = keep_charset ? was : out_charset;
    if (!write_text(out_path, out, target)) {
        if (err) *err = "could not write output subtitle";
        return false;
    }
    return true;
}

bool is_junk_cue(const std::string& text) {
    const std::string t = trim(text);
    if (t.empty()) return true;

    if (is_wrapped_in_marks(t)) return true;
    if (contains_any(t, kOpenMarks, sizeof(kOpenMarks) / sizeof(kOpenMarks[0])) &&
        contains_any(t, kOpenMarks + 1, 1))
        return true;

    // A cue made only of note marks and punctuation carries no dialogue.
    {
        bool only_notes = true;
        for (char c : t) {
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            if (std::strchr("\xE2\x99\xAA\xE2\x99\xAB\xE2\x99\xAC\xE2\x99\xA9#*~.-_\x01", c)) continue;
            only_notes = false;
            break;
        }
        if (only_notes) return true;
    }

    const std::string low = lower(t);
    for (const char* m : kCreditMarks) {
        if (low.find(m) != std::string::npos) return true;
    }
    (void)kNoteMarks;
    return false;
}

std::string strip_markup(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    int angle = 0, brace = 0;
    for (std::size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '<') { angle++; continue; }
        if (c == '>') { if (angle) angle--; continue; }
        if (c == '{') { brace++; continue; }
        if (c == '}') { if (brace) brace--; continue; }
        if (angle || brace) continue;
        if (c == '\\' && i + 1 < text.size() &&
            (text[i + 1] == 'N' || text[i + 1] == 'n' || text[i + 1] == 'h')) {
            out.push_back(' ');
            i++;
            continue;
        }
        out.push_back(c);
    }
    // Leading dialogue dashes are markup for matching purposes.
    std::string trimmed = trim(out);
    while (!trimmed.empty() && (trimmed.front() == '-' || trimmed.front() == '\xE2')) {
        if (trimmed.front() == '-') { trimmed.erase(0, 1); trimmed = trim(trimmed); continue; }
        break;
    }
    return trimmed;
}

std::vector<Span> cues_to_spans(const std::vector<Cue>& cues) {
    std::vector<Span> out;
    out.reserve(cues.size());
    for (const Cue& c : cues) {
        if (is_junk_cue(c.text)) continue;
        if (c.end <= c.start) continue;
        out.emplace_back(c.start, c.end);
    }
    return out;
}

double fps_from_length(const std::string& sub_path, const std::string& other_path) {
    const std::string kind = subtitle_kind(sub_path);
    if (kind != ".sub") return 0.0;

    const std::string text = read_text(sub_path);
    if (text.empty()) return 0.0;

    // Parse frames without an fps to get the raw frame span.
    const Lines l = split_lines(text);
    long long first = -1, last = -1;
    for (const std::string& line : l.lines) {
        const std::string s = trim(line);
        if (s.empty() || s.front() != '{') continue;
        const std::size_t c1 = s.find('}');
        if (c1 == std::string::npos) continue;
        const std::size_t o2 = s.find('{', c1 + 1);
        if (o2 == std::string::npos) continue;
        const std::size_t c2 = s.find('}', o2 + 1);
        if (c2 == std::string::npos) continue;
        const std::string f1 = s.substr(1, c1 - 1);
        const std::string f2 = s.substr(o2 + 1, c2 - o2 - 1);
        if (!is_all_digits(f1) || !is_all_digits(f2)) continue;
        long long a = 0, b = 0;
        for (char c : f1) a = a * 10 + (c - '0');
        for (char c : f2) b = b * 10 + (c - '0');
        if (first < 0) first = a;
        last = b;
    }
    if (first < 0 || last <= first) return 0.0;

    const double frames = static_cast<double>(last - first);

    // Reference runtime in ms from the other subtitle.
    const std::vector<Cue> other = read_subtitle(other_path);
    if (other.size() < 2) return 0.0;
    const double ref_ms = static_cast<double>(other.back().end - other.front().start);
    if (ref_ms <= 0) return 0.0;

    const double got = frames / (ref_ms / 1000.0);
    static const double kRates[] = {23.976, 24.0, 25.0, 29.97, 30.0};
    double best = 0.0;
    double best_err = 1e9;
    for (double r : kRates) {
        const double err = std::abs(got - r) / r;
        if (err < best_err) { best_err = err; best = r; }
    }
    if (best_err > 0.08) return 0.0;
    return best;
}

} // namespace subsync
