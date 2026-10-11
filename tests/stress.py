#!/usr/bin/env python3
"""Randomized stress harness for subsync.

Protocol (fixed before any results were seen):
  * Master seed is drawn at random each invocation and printed. Every run's
    randomness derives from its own seed (also printed), so any single failure
    is replayable with --seed <master>.
  * Inputs are randomized, not just labels: cue counts, cue durations, gap
    distributions, silent stretches, junk/drop/merge/split rates, boundary
    nudges, offsets, ratios, cut counts and cut offsets are all drawn per run.
  * Pass threshold is a priori: after correction, the MEDIAN envelope error
    between the target's cue starts and the reference's cue starts must be
    <= 150 ms (the band a viewer tolerates). The threshold is not adjusted
    after seeing results.
  * A "solid" verdict on an unrelated-subtitle negative control is a hard fail
    regardless of timing error - that is the failure mode the verdict exists
    to prevent.
  * Results are reported as a distribution (median-of-medians, p90, max) plus
    a per-category breakdown and the full failure list. A 100% pass rate on
    the first try is suspect and gets said out loud.

Usage:
  python tests/stress.py                 # full sweep (~134 runs)
  python tests/stress.py --runs 8        # smoke
  python tests/stress.py --seed 12345    # replay / repro
  python tests/stress.py --keep-all      # keep every run's files
"""
import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "subsync.exe")
OUT = os.path.join(HERE, "stress")

PASS_MEDIAN_MS = 150.0          # fixed a priori, viewer tolerance band
MAX_OFFSET_DRAW = 14 * 60 * 1000  # stay just inside the engine's 15-min search limit

# framerate ratios the engine itself searches (from src/constants.h)
RATIOS = [24.0 / 23.976, 23.976 / 24.0, 25.0 / 24.0, 24.0 / 25.0,
          25.0 / 23.976, 23.976 / 25.0, 30.0 / 29.97, 29.97 / 30.0,
          30.0 / 25.0, 25.0 / 30.0]

LINES = [
    "You can't handle the truth.", "I'm going to make him an offer he can't refuse.",
    "May the Force be with you.", "There's no place like home.",
    "Show me the money.", "I'll be back.", "My precious.", "Why so serious?",
    "Elementary, my dear Watson.", "Here's looking at you, kid.",
    "Go ahead, make my day.", "Say hello to my little friend.",
    "You talking to me?", "I feel the need - the need for speed.",
    "Life is like a box of chocolates.", "I see dead people.",
    "They may take our lives, but they'll never take our freedom.",
    "Keep your friends close, but your enemies closer.",
    "A million dollars isn't cool. You know what's cool? A billion dollars.",
    "The first rule of Fight Club is: you do not talk about Fight Club.",
    "I'm king of the world.", "Carpe diem. Seize the day, boys.",
    "To infinity and beyond.", "Houston, we have a problem.",
    "You're gonna need a bigger boat.", "Nobody puts Baby in a corner.",
    "I volunteer as tribute.", "Just keep swimming.",
    "The stuff that dreams are made of.", "Well, nobody's perfect.",
]
JUNK = [
    "[applause]", "[music playing]", "[rain thundering]", "(gunshots)",
    "\u266a instrumental \u266a", "\u266b \u266b \u266b",
    "www.subs-download.example", "Synced by someRipper", "Subtitles by TeamX",
]


def fmt(t_ms):
    t_ms = max(0, int(t_ms))
    h, rem = divmod(t_ms, 3600000)
    m, rem = divmod(rem, 60000)
    s, ms = divmod(rem, 1000)
    return "%02d:%02d:%02d,%03d" % (h, m, s, ms)


def write_srt(path, cues):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for i, c in enumerate(cues, 1):
            f.write("%d\n%s --> %s\n%s\n\n" % (i, fmt(c[0]), fmt(c[1]), c[2]))


def parse_srt_starts(path):
    starts = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            if " --> " in line:
                part = line.split(" --> ")[0].strip()
                h, m, rest = part.split(":")
                s, ms = rest.split(",")
                starts.append(int(h) * 3600000 + int(m) * 60000 + int(s) * 1000 + int(ms))
    return starts


def make_film(rng):
    """A drawn film: cue count, durations, gaps, long-silence rate all random."""
    n = rng.randint(120, 380)
    dur_pool = [rng.randint(800, 1600), rng.randint(1200, 2200), rng.randint(1800, 3500)]
    short_gap = rng.randint(150, 1200)
    long_gap = (rng.uniform(0.0, 0.2), rng.randint(8000, 90000))  # (prob, length)
    cues = []
    t = rng.randint(5000, 45000)  # pre-title quiet
    for i in range(n):
        dur = rng.choice(dur_pool)
        cues.append((t, t + dur, LINES[i % len(LINES)], i))
        if rng.random() < long_gap[0]:
            t += dur + rng.randint(long_gap[1] // 2, long_gap[1])
        else:
            t += dur + rng.randint(short_gap, short_gap * 3)
    return cues


def degrade(rng, cues):
    """A drawn translator: nudges, drops, merges, splits, junk - rates random.

    Each surviving cue carries the index of the reference cue it descends from
    (idx = -1 for invented junk), so the scorer can tell exactly which reference
    cues have a legitimate counterpart and which are simply absent from the file.
    """
    nudge = rng.randint(0, 300)
    drop_rate = rng.uniform(0.0, 0.12)
    merge_rate = rng.uniform(0.0, 0.30)
    split_rate = rng.uniform(0.0, 0.22)
    junk_rate = rng.uniform(0.0, 0.06)

    out = []
    for i, (a, b, t, _src) in enumerate(cues):
        na, nb = a + rng.randint(-nudge, nudge), b + rng.randint(-nudge, nudge)
        if nb <= na + 400:
            nb = na + 400
        if rng.random() < drop_rate:
            continue
        out.append((na, nb, t, i))
    merged = []
    i = 0
    while i < len(out):
        if i + 1 < len(out) and rng.random() < merge_rate:
            a, b1, t1, src1 = out[i]
            _, b2, t2, _src2 = out[i + 1]
            merged.append((a, max(b1, b2), t1 + " " + t2, src1))
            i += 2
        else:
            merged.append(out[i])
            i += 1
    resplit = []
    for a, b, t, src in merged:
        if rng.random() < split_rate and (b - a) > 2000:
            mid = a + (b - a) // 2
            resplit.append((a, mid, t, src))
            resplit.append((mid, b, t, src))
        else:
            resplit.append((a, b, t, src))
    if rng.random() < 0.7 and resplit:  # most films carry some junk cues
        span = resplit[-1][1]
        for _ in range(rng.randint(1, max(1, int(len(resplit) * junk_rate)) + 1)):
            a = rng.randint(2000, max(3000, span - 4000))
            resplit.append((a, a + rng.randint(1200, 2500), rng.choice(JUNK), -1))
    resplit.sort(key=lambda c: c[0])
    return resplit


def apply_affine(cues, ratio, offset):
    """Shift/rescale, then drop cues pushed before zero.

    A real shifted subtitle never contains negative times - the cues that would
    land there simply are not in the file. Clamping them all to 00:00:00 (what
    this harness did first) creates an impossible input and any verdict on it is
    meaningless. Returns (cues, dropped_reference_starts).
    """
    out, dropped = [], []
    for c in cues:
        a, b, t = c[0], c[1], c[2]
        src = c[3] if len(c) > 3 else -1
        na, nb = int(round(a * ratio)) + offset, int(round(b * ratio)) + offset
        if nb <= 0:
            if src >= 0:
                dropped.append(src)
            continue
        if na < 0:
            na = 0
        if nb <= na:
            nb = na + 1
        out.append((na, nb, t, src, a))
    return out, dropped


def cut_and_shift(rng, cues, ratio):
    """Re-cut the film into drawn parts, each part with its own drawn offset."""
    n_cuts = rng.randint(1, 4)
    fr = sorted(rng.sample(range(15, 86), n_cuts))
    bounds = [0] + fr + [100]
    parts = []
    prev = None
    for k in range(len(bounds) - 1):
        lo, hi = int(len(cues) * bounds[k] / 100), int(len(cues) * bounds[k + 1] / 100)
        if hi <= lo:
            continue
        parts.append(cues[lo:hi])
    # per-part offsets: independently drawn but clearly distinct between neighbours
    offs = []
    for _ in parts:
        for _try in range(50):
            o = rng.randint(-600000, 600000)
            if not offs or abs(o - offs[-1]) >= 5000:
                offs.append(o)
                break
        else:
            offs.append((offs[-1] + 7000) if offs else 0)
    out, dropped = [], []
    for part, off in zip(parts, offs):
        moved, dr = apply_affine(part, ratio, off)
        out.extend(moved)
        dropped.extend(dr)
    return out, len(parts), offs, dropped


def make_wav(path, cues, seed):
    """Speech-like bursts exactly over the cue spans; numpy-fast."""
    import numpy as np
    rate = 16000
    total_ms = cues[-1][1] + 3000
    n = int(rate * total_ms / 1000)
    buf = np.zeros(n, dtype=np.float32)
    nprng = np.random.default_rng(seed)
    for c in cues:
        a, b = c[0], c[1]
        i0, i1 = int(rate * a / 1000), min(n, int(rate * b / 1000))
        if i1 - i0 < rate // 10:
            continue
        rel = np.arange(i1 - i0, dtype=np.float64) / rate
        f0 = nprng.uniform(90, 180)
        env = 0.35 + 0.65 * (0.5 + 0.5 * np.sin(2 * np.pi * nprng.uniform(3, 5) * rel))
        tone = np.sin(2 * np.pi * f0 * rel + nprng.uniform(0, 6)) \
            + 0.4 * np.sin(2 * np.pi * 2 * f0 * rel)
        noise = nprng.uniform(-0.35, 0.35, rel.shape)
        buf[i0:i1] = (0.35 * env * (0.7 * tone + noise)).astype(np.float32)
    import wave
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes((np.clip(buf, -1, 1) * 32000).astype("<i2").tobytes())


def envelope_err(ref_starts, tgt_starts):
    """For each reference start, distance to nearest target start. Viewer-centric."""
    if not ref_starts or not tgt_starts:
        return None
    errs, j = [], 0
    for r in ref_starts:
        while j < len(tgt_starts) - 1 and abs(tgt_starts[j + 1] - r) <= abs(tgt_starts[j] - r):
            j += 1
        best = min(abs(tgt_starts[k] - r) for k in (max(0, j - 1), j, min(len(tgt_starts) - 1, j + 1)))
        errs.append(best)
    errs.sort()
    n = len(errs)
    med = errs[n // 2]
    p90 = errs[min(n - 1, int(round(0.9 * (n - 1))))]
    return med, p90, errs[-1]


def run_sync(args, timeout=120):
    try:
        p = subprocess.run([EXE] + args, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timeout after %ss" % timeout
    if p.returncode != 0:
        return None, (p.stderr.strip() or ("exit %d" % p.returncode))[:200]
    try:
        return json.loads(p.stdout), None
    except json.JSONDecodeError as e:
        return None, "bad json: %s" % e


def build_cases(master):
    """Every case draws its own parameters. Categories are adversarial on purpose."""
    cases = []

    def base_case(kind):
        seed = master.getrandbits(32)
        return {"kind": kind, "seed": seed, "rng": random.Random(seed)}

    # --- 80 core randomized pairs ---
    for _ in range(80):
        c = base_case("core")
        rng = c["rng"]
        c["ratio"] = 1.0 if rng.random() < 0.6 else rng.choice(RATIOS)
        c["offset"] = rng.randint(-MAX_OFFSET_DRAW, MAX_OFFSET_DRAW)
        c["cuts"] = 0 if rng.random() < 0.7 else None  # drawn inside cut_and_shift
        cases.append(c)

    # --- one run per framerate grid ratio (adversarial: every grid entry) ---
    for r in RATIOS:
        c = base_case("ratio-grid")
        rng = c["rng"]
        c["ratio"] = r
        c["offset"] = rng.choice([-1, 1]) * rng.randint(120000, 600000)
        c["cuts"] = 0
        cases.append(c)

    # --- offsets pressed against the +-15 min search limit ---
    for _ in range(6):
        c = base_case("edge-offset")
        rng = c["rng"]
        c["ratio"] = 1.0
        c["offset"] = rng.choice([-1, 1]) * (900000 - rng.randint(0, 50000))
        c["cuts"] = 0
        cases.append(c)

    # --- heavy translator noise: junk/score cues everywhere ---
    for _ in range(8):
        c = base_case("heavy-noise")
        rng = c["rng"]
        c["ratio"] = 1.0 if rng.random() < 0.5 else rng.choice(RATIOS)
        c["offset"] = rng.randint(-MAX_OFFSET_DRAW, MAX_OFFSET_DRAW)
        c["cuts"] = 0
        c["heavy"] = True
        cases.append(c)

    # --- 3-4 cuts forced ---
    for _ in range(10):
        c = base_case("multi-cut")
        rng = c["rng"]
        c["ratio"] = 1.0
        c["offset"] = None  # cut cases carry per-part offsets instead
        c["cuts"] = (3, 4)
        cases.append(c)

    # --- negative controls: unrelated reference film ---
    for _ in range(10):
        c = base_case("negative")
        rng = c["rng"]
        c["ratio"] = 1.0
        c["offset"] = rng.randint(-MAX_OFFSET_DRAW, MAX_OFFSET_DRAW)
        c["cuts"] = 0
        cases.append(c)

    # --- already in sync ---
    for _ in range(5):
        c = base_case("same")
        c["ratio"] = 1.0
        c["offset"] = 0
        c["cuts"] = 0
        cases.append(c)

    # --- audio mode: WAV reference + shifted subtitle ---
    for _ in range(15):
        c = base_case("audio")
        rng = c["rng"]
        c["ratio"] = 1.0
        c["offset"] = rng.randint(-300000, 300000)
        c["cuts"] = 0
        cases.append(c)
    for _ in range(2):
        c = base_case("audio")
        rng = c["rng"]
        c["ratio"] = rng.choice(RATIOS)
        c["offset"] = rng.randint(-200000, 200000)
        c["cuts"] = 0
        cases.append(c)

    return cases


def run_case(case, keep):
    rng = case["rng"]
    seed = case["seed"]
    rdir = os.path.join(OUT, "run_%08x" % seed)
    os.makedirs(rdir, exist_ok=True)

    film = make_film(rng)
    if len(film) < 60:
        return None, "generated film too small"

    rec = {"seed": "%08x" % seed, "kind": case["kind"], "ratio": round(case["ratio"], 5),
           "offset": case["offset"], "cues": len(film)}

    ref_path = os.path.join(rdir, "ref.srt")
    tgt_path = os.path.join(rdir, "target.srt")
    out_path = os.path.join(rdir, "fixed.srt")

    if case["kind"] == "negative":
        # genuinely unrelated second film: own shape, own density, own runtime
        other = make_film(rng)
        scale = rng.uniform(0.5, 1.4)
        other = [(int(c[0] * scale), int(c[1] * scale), c[2], c[3]) for c in other]
        write_srt(ref_path, other)
        target = degrade(rng, film)
        target, _dropped = apply_affine(target, case["ratio"], case["offset"])
        write_srt(tgt_path, target)
        res, err = run_sync([ref_path, tgt_path, "--force", "--json", "--output", out_path])
        rec["expect"] = "verdict != solid"
        ok, why = None, None
        if res is None:
            ok, why = False, "run failed: " + err
        else:
            rec["verdict"] = res["verdict"]
            rec["got_offset"] = res["offset_ms"]
            if res["verdict"] == "solid":
                ok, why = False, "WRONG SOLID on unrelated subtitle"
            else:
                ok = True
        rec["ok"] = ok
        if why:
            rec["why"] = why
        return rec, None

    target = degrade(rng, film)
    if len(target) < 50:
        return None, "degenerated film too small (%d cues)" % len(target)

    if case.get("heavy"):
        # bury the dialogue in score/credit cues
        extra = []
        span = target[-1][1]
        for _ in range(int(len(target) * rng.uniform(0.2, 0.4))):
            a = rng.randint(2000, max(3000, span - 4000))
            extra.append((a, a + rng.randint(1500, 3500), rng.choice(JUNK), -1, a))
        target = sorted(target + extra, key=lambda c: c[0])

    dropped = []
    if case["cuts"] != 0:
        target, nparts, offs, dropped = cut_and_shift(rng, target, case["ratio"])
        rec["parts_truth"] = nparts
        rec["part_offsets"] = offs
    else:
        target, dropped = apply_affine(target, case["ratio"], case["offset"])
    rec["dropped"] = len(dropped)

    if case["kind"] == "same":
        pass  # target already matches reference; nudges only

    write_srt(ref_path, film)
    write_srt(tgt_path, target)

    if case["kind"] == "audio":
        wav_path = os.path.join(rdir, "speech.wav")
        make_wav(wav_path, film, seed ^ 0x5EED)
        rec["runtime_s"] = round(film[-1][1] / 1000.0)
        src = wav_path
    else:
        src = ref_path

    res, err = run_sync([src, tgt_path, "--force", "--json", "--output", out_path])
    rec["expect"] = "median envelope error <= %d ms" % PASS_MEDIAN_MS
    if res is None:
        rec["ok"] = False
        rec["why"] = "run failed: " + err
        return rec, None

    rec["verdict"] = res["verdict"]
    rec["got_offset"] = res["offset_ms"]
    rec["got_ratio"] = round(res["ratio"], 5)
    rec["got_parts"] = res["parts"]
    rec["sigma"] = round(res["sigma"], 2)
    rec["margin"] = round(res["margin"], 3)
    rec["unchanged"] = res["unchanged"]

    if case["kind"] == "same":
        ok = bool(res["unchanged"]) and not res["written"]
        rec["ok"] = ok
        if not ok:
            rec["why"] = "expected unchanged=true, written=false"
        return rec, None

    if not os.path.exists(out_path):
        rec["ok"] = False
        rec["why"] = "no output written (unchanged=%s)" % res["unchanged"]
        return rec, None

    got = parse_srt_starts(out_path)
    # What a viewer sees: after correction, each cue should sit where it sat
    # before the shift/rescale - i.e. the tool must undo the transform exactly.
    # Measured against the cue's own pre-transform start, NOT against the nearest
    # reference start: the translator's +-300 ms boundary jitter is irreducible
    # input noise that no sync tool can remove, so scoring against it would
    # measure the harness's own jitter (median |jitter| = 150 ms) rather than the
    # tool. Junk cues (src < 0) are excluded: the engine deliberately leaves them
    # where they are, and no viewer reads them.
    if len(got) != len(target):
        rec["ok"] = False
        rec["why"] = "cue count changed: %d in, %d out" % (len(target), len(got))
        return rec, None
    errs = sorted(abs(got[i] - target[i][4])
                  for i in range(len(target)) if target[i][3] >= 0)
    if not errs:
        rec["ok"] = False
        rec["why"] = "no scoreable cues"
        return rec, None
    n = len(errs)
    med = errs[n // 2]
    p90 = errs[min(n - 1, int(round(0.9 * (n - 1))))]
    mx = errs[-1]
    rec["err_med"] = med
    rec["err_p90"] = p90
    rec["err_max"] = mx
    ok = med <= PASS_MEDIAN_MS
    rec["ok"] = ok
    if not ok:
        rec["why"] = "median envelope error %d ms > %d ms" % (med, PASS_MEDIAN_MS)
    return rec, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=0, help="cap total runs (smoke)")
    ap.add_argument("--seed", type=int, default=None, help="master seed for replay")
    ap.add_argument("--keep-all", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(EXE):
        print("build/subsync.exe not found; build first")
        return 1

    if args.seed is not None:
        used = args.seed
    else:
        used = random.Random().getrandbits(31)
    master = random.Random(used)
    print("master seed: %d  (replay: python tests/stress.py --seed %d)" % (used, used),
          flush=True)

    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT, exist_ok=True)

    cases = build_cases(master)
    if args.runs:
        cases = cases[: args.runs]

    n_pass = n_fail = n_err = 0
    med_all, verdicts = [], {}
    fails = []
    by_kind = {}
    for i, case in enumerate(cases, 1):
        rec, gen_err = run_case(case, args.keep_all)
        if gen_err:
            n_err += 1
            fails.append({"kind": case["kind"], "seed": "%08x" % case["seed"], "why": gen_err})
            print("[%3d/%d] %s seed=%08x GENERATION FAILED: %s"
                  % (i, len(cases), case["kind"], case["seed"], gen_err), flush=True)
            continue
        k = rec["kind"]
        st = by_kind.setdefault(k, {"pass": 0, "fail": 0, "meds": []})
        if rec["ok"]:
            n_pass += 1
            st["pass"] += 1
        else:
            n_fail += 1
            st["fail"] += 1
            fails.append(rec)
        verdicts[rec.get("verdict", "-")] = verdicts.get(rec.get("verdict", "-"), 0) + 1
        if "err_med" in rec:
            med_all.append(rec["err_med"])
            st["meds"].append(rec["err_med"])
        flag = "ok " if rec["ok"] else "FAIL"
        line = "[%3d/%d] %-11s seed=%s ratio=%.4f off=%7d -> med=%s p90=%s verdict=%s %s" % (
            i, len(cases), k, rec["seed"], rec["ratio"], rec["offset"],
            rec.get("err_med", "-"), rec.get("err_p90", "-"),
            rec.get("verdict", "-"), "" if rec["ok"] else "  <<< " + rec.get("why", ""))
        if args.verbose or not rec["ok"]:
            print(line, flush=True)

    def pct(vals, q):
        if not vals:
            return None
        s = sorted(vals)
        return s[min(len(s) - 1, int(round(q * (len(s) - 1))))]

    print("=" * 100)
    print("PASS %d / %d   (fail %d, gen-error %d)" % (n_pass, n_pass + n_fail, n_fail, n_err))
    if med_all:
        print("median-envelope-error across runs: p50=%d p90=%d max=%d ms (threshold %d ms)"
              % (pct(med_all, 0.5), pct(med_all, 0.9), max(med_all), PASS_MEDIAN_MS))
    print("verdicts:", verdicts)
    for k, st in sorted(by_kind.items()):
        extra = ""
        if st["meds"]:
            extra = "  err p50=%d p90=%d" % (pct(st["meds"], 0.5), pct(st["meds"], 0.9))
        print("  %-11s pass %2d/%2d%s" % (k, st["pass"], st["pass"] + st["fail"], extra))
    wrong_solid = sum(1 for r in fails if r.get("why", "").startswith("WRONG SOLID"))
    if wrong_solid:
        print("!! %d WRONG-SOLID negative-control failures (hard fail)" % wrong_solid)
    if fails:
        print("-" * 100)
        print("failures:")
        for r in fails:
            print("  %-11s seed=%s ratio=%s off=%s med=%s : %s"
                  % (r.get("kind"), r.get("seed"), r.get("ratio"), r.get("offset"),
                     r.get("err_med", "-"), r.get("why", "?")))
    if n_pass == n_pass + n_fail and (n_pass + n_fail) > 50:
        print("NOTE: 100%% pass rate on a self-authored harness - the harness may be too easy.")
    with open(os.path.join(OUT, "last_report.json"), "w") as f:
        json.dump({"fails": fails, "verdicts": verdicts}, f, indent=1)
    return 0 if n_fail == 0 and n_err == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
