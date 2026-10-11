// ASS / SSA parse/write.
//   [Events]
//   Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
//   Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,text
// The Format: field order is variable and is honoured when locating Start/End/Text.
#include <string>
#include <vector>

#include "subtitle.h"
#include "textutil.h"

namespace subsync {

namespace {

std::vector<std::string> split_csv(const std::string& s, std::size_t limit) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',' && out.size() + 1 < limit) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

struct AssEvent {
    std::size_t line = 0;
    long long start = 0;
    long long end = 0;
};

struct AssLayout {
    int start_field = 1;
    int end_field = 2;
    int text_field = 9;
    int fields = 10;
    bool found_format = false;
};

AssLayout layout_of(const Lines& l) {
    AssLayout out;
    bool in_events = false;
    for (const std::string& raw : l.lines) {
        const std::string line = trim(raw);
        if (!line.empty() && line.front() == '[') {
            in_events = lower(line) == "[events]";
            continue;
        }
        if (!in_events) continue;
        if (starts_with(lower(line), "format:")) {
            const std::string rest = line.substr(7);
            const std::vector<std::string> names = split_csv(rest, 64);
            out.fields = static_cast<int>(names.size());
            for (int i = 0; i < out.fields; i++) {
                const std::string f = lower(trim(names[static_cast<std::size_t>(i)]));
                if (f == "start") out.start_field = i;
                else if (f == "end") out.end_field = i;
                else if (f == "text") out.text_field = i;
            }
            out.found_format = true;
            return out;
        }
    }
    return out;
}

std::vector<AssEvent> scan(const Lines& l, const AssLayout& lay) {
    std::vector<AssEvent> out;
    bool in_events = false;
    for (std::size_t i = 0; i < l.lines.size(); i++) {
        const std::string line = trim(l.lines[i]);
        if (!line.empty() && line.front() == '[') {
            in_events = lower(line) == "[events]";
            continue;
        }
        if (!in_events) continue;
        const std::string low = lower(line);
        if (!starts_with(low, "dialogue:")) continue;

        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string rest = line.substr(colon + 1);
        if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);

        const std::vector<std::string> f = split_csv(rest, static_cast<std::size_t>(lay.fields));
        if (static_cast<int>(f.size()) <= std::max(lay.start_field, lay.end_field)) continue;

        long long a = 0, b = 0;
        if (!parse_ass_time(f[static_cast<std::size_t>(lay.start_field)], &a)) continue;
        if (!parse_ass_time(f[static_cast<std::size_t>(lay.end_field)], &b)) continue;

        AssEvent e;
        e.line = i;
        e.start = a;
        e.end = b;
        out.push_back(e);
    }
    return out;
}

} // namespace

bool parse_ass(const std::string& text, std::vector<Cue>& out) {
    const Lines l = split_lines(text);
    const AssLayout lay = layout_of(l);
    const std::vector<AssEvent> events = scan(l, lay);
    if (events.empty()) return false;

    for (const AssEvent& e : events) {
        const std::string line = trim(l.lines[e.line]);
        const std::size_t colon = line.find(':');
        std::string rest = line.substr(colon + 1);
        if (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
        const std::vector<std::string> f = split_csv(rest, static_cast<std::size_t>(lay.fields));

        std::string body;
        if (static_cast<int>(f.size()) > lay.text_field)
            body = f[static_cast<std::size_t>(lay.text_field)];

        Cue c;
        c.start = e.start;
        c.end = e.end;
        c.text = body;
        c.raw = l.lines[e.line];
        out.push_back(c);
    }
    return !out.empty();
}

std::string write_ass(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne) {
    Lines l = split_lines(text);
    const AssLayout lay = layout_of(l);
    const std::vector<AssEvent> events = scan(l, lay);
    const std::size_t n = std::min(events.size(), std::min(ns.size(), ne.size()));

    for (std::size_t k = 0; k < n; k++) {
        const AssEvent& e = events[k];
        const std::string orig = l.lines[e.line];
        const std::size_t colon = orig.find(':');
        if (colon == std::string::npos) continue;

        const std::string head = orig.substr(0, colon + 1);
        std::string rest = orig.substr(colon + 1);
        const bool leading_space = !rest.empty() && rest.front() == ' ';
        if (leading_space) rest.erase(0, 1);

        const std::vector<std::string> f = split_csv(rest, static_cast<std::size_t>(lay.fields));
        if (static_cast<int>(f.size()) <= std::max(lay.start_field, lay.end_field)) continue;

        std::vector<std::string> g = f;
        g[static_cast<std::size_t>(lay.start_field)] =
            format_time_like(f[static_cast<std::size_t>(lay.start_field)], ns[k], 2);
        g[static_cast<std::size_t>(lay.end_field)] =
            format_time_like(f[static_cast<std::size_t>(lay.end_field)], ne[k], 2);

        std::string rebuilt = head;
        if (leading_space) rebuilt += ' ';
        for (std::size_t i = 0; i < g.size(); i++) {
            if (i) rebuilt += ',';
            rebuilt += g[i];
        }
        l.lines[e.line] = rebuilt;
    }
    return join_lines(l);
}

} // namespace subsync
