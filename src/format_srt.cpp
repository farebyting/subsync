// SRT (SubRip) parse/write.
//   index line
//   HH:MM:SS,mmm --> HH:MM:SS,mmm
//   text...
//   <blank>
#include <string>
#include <vector>

#include "subtitle.h"
#include "textutil.h"

namespace subsync {

namespace {

struct SrtEntry {
    std::size_t time_line = 0;   // index into Lines::lines
    long long start = 0;
    long long end = 0;
    std::string start_token;
    std::string end_token;
};

bool parse_arrow_line(const std::string& line, long long* a, long long* b,
                      std::string* ta, std::string* tb) {
    const std::size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    const std::string left = trim(line.substr(0, arrow));
    std::string right = trim(line.substr(arrow + 3));
    // SRT may carry trailing position coordinates after the end time; keep them out of the time token.
    std::size_t cut = right.find(' ');
    if (cut != std::string::npos) right = right.substr(0, cut);
    if (left.empty() || right.empty()) return false;
    long long x = 0, y = 0;
    if (!parse_time_flexible(left, 3, &x)) return false;
    if (!parse_time_flexible(right, 3, &y)) return false;
    *a = x;
    *b = y;
    *ta = left;
    *tb = right;
    return true;
}

std::vector<SrtEntry> scan(const Lines& l) {
    std::vector<SrtEntry> out;
    for (std::size_t i = 0; i < l.lines.size(); i++) {
        if (l.lines[i].find("-->") == std::string::npos) continue;
        SrtEntry e;
        if (!parse_arrow_line(l.lines[i], &e.start, &e.end, &e.start_token, &e.end_token)) continue;
        e.time_line = i;
        out.push_back(e);
    }
    return out;
}

} // namespace

bool parse_srt(const std::string& text, std::vector<Cue>& out) {
    const Lines l = split_lines(text);
    const std::vector<SrtEntry> entries = scan(l);
    if (entries.empty()) return false;

    for (std::size_t k = 0; k < entries.size(); k++) {
        const SrtEntry& e = entries[k];
        const std::size_t begin = e.time_line + 1;
        const std::size_t stop = (k + 1 < entries.size())
                                     ? entries[k + 1].time_line
                                     : l.lines.size();

        // The block raw text covers the index line (if present) through the text lines.
        std::size_t block_begin = e.time_line;
        if (block_begin > 0 && is_all_digits(trim(l.lines[block_begin - 1]))) block_begin--;

        std::string raw;
        for (std::size_t i = block_begin; i < stop; i++) {
            if (i + 1 == stop && i >= begin && trim(l.lines[i]).empty() && l.trailing_newline == false)
                continue;
            raw += l.lines[i];
            raw += "\n";
        }

        std::string body;
        for (std::size_t i = begin; i < stop; i++) {
            if (!body.empty()) body += "\n";
            body += l.lines[i];
        }
        // Trim trailing blank lines that belong to the block separator.
        while (!body.empty() && (body.back() == '\n')) body.pop_back();
        while (!body.empty() && body.back() == '\r') body.pop_back();
        // strip a trailing empty line produced by the separator blank
        if (!body.empty()) {
            std::size_t last = body.find_last_not_of("\n\r");
            if (last != std::string::npos && last + 1 < body.size()) body.erase(last + 1);
        }

        Cue c;
        c.start = e.start;
        c.end = e.end;
        c.text = body;
        c.raw = raw;
        out.push_back(c);
    }
    return !out.empty();
}

std::string write_srt(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne) {
    Lines l = split_lines(text);
    const std::vector<SrtEntry> entries = scan(l);
    const std::size_t n = std::min(entries.size(), std::min(ns.size(), ne.size()));

    for (std::size_t k = 0; k < n; k++) {
        const SrtEntry& e = entries[k];
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
