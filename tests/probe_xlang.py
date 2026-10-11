#!/usr/bin/env python3
"""
Cross-language sync probe: does timing-based sub-to-sub sync survive the
differences between a real English subtitle and a real Indonesian one?

The point: subsync's sub-to-sub mode correlates *cue timing spans*, not text.
So the language should not matter. What CAN matter is that a translator
re-segments the dialogue: merges two English cues into one Indonesian cue,
splits one into two, nudges boundaries, drops lines, adds translator junk.

This script builds realistic EN/ID pairs with exactly those differences,
injects a known offset, and measures whether sync recovers it.
"""
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "build", "subsync.exe")

# ---------------------------------------------------------------- text pools
# Realistic English film dialogue (short, subtitle-shaped).
EN_LINES = [
    "What are you doing here?", "I told you not to come.", "We need to talk.",
    "It's not what you think.", "Get out of the car.", "He's been waiting for you.",
    "You don't understand.", "This changes everything.", "Where were you last night?",
    "I can't do this anymore.", "Listen to me very carefully.",
    "The police are outside.", "Take the money and run.", "Nobody has to know.",
    "That's not my name.", "You lied to me.", "I'm not going back there.",
    "It's over.", "Give me the file.", "We're running out of time.",
    "Do you trust me?", "I did what I had to do.", "She's not coming.",
    "They know everything.", "Keep your voice down.", "Look at me.",
    "This is the last time.", "I never wanted any of this.",
    "You should have told me sooner.", "It doesn't matter now.",
    "Who sent you?", "I'm sorry. I really am.", "Don't move.",
    "There's something you should see.", "We have to go. Now.",
    "How long have you known?", "It was never about the money.",
    "Promise me you'll stay.", "I made a mistake.", "You're safe now.",
]

# Realistic Indonesian subtitle lines. Indonesian runs ~10-20% longer than
# English, which is exactly why translators re-segment.
ID_LINES = [
    "Apa yang kamu lakukan di sini?", "Sudah kubilang jangan datang.",
    "Kita perlu bicara.", "Bukan seperti yang kamu pikirkan.",
    "Keluar dari mobil.", "Dia sudah menunggumu.",
    "Kamu tidak mengerti.", "Ini mengubah segalanya.",
    "Kamu di mana tadi malam?", "Aku tidak bisa terus begini.",
    "Dengarkan aku baik-baik.", "Polisi ada di luar.",
    "Ambil uangnya dan pergi.", "Tidak ada yang perlu tahu.",
    "Itu bukan namaku.", "Kamu membohongiku.",
    "Aku tidak akan kembali ke sana.", "Semuanya sudah berakhir.",
    "Serahkan berkasnya.", "Waktu kita hampir habis.",
    "Kamu percaya padaku?", "Aku melakukan apa yang harus kulakukan.",
    "Dia tidak akan datang.", "Mereka tahu semuanya.",
    "Pelan-pelan bicaranya.", "Lihat aku.",
    "Ini yang terakhir kali.", "Aku tidak pernah menginginkan ini.",
    "Seharusnya kamu bilang dari tadi.", "Tidak penting lagi sekarang.",
    "Siapa yang mengirimmu?", "Maaf. Aku benar-benar minta maaf.",
    "Jangan bergerak.", "Ada sesuatu yang harus kamu lihat.",
    "Kita harus pergi. Sekarang.", "Sejak kapan kamu tahu?",
    "Ini tidak pernah soal uang.", "Berjanjilah kamu akan tinggal.",
    "Aku membuat kesalahan.", "Kamu aman sekarang.",
]

# Indonesian junk / translator-noise cues, as they really appear.
ID_JUNK = [
    "[musik]", "[tertawa]", "[berbisik]", "(tertawa)",
    "[suara pintu]", "Diterjemahkan oleh: IndoSub Team",
    "Subtitle Indonesia oleh www.IndoSub.com", "\u266a\u266a\u266a",
    "Kunjungi situs kami untuk subtitle lainnya",
]


def srt_time(ms):
    h, ms = divmod(int(ms), 3600000)
    m, s = divmod(ms, 60000)
    s, ms = divmod(s, 1000)
    return "%02d:%02d:%02d,%03d" % (h, m, s, ms)


def write_srt(path, cues, eol="\n"):
    """cues: list of (start_ms, end_ms, text)"""
    parts = []
    for i, (a, b, t) in enumerate(cues, 1):
        parts.append("%d%s%s --> %s%s%s%s" % (i, eol, srt_time(a), srt_time(b), eol, t, eol))
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write((eol + eol).join(parts) + eol + eol)


def read_srt(path):
    import re
    raw = open(path, "rb").read().decode("utf-8-sig", "replace")
    cues = []
    for m in re.finditer(
        r"(\d\d):(\d\d):(\d\d),(\d\d\d)\s*-->\s*(\d\d):(\d\d):(\d\d),(\d\d\d)", raw
    ):
        g = [int(x) for x in m.groups()]
        a = ((g[0] * 3600 + g[1] * 60 + g[2]) * 1000) + g[3]
        b = ((g[4] * 3600 + g[5] * 60 + g[6]) * 1000) + g[7]
        cues.append((a, b))
    return cues


def make_english_film(rng, n_cues):
    """Film-like dialogue: bursts of speech separated by quiet gaps."""
    cues = []
    t = rng.randint(15000, 45000)  # pre-title quiet
    for i in range(n_cues):
        dur = rng.choice([1200, 1500, 1800, 2100, 2500, 3000, 3500])
        cues.append((t, t + dur, EN_LINES[i % len(EN_LINES)]))
        # mostly short inter-cue gaps, occasionally a long quiet stretch
        if rng.random() < 0.08:
            t += dur + rng.randint(6000, 25000)
        else:
            t += dur + rng.randint(200, 1400)
    return cues


def translate_to_indonesian(rng, en_cues, style):
    """Build the Indonesian subtitle the way a real translator would.

    style controls how aggressively lines get re-segmented:
      'faithful' - same segmentation, text translated only
      'realistic'- some merges/splits, boundaries nudged
      'sloppy'   - heavy re-segmentation, dropped lines, junk cues
    """
    id_cues = []
    for i, (a, b, _) in enumerate(en_cues):
        text = ID_LINES[i % len(ID_LINES)]
        if style == "faithful":
            id_cues.append((a, b, text))
            continue

        # Translator nudges the cue boundary (subjective timing).
        na = a + rng.randint(-250, 250)
        nb = b + rng.randint(-250, 250)
        if nb <= na + 400:
            nb = na + 400

        if style == "sloppy" and rng.random() < 0.10:
            continue  # translator dropped this line entirely

        id_cues.append((na, nb, text))

    if style == "faithful":
        return id_cues

    # Merge: two adjacent cues become one (translator combined short lines).
    merged = []
    i = 0
    merge_rate = 0.18 if style == "realistic" else 0.30
    while i < len(id_cues):
        if i + 1 < len(id_cues) and rng.random() < merge_rate:
            a, _, t1 = id_cues[i]
            _, b, t2 = id_cues[i + 1]
            merged.append((a, b, t1 + " " + t2))
            i += 2
        else:
            merged.append(id_cues[i])
            i += 1

    # Split: one cue becomes two (Indonesian is longer, so it wraps out).
    split = []
    split_rate = 0.12 if style == "realistic" else 0.22
    for a, b, t in merged:
        if rng.random() < split_rate and (b - a) > 2000:
            mid = a + (b - a) // 2
            split.append((a, mid, t))
            split.append((mid, b, t))
        else:
            split.append((a, b, t))

    # Translator junk cues.
    junk_rate = 0.02 if style == "realistic" else 0.05
    for a, b, t in list(split):
        if rng.random() < junk_rate:
            split.append((a, b, rng.choice(ID_JUNK)))
    split.sort(key=lambda c: c[0])
    return split


def apply_shift(cues, offset_ms):
    return [(max(0, a + offset_ms), max(0, b + offset_ms), t) for a, b, t in cues]


def apply_ratio(cues, ratio, anchor=0):
    return [
        (int((a - anchor) * ratio) + anchor, int((b - anchor) * ratio) + anchor, t)
        for a, b, t in cues
    ]


def run_sync(src, tgt, extra=None):
    cmd = [EXE, src, tgt, "--dry-run", "--json"] + (extra or [])
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        return None, p.stderr.strip()
    import json
    return json.loads(p.stdout), None


def cue_error(reference, target):
    """Median/max |start difference| between paired cues, index-aligned.

    Real subtitles do not have a 1:1 cue mapping after re-segmentation, so we
    compare the *envelope*: for each target cue start, distance to the nearest
    reference cue start. That is what a viewer actually perceives as in-sync.
    """
    ref_starts = [c[0] for c in reference]
    if not ref_starts:
        return None, None
    errs = []
    for a, _, _ in target:
        errs.append(min(abs(a - r) for r in ref_starts))
    errs.sort()
    return errs[len(errs) // 2], errs[-1]


def main():
    rng = random.Random(20261011)
    outdir = os.path.join(HERE, "xlang")
    os.makedirs(outdir, exist_ok=True)

    if not os.path.exists(EXE):
        print("build/subsync.exe not found; build first")
        return 1

    scenarios = []
    for style in ("faithful", "realistic", "sloppy"):
        for offset in (2500, -1800, 12000, -45000):
            scenarios.append((style, offset, 1.0, None))
    # cross-language drift (ID made at 25fps, EN at 23.976)
    for style in ("faithful", "realistic", "sloppy"):
        scenarios.append((style, 0, 25.0 / 23.976, None))
        scenarios.append((style, 3000, 24.0 / 25.0, None))

    print("=" * 100)
    print("CROSS-LANGUAGE SYNC PROBE: English reference -> Indonesian target")
    print("=" * 100)
    print("%-10s %8s %7s | %10s %8s %8s | %-8s %-6s %s"
          % ("style", "offset", "ratio", "got_off", "got_ratio", "err_med", "verdict", "parts", "note"))
    print("-" * 100)

    n_pass = n_total = 0
    for style, offset, ratio, _ in scenarios:
        en = make_english_film(rng, 220)
        idc = translate_to_indonesian(rng, en, style)
        if ratio != 1.0:
            idc = apply_ratio(idc, ratio)
        idc = apply_shift(idc, offset)

        ref_path = os.path.join(outdir, "ref_en.srt")
        tgt_path = os.path.join(outdir, "target_id.srt")
        write_srt(ref_path, en)
        write_srt(tgt_path, idc)

        res, err = run_sync(ref_path, tgt_path)
        if res is None:
            print("%-10s %8d %7.4f | FAILED: %s" % (style, offset, ratio, err))
            n_total += 1
            continue

        # How well would the *output* line up with the reference?
        # Simulate the applied transform and measure the envelope error.
        got_off = res["offset_ms"]
        got_ratio = res["ratio"]
        applied = [
            (int((a) * got_ratio) + got_off, int((b) * got_ratio) + got_off, t)
            for a, b, t in idc
        ]
        med, mx = cue_error(en, applied)

        # The truth: after the ideal inverse transform, error should be ~0.
        ideal = [
            (int(a / ratio) if ratio != 1.0 else a, int(b / ratio) if ratio != 1.0 else b, t)
            for a, b, t in idc
        ]
        ideal = apply_shift(ideal, -offset)
        imed, imx = cue_error(en, ideal)

        ok = med is not None and med <= 250
        n_total += 1
        n_pass += 1 if ok else 0
        print("%-10s %8d %7.4f | %10d %8.4f %8s | %-8s %-6d %s"
              % (style, offset, ratio, got_off, got_ratio,
                 ("%d" % med) if med is not None else "-",
                 res["verdict"], res["parts"],
                 "OK" if ok else ("ideal-err=%s" % imed)))

    print("-" * 100)
    print("recovered within 250ms envelope: %d/%d" % (n_pass, n_total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
