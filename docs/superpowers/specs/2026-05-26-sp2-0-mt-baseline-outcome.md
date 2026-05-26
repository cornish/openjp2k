# SP-2.0 — Multi-Thread Measurement Baseline (Outcome)

## Goal

Profile current openjp2k multi-thread decode behavior across 1, 2, 4, 8
threads, comparing to openjpeg 2.5.3 and grok 20.3.3 on the same
host (8 cores). Establish where the existing thread pool's barriers
hurt most, providing data to scope SP-2.1 (per-tile DAG scheduling).

This is **SP-2.0** — a tactical investigation, not a code deliverable.
Output is bench data + this writeup, no source changes.

## Method

Two iter bench runs on `2026-05-26`:

1. **`sp2_0_mt_scaling_20260526_075557.jsonl`** — single bench invocation with `--threads 1,2,4,8` across `corpus/synthetic-iter.txt + corpus/public/` (308-341 files). **Caveat: grok's adapter (per `adapter_grok.cpp` header) initializes its singleton thread pool ONCE at first thread count. So this run measures openjp2k and openjpeg correctly at all four thread counts, but grok is stuck at t=1 for the t=2/4/8 measurements.**

2. **`sp2_0_t8_fresh_20260526_095534.jsonl`** — separate bench invocation with only `--threads 8`, providing a fresh grok thread-pool init at t=8 for an apples-to-apples comparison.

Paired within-run ratios (openjpeg/openjp2k, grok/openjp2k) per file, then geomean per slice.

## Findings

### 1. openjp2k tracks openjpeg's scaling, slightly worse

| Decoder | t=1 ms (gm) | t=2 | t=4 | t=8 ms (gm) | t=8 speedup |
|---|---:|---:|---:|---:|---:|
| openjpeg | 15.28 | 1.44× | 1.83× | 7.97 | **1.92×** |
| openjp2k | 16.33 | 1.47× | 1.89× | 8.78 | **1.86×** |
| grok | 17.13 | (stuck) | (stuck) | 6.95 (fresh) | **2.47×** |

openjp2k and openjpeg use the same threading model (codeblock-level pool with stage barriers) so their scaling curves are nearly identical. Grok uses a Taskflow-based shared pool (per its adapter comments) and scales noticeably better — 2.47× at t=8 vs our 1.86×.

### 2. The multi-thread competitive position is a clear regression vs single-thread

| Comparison | t=1 | t=8 |
|---|---:|---:|
| openjp2k vs openjpeg (overall) | −1.73% | **−6.14%** |
| openjp2k vs grok (overall) | +4.92% (we win) | **−20.82%** |

At t=1 we're at openjpeg parity and slightly ahead of grok. At t=8 we lose to both — openjpeg by 6%, grok by 21%. The −21% gap to grok is the largest competitive deficit we've measured anywhere in this project.

### 3. Per-slice scaling reveals where SP-2.1 should target

openjp2k's own speedup curve, by slice:

| Slice | n | t=1 ms (gm) | t=2 sp | t=4 sp | t=8 sp |
|---|---:|---:|---:|---:|---:|
| **8-bit lossless** | 19 | 221.32 | 1.76× | 2.52× | **3.22×** |
| **12-bit lossless** | 19 | 119.33 | 1.78× | 2.74× | **3.67×** |
| **16-bit lossless** | 19 | 85.95 | 1.76× | 2.78× | **3.36×** |
| Synthetic (overall) | 90 | 62.32 | 1.65× | 2.35× | 2.90× |
| 16-bit lossy | 11 | 18.91 | 1.59× | 2.18× | 2.69× |
| 12-bit lossy | 11 | 11.94 | 1.53× | 2.02× | 2.37× |
| Medical | 40 | 16.62 | 1.44× | 1.83× | 2.04× |
| Conformance | 61 | 30.66 | 1.33× | 1.62× | **1.75×** |
| Archival | 2 | 1802.03 | 1.37× | 1.57× | **1.75×** |
| 8-bit lossy | 11 | 22.44 | 1.32× | 1.52× | **1.65×** |

Best scaling: 8/12/16-bit lossless synthetic at 3.2-3.7× (40-46% efficient on 8 cores). Worst: 8-bit lossy and heterogeneous conformance/archival buckets at 1.65-1.75× (~21% efficient).

The pattern: lossless synthetic = many codeblocks per tile, easy parallelism. Lossy + heterogeneous = fewer codeblocks per tile, or tiny tiles where the existing pool's per-stage barrier overhead dominates the actual decode work.

### 4. Per-slice openjp2k vs grok at t=8 — where Grok's DAG architecture wins

| Slice | n | openjp2k vs openjpeg | openjp2k vs grok |
|---|---:|---:|---:|
| **8-bit lossy** | 11 | +2.48% | **−43.95%** |
| **Medical (DICOM J2KR mostly)** | 7-40 | +7.08% | **−32.32%** |
| **12-bit lossy** | 11 | +4.10% | −24.66% |
| Synthetic overall | 90 | −5.47% | −14.07% |
| Conformance | 61 | −3.81% | −13.49% |
| Archival | 2 | +1.20% | −12.36% |
| 8-bit lossless | 19 | −10.85% | −8.75% |
| 16-bit lossy | 11 | +2.51% | −6.65% |
| 12-bit lossless | 19 | −11.03% | −5.99% |
| 16-bit lossless | 19 | −8.30% | −2.56% |

Grok's biggest leads (8-bit lossy, medical, 12-bit lossy) are slices that scale poorly in openjp2k's existing pool. **These are the slices SP-2.1 (DAG-based scheduling) should target most aggressively.** Lossless synthetic — where we already scale well — has the smallest grok lead.

Interesting: openjp2k beats openjpeg on lossy slices at t=8 (+2-4%) but loses on lossless (−8-11%). The lossy 9/7 path (SP3.1's AVX2 work) and the SP3.2 / D7 / D7.1 lossless T1 work appear to interact differently with the threading model — worth a deeper look but not blocking SP-2.1.

## Implications for SP-2.1

1. **DAG-based per-tile scheduling is the right architectural call.** The data corroborates the spec's framing: openjp2k and openjpeg's barrier-synchronized pool both scale ~1.9× at t=8; grok's Taskflow-based pool scales 2.47×. The gap is in scheduling architecture, not per-codeblock work.

2. **Target the slices that scale worst.** SP-2.1's bench gate should weight 8-bit lossy, 12-bit lossy, conformance, and medical buckets — where openjp2k currently scales 1.5-2.0× and grok still wins by 25-44%. Synthetic lossless (already 3.2-3.7× at t=8) is a check, not a target.

3. **Headroom ceiling is finite but real.** Amdahl's law: openjpeg at 1.92× and grok at 2.47× both fall well short of the 8× theoretical limit, suggesting strong serial portions (T2 parsing, mqc state, output write). SP-2.1's realistic upper bound is ~2.5-3× (matching grok). Beating grok requires either (a) the DAG approach reducing serial portions further, or (b) cross-tile pipelining (SP-2.2) extracting parallelism the per-tile DAG can't.

4. **Bench infrastructure has a usability gap.** The `--threads N1,N2,...` sweep silently produces wrong grok numbers because grok inits its pool once per process. Worth a follow-up to `run_bench.sh` that either (a) re-launches the bench binary per thread count, or (b) documents the limitation more prominently. Currently the documentation lives in `adapter_grok.cpp` only — easy to miss.

5. **Methodology — same-process sweeps misleading for cross-decoder MT comparisons.** Future SP-2.x bench gates should run separate bench invocations per thread count to avoid this trap. The cost (3-4 separate bench runs) is acceptable.

## Carry-over for SP-2.1 spec

- **Bench gate slices (weighted by current weakness):** 8-bit lossy, 12-bit lossy, medical, conformance buckets at t=8. Target: close the 25-44% grok gap on these slices.
- **Reference architecture:** Grok's Taskflow-based DAG (per cleanroom Grok research at `docs/grok-cleanroom-research/`). Spec/audit identifiers and constants — see memory note `cleanroom-grok-research.md`.
- **Threading library decision:** Taskflow (single-header MIT C++17 per the original SP-2 framing) vs hand-rolled with `std::thread` / `std::condition_variable`. Worth a brainstorm question at SP-2.1 design time.
- **Cross-tile pipelining (SP-2.2) is a separate deliverable.** SP-2.1 = per-tile DAG only; SP-2.2 = parser back-pressure + per-tile-row completion tracking on top.

## Next step

Brainstorm + spec for SP-2.1 — per-tile DAG scheduling. Architecture-level
deliverable, multi-week. Should reuse the bench data from this run rather
than re-measuring single-thread (data is fresh as of 2026-05-26).
