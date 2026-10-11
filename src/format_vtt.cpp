// WebVTT parse/write.
//   WEBVTT
//   [id line]
//   [HH:]MM:SS.mmm --> [HH:]MM:SS.mmm [settings]
//   text...
#include <string>
#include <vector>

#include "subtitle.h"
#include "textutil.h"

namespace subsync {

namespace {

struct VttEntry {
    std::size_t time_line = 0;
    long long start = 0;
    long long end = 0;
};

bool parse_vtt_arrow(const std::string& line, long long* a, long long* b) {
    const std::size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    const std::string left = trim(line.substr(0, arrow));
    std::string right = trim(line.substr(arrow + 3));
    const std::size_t sp = right.find(' ');
    if (sp != std::string::npos) right = right.substr(0, sp);
    if (left.empty() || right.empty()) return false;
    long long x = 0, y = 0;
    if (!parse_time_flexible(left, 3, &x)) return false;
    if (!parse_time_flexible(right, 3, &y)) return false;
    *a = x;
    *b = y;
    return true;
}

std::vector<VttEntry> scan(const Lines& l) {
    std::vector<VttEntry> out;
    for (std::size_t i = 0; i < l.lines.size(); i++) {
        if (l.lines[i].find("-->") == std::string::npos) continue;
        VttEntry e;
        if (!parse_vtt_arrow(l.lines[i], &e.start, &e.end)) continue;
        e.time_line = i;
        out.push_back(e);
    }
    return out;
}

} // namespace

bool parse_vtt(const std::string& text, std::vector<Cue>& out) {
    const Lines l = split_lines(text);
    const std::vector<VttEntry> entries = scan(l);
    if (entries.empty()) return false;

    for (std::size_t k = 0; k < entries.size(); k++) {
        const VttEntry& e = entries[k];
        const std::size_t begin = e.time_line + 1;
        const std::size_t stop = (k + 1 < entries.size()) ? entries[k + 1].time_line : l.lines.size();

        std::size_t block_begin = e.time_line;
        // A cue identifier is the single line directly above the timing line.
        if (block_begin > 0) {
            const std::string prev = trim(l.lines[block_begin - 1]);
            if (!prev.empty() && prev.find("-->") == std::string::npos &&
                lower(prev) != "webvtt")
                block_begin--;
        }

        std::string raw;
        for (std::size_t i = block_begin; i < stop; i++) {
            raw += l.lines[i];
            raw += "\n";
        }

        std::string body;
        for (std::size_t i = begin; i < stop; i++) {
            if (!body.empty()) body += "\n";
            body += l.lines[i];
        }
        std::size_t last = body.find_last_not_of("\n\r");
        if (last != std::string::npos && last + 1 < body.size()) body.erase(last + 1);

        Cue c;
        c.start = e.start;
        c.end = e.end;
        c.text = body;
        c.raw = raw;
        out.push_back(c);
    }
    return !out.empty();
}

std::string write_vtt(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne) {
    Lines l = split_lines(text);
    const std::vector<VttEntry> entries = scan(l);
    const std::size_t n = std::min(entries.size(), std::min(ns.size(), ne.size()));

    for (std::size_t k = 0; k < n; k++) {
        const VttEntry& e = entries[k];
        const std::string line = l.lines[e.time_line];
        const std::size_t arrow = line.find("-->");
        if (arrow == std::string::npos) continue;

        std::size_t la = 0, lb = 0, ra = 0, rb = 0;
        if (!token_before(line, arrow, &la, &lb)) continue;
        if (!token_after(line, arrow + 3, &ra, &rb)) continue;

        const std::string left_tok = line.substr(la, lb - la);
        const std::string right_tok = line.substr(ra, rb - ra);

        const std::string ns_tok = format_time_like(left_tok, ns[k], 3);
        const std::string ne_tok = format_time_like(right_tok, ne[k], 3);

        std::string rebuilt = line.substr(0, la) + ns_tok;
        rebuilt += line.substr(lb, ra - lb);
        rebuilt += ne_tok;
        rebuilt += line.substr(rb);
        l.lines[e.time_line] = rebuilt;
    }
    return join_lines(l);
}

} // namespace subsync
