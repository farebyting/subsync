// subsync — CLI, pipeline, verdicts and reporting
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "align.h"
#include "charset.h"
#include "constants.h"
#include "correlate.h"
#include "drift.h"
#include "log.h"
#include "profile.h"
#include "subtitle.h"
#include "textutil.h"
#include "vad.h"
#include "wav.h"

namespace fs = std::filesystem;

namespace subsync {
namespace {

const char* kVersion = "subsync 1.0.0";

struct Options {
    std::string source;
    std::string target;
    std::string mode;      // "", auto, ols, nosplit, split
    double penalty = 1.0;
    std::string output;
    bool backup = true;
    bool dry_run = false;
    bool as_json = false;
    bool strict = false;
    bool force = false;
    bool snap = false;
    ms snap_window = SNAP_WINDOW_MS;
    double confidence = SOME_MARGIN;  // minimum margin before the file is overwritten
    double fps = 0.0;
    bool have_encoding = false;
    Charset encoding = Charset::Utf8;
    ms min_shift = 1;
    std::string undo;
};

struct Report {
    std::string mode = "ols";
    std::string reference = "vad";
    ms offset = 0;
    double ratio = 1.0;
    double confidence = 0;
    double margin = 0;
    double sigma = 0;
    double agreement = 0;
    double coverage = 1.0;
    int cues = 0;
    int ignored = 0;
    int parts = 1;
    int snapped = 0;
    bool written = false;
    bool unchanged = false;
    std::string verdict = "nothing";
    std::string why;
    std::string output;
    std::vector<int> splits;
    std::vector<ms> offsets;
};

bool number(const std::string& s, double* out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || (end && *end != '\0')) return false;
    *out = v;
    return true;
}

std::string escaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (static_cast<unsigned char>(c) < 0x20) continue;
        else out += c;
    }
    return out;
}

std::string beside(const std::string& path, const std::string& tag) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return path + tag;
    return path.substr(0, dot) + tag + path.substr(dot);
}

void usage() {
    std::cerr
        << "Usage: subsync <source> <target.srt> [auto|ols|nosplit|split] [penalty]\n"
        << "               [--output <path>] [--no-backup] [--dry-run] [--json] [--quiet]\n"
        << "               [--strict] [--force] [--confidence N] [--fps N] [--snap [ms]]\n"
        << "               [--encoding NAME] [--min-shift ms]\n"
        << "       subsync --version | --formats | --undo <subtitle>\n"
        << "  <source> is a WAV (audio mode) or a subtitle (sub-to-sub mode).\n";
}

void report_human(const Report& r) {
    say() << "Done (" << r.mode << "): offset=" << r.offset << "ms";
    if (std::abs(r.ratio - 1.0) > 1e-9) say() << " ratio=" << r.ratio;
    if (r.parts > 1) {
        say() << " parts=" << r.parts << " offsets=";
        for (std::size_t i = 0; i < r.offsets.size(); i++) say() << (i ? "," : "") << r.offsets[i];
    }
    if (r.snapped) say() << " snapped=" << r.snapped;
    say() << " sigma=" << r.sigma << " agree=" << r.agreement << " confidence=" << r.confidence;
    say() << " [" << r.verdict << "]";
    if (!r.written) {
        if (r.unchanged) say() << " -> already in sync, left alone";
        else say() << " NOT WRITTEN (" << r.why << ")";
    } else {
        say() << " -> " << r.output;
    }
    say() << '\n';
}

void report_json(const Report& r) {
    std::cout << "{\"mode\":\"" << r.mode << "\""
              << ",\"reference\":\"" << r.reference << "\""
              << ",\"offset_ms\":" << r.offset
              << ",\"ratio\":" << r.ratio
              << ",\"confidence\":" << r.confidence
              << ",\"margin\":" << r.margin
              << ",\"sigma\":" << r.sigma
              << ",\"agreement\":" << r.agreement
              << ",\"verdict\":\"" << r.verdict << "\""
              << ",\"coverage\":" << r.coverage
              << ",\"cues\":" << r.cues
              << ",\"ignored_cues\":" << r.ignored
              << ",\"parts\":" << r.parts
              << ",\"snapped\":" << r.snapped
              << ",\"written\":" << (r.written ? "true" : "false")
              << ",\"unchanged\":" << (r.unchanged ? "true" : "false");
    if (!r.why.empty()) std::cout << ",\"why\":\"" << escaped(r.why) << "\"";
    std::cout << ",\"output\":\"" << escaped(r.output) << "\"";
    std::cout << ",\"splits\":[";
    for (std::size_t i = 0; i < r.splits.size(); i++) std::cout << (i ? "," : "") << r.splits[i];
    std::cout << "],\"offsets\":[";
    for (std::size_t i = 0; i < r.offsets.size(); i++) std::cout << (i ? "," : "") << r.offsets[i];
    std::cout << "]}\n";
}

std::string verdict_of(double margin, double sigma) {
    if (sigma <= 0.0) return "nothing";
    if (margin >= SOLID_MARGIN) return "solid";
    if (margin >= SOME_MARGIN) return "unsure";
    return "nothing";
}

int agree_count(const std::vector<Chunk>& chunks, double slope, double intercept) {
    int agreed = 0;
    for (const Chunk& c : chunks) {
        const double expected = slope * c.time_s + intercept;
        if (std::abs(c.offset_ms - expected) <= AGREE_MS) agreed++;
    }
    return agreed;
}

bool restore_backup(const std::string& path) {
    const std::string backup = path + ".bak";
    std::error_code ec;
    if (!fs::exists(backup, ec)) {
        std::cerr << "No backup to restore: " << backup << '\n';
        return false;
    }
    fs::copy_file(backup, path, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "Could not put " << backup << " back: " << ec.message() << '\n';
        return false;
    }
    fs::remove(backup, ec);
    say() << "Restored " << path << " from the backup\n";
    return true;
}

int run(int argc, char** argv) {
    Options o;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--json") { o.as_json = true; quiet = true; }
        else if (a == "--quiet") quiet = true;
        else if (a == "--dry-run") o.dry_run = true;
        else if (a == "--strict") o.strict = true;
        else if (a == "--force") o.force = true;
        else if (a == "--no-backup") o.backup = false;
        else if (a == "--version") { std::cout << kVersion << '\n'; return 0; }
        else if (a == "--formats") {
            std::cout << "srt vtt ass ssa sub (MicroDVD; needs --fps or a reference)\n";
            return 0;
        }
        else if (a == "--undo") {
            if (i + 1 >= argc) { usage(); return -1; }
            o.undo = argv[++i];
        }
        else if (a == "--output") {
            if (i + 1 >= argc) { usage(); return -1; }
            o.output = argv[++i];
        }
        else if (a == "--confidence") {
            double v = 0;
            if (i + 1 >= argc || !number(argv[++i], &v)) { usage(); return -1; }
            o.confidence = v;
        }
        else if (a == "--fps") {
            double v = 0;
            if (i + 1 >= argc || !number(argv[++i], &v) || v < 10 || v > 120) { usage(); return -1; }
            o.fps = v;
        }
        else if (a == "--min-shift") {
            double v = 0;
            if (i + 1 >= argc || !number(argv[++i], &v)) { usage(); return -1; }
            o.min_shift = static_cast<ms>(v);
        }
        else if (a == "--encoding") {
            Charset c;
            if (i + 1 >= argc || !charset_from_name(argv[++i], &c)) {
                std::cerr << "--encoding wants utf8, utf8-bom, utf16le, utf16be or latin1\n";
                return -1;
            }
            o.have_encoding = true;
            o.encoding = c;
        }
        else if (a == "--snap") {
            o.snap = true;
            if (i + 1 < argc) {
                double v = 0;
                if (number(argv[i + 1], &v)) { o.snap_window = static_cast<ms>(v); i++; }
            }
        }
        else if (!a.empty() && a[0] == '-') { usage(); return -1; }
        else positional.push_back(a);
    }

    if (!o.undo.empty()) return restore_backup(o.undo) ? 0 : -1;

    if (positional.size() < 2) { usage(); return -1; }
    o.source = positional[0];
    o.target = positional[1];
    if (positional.size() >= 3) o.mode = positional[2];
    if (positional.size() >= 4) {
        double v = 0;
        if (number(positional[3], &v)) o.penalty = v;
    }

    Report rep;

    // ---- target subtitle ----
    Charset target_charset = Charset::Utf8;
    read_text(o.target, &target_charset);
    // A frame-based target cannot even be read without a framerate, so resolve it
    // first: explicit --fps, else infer from the reference's runtime.
    if (subtitle_kind(o.target) == ".sub" && o.fps <= 0.0) {
        o.fps = fps_from_length(o.target, o.source);
        if (o.fps <= 0.0) {
            std::cerr << "MicroDVD (.sub) is frame-based and needs a framerate: pass --fps N\n";
            return -1;
        }
    }

    std::vector<Cue> target_cues = read_subtitle(o.target, o.fps);
    if (target_cues.empty()) {
        std::cerr << "Could not read any cues from " << o.target << '\n';
        return -1;
    }
    rep.cues = static_cast<int>(target_cues.size());

    const std::pair<std::vector<Span>, std::vector<int>> processed = process_spans(target_cues);
    const std::vector<Span>& cues = processed.first;
    rep.ignored = static_cast<int>(target_cues.size()) - static_cast<int>(cues.size());

    if (cues.size() < MIN_CUES) {
        rep.why = "too few usable cues (" + std::to_string(cues.size()) + ")";
        if (o.as_json) report_json(rep); else report_human(rep);
        return 0;
    }

    // ---- reference ----
    std::vector<Span> reference;
    std::vector<float> weights;
    double coverage = 1.0;
    const std::string src_kind = subtitle_kind(o.source);

    if (src_kind.empty()) {
        Audio audio;
        std::string err;
        if (!read_wav(o.source, audio, &err)) {
            std::cerr << err << '\n';
            return -1;
        }
        const std::vector<float> prob = speech_profile(audio, &coverage);
        if (prob.empty()) {
            std::cerr << "No audio frames decoded from " << o.source << '\n';
            return -1;
        }
        const auto rs = reference_spans(prob);
        reference = rs.first;
        weights = rs.second;
        rep.reference = "vad";
    } else {
        const std::vector<Cue> ref_cues = read_subtitle(o.source);
        if (ref_cues.empty()) {
            std::cerr << "Could not read any cues from " << o.source << '\n';
            return -1;
        }
        const auto rs = process_spans(ref_cues);
        reference = rs.first;
        weights.assign(reference.size(), 1.0f);
        rep.reference = "subtitle";
    }

    if (reference.size() < 4) {
        rep.why = "reference has too few spans";
        if (o.as_json) report_json(rep); else report_human(rep);
        return 0;
    }
    rep.coverage = coverage;

    // MicroDVD needs a framerate to be read/written meaningfully.
    if (subtitle_kind(o.target) == ".sub" && o.fps <= 0.0) {
        o.fps = fps_from_length(o.target, o.source);
        if (o.fps <= 0.0) {
            std::cerr << "MicroDVD (.sub) needs a framerate; pass --fps\n";
            return -1;
        }
    }

    // ---- correlate ----
    const OffsetResult base = best_offset(cues, reference, weights, coverage);

    ms base_offset = base.offset_ms;
    double sigma = base.sigma;
    double margin = base.margin;
    double confidence = base.confidence;

    // Joint framerate + offset fit. This is what explains a subtitle authored for
    // one framerate and played at another. Once a multiplier is chosen the
    // reference is rescaled once, which turns the whole problem back into a
    // constant offset plus a small residual drift.
    const DriftFit drift = fit_drift(cues, reference, weights, base_offset, sigma, margin);
    const double mult = drift.mult;

    std::vector<Span> ref_used;
    ref_used.reserve(reference.size());
    for (const Span& s : reference) {
        if (std::abs(mult - 1.0) < 1e-9) {
            ref_used.push_back(s);
        } else {
            ref_used.emplace_back(static_cast<ms>(std::llround(static_cast<double>(s.first) / mult)),
                                  static_cast<ms>(std::llround(static_cast<double>(s.second) / mult)));
        }
    }
    if (std::abs(mult - 1.0) > 1e-9) {
        base_offset = static_cast<ms>(std::llround(drift.add_ms));
        sigma = drift.sigma;
        margin = drift.margin;
    }

    align_setup(ref_used, weights);

    // Refine the offset onto the exact overlap optimum.
    base_offset = align_refine(cues, base_offset);
    {
        const double refined_sigma = onset_z(cues, base_offset);
        if (refined_sigma > sigma) sigma = refined_sigma;
    }

    const int want_chunks = std::max(2, static_cast<int>(cues.size() / 40));
    std::vector<Chunk> chunks = chunk_offsets(cues, ref_used, weights, want_chunks);

    // ---- mode ----
    std::vector<ms> new_starts(target_cues.size());
    std::vector<ms> new_ends(target_cues.size());
    for (std::size_t i = 0; i < target_cues.size(); i++) {
        new_starts[i] = target_cues[i].start;
        new_ends[i] = target_cues[i].end;
    }

    const bool force_split = (o.mode == "split");
    const bool force_nosplit = (o.mode == "nosplit");
    const bool force_ols = (o.mode == "ols");
    const bool auto_mode = o.mode.empty() || o.mode == "auto";

    const std::vector<int> jumps = backward_jumps(cues, ref_used, weights);
    const bool split_ok = !force_nosplit && !force_ols &&
                          (force_split || (auto_mode && worth_splitting(chunks, base_offset)));

    double slope = 0.0;
    double intercept = base_offset;
    double ratio = mult;
    int parts = 1;
    SplitResult split_res;
    std::vector<ms> per_cue(cues.size(), base_offset);

    if (split_ok && !chunks.empty()) {
        split_res = split_alignment(cues, ref_used, weights, o.penalty, base_offset);
        parts = static_cast<int>(split_res.offsets.size());
        if (parts > 1) {
            per_cue = split_res.per_cue_offset;
            rep.splits = split_res.splits;
            rep.offsets = split_res.offsets;

            // A split answer is judged by its *weakest* segment: every piece has to
            // be individually convincing, not merely the average.
            double worst_margin = 1e9;
            double worst_sigma = 1e9;
            for (std::size_t s = 0; s < split_res.margins.size(); s++) {
                worst_margin = std::min(worst_margin, split_res.margins[s]);
                worst_sigma = std::min(worst_sigma, split_res.sigmas[s]);
            }
            if (worst_margin < 1e8) margin = worst_margin;
            if (worst_sigma < 1e8) sigma = worst_sigma;
        } else {
            parts = 1;
        }
    }

    if (parts <= 1) {
        // `chunks` measure, per time chunk, the offset that maps a cue onto the
        // rescaled reference. Composing that with the framerate multiplier gives
        // the final transform: new = (t + off(t)) * mult.
        if (!force_nosplit && chunks.size() >= 2) {
            const std::pair<double, double> line = robust_line(chunks);
            const double total_ratio = mult * (1.0 + line.first / 1000.0);
            slope = (total_ratio - 1.0) * 1000.0;
            intercept = mult * line.second;
        } else {
            const double total_ratio = mult;
            slope = (total_ratio - 1.0) * 1000.0;
            intercept = mult * static_cast<double>(base_offset);
        }
        for (std::size_t i = 0; i < cues.size(); i++) per_cue[i] = 0;  // unused
        rep.offsets.assign(1, static_cast<ms>(std::llround(intercept)));
    }

    ratio = 1.0 + slope / 1000.0;

    // ---- verdict ----
    double agreeing = 0;
    if (parts <= 1 && !chunks.empty()) {
        agreeing = agree_count(chunks, slope, intercept);
    } else {
        agreeing = static_cast<double>(chunks.size());
    }
    rep.agreement = chunks.empty() ? 0.0 : agreeing / static_cast<double>(chunks.size());
    rep.sigma = sigma;
    rep.margin = margin;
    rep.confidence = confidence;
    rep.offset = static_cast<ms>(std::llround(parts > 1 ? base_offset : intercept));
    rep.ratio = ratio;
    rep.parts = parts;
    rep.mode = parts > 1 ? "split" : (force_nosplit ? "nosplit" : (o.mode.empty() ? "auto" : o.mode));
    if (rep.mode == "auto" && std::abs(ratio - 1.0) < 1e-9) rep.mode = "ols";

    rep.verdict = verdict_of(margin, sigma);
    // --confidence is a margin threshold: it is scale-free, so one number means the
    // same thing for a 40-cue file and a 1500-cue file.
    if (margin < o.confidence) rep.verdict = "unsure";
    if (sigma <= 0.0) rep.verdict = "nothing";

    // ---- map spans back onto the full cue list ----
    // `cues` are the surviving spans; processed.second maps span i -> cue index.
    std::vector<ms> move_start(target_cues.size(), 0), move_end(target_cues.size(), 0);
    std::vector<bool> touched(target_cues.size(), false);

    if (parts > 1) {
        for (std::size_t i = 0; i < cues.size(); i++) {
            const int cue_idx = processed.second[i];
            if (cue_idx < 0 || cue_idx >= static_cast<int>(target_cues.size())) continue;
            const ms off = per_cue[i];
            const Cue& c = target_cues[static_cast<std::size_t>(cue_idx)];
            // A split offset is measured against the rescaled reference, so it is
            // scaled by the same multiplier before being applied.
            move_start[static_cast<std::size_t>(cue_idx)] =
                static_cast<ms>(std::llround((static_cast<double>(c.start) + off) * mult));
            move_end[static_cast<std::size_t>(cue_idx)] =
                static_cast<ms>(std::llround((static_cast<double>(c.end) + off) * mult));
            touched[static_cast<std::size_t>(cue_idx)] = true;
        }
    } else {
        for (std::size_t i = 0; i < cues.size(); i++) {
            const int cue_idx = processed.second[i];
            if (cue_idx < 0 || cue_idx >= static_cast<int>(target_cues.size())) continue;
            const Cue& c = target_cues[static_cast<std::size_t>(cue_idx)];
            move_start[static_cast<std::size_t>(cue_idx)] = apply_line(c.start, slope, intercept);
            move_end[static_cast<std::size_t>(cue_idx)] = apply_line(c.end, slope, intercept);
            touched[static_cast<std::size_t>(cue_idx)] = true;
        }
    }
    // Untouched cues (junk/ignored) keep their original times.
    for (std::size_t i = 0; i < target_cues.size(); i++) {
        if (!touched[i]) {
            move_start[i] = target_cues[i].start;
            move_end[i] = target_cues[i].end;
        }
    }

    // ---- snap ----
    if (o.snap && !chunks.empty()) {
        ActivityIndex idx;
        idx.build(activity(reference));
        int snapped = 0;
        for (std::size_t i = 0; i < target_cues.size(); i++) {
            if (!touched[i]) continue;
            const ms len = move_end[i] - move_start[i];
            const ms before = move_start[i];
            ms best = move_start[i];
            double best_cov = idx.covered_fraction(Span{move_start[i], move_end[i]}, 0);
            for (ms d = -o.snap_window; d <= o.snap_window; d += 5) {
                const ms cand = move_start[i] + d;
                if (cand < 0) continue;
                if (i > 0 && cand < move_end[i - 1]) continue;
                if (cand + len <= cand) continue;
                const double cov = idx.covered_fraction(Span{cand, cand + len}, 0);
                if (cov > best_cov + 1e-9) { best_cov = cov; best = cand; }
            }
            if (best != before) { move_start[i] = best; move_end[i] = best + len; snapped++; }
        }
        rep.snapped = snapped;
    }

    // ---- clamp ----
    int moved = 0;
    for (std::size_t i = 0; i < target_cues.size(); i++) {
        ms s = move_start[i], e = move_end[i];
        if (s < 0) { e -= s; s = 0; }
        if (s > MAX_TIME_MS) s = MAX_TIME_MS;
        if (e > MAX_TIME_MS) e = MAX_TIME_MS;
        if (e <= s) e = s + 1;
        if (s != target_cues[i].start || e != target_cues[i].end) moved++;
        move_start[i] = s;
        move_end[i] = e;
    }

    // ---- min shift / unchanged ----
    ms biggest = 0;
    for (std::size_t i = 0; i < target_cues.size(); i++)
        biggest = std::max(biggest, abs_ms(move_start[i] - target_cues[i].start));

    if (moved == 0 || biggest < o.min_shift) {
        rep.unchanged = true;
        rep.written = false;
        rep.why = "already in sync";
        if (o.as_json) report_json(rep); else report_human(rep);
        align_drop();
        return 0;
    }

    // ---- write decision ----
    const bool confident = (rep.verdict == "solid") || o.force;
    if (o.dry_run) {
        rep.written = false;
        rep.why = "dry-run";
        if (o.as_json) report_json(rep); else report_human(rep);
        align_drop();
        return 0;
    }

    if (!confident) {
        if (o.strict) {
            rep.written = false;
            rep.why = "not confident enough (strict)";
        } else {
            const std::string side = beside(o.target, ".lapse-unsure");
            std::string err;
            const bool ok = write_subtitle(o.target, side, o.encoding, !o.have_encoding,
                                           move_start, move_end, o.fps, &err);
            rep.written = ok;
            rep.output = ok ? side : std::string();
            rep.why = ok ? "not confident enough; wrote a sidecar" : err;
        }
        if (o.as_json) report_json(rep); else report_human(rep);
        align_drop();
        return 0;
    }

    const std::string dest = o.output.empty() ? o.target : o.output;
    if (o.backup && dest == o.target) {
        std::error_code ec;
        fs::copy_file(o.target, o.target + ".bak", fs::copy_options::overwrite_existing, ec);
    }

    std::string err;
    const bool ok = write_subtitle(o.target, dest, o.encoding, !o.have_encoding,
                                   move_start, move_end, o.fps, &err);
    rep.written = ok;
    rep.output = ok ? dest : std::string();
    if (!ok) rep.why = err;

    if (o.as_json) report_json(rep); else report_human(rep);
    align_drop();
    return ok ? 0 : -1;
}

} // namespace
} // namespace subsync

int main(int argc, char** argv) {
    try {
        return subsync::run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return -1;
    }
}
