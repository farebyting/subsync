// subsync — subtitle read/write seam and matching helpers
#pragma once
#include <string>
#include <vector>

#include "charset.h"
#include "types.h"

namespace subsync {

// ".srt", ".vtt", ".ass", ".ssa", ".sub" (MicroDVD) or "" when unknown.
std::string subtitle_kind(const std::string& path);

// Parse a subtitle into cues (ms). Returns empty on failure.
// fps is required for frame-based formats (MicroDVD .sub).
std::vector<Cue> read_subtitle(const std::string& path, double fps = 0.0);

// Rewrite a subtitle's cue times. new_starts/new_ends must be 1:1 with the cues
// returned by read_subtitle() for the same file. fps is required for MicroDVD.
bool write_subtitle(const std::string& in_path, const std::string& out_path,
                    Charset out_charset, bool keep_charset,
                    const std::vector<ms>& new_starts, const std::vector<ms>& new_ends,
                    double fps = 0.0, std::string* err = nullptr);

// --- matching helpers (never used for write-back) ---
bool is_junk_cue(const std::string& text);
std::string strip_markup(const std::string& text);

// Cue spans for correlation, junk dropped.
std::vector<Span> cues_to_spans(const std::vector<Cue>& cues);

// Guess a frame-based subtitle's fps by comparing its runtime to a reference file.
// Returns 0 when it cannot be determined confidently.
double fps_from_length(const std::string& sub_path, const std::string& other_path);

// --- per-format hooks (implemented in format_*.cpp) ---
bool parse_srt(const std::string& text, std::vector<Cue>& out);
std::string write_srt(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne);

bool parse_vtt(const std::string& text, std::vector<Cue>& out);
std::string write_vtt(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne);

bool parse_ass(const std::string& text, std::vector<Cue>& out);
std::string write_ass(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne);

bool parse_sub(const std::string& text, std::vector<Cue>& out, double fps);
std::string write_sub(const std::string& text, const std::vector<ms>& ns, const std::vector<ms>& ne,
                      double fps);

} // namespace subsync
