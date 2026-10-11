#include "charset.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <vector>

namespace subsync {

namespace {

bool valid_utf8(const std::string& s) {
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        unsigned int cp = 0;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; if (cp < 2) return false; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else return false;
        if (i + static_cast<std::size_t>(extra) >= n) return false;
        for (int k = 1; k <= extra; k++) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        // reject overlong / surrogate / out-of-range
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += static_cast<std::size_t>(extra) + 1;
    }
    return true;
}

void append_utf8(std::string& out, unsigned int cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decode UTF-8 into codepoints; invalid bytes become U+FFFD.
std::vector<unsigned int> decode_utf8(const std::string& s) {
    std::vector<unsigned int> cps;
    cps.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        unsigned int cp = 0;
        if (c < 0x80) { cps.push_back(c); i++; continue; }
        else if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1Fu; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0Fu; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07u; }
        else { cps.push_back(0xFFFD); i++; continue; }
        if (i + static_cast<std::size_t>(extra) >= n) { cps.push_back(0xFFFD); i++; continue; }
        bool ok = true;
        for (int k = 1; k <= extra; k++) {
            const unsigned char cc = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) { cps.push_back(0xFFFD); i++; continue; }
        cps.push_back(cp);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return cps;
}

std::string utf16_to_utf8(const std::string& b, bool little) {
    std::string out;
    const std::size_t n = b.size() / 2;
    auto unit = [&](std::size_t k) -> unsigned int {
        const unsigned char lo = static_cast<unsigned char>(b[k * 2]);
        const unsigned char hi = static_cast<unsigned char>(b[k * 2 + 1]);
        return little ? (static_cast<unsigned int>(hi) << 8) | lo
                      : (static_cast<unsigned int>(lo) << 8) | hi;
    };
    for (std::size_t k = 0; k < n; k++) {
        unsigned int u = unit(k);
        if (u >= 0xD800 && u <= 0xDBFF && k + 1 < n) {
            const unsigned int lo = unit(k + 1);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                const unsigned int cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                append_utf8(out, cp);
                k++;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) { append_utf8(out, 0xFFFD); continue; }
        append_utf8(out, u);
    }
    return out;
}

} // namespace

Charset detect_charset(const std::string& bytes) {
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        return Charset::Utf8Bom;
    if (bytes.size() >= 2) {
        const unsigned char a = static_cast<unsigned char>(bytes[0]);
        const unsigned char b = static_cast<unsigned char>(bytes[1]);
        if (a == 0xFF && b == 0xFE) return Charset::Utf16LE;
        if (a == 0xFE && b == 0xFF) return Charset::Utf16BE;
    }
    if (valid_utf8(bytes)) return Charset::Utf8;
    return Charset::Latin1;
}

std::string to_utf8(const std::string& bytes, Charset c) {
    switch (c) {
        case Charset::Utf8:
            return bytes;
        case Charset::Utf8Bom:
            return bytes.size() >= 3 ? bytes.substr(3) : std::string();
        case Charset::Utf16LE:
            return utf16_to_utf8(bytes.size() >= 2 ? bytes.substr(2) : bytes, true);
        case Charset::Utf16BE:
            return utf16_to_utf8(bytes.size() >= 2 ? bytes.substr(2) : bytes, false);
        case Charset::Latin1: {
            std::string out;
            out.reserve(bytes.size());
            for (unsigned char ch : bytes) append_utf8(out, ch);
            return out;
        }
    }
    return bytes;
}

std::string from_utf8(const std::string& utf8, Charset c) {
    switch (c) {
        case Charset::Utf8:
            return utf8;
        case Charset::Utf8Bom:
            return std::string("\xEF\xBB\xBF", 3) + utf8;
        case Charset::Utf16LE:
        case Charset::Utf16BE: {
            const bool little = (c == Charset::Utf16LE);
            std::string out;
            out.push_back(little ? '\xFF' : '\xFE');
            out.push_back(little ? '\xFE' : '\xFF');
            for (unsigned int cp : decode_utf8(utf8)) {
                auto emit = [&](unsigned int u) {
                    const unsigned char lo = static_cast<unsigned char>(u & 0xFF);
                    const unsigned char hi = static_cast<unsigned char>((u >> 8) & 0xFF);
                    out.push_back(little ? static_cast<char>(lo) : static_cast<char>(hi));
                    out.push_back(little ? static_cast<char>(hi) : static_cast<char>(lo));
                };
                if (cp < 0x10000) {
                    emit(cp);
                } else if (cp <= 0x10FFFF) {
                    const unsigned int v = cp - 0x10000;
                    emit(0xD800 + (v >> 10));
                    emit(0xDC00 + (v & 0x3FF));
                } else {
                    emit(0xFFFD);
                }
            }
            return out;
        }
        case Charset::Latin1: {
            std::string out;
            out.reserve(utf8.size());
            for (unsigned int cp : decode_utf8(utf8))
                out.push_back(cp <= 0xFF ? static_cast<char>(cp) : '?');
            return out;
        }
    }
    return utf8;
}

std::string read_text(const std::string& path, Charset* was) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const Charset c = detect_charset(bytes);
    if (was) *was = c;
    return to_utf8(bytes, c);
}

bool write_text(const std::string& path, const std::string& utf8, Charset c) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const std::string bytes = from_utf8(utf8, c);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

bool charset_from_name(const std::string& name, Charset* out) {
    if (name == "utf8" || name == "utf-8") { *out = Charset::Utf8; return true; }
    if (name == "utf8-bom" || name == "utf-8-bom") { *out = Charset::Utf8Bom; return true; }
    if (name == "utf16le" || name == "utf-16le") { *out = Charset::Utf16LE; return true; }
    if (name == "utf16be" || name == "utf-16be") { *out = Charset::Utf16BE; return true; }
    if (name == "latin1" || name == "latin-1" || name == "iso-8859-1") { *out = Charset::Latin1; return true; }
    return false;
}

const char* charset_name(Charset c) {
    switch (c) {
        case Charset::Utf8: return "utf8";
        case Charset::Utf8Bom: return "utf8-bom";
        case Charset::Utf16LE: return "utf16le";
        case Charset::Utf16BE: return "utf16be";
        case Charset::Latin1: return "latin1";
    }
    return "utf8";
}

} // namespace subsync
