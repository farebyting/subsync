#include "textutil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

namespace subsync {

Lines split_lines(const std::string& text) {
    Lines out;
    out.eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    out.trailing_newline = !text.empty() && text.back() == '\n';
    std::string cur;
    for (std::size_t i = 0; i < text.size(); i++) {
        const char c = text[i];
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.lines.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.lines.push_back(cur);
    return out;
}

std::string join_lines(const Lines& l) {
    std::string out;
    for (std::size_t i = 0; i < l.lines.size(); i++) {
        out += l.lines[i];
        if (i + 1 < l.lines.size() || l.trailing_newline) out += l.eol;
    }
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
    return s.substr(a, b - a);
}

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool starts_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

bool is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

namespace {

bool parse_uint(const std::string& s, long long* out) {
    if (s.empty()) return false;
    long long v = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        v = v * 10 + (c - '0');
        if (v > 1000000000LL) return false;
    }
    *out = v;
    return true;
}

std::vector<std::string> split_char(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

// Parses [H:]M:S<sep>frac where frac has exactly frac_digits digits.
bool parse_time_generic(const std::string& in, char sep, int frac_digits, long long* out_ms) {
    const std::string s = trim(in);
    const std::size_t dot = s.rfind(sep);
    if (dot == std::string::npos) return false;

    const std::string frac = s.substr(dot + 1);
    if (frac.size() != static_cast<std::size_t>(frac_digits)) return false;
    long long f = 0;
    if (!parse_uint(frac, &f)) return false;
    const long long frac_ms = (frac_digits == 3) ? f : f * 10;

    const std::vector<std::string> parts = split_char(s.substr(0, dot), ':');
    if (parts.size() < 2 || parts.size() > 3) return false;

    long long secs = 0, mins = 0, hours = 0;
    if (!parse_uint(parts[parts.size() - 1], &secs)) return false;
    if (!parse_uint(parts[parts.size() - 2], &mins)) return false;
    if (parts.size() == 3 && !parse_uint(parts[0], &hours)) return false;

    *out_ms = ((hours * 3600 + mins * 60 + secs) * 1000) + frac_ms;
    return true;
}

std::string fmt(long long ms, char sep, bool always_hours) {
    if (ms < 0) ms = 0;
    const long long h = ms / 3600000;
    ms %= 3600000;
    const long long m = ms / 60000;
    ms %= 60000;
    const long long s = ms / 1000;
    const long long milli = ms % 1000;
    char buf[40];
    if (always_hours || h > 0)
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld%c%03lld", h, m, s, sep, milli);
    else
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld%c%03lld", m, s, sep, milli);
    return buf;
}

} // namespace

bool parse_srt_time(const std::string& s, long long* out_ms) {
    return parse_time_generic(s, ',', 3, out_ms);
}

bool parse_vtt_time(const std::string& s, long long* out_ms) {
    return parse_time_generic(s, '.', 3, out_ms);
}

bool parse_ass_time(const std::string& s, long long* out_ms) {
    return parse_time_generic(s, '.', 2, out_ms);
}

bool parse_time_flexible(const std::string& s, int frac_digits, long long* out_ms) {
    const std::string t = trim(s);
    const std::size_t comma = t.rfind(',');
    const std::size_t dot = t.rfind('.');
    std::size_t sep = std::string::npos;
    if (comma != std::string::npos && dot != std::string::npos) sep = std::max(comma, dot);
    else if (comma != std::string::npos) sep = comma;
    else sep = dot;
    if (sep == std::string::npos) return false;
    return parse_time_generic(t, t[sep], frac_digits, out_ms);
}

std::string format_srt_time(long long ms) { return fmt(ms, ',', true); }
std::string format_vtt_time(long long ms) { return fmt(ms, '.', true); }

std::string format_ass_time(long long ms) {
    if (ms < 0) ms = 0;
    const long long h = ms / 3600000;
    ms %= 3600000;
    const long long m = ms / 60000;
    ms %= 60000;
    const long long s = ms / 1000;
    const long long cs = (ms % 1000) / 10;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld", h, m, s, cs);
    return buf;
}

bool token_before(const std::string& line, std::size_t end, std::size_t* a, std::size_t* b) {
    if (end > line.size()) end = line.size();
    std::size_t e = end;
    while (e > 0 && std::isspace(static_cast<unsigned char>(line[e - 1]))) e--;
    if (e == 0) return false;
    std::size_t s = e;
    while (s > 0 && !std::isspace(static_cast<unsigned char>(line[s - 1]))) s--;
    *a = s;
    *b = e;
    return true;
}

bool token_after(const std::string& line, std::size_t begin, std::size_t* a, std::size_t* b) {
    std::size_t s = begin;
    while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s]))) s++;
    if (s >= line.size()) return false;
    std::size_t e = s;
    while (e < line.size() && !std::isspace(static_cast<unsigned char>(line[e]))) e++;
    *a = s;
    *b = e;
    return true;
}

std::string format_time_like(const std::string& token, long long ms, int frac_digits) {
    if (ms < 0) ms = 0;
    int colons = 0;
    int first_width = 0;
    char sep = (frac_digits == 2) ? '.' : ',';
    for (char c : token) {
        if (c == ':') { colons++; continue; }
        if (c == ',' || c == '.') { sep = c; continue; }
        if (colons == 0 && std::isdigit(static_cast<unsigned char>(c))) first_width++;
    }
    if (first_width < 1) first_width = 1;

    long long h = ms / 3600000;
    ms %= 3600000;
    long long m = ms / 60000;
    ms %= 60000;
    const long long s = ms / 1000;
    const long long rest = ms % 1000;

    char buf[64];
    if (colons >= 2) {
        if (frac_digits == 2)
            std::snprintf(buf, sizeof(buf), "%0*lld:%02lld:%02lld%c%02lld",
                          first_width, h, m, s, sep, rest / 10);
        else
            std::snprintf(buf, sizeof(buf), "%0*lld:%02lld:%02lld%c%03lld",
                          first_width, h, m, s, sep, rest);
        return buf;
    }

    // MM:SS shape (VTT allows it): fold hours into minutes.
    m += h * 60;
    if (frac_digits == 2)
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld%c%02lld", m, s, sep, rest / 10);
    else
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld%c%03lld", m, s, sep, rest);
    return buf;
}

} // namespace subsync
