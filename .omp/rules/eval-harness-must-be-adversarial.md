---
name: eval-harness-must-be-adversarial
description: "Tool-level evaluation harnesses must randomize inputs per run and assert real limits, not fix a seed so the tool is expected to pass"
condition: "random\\.Random\\(\\s*\\d+|random\\.seed\\(\\s*\\d+|seed\\s*=\\s*\\d+"
question: "Is this a harness that evaluates the tool's real-world accuracy, where a fixed seed, curated scenario list, or hand-picked tolerance makes the outcome reproducible-by-construction and expected to pass — rather than randomizing inputs across runs and asserting the tool's genuine limits?"
scope: ["tool:write(*.py)", "tool:edit(*.py)"]
---

A tool-level evaluation exists to find failures, not to confirm success.

**Randomize the inputs, not just the labels.**
- Draw the seed per run (`random.Random()` with no argument, or a seed printed so the run can be replayed). A hard-coded seed makes every run identical, so the numbers never move and the harness can never surprise you.
- Sweep many runs: 100+ pairs, each with independently drawn offsets, ratios, cut counts, cue counts, gap distributions, drop/merge/split rates. Report the *distribution* of outcomes, not one aggregate that hides the tail.
- Include adversarial cases on purpose: offsets near the ±15 min search limit, ratios at every entry of the framerate grid, 3–4 cuts, long silent stretches, heavy score, an unrelated subtitle as a negative control.

**Never choose the tolerance to make the tool pass.**
- Fix the pass threshold *before* seeing results, from what a viewer tolerates (±100–250 ms), and report pass/fail against it. Widening it afterwards is the bug, not the fix.
- A 100% pass rate on a self-authored harness is a signal the harness is too easy. Expected shape is a spread with a visible failure tail.
- A wrong `solid` verdict is a hard fail regardless of timing error — that is the failure mode the verdict exists to prevent.

**Report honestly.**
- If the tool fails cases, say which and why; do not average them away or describe a partial recovery as success.
- Fixed seeds are fine only for unit tests asserting exact numeric output of a pure function. They are wrong for anything measuring the tool's accuracy on real inputs.