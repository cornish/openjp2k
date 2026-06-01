# AVX2-valid decode re-localization — design

**Date:** 2026-06-01
**Type:** Measurement deliverable (no optimization, no fork source changes)
**Status:** Design — pending implementation plan
**Feeds:** selection of the next Sub-project / deliverable on the decode-perf roadmap
(`docs/superpowers/specs/2026-05-16-decode-perf-design.md`)

## Motivation

Every stage-profile that localized the current roadmap targets was measured through
`jp2k-bench`, whose decoder libraries are built as CMake `ExternalProject` subbuilds.
Those subbuilds were silently compiling **without AVX2** until openjp2k-bench `ddbba57`
(the subbuilds did not inherit the parent cache's `-march=native`). So the profiles that
motivated targets like the `opj_tcd_decode_tile` orchestration gap (SP-2.1a) ran against a
decoder with **every AVX2-gated path compiled out** — DWT, the D8 DC level-shift, SP-3.2
int16 5/3, all scalar in the measured binary.

Now that `-march=native` actually reaches both decoder libs (verified vanilla=1329 /
fork=1899 `%ymm`), the vectorized paths are real and the **relative** stage contributions
have almost certainly shifted: the families D8/SP-3.2/SP-3.5 sped up shrink, and whatever
did not vectorize (entropy, orchestration, allocation) grows as a fraction. Picking the
next deliverable off the old map risks another architecture-first miss — the failure mode
behind D3 (reverted), SP3.2b (reverted), and SP-3.5 (negative). D8, the one big recent win,
came from profile-first discipline. This deliverable re-establishes that map on a valid build.

## Goal

A fresh, function-level attribution of **single-thread** decode time in openjp2k on the
AVX2-valid build, stratified across the synthetic controlled sweep plus a real-world slice,
cross-referenced against grok's coarse decode delta — culminating in a **decision tree** that
names the next perf deliverable from current data.

This deliverable produces a **findings document and a chosen target**, not an optimization.

## Instrument

Two layers, because no single tool gives both the codec-vs-adapter split and intra-decode
attribution.

### Coarse — `jp2k-bench --profile-stages`
Run for **openjp2k and grok**, single-thread, on every stratum. Yields the four sub-times
`setup / decode / unpack / teardown`. Purpose:
1. Confirm `decode` dominates and adapter overhead (`setup/unpack/teardown`) is small, so the
   fine-grained `perf` pass is attributing the right bucket.
2. Provide the openjp2k-vs-grok **`decode`** delta per stratum. Per
   `openjp2k-bench/docs/methodology.md`, only `decode/setup/teardown` are codec-comparable;
   `unpack` is adapter-internal and is **not** cited as a codec delta.

### Fine — `perf record --call-graph dwarf`
On the fork's `libopenjp2.so`, **openjp2k decoder only**, `--threads 1`. Sampling frequency
`-F 999`. DWARF call-graph (not frame-pointer) so codegen is identical to the shipping build.
Post-process `perf report` self-time, bucketing symbols into families:

| family | representative symbols |
|---|---|
| MQ / T1 entropy | `opj_t1_decode_cblk*`, `opj_mqc_*`, `opj_t1_dec_*` |
| DWT 5/3 | `opj_dwt_decode*` (reversible / int paths) |
| DWT 9/7 | `opj_dwt_decode_real*`, `opj_v8dwt_*` |
| MCT | `opj_mct_decode*`, `opj_mct_decode_real*` |
| DC level-shift | `opj_tcd_dc_level_shift_decode*` and the D8 AVX2 kernel |
| dequant | `opj_dwt_calc_explicit_stepsizes` / dequant in `opj_dwt_decode_*` |
| tcd orchestration | `opj_tcd_decode_tile`, `opj_tcd_*` glue, `opj_t1_decode_cblks` driver |
| alloc / memcpy | `malloc`/`free`/`memcpy`/`memset`, `opj_*_create`/`_destroy` |

Bucketing is by symbol regex applied to family groups, not by trusting individual line
attribution (perf line attribution is unreliable under `-O2/-O3` inlining; family-level
grouping is robust to it).

## Build

A dedicated profiling build, configured separately from the canonical bench build so it is
not confused with a shippable measurement:

- Canonical `-march=native` flags (the `ddbba57` fix), **plus `-g`** for DWARF debug info.
- **No** `-fno-omit-frame-pointer` (DWARF call-graph removes the need; keeping frame-pointer
  omission means codegen is byte-identical to the shipping build).
- Verify AVX2 is present before profiling: `objdump -d libopenjp2.so.7 | grep -c '%ymm' > 0`,
  abort otherwise (same guard the canonical bench script uses).

No openjp2k source changes. `-g` adds DWARF sections only; it does not alter optimization.

## Strata

**Synthetic (controlled):** representative files spanning the axes that distinguish decode
work — one `perf` run per file for clean per-stratum attribution:

- bit-depth ∈ {8, 12, 16}
- compression ∈ {lossy (9/7), lossless (5/3)}
- decomposition depth ∈ {d1, d5, d8}

Target ~6–9 files (not the full cross product — pick the cells that most change the family
mix: 8-bit-lossy-d1, 8-bit-lossy-d8, 12-bit-lossy-d5, 16-bit-lossless-d5, 16-bit-lossy-d8,
8-bit-lossless-d1, plus 1–2 fills). The exact file list is fixed in the implementation plan
from `corpus/synthetic/` and recorded in the findings doc.

**Real-world (deployment reality-check):** three files from `corpus/public/`:
- a WSI tile — 8-bit RGB lossy
- a DICOM 16-bit monochrome (lossless if available; else closest)
- a photographic 8-bit

These confirm the synthetic attribution generalizes; divergence between synthetic and
real-world family mixes is itself a finding.

## Output — the deliverable

A findings document at `openjp2k-bench/docs/findings/2026-06-01-avx2-valid-relocalization.md`
(matching that repo's findings convention), containing:

1. **Per-stratum family self-time table** (% of decode time per family, from `perf`).
2. **Coarse stage table** — openjp2k vs grok `setup/decode/unpack/teardown` per stratum, with
   the codec-comparable `decode` delta highlighted.
3. **Decision tree → next deliverable.** Mapping of the dominant finding to a roadmap target:
   - tcd-orchestration is a large self-time share and/or aligns with grok's `decode` lead →
     Sub-project 2/5 orchestration work (the SP-2.1a ~7pp gap), single-thread-relevant.
   - MCT is material → Sub-project 4 / parking-lot P2 (MCT vectorization), now data-motivated.
   - alloc/memcpy is material → Sub-project 5 / parking-lot P1 (TCD buffers, allocation churn).
   - DWT-9/7 still material despite the SP-3.x work → DWT polish (lower prior — SP-3.x is
     largely exhausted).
   - MQ/T1 dominant **and** already at grok parity (per D1 / SP-2.1a) → diminishing single-
     thread returns; the lever is MT scheduling, not the inner loop — recommend pivoting the
     next deliverable to multi-thread scaling (Sub-project 2 scheduling).
4. **Recommended next target**, with the data that justifies it, as the input to the next
   sub-project's own spec.

## Success criteria

- AVX2 confirmed present in the profiled binary (`%ymm` count > 0).
- `perf` sample density sufficient that the top-3 families per stratum are stable across two
  runs of the same file (re-run one stratum to confirm; if unstable, raise iters / `-F`).
- Every stratum has both a coarse stage row and a fine family breakdown.
- The findings doc names exactly one recommended next deliverable with cited numbers.

## Out of scope

- Any optimization or fork source change (this is measurement only; Approach A was chosen
  specifically to avoid touching `t1.c`/`tcd.c`).
- Multi-thread profiling. Single-thread is the focus (it matches the concurrency deployment).
  "MT scheduling is the real lever" is a permitted **conclusion** of this pass, not work it does.
- Grok-internal function attribution — grok is the black-box competitor; only its coarse
  `--profile-stages` decode time is used, never its internals.
- In-codec RDTSC stage timers (Approach B) — deferred into whichever deliverable is chosen;
  it will want precise per-phase gating placed exactly where it matters.

## Risks

- **Inlining defeats attribution.** Mitigated by DWARF call-graph + family-level bucketing and
  `perf report --inline`. If a family still can't be separated (e.g. dequant fused into a DWT
  loop), report it as a combined family rather than guessing a split.
- **`perf` permissions.** May need `perf_event_paranoid` lowered or `CAP_PERFMON`; the plan
  records the exact requirement and a `--profile-stages`-only fallback if `perf` is unavailable
  (coarse-only still distinguishes decode-bound from adapter-bound and gives the grok delta,
  just without intra-decode families).
- **Single representative file per stratum is noisy.** Mitigated by the stability re-run in
  success criteria; widen to 2–3 files per stratum if the top families are unstable.
