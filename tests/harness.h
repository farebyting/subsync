// Minimal assertion harness for subsync tests (no external framework).
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

namespace t {

inline int failures = 0;
inline int checks = 0;

inline void ok(bool cond, const std::string& what) {
    checks++;
    if (!cond) {
        failures++;
        std::printf("  FAIL: %s\n", what.c_str());
    }
}

inline void eq_i(long long got, long long want, const std::string& what) {
    checks++;
    if (got != want) {
        failures++;
        std::printf("  FAIL: %s (got %lld, want %lld)\n", what.c_str(), got, want);
    }
}

inline void near(double got, double want, double tol, const std::string& what) {
    checks++;
    if (!(std::fabs(got - want) <= tol)) {
        failures++;
        std::printf("  FAIL: %s (got %g, want %g +/- %g)\n", what.c_str(), got, want, tol);
    }
}

inline int done(const char* name) {
    if (failures == 0) {
        std::printf("%s: all %d checks passed\n", name, checks);
        return 0;
    }
    std::printf("%s: %d/%d checks FAILED\n", name, failures, checks);
    return 1;
}

} // namespace t
