#include <string>
#include <vector>

#include "charset.h"
#include "harness.h"

using namespace subsync;

int main() {
    // Detection
    t::ok(detect_charset("\xEF\xBB\xBFhello") == Charset::Utf8Bom, "utf8 bom detected");
    t::ok(detect_charset(std::string("\xFF\xFEh\0i\0", 6)) == Charset::Utf16LE, "utf16le detected");
    t::ok(detect_charset(std::string("\xFE\xFF\0h\0i", 6)) == Charset::Utf16BE, "utf16be detected");
    t::ok(detect_charset("plain ascii") == Charset::Utf8, "ascii is utf8");
    t::ok(detect_charset("caf\xC3\xA9") == Charset::Utf8, "valid utf8 multi-byte");
    t::ok(detect_charset("caf\xE9") == Charset::Latin1, "invalid utf8 falls back to latin1");

    // Decode
    t::ok(to_utf8("\xEF\xBB\xBF" "abc", Charset::Utf8Bom) == "abc", "utf8 bom stripped");
    t::ok(to_utf8("caf\xE9", Charset::Latin1) == "caf\xC3\xA9", "latin1 -> utf8");
    t::ok(to_utf8(std::string("\xFF\xFE") + std::string("c\0a\0f\0", 6), Charset::Utf16LE) == "caf",
          "utf16le -> utf8");
    t::ok(to_utf8(std::string("\xFE\xFF") + std::string("\0c\0a\0f", 6), Charset::Utf16BE) == "caf",
          "utf16be -> utf8");

    // Encode round trips
    t::ok(from_utf8("caf\xC3\xA9", Charset::Latin1) == "caf\xE9", "utf8 -> latin1");
    t::ok(from_utf8("caf\xC3\xA9", Charset::Utf8Bom) == "\xEF\xBB\xBF" "caf\xC3\xA9",
          "utf8 -> utf8 bom");
    t::ok(from_utf8("\xE2\x99\xAA", Charset::Latin1) == "?", "unmappable -> '?'");

    // Surrogate pair round trip (U+1F600)
    {
        const std::string emoji = "\xF0\x9F\x98\x80";
        const std::string le = from_utf8(emoji, Charset::Utf16LE);
        t::eq_i(static_cast<long long>(le.size()), 6, "surrogate pair is 4 bytes + BOM");
        t::ok(to_utf8(le, Charset::Utf16LE) == emoji, "surrogate pair round trip");
    }

    // CLI name mapping
    {
        Charset c;
        t::ok(charset_from_name("utf8", &c) && c == Charset::Utf8, "name utf8");
        t::ok(charset_from_name("latin1", &c) && c == Charset::Latin1, "name latin1");
        t::ok(!charset_from_name("bogus", &c), "unknown name rejected");
    }

    return t::done("test_charset");
}
