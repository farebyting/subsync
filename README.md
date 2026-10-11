# subsync

A self-contained subtitle synchronization engine in C++17. It aligns a subtitle's
timing to a reference, recovering a constant offset, a framerate mismatch, or a
re-cut (split), and reports how confident it is before touching your file.

No external libraries: the FFT, the VAD, the WAV reader, the charset handling and
every subtitle parser are implemented in this repository. The build produces a
single statically linked executable.

## Modes

| Mode | Source | Use |
|---|---|---|
| Audio | a `.wav` file | align a subtitle to the speech in the audio |
| Sub-to-sub | another subtitle | align one subtitle to a known-good one |

The mode is chosen from the source file's extension.

## Usage

```
subsync <source> <target.srt> [auto|ols|nosplit|split] [penalty]
        [--output <path>] [--no-backup] [--dry-run] [--json] [--quiet]
        [--strict] [--force] [--confidence N] [--fps N] [--snap [ms]]
        [--encoding NAME] [--min-shift ms]
subsync --version | --formats | --undo <subtitle>
```

```sh
# Align a subtitle to a reference subtitle (auto-detects offset / drift / split)
subsync reference.srt out-of-sync.srt

# Align to the audio
subsync episode.wav out-of-sync.srt

# Look before you leap: report only, write nothing
subsync reference.srt out-of-sync.srt --dry-run --json

# Write the fixed subtitle somewhere else
subsync reference.srt out-of-sync.srt --output fixed.srt

# Undo a previous run (restores the .bak)
subsync --undo out-of-sync.srt
```

### Options

| Option | Meaning |
|---|---|
| `--dry-run` | compute and report, write nothing |
| `--json` | machine-readable result on stdout (implies `--quiet`) |
| `--quiet` | suppress the human-readable line |
| `--output PATH` | write the result to PATH instead of overwriting the target |
| `--no-backup` | do not create `<target>.bak` before overwriting |
| `--force` | overwrite even when the result is not confident |
| `--strict` | never write a sidecar; leave the file alone unless confident |
| `--confidence N` | minimum margin (default `0.15`) required before overwriting |
| `--snap [ms]` | pull cue starts onto the nearest reference onset (default 120 ms) |
| `--fps N` | framerate for frame-based `.sub` files |
| `--encoding NAME` | output encoding: `utf8`, `utf8-bom`, `utf16le`, `utf16be`, `latin1` |
| `--min-shift ms` | treat the file as already in sync if nothing moves by this much (default 1) |
| `auto` | (default) pick the best of offset / drift / split |
| `ols` | force a single linear drift model |
| `nosplit` | force a single constant offset |
| `split` | force piecewise offsets; `penalty` (default 1.0) tunes break sensitivity |

### Safety

* The original file is never overwritten unless the result is **solid** (or `--force`).
* A backup (`<target>.bak`) is written before any overwrite; `--undo` restores it.
* An unsure result is written to `<name>.lapse-unsure<ext>` and the original is left alone.
* `--dry-run` writes nothing at all.

### JSON output

```json
{"mode":"ols","reference":"subtitle","offset_ms":-2500,"ratio":1,"confidence":1,
 "margin":0.581,"sigma":115.5,"agreement":1,"verdict":"solid","coverage":1,"cues":200,
 "ignored_cues":0,"parts":1,"snapped":0,"written":false,"unchanged":false,"why":"dry-run",
 "output":"","splits":[],"offsets":[-2500]}
```

`verdict` is `solid`, `unsure` or `nothing`. `offset_ms` is the shift to apply
(negative means the subtitle is late). `ratio` is the framerate correction
(`1.0417` = a 25/24 mismatch). `parts` > 1 means a split was found, with the
break points in `splits` and the per-segment offsets in `offsets`.

## Supported formats

| Format | Read | Write | Notes |
|---|---|---|---|
| SubRip `.srt` | yes | yes | CRLF/LF preserved |
| WebVTT `.vtt` | yes | yes | cue settings and `MM:SS` form preserved |
| ASS/SSA `.ass`, `.ssa` | yes | yes | any `Format:` field order; other sections untouched |
| MicroDVD `.sub` | yes | yes | frame-based; needs `--fps` or a reference |

Unchanged times round-trip byte-for-byte; only the time fields are rewritten.
Encodings are detected (BOM, UTF-8 validity, Latin-1 fallback) and preserved.

## Building

Requires a C++17 compiler and CMake 3.20+.

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The binary is `build/subsync.exe` (Windows) or `build/subsync`, and is statically
linked, so it runs without the toolchain on `PATH`.

### Tests

```sh
python tests/make_fixtures.py    # generate fixtures (stdlib only)
ctest --test-dir build --output-on-failure
```

`test_fft`, `test_charset`, `test_formats`, `test_correlate`, `test_audio` are
self-contained unit suites. `test_e2e` drives the real binary over generated
fixtures (constant offset, framerate drift, split, already-in-sync, audio mode)
and skips cleanly if the fixtures have not been generated.

## How it works

1. **Cues → spans.** The target subtitle is parsed, junk cues (sound-effect
   brackets, music notes, credit lines) are dropped, and overlapping cues merged.
2. **Reference.** Audio is decoded from WAV, an energy/zero-crossing VAD produces
   a per-frame speech probability, and that is turned into weighted speech spans.
   For sub-to-sub the reference is simply the other subtitle's cue spans.
3. **Correlation.** An exact sweep-line overlap search scores every candidate
   offset in one pass (overlap is piecewise-linear in the offset, so four events
   per cue pair capture it exactly). It yields the best offset plus a confidence,
   a margin over the best rival, and a MAD-based sigma. A separate two-channel
   (block + onset) FFT cross-correlator provides a fast approximate offset that
   is then refined onto the exact optimum.
4. **Framerate and drift.** The framerate grid is searched jointly with the
   offset; the chosen ratio rescales the reference once, turning the problem back
   into a constant offset plus a small residual. Per-chunk offsets are fitted with
   a weighted least-squares line for linear drift.
5. **Splits.** When per-chunk offsets disagree, a dynamic program finds
   piecewise-constant segments, merging adjacent ones whose offsets agree.
6. **Verdict and write.** The margin (peak vs best rival) drives the verdict; it
   is scale-free, so one threshold works for a 40-cue short and a 1500-cue film.

### Tuning constants

All thresholds live in `src/constants.h`. They are the values used by the
`Schwponaco-org/lapse` engine where the two overlap (offset/search windows,
framerate ratios, split windows), plus the peak-significance constants
(`SOLID_MARGIN`, `SOME_MARGIN`, `SIGMA_EXCLUDE_BUCKETS`) which were chosen by
measuring the sweep's bucket distribution on clean, partial and unrelated inputs.

## Limitations

* **Audio input is PCM WAV only.** There is no external decoder, so mp3/aac/FLAC
  are not read. Convert to WAV first.
* **The VAD is energy-based, not a neural network.** It finds speech regions well
  enough to correlate against, but it is less precise than a model-based VAD on
  very quiet or heavily-scored material. Falling back to sub-to-sub mode against a
  known-good subtitle is the robust workaround.
* **Latin-1 output transliterates** characters outside Latin-1 to `?`. Use
  `--encoding utf8` to avoid this.
* Formats beyond the four above are not implemented; the parse/write seam in
  `src/subtitle.h` is where new ones would attach.
