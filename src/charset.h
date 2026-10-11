// subsync — text encoding detection and conversion (self-contained)
#pragma once
#include <string>

namespace subsync {

enum class Charset { Utf8, Utf8Bom, Utf16LE, Utf16BE, Latin1 };

// BOM sniff, else UTF-8 validity check, else Latin-1.
Charset detect_charset(const std::string& bytes);

// Decode raw bytes to UTF-8 for internal use.
std::string to_utf8(const std::string& bytes, Charset c);

// Encode UTF-8 back to the requested charset (Latin-1 transliterates unmappable to '?').
std::string from_utf8(const std::string& utf8, Charset c);

// Read a whole file, detect its encoding, return UTF-8 text. Empty on failure.
std::string read_text(const std::string& path, Charset* was = nullptr);

// Write UTF-8 text out in the requested charset (adds BOM for Utf8Bom/Utf16*).
bool write_text(const std::string& path, const std::string& utf8, Charset c);

// CLI helpers for --encoding NAME.
bool charset_from_name(const std::string& name, Charset* out);
const char* charset_name(Charset c);

} // namespace subsync
