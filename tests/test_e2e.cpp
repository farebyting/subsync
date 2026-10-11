// End-to-end tests: drive the real subsync binary over the generated fixtures.
// Skipped (with a clear message) when the fixtures or binary are absent, so the
// unit suites still run on a machine where fixtures have not been generated.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "harness.h"

namespace {

bool exists(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return static_cast<bool>(f);
}

// cmd.exe (which popen uses on Windows) rejects forward slashes inside a quoted
// command, so every path handed to it is converted to backslashes.
std::string native(const std::string& p) {
    std::string out = p;
    for (char& c : out)
        if (c == '/') c = '\\';
    return out;
}

// Minimal JSON field readers (the CLI's --json output is flat and predictable).
std::string json_str(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\":\"";
    const std::size_t p = j.find(pat);
    if (p == std::string::npos) return {};
    const std::size_t a = p + pat.size();
    const std::size_t b = j.find('"', a);
    if (b == std::string::npos) return {};
    return j.substr(a, b - a);
}

double json_num(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\":";
    const std::size_t p = j.find(pat);
    if (p == std::string::npos) return 0.0;
    return std::atof(j.c_str() + p + pat.size());
}

bool json_bool(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\":";
    const std::size_t p = j.find(pat);
    if (p == std::string::npos) return false;
    return j.compare(p + pat.size(), 4, "true") == 0;
}

// cmd.exe (which popen uses on Windows) mangles a command line containing several
// quoted arguments. Only the program path needs quoting; argument paths are passed
// bare. That is safe here because the fixture and build paths contain no spaces.
std::string quote(const std::string& s) {
    if (s.find(' ') == std::string::npos) return s;
    return "\"" + s + "\"";
}

// Run the binary and capture stdout.
std::string run(const std::string& exe, const std::string& args) {
    const std::string cmd = quote(native(exe)) + " " + args + " 2>nul";
    if (std::getenv("SUBSYNC_TEST_DEBUG")) std::fprintf(stderr, "[cmd] %s\n", cmd.c_str());
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    return out;
}

std::string path(const std::string& p) { return native(p); }

const char* kExe = SUBSYNC_EXE;
const char* kFx = SUBSYNC_FX;

std::string fx(const std::string& name) { return native(std::string(kFx) + "/" + name); }

} // namespace

int main() {
    if (!exists(kExe)) {
        std::printf("test_e2e: SKIPPED (binary not found at %s)\n", kExe);
        return 0;
    }
    if (!exists(fx("ref.srt"))) {
        std::printf("test_e2e: SKIPPED (fixtures not found at %s; run tests/make_fixtures.py)\n",
                    kFx);
        return 0;
    }

    // ---- constant offset ----
    {
        const std::string j = run(kExe, path(fx("ref.srt")) + " " + path(fx("t_plus2500.srt")) +
                                            " --dry-run --json");
        const double off = json_num(j, "offset_ms");
        t::ok(!j.empty(), "offset: produced json");
        t::near(off, -2500.0, 60.0, "offset: recovered -2500");
        t::ok(json_str(j, "verdict") == "solid", "offset: verdict solid");
        t::ok(!json_bool(j, "written"), "offset: dry-run wrote nothing");
    }

    // ---- framerate drift ----
    {
        const std::string j = run(kExe, path(fx("ref.srt")) + " " + path(fx("t_drift.srt")) +
                                            " --dry-run --json");
        const double ratio = json_num(j, "ratio");
        t::near(ratio, 25.0 / 24.0, 0.005, "drift: ratio is 25/24");
        t::ok(json_str(j, "verdict") == "solid", "drift: verdict solid");
    }

    // ---- split ----
    {
        const std::string j = run(kExe, path(fx("ref.srt")) + " " + path(fx("t_split.srt")) +
                                            " --dry-run --json");
        t::near(json_num(j, "parts"), 2.0, 0.1, "split: two parts");
        const std::string offs = json_str(j, "offsets");
        // offsets is an array; check both expected values appear
        t::ok(j.find("-1500") != std::string::npos, "split: first offset -1500");
        t::ok(j.find("4500") != std::string::npos, "split: second offset 4500");
        (void)offs;
    }

    // ---- already in sync ----
    {
        const std::string j = run(kExe, path(fx("ref.srt")) + " " + path(fx("t_same.srt")) +
                                            " --dry-run --json");
        t::ok(json_bool(j, "unchanged"), "same: reported unchanged");
        t::ok(!json_bool(j, "written"), "same: wrote nothing");
    }

    // ---- audio mode ----
    if (exists(fx("speech.wav"))) {
        const std::string j = run(kExe, path(fx("speech.wav")) + " " + path(fx("audio_off.srt")) +
                                            " --dry-run --json");
        t::near(json_num(j, "offset_ms"), 1200.0, 80.0, "audio: recovered +1200");
        t::ok(json_str(j, "reference") == "vad", "audio: reference is vad");
        t::ok(json_str(j, "verdict") == "solid", "audio: verdict solid");
    }

    return t::done("test_e2e");
}
