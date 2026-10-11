// subsync — human output routing (--quiet / --json silence everything here)
#pragma once
#include <iostream>
#include <ostream>
#include <streambuf>

namespace subsync {

inline bool quiet = false;

namespace detail {
struct NullBuf : std::streambuf {
    int overflow(int c) override { return c; }
};
inline std::ostream& null_stream() {
    static NullBuf nb;
    static std::ostream os(&nb);
    return os;
}
} // namespace detail

// All human-readable progress output goes through say(). Nothing else writes to std::cout.
inline std::ostream& say() { return quiet ? detail::null_stream() : std::cout; }

} // namespace subsync
