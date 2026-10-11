// MicroDVD (.sub) parse/write.
//   {startFrame}{endFrame}text
// Frame-based: needs an fps to convert to/from ms. '|' is a line break.
#include <string>
#include <vector>

#include "subtitle.h"
#include "textutil.h"

namespace subsync {

namespace {

struct SubEntry {
    std::size_t line = 0;
    long long start_frame = 0;
    long long end_frame = 0;
};

// "{a}{b}text" -> frames + body
bool parse_frame_line(const std::string& line, long long* a, long long* b, std::string* body) {
    const std::string s = trim(line);
    if (s.empty() || s.front() != '{') return false;
    const std::size_t c1 = s.find('}');
    if (c1 == std::string::npos) return false;
    const std::size_t o2 = s.find('{', c1 + 1);
    if (o2 == std::string::npos) return false;
    const std::size_t c2 = s.find('}', o2 + 1);
    if (c2 == std::string::npos) return false;

    const std::string f1 = s.substr(1, c1 - 1);
    const std::string f2 = s.substr(o2 + 1, c2 - o2 - 1);
    if (!is_all_digits(f1) || !is_all_digits(f2)) return false;

    long long x = 0, y = 0;
    for (char c : f1) x = x * 10 + (c - '0');
    for (char c : f2) y = y * 10 + (c - '0');
    *a = x;
    *b = y;
    *body = s.substr(c2 + 1);
    return true;
}

std::vector<SubEntry> scan(const Lines& l) {
    std::vector<SubEntry> out;
    for (std::size_t i = 0; i < l.lines.size(); i++) {
        long long a = 0, b = 0;
        std::string body;
        if (!parse_frame_line(l.lines[i], &a, &b, &body)) continue;
        SubEntry e;
        e.line = i;
        e.start_frame = a;
        e.end_frame = b;
        out.push_back(e);
    }
    return out;
}

std::string frames_to_body(const std::string& body) {
    std::string out;
    for (char c : body) out += (c == '|') ? "\n" : std::string(1, c);
    return out;
}

} // namespace

bool parse_sub(const std::string& text, std::vector<Cue>& out, double fps) {
    if (fps <= 0.0) return false;
    const Lines l = split_lines(text);
    const std::vector<SubEntry> entries = scan(l);
    if (entries.empty()) return false;

    for (const SubEntry& e : entries) {
        long long a = 0, b = 0;
        std::string body;
        parse_frame_line(l.lines[e.line], &a, &b, &body);

        Cue c;
        c.start = static_cast<long long>(a / fps * 1000.0 + 0.5);
        c.end = static_cast<long long>(b / fps * 1000.0 + 0.5);
        c.text = frames_to_body(body);
        c.raw = l.lines[e.line];
        out.push_back(c);
    }
    return !out.empty();
}

std::string write_sub(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne,
                      double fps) {
    Lines l = split_lines(text);
    if (fps <= 0.0) return join_lines(l);
    const std::vector<SubEntry> entries = scan(l);
    const std::size_t n = std::min(entries.size(), std::min(ns.size(), ne.size()));

    for (std::size_t k = 0; k < n; k++) {
        const SubEntry& e = entries[k];
        const std::string orig = l.lines[e.line];
        long long a = 0, b = 0;
        std::string body;
        if (!parse_frame_line(orig, &a, &b, &body)) continue;

        const long long nf = static_cast<long long>(ns[k] / 1000.0 * fps + 0.5);
        const long long nf2 = static_cast<long long>(ne[k] / 1000.0 * fps + 0.5);

        std::string rebuilt = "{" + std::to_string(nf) + "}{" + std::to_string(nf2) + "}" + body;
        l.lines[e.line] = rebuilt;
    }
    return join_lines(l);
}

} // namespace subsync
