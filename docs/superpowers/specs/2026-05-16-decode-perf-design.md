# Decode performance roadmap + Sub-project 1 (T1 single-thread tightening)

**Date:** 2026-05-16
**Status:** Design / pre-implementation
**Scope:** Decode-path performance for the openjp2k fork. Target framing is
**general-purpose JPEG 2000 decode** — permissively-licensed, Kakadu-class
throughput across the production domains: radiology / DICOM (8/12/16-bit
monochrome and color), geospatial (GeoJP2 / NITF), general photographic
imaging, and whole-slide imaging. WSI prompted this fork and remains a
first-class workload, but optimizations are evaluated against the full
cross-domain corpus, not WSI alone.

---

## 1. Roadmap framing

The fork's decode hot paths, in priority order. Each item below becomes its own
spec → plan → implementation cycle. Only Sub-project 1 is detailed in this
document; the others are scoped here only enough to justify the ordering.

### Sub-project 0 — Measurement scaffolding *(prerequisite)*

Runs in the sister repo `openjp2k-bench`. Produces:

- A versioned, reproducible **cross-domain corpus**: photographic, DICOM
  medical (8/12/16-bit, monochrome and color), geospatial (GeoJP2 / NITF
  including large single-image streams), and WSI tiles (256² to 4096²);
  lossless 5-3 and lossy 9-7; partial-region / ROI decode requests across
  all four domains.
- Methodology doc: pinned compiler flags, warm-up policy, cycles + wallclock,
  per-stage breakdown.
- Vanilla openjpeg + Grok baselines captured. Kakadu published numbers pasted
  from publications (no adapter).
- CI regression gate: a PR fails if any corpus item regresses >2% wallclock.

**Gate:** nothing in Sub-projects 1–5 lands without passing through this harness.

### Sub-project 0.5 — Clean-room Grok techniques report

A *separate AI instance* (not the one implementing) reads Grok source and
produces a conceptual report: algorithms, data-structure shapes, engineering
choices (LUT sizes, threading granularity, inner-loop organization).

Hard constraints on the report:

- No code snippets, no function signatures, no identifier names, no close
  paraphrases.
- Audited for code-shaped content before being handed to the implementing
  instance.
- Treated as one input alongside public-knowledge sources (Taubman's textbook,
  HTJ2K standard, Kakadu manual descriptions, conference papers).

This preserves clean-room separation: ideas are not copyrightable; specific
expression is. The reader instance is firewalled from the implementer instance.

### Sub-project 1 — T1 single-thread tightening *(detailed in §2)*

MQ decoder + bitplane coder. Largest CPU share. Single-threaded so wins are
cleanly measurable per codeblock.

### Sub-project 2 — Codeblock-level threading

Decode independent codeblocks in parallel. Touches `tcd.c` orchestration and
the t1 entry. Multiplies Sub-project 1's per-codeblock wins by core count.
Largest practical wallclock win for any image with many codeblocks per
component per resolution — which covers essentially every production workload
(WSI tiles, geospatial scenes, large radiology series, multi-megapixel
photographic images).

### Sub-project 3 — Inverse DWT polish

Upstream's recent NEON IDWT work is a starting point. Add AVX2/AVX-512 paths
for the 9-7 and 5-3 IDWTs. Add partial-region short-circuiting: skip DWT
samples outside the requested region. Region-of-interest decode matters
broadly — geospatial viewers (panning over a tiled GeoJP2), DICOM viewers
(zooming into a region of a radiograph), and WSI viewers all decode partial
regions, but vanilla openjpeg does not optimize for it.

### Sub-project 4 — MCT vectorization

Inverse color transform. Memory-bound; gains from SIMD + cache-friendly access.
Small file but disproportionately easy to win.

### Sub-project 5 — TCD buffer + allocation churn

Reuse tile-component buffers across tiles in a session; eliminate per-tile
mallocs. Less CPU-bound but matters for any session-style consumer:
interactive viewers (WSI, geospatial, DICOM) that decode many tiles in
sequence, and batch pipelines that process many images per process.

### Out of scope

- **HTJ2K (Part 15) entropy coding.** The overwhelming majority of production
  streams across all four target domains are Part-1. HT adoption is ramping
  fastest in WSI but is still a minority. Revisit when the bench corpus shows
  meaningful HT volume in any domain.

### Cross-cutting constraints

Every sub-project must keep all four SIMD targets (AVX2, AVX-512, NEON, SVE2)
buildable. SVE2 may fall back to NEON for a specific optimization if SVE2
work is impractical for that deliverable.

---

## 2. Sub-project 1 — T1 single-thread tightening (detailed)

### 2.1 Scope

Optimize the per-codeblock decode path in `src/lib/openjp2/t1.c`:

- MQ arithmetic decoder
- Bitplane coder (Significance-Propagation, Magnitude-Refinement, Cleanup passes)
- Dequantization (when fused with the last pass)

Out of scope here: threading, IDWT, MCT, TCD buffer mgmt.

### 2.2 Deliverables

Each lands independently and is gated by the bench (see §2.5).

**D1 — Branchless MQ renormalize + packed state LUT.**
Replace the `while (A < 0x8000)` renormalize loop with a `clz`-driven single
shift. Pack `(Qe, NMPS, NLPS, switch_flag)` into one 32-bit entry per state so
a decode step touches one cache line. Keep MQ state register-resident through
the inner loop.

**D2 — Pass-dispatch despecialization.**
Today the SP/MR/CU passes are reached via function-pointer dispatch
parameterized by codeblock style. Generate compile-time-specialized variants
for the dominant cases (lossless 5-3, lossy 9-7, single-component,
RESET/RESTART/TERMALL combinations as observed in the corpus) and select once
per codeblock, not per pixel.

**D3 — Sign decoding via table.**
Replace the conditional logic that derives sign context + XOR bit from
horizontal/vertical neighbor contributions with a precomputed table indexed
by `(h_contrib, v_contrib)`.

**D4 — Stripe coefficient-update SIMD.**
After bits are decoded for a 4-row × N-column stripe, the application of those
bits to the coefficient array is data-parallel. AVX2 and NEON paths; AVX-512
if the stripe width justifies it; SVE2 falls back to NEON if not productive.

**D5 — Dequant fused into the last bitplane pass.**
Merge the dequantization multiply/shift into the final pass's write so
coefficients are emitted in their final form, avoiding a separate sweep.

**D6 — T1 working-state cleanup.**
*(Narrowed after code inspection on 2026-05-16: upstream already consolidates
state in `opj_t1_t`, already has per-thread TLS instances, and already
amortizes `data`/`flags` allocations across codeblocks. D6 is the residual
cleanup that remains worthwhile.)*

- **D6.1** Split `opj_t1_t` into `opj_t1_dec_t` and `opj_t1_enc_t`. Remove
  the `encoder` boolean and dual-purpose fields. Decoder hot path uses a
  smaller, decoder-only struct (better cache footprint, fewer branches).
  *(Status 2026-05-17: dead `encoder` field landed; full struct split
  deferred — revisit when D1 work surfaces what fields the fast paths
  actually need.)*
- **D6.2** Pre-size `data` and `flags` at tile setup using the tile's max
  codeblock dimensions, eliminating the cold-codeblock realloc path.
- **D6.3** Pool per-codeblock `opj_t1_cblk_decode_processing_job_t`
  allocations.
- **D6.4** Route `cblk->decoded_data` in non-whole-tile mode through a
  reusable per-thread buffer (same TLS pattern as the t1 instance).
- **D6.5** Land `t1_fast.c` / `t1_fast.h` scaffolding under Apache-2.0 and
  wire the `OPJ_T1_LEGACY_ONLY` build switch (per §3.3) so the diff-test
  infrastructure exists when D1 arrives.

### 2.3 Ordering

`D6 → D1 → D2 → D3 → D5 → D4`.

- D6 is the structural prerequisite (no globals, no per-decode allocs).
- D1 is the highest-leverage scalar win and forces the bench-driven discipline
  into the team's muscle memory.
- D4 (SIMD) is last because it benefits from the cleaner state struct and the
  despecialized dispatch.

### 2.4 Code organization & licensing

New optimized variants go in a new file `src/lib/openjp2/t1_fast.c`
(Apache-2.0, SPDX header per `CONTRIBUTING.md`).

The existing `t1.c` keeps its preserved BSD-2 header, gains a small runtime
dispatch shim, and stays as the correctness-fallback reference path.
Diff-testing the two paths becomes a built-in regression check (§2.6).

This separation keeps the IP boundary clean: new code under our copyright,
inherited code untouched.

### 2.5 CPU dispatch

Per-target build flags compile in the variants that apply. Runtime selection
uses `cpuid` (x86) / `getauxval` (ARM Linux) / `sysctlbyname` (Darwin) once
at startup, results cached. No per-codeblock CPU checks. SVE2 build is allowed
but may dispatch to the NEON path if SVE2-specific work hasn't landed for
a given deliverable.

### 2.6 Verification

- **Correctness:** byte-exact decode equivalence to the legacy path across the
  bench corpus. Diff-test runs in CI on every PR.
- **Performance:** per-codeblock cycle counts captured by the bench harness
  (`perf stat` on Linux, `mach_absolute_time` on macOS). Each deliverable must
  show measurable cycle reduction on ≥3 corpus items without regressing any
  item by >2% wallclock.
- **Microbenchmarks:** per-deliverable hot-loop benchmark (decode N random
  codeblocks of fixed dimensions) committed to `openjp2k-bench` so regressions
  in *that specific* optimization are bisectable independent of full-decode
  noise.

### 2.7 Milestone gate (applies to every deliverable D1–D6)

No deliverable proceeds to the next, and no PR merges to `main`, until **all**
of the following are recorded:

1. **Correctness:** full bench-corpus diff-test against the legacy path is
   byte-exact.
2. **Performance:** per-codeblock cycle count for the targeted hot loop is
   measurably reduced (signed-rank test across ≥3 corpus items, p<0.05 by
   repeated runs); no corpus item regresses >2% wallclock.
3. **Cross-target:** the gate runs on at least x86-64 AVX2 and ARM64 NEON
   before merge. AVX-512 and SVE2 results may follow asynchronously but cannot
   regress when they do.
4. **Log entry:** results appended to `docs/superpowers/perf-log.md` with date,
   deliverable, hardware, corpus version, and the actual numbers.

If a deliverable fails the gate, it goes back for rework — it does *not* land
"with a TODO" or under a feature flag set to off. The bench is the authority;
intuition about whether a change "should" be faster is not.

This same discipline propagates to every later sub-project.

### 2.8 Risks

- **MQ correctness is unforgiving.** A single off-by-one in the renormalize
  collapses decode silently for streams that happen not to hit the bad path
  during dev testing. *Mitigation:* D1 lands behind a build flag for one
  cycle; CI diffs against the legacy path on the full corpus before flipping
  the default.
- **Compile-time specialization explosion (D2).** Specializing all combinations
  blows up code size. *Mitigation:* corpus-driven — only specialize the
  codeblock-style combinations that actually appear in WSI files.
- **Stripe SIMD on partial-region decodes (D4).** Partial-region decode can
  produce stripes narrower than the SIMD vector. *Mitigation:* scalar tail
  handler; benchmarked separately because ROI / partial-region decode is a
  cross-domain concern (geospatial, DICOM, WSI viewers all hit this path).
- **Clean-room contamination.** The Sub-project 0.5 Grok report must be
  audited before D1–D5 work begins, and the report-reader instance must not
  be the same instance that implements.

---

## 3. Measurement methodology (what this fork commits to)

Most of the harness lives in the sister `openjp2k-bench` repo. This section
pins what *this* fork commits to so the gate in §2.7 is reproducible.

### 3.1 Corpus assumptions

The bench provides a versioned cross-domain corpus (see Sub-project 0)
spanning photographic, DICOM, geospatial, and WSI streams. Corpus version is
recorded with every perf-log entry. Numbers are never compared across corpus
versions. Per-deliverable results must be reported broken down by domain so
domain-specific regressions cannot hide behind aggregate wins.

### 3.2 Build flags

Release builds: `-O3 -DNDEBUG` plus per-target SIMD flags. LTO enabled for
bench builds (matches what consumers ship). Debug builds keep frame pointers;
bench runs do not. Compiler is pinned per perf-log entry; "it got faster"
across compiler versions is not accepted without a same-compiler before/after.

### 3.3 What this fork must expose

- A stable C API for "decode this codeblock from this buffer" so the bench
  can microbenchmark D1–D5 in isolation, not just whole-file decode. The
  microbench API lives behind `OPJ_BENCH_API` so it does not bleed into the
  public surface.
- A compile-time switch (`OPJ_T1_LEGACY_ONLY`) that forces the old `t1.c`
  path, used by the diff-test in CI.
- Built-in per-stage timing counters (`OPJ_PERF_COUNTERS`) gated by a build
  flag, so we can attribute time to MQ / SP / MR / CU / dequant without
  external `perf` instrumentation when needed.

### 3.4 Statistical hygiene

Each bench run: N≥10 repetitions with warm cache; cold-cache runs reported
separately. Report median + IQR, not mean. Cycle counts via `rdtsc` (x86) /
`cntvct_el0` (ARM) with affinity pinned and frequency scaling disabled where
possible. The bench enforces this; this fork exposes the seams.

### 3.5 Perf log

`docs/superpowers/perf-log.md` in this repo. Append-only. One entry per
measured change. Format: deliverable ID, date, commit SHA, corpus version,
hardware, compiler, results table, conclusion (landed / reworked / abandoned).

---

## 4. Next steps

1. Stand up Sub-project 0 (measurement scaffolding) in `openjp2k-bench`.
2. Kick off Sub-project 0.5 (clean-room Grok report) in a separate AI
   instance.
3. Once 0 has baselines and 0.5 has been audited, generate the implementation
   plan for Sub-project 1 starting with D6.
