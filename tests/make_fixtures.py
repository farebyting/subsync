#!/usr/bin/env python3
"""Generate end-to-end fixtures for subsync.

Writes into tests/fx/:
  ref.srt          a plausible 200-cue subtitle over ~25 minutes
  t_plus2500.srt   ref shifted +2500 ms  (subsync must recover -2500)
  t_drift.srt      ref rescaled by 25/24 (framerate mismatch; ratio ~1.0417)
  t_split.srt      first half +1500 ms, second half -4500 ms
  t_same.srt       an exact copy of ref (must report "already in sync")
  speech.wav       16 kHz mono speech-like bursts matching ref's cue positions
  audio_off.srt    ref shifted -1200 ms (audio mode must recover +1200)

Only the standard library is used.
"""
import math
import os
import random
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
FX = os.path.join(HERE, "fx")

RATE = 16000


def fmt(t_ms):
    if t_ms < 0:
        t_ms = 0
    h, rem = divmod(int(t_ms), 3600000)
    m, rem = divmod(rem, 60000)
    s, ms = divmod(rem, 1000)
    return f"{h:02d}:{m:02d}:{s:02d},{ms:03d}"


def make_cues(count=200, seed=7):
    """Cue spans in ms: 1-4 s long, 0.5-6 s apart, spread over ~25 minutes."""
    rng = random.Random(seed)
    cues = []
    t = 2000
    for _ in range(count):
        length = rng.randint(1000, 4000)
        cues.append((t, t + length))
        t += length + rng.randint(500, 6000)
    return cues


def write_srt(path, cues):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for i, (a, b) in enumerate(cues, 1):
            f.write(f"{i}\n{fmt(a)} --> {fmt(b)}\nLine {i} of dialogue.\n\n")


def shift(cues, d):
    return [(max(0, a + d), max(0, b + d)) for a, b in cues]


def rescale(cues, r):
    return [(int(round(a * r)), int(round(b * r))) for a, b in cues]


def write_speech_wav(path, cues, total_ms=None, seed=3):
    """Speech-like bursts (amplitude-modulated tone+noise) exactly over the cues."""
    rng = random.Random(seed)
    if total_ms is None:
        total_ms = (cues[-1][1] + 2000) if cues else 5000
    n = int(RATE * total_ms / 1000)
    buf = [0.0] * n

    for (a, b) in cues:
        s0 = int(RATE * a / 1000)
        s1 = min(n, int(RATE * b / 1000))
        if s1 <= s0:
            continue
        # syllable-like amplitude modulation around 4 Hz
        for i in range(s0, s1):
            rel = (i - s0) / RATE
            env = 0.5 + 0.5 * math.sin(2 * math.pi * 4.0 * rel)
            # keep the burst above the detector's floor for most of its length
            env = 0.35 + 0.65 * env
            f0 = 120.0 + 40.0 * math.sin(2 * math.pi * 1.7 * rel)
            tone = math.sin(2 * math.pi * f0 * rel) + 0.4 * math.sin(2 * math.pi * 2 * f0 * rel)
            noise = rng.uniform(-0.35, 0.35)
            buf[i] = 0.35 * env * (0.7 * tone + noise)

    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        frames = bytearray()
        for v in buf:
            v = max(-1.0, min(1.0, v))
            frames += struct.pack("<h", int(v * 32000))
        w.writeframes(bytes(frames))


def main():
    os.makedirs(FX, exist_ok=True)
    ref = make_cues()

    write_srt(os.path.join(FX, "ref.srt"), ref)
    write_srt(os.path.join(FX, "t_plus2500.srt"), shift(ref, 2500))
    # A subtitle authored at a slightly different framerate: it runs 24/25 too fast,
    # so the correction subsync must find is 25/24 (~1.0417).
    write_srt(os.path.join(FX, "t_drift.srt"), rescale(ref, 24.0 / 25.0))

    half = len(ref) // 2
    split = [(a + 1500, b + 1500) for a, b in ref[:half]] + \
            [(a - 4500, b - 4500) for a, b in ref[half:]]
    write_srt(os.path.join(FX, "t_split.srt"), split)

    write_srt(os.path.join(FX, "t_same.srt"), ref)
    write_srt(os.path.join(FX, "audio_off.srt"), shift(ref, -1200))

    write_speech_wav(os.path.join(FX, "speech.wav"), ref)

    print(f"fixtures written to {FX}")
    print(f"  ref cues: {len(ref)}, runtime {ref[-1][1] / 1000:.0f}s")


if __name__ == "__main__":
    main()
