// subsync — small text helpers shared by the format parsers/writers
#pragma once
#include <string>
#include <vector>

namespace subsync {

// Line-oriented view of a text file that remembers its dominant EOL style
// and whether the file ended with a newline (needed for byte-exact writes).
struct Lines {
    std::vector<std::string> lines;
    std::string eol = "\n";  // "\n" or "\r\n"
    bool trailing_newline = true;
};

Lines split_lines(const std::string& text);
std::string join_lines(const Lines& l);

std::string trim(const std::string& s);
std::string lower(const std::string& s);
bool starts_with(const std::string& s, const std::string& p);
bool ends_with(const std::string& s, const std::string& p);
bool is_all_digits(const std::string& s);

// Timecode parsing/formatting (ms is the internal unit).
bool parse_srt_time(const std::string& s, long long* out_ms);   // HH:MM:SS,mmm
bool parse_vtt_time(const std::string& s, long long* out_ms);   // [HH:]MM:SS.mmm
bool parse_ass_time(const std::string& s, long long* out_ms);   // H:MM:SS.cc
// Accepts either ',' or '.' as the fraction separator, with the given digit count.
bool parse_time_flexible(const std::string& s, int frac_digits, long long* out_ms);
std::string format_srt_time(long long ms);
std::string format_vtt_time(long long ms);
std::string format_ass_time(long long ms);

// Reformat ms into the same *shape* as an existing time token (same colon count,
// same separator, same fraction width). Used so that unchanged times round-trip
// byte-for-byte and changed times keep the file's original style.
std::string format_time_like(const std::string& token, long long ms, int frac_digits);

// Locate the non-space token ending at `end` (searching backwards).
// Returns false when there is no such token.
bool token_before(const std::string& line, std::size_t end, std::size_t* a, std::size_t* b);
// Locate the first non-space token at or after `begin`.
bool token_after(const std::string& line, std::size_t begin, std::size_t* a, std::size_t* b);

} // namespace subsync
