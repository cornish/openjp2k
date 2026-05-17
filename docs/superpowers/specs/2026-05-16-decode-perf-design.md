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

### Sub-project 2 — Codeblock threading + tile-graph scheduling

*(Reframed 2026-05-17 after cleanroom audit — see §4.3.)* Decode independent
codeblocks in parallel **and** express each tile's decompression as a DAG
with cross-tile pipelining and parser back-pressure. Touches `tcd.c`
orchestration, the t1 entry, and likely introduces a task-graph dependency
(Taskflow or equivalent). Multiplies Sub-project 1's per-codeblock wins by
core count and additionally extracts parallelism across components,
resolutions, and tiles simultaneously. Largest practical wallclock win for
any image with many codeblocks per component per resolution — which covers
essentially every production workload (WSI tiles, geospatial scenes, large
radiology series, multi-megapixel photographic images).

### Sub-project 3 — Inverse DWT polish

Upstream's recent NEON IDWT work is a starting point. Add AVX2/AVX-512 paths
for the 9-7 and 5-3 IDWTs. Add partial-region short-circuiting: skip DWT
samples outside the requested region. Region-of-interest decode matters
broadly — geospatial viewers (panning over a tiled GeoJP2), DICOM viewers
(zooming into a region of a radiograph), and WSI viewers all decode partial
regions, but vanilla openjpeg does not optimize for it.

*(Augmented 2026-05-17 after cleanroom audit — see §4.4. Adds a 16-bit DWT
path for 5/3 + ≤12-bit precision, a per-worker DWT scratch pool, and an
open question on Highway-based SIMD dispatch.)*

### Sub-project 4 — MCT vectorization

Inverse color transform. Memory-bound; gains from SIMD + cache-friendly access.
Small file but disproportionately easy to win.

### Sub-project 5 — TCD buffers, allocation churn, page release

Reuse tile-component buffers across tiles in a session; eliminate per-tile
mallocs. Less CPU-bound but matters for any session-style consumer:
interactive viewers (WSI, geospatial, DICOM) that decode many tiles in
sequence, and batch pipelines that process many images per process.

*(Augmented 2026-05-17 after cleanroom audit — see §4.5. Adds a glibc /
Windows `malloc_trim`-equivalent page release after tile-row drain, and
incremental stripe compositing so resident memory tracks ~2 tile-rows
rather than the whole tile-component plane.)*

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
*(D7, D8, D9 added 2026-05-17 from cleanroom audit findings — see §4.2.)*

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
- **Clean-room contamination.** *(Status 2026-05-17: report audited and
  folded into §4 by the implementing instance with the user's explicit
  consent to override the original instance-firewall rule. Going forward,
  the discipline that matters is: (a) do NOT mirror Grok-internal
  identifier names in our code, (b) do NOT import Grok-tuning empirical
  constants — re-derive them in the bench. See §4.1.)*

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

## 4. Findings from cleanroom Grok report *(added 2026-05-17)*

### 4.1 Audit conclusion

The Sub-project 0.5 cleanroom report (`docs/grok-cleanroom-research/`) was
audited 2026-05-17 against the rules in §1 "Sub-project 0.5". Conclusion:
**safe to use as input to spec and plan work in this fork**, with the
following hard constraints carried forward:

- The implementing instance MUST NOT mirror Grok-internal identifier names
  observed in the report (e.g. anything matching a Grok class / member /
  variable / CV name). Pick our own names.
- Empirical Grok-tuning constants flagged by the report itself
  (e.g. PCRD α=0.75, T1 early-stop window of 7, factor-of-3 bitplane
  escalation) MUST NOT be imported literally. Re-derive any such value by
  experiment in `openjp2k-bench`.
- Mathematical constants from the JPEG 2000 standard (9/7 lifting
  coefficients, BIBO bounds, etc.) are public-knowledge and freely usable.
- Third-party library names cited as Grok dependencies (Taskflow, Highway,
  OpenJPH, libcurl) are independently obtainable open-source projects;
  naming them in our spec/plan is not a cleanroom concern.

The report names four documents, of which the most relevant to this spec is
`grok-performance-innovations.md`. Section references below (§G.x) point
into that document for traceability.

### 4.2 Additional deliverables for Sub-project 1 (T1)

The report (§G.6) validates the spec's existing D1-D6 direction
(particularly §G.6.2: MQ is fundamentally sequential, **do not vectorize
it** — our D1 scalar-tightening framing is correct). Three additional
small-but-concrete deliverables surfaced:

- **D7 — Multi-buffer MQ input.** Today the decoder concatenates a
  codeblock's segments into a single temporary buffer (`cblkdatabuffer`,
  see t1.c:2054-2078). Eliminate the concatenation by teaching the MQ
  state to step across an array of `(ptr,len)` chunks. Wins a memcpy per
  multi-segment codeblock; matters most for codestreams with many
  tile-parts or with PPM/PPT marker use. (Report ref: §G.6.9.)
- **D8 — Pre-shifted context LUTs.** Bind the zero-coding / sign-coding
  LUT pointer to the current subband's orientation at codeblock entry,
  rather than re-doing the orientation arithmetic per coded symbol. Small
  scalar win, no SIMD required. (Report ref: §G.6.4.)
- **D9 — Coder pool keyed by `(worker, codeblock_size)`.** OpenJPEG's TLS
  scheme stores one T1 instance per worker thread; the buffers are
  resized as larger blocks appear. Keying additionally by codeblock-size
  exponents means each (worker, size) combination has its scratch sized
  exactly once and avoids the lazy-grow on heterogeneous codestreams.
  Matters most for cross-domain corpora where different encoders produce
  different block sizes within a session. Folds together with the
  deferred D6.1-remainder (struct split) since both touch the same
  per-worker state. (Report refs: §G.3.4, §G.6.6.)

Ordering inside Sub-project 1 with these additions: D6 → D1 → D2 → D3 →
D5 → D4 → D8 → D7 → D9. D8 / D7 are small follow-ups to the main T1 work;
D9 should be planned after D6.1-remainder is settled so the two land
together.

### 4.3 Reframe for Sub-project 2 (codeblock threading)

The existing description ("decode independent codeblocks in parallel") is
too narrow. The report (§G.3) documents that the substantive threading win
is architectural, not just per-codeblock parallelism — OpenJPEG already
runs codeblock decode on a thread pool. Reframed scope:

- **DAG-based per-tile scheduling.** Express each tile's decompression as
  a directed acyclic graph (T2 → T1 → DWT-H → DWT-V → MCT) with explicit
  dependencies. Tasks become runnable as predecessors complete, not when
  the main thread joins a barrier. Cross-tile work fills the cores while
  any individual tile is partially blocked. Candidate dependency:
  Taskflow (MIT-licensed, single-header C++17; license-compatible).
- **Cross-tile pipelining with parser back-pressure.** A parser thread
  feeds tiles into the DAG ahead of the consumer by a small bounded
  number of tile-rows (the report cites 2). Consumer-side advances a
  drain pointer and wakes the parser when work has been retired.
- **Per-tile-row completion tracking.** Glue between the DAG and the
  consumer (compositor / writer), so row callbacks fire off the
  completion-tracker lock to avoid serializing other workers.

This expanded SP-2 is a significant architectural change; it likely splits
into 2-3 sub-projects when planned in detail. (Report refs: §G.3.1, §G.3.3,
§G.3.6, §G.3.7.)

### 4.4 Additional deliverables for Sub-project 3 (IDWT)

- **16-bit DWT path for 5/3 and ≤12-bit precision.** Promote the 5/3
  reversible IDWT (and, optionally, a Q1.15 fixed-point 9/7) to operate on
  `int16` coefficients when the codestream precision permits. Halves
  bandwidth and cache footprint, which is decisive for memory-bound DWT.
  Covers most DICOM (≤12-bit common) and 8-bit photographic — i.e. a
  majority of the cross-domain corpus. Requires overflow-safe SIMD
  averaging primitives; derivation is in the report. (Report refs:
  §G.5.1-§G.5.5.)
- **Per-worker wavelet scratch pool.** Same shape as the existing T1 coder
  pool — pre-allocate DWT scratch per worker thread, no allocations during
  the IDWT hot loop. (Report ref: §G.5.8.)
- **Open question: Highway-based SIMD dispatch.** Google's Highway
  (Apache-2.0) provides portable SIMD with runtime dispatch across SSE2 /
  SSSE3 / AVX2 / AVX-512 / NEON / SVE / WASM / RISC-V V from a single
  source. Alternative to maintaining hand-rolled per-target intrinsics for
  the IDWT lifting kernels. Trade-off: dependency vs. maintenance burden.
  Decide before D-IDWT-1 lands. (Report refs: §G.5.6, §G.10.2.)

### 4.5 Additional deliverables for Sub-project 5 (TCD / buffers)

- **Page release after tile-row drain.** A thin allocator wrapper that, in
  addition to tracking aggregate live bytes for 64-byte alignment,
  invokes `malloc_trim(0)` on glibc (and the equivalent on Windows /
  macOS) immediately after a tile-row's data is released. Without this,
  `free()` returns memory to the allocator's free list but RSS stays at
  the high-water mark — defeating any incremental-memory work. (Report
  ref: §G.3.5.)
- **Incremental stripe compositing.** For session-style consumers
  (viewers, batch processors) that consume image rows as they become
  available, keep resident memory proportional to ~2 tile-rows rather
  than the whole tile-component plane. Requires SP-2's per-tile-row
  completion tracking and the page-release mechanism above. (Report refs:
  §G.4.1-§G.4.6.)

### 4.6 Provisional Sub-project 6 — cloud / random-access *(not yet in scope)*

The report (§G.7, §G.8, §G.10) describes a substantial cloud-decode
architecture: TLM/PLT marker-driven selective byte-range fetching,
two-tier compressed-chunk cache (in-memory LRU + disk spill),
HTTP/2-multiplexed range fetcher with connection pooling. This is
transformative for cloud workloads (large WSI tiles on object storage,
NITF on S3, etc.) but is **not currently in this fork's scope**.

Add as Sub-project 6 only when a consumer surfaces concrete demand
(e.g., openscope adds remote-corpus support, or a geospatial / pathology
consumer asks for it). Note its existence here so it doesn't get
re-invented during SP-2 planning. (Report refs: §G.7.1, §G.7.2, §G.8.1.)

### 4.7 Explicitly out of scope

In addition to HTJ2K (§1):

- **Encoder PCRD slope-estimator early termination** (report §G.6.7,
  §G.7.4). Encoder-side optimization; CLAUDE.md states encode is not a
  priority. Documented here only so the area is not accidentally
  inherited as a goal.
- **Excalibur scheduler backend** (report §G.3.2). Per the report,
  experimental / no documented use case in Grok. Skip.

### 4.8 HTJ2K direction *(when revisited)*

When HTJ2K is revisited (deferred per §1), the report (§G.6.8, §G.10.2)
recommends embedding **OpenJPH** (BSD-2-Clause; Apache-2.0-compatible)
rather than reimplementing Part-15 block coding ourselves. OpenJPH ships
scalar / SSSE3 / AVX2 / AVX-512 variants and CPU-dispatches at module
load. OpenJPEG ships its own `ht_dec.c` which is scalar-only on the
MEL / VLC / MAGREF loops — a known performance gap.

---

## 5. Next steps

1. Stand up Sub-project 0 (measurement scaffolding) in `openjp2k-bench`.
2. ~~Kick off Sub-project 0.5 (clean-room Grok report) in a separate AI
   instance.~~ *(Done 2026-05-17; audited and folded into §4.)*
3. Once 0 has baselines, generate the implementation plan for Sub-project 1
   starting with D6. (D6.5 + D6.1-trimmed + D6.2 landed 2026-05-17 ahead of
   the bench; D1+ remain blocked on the bench.)
