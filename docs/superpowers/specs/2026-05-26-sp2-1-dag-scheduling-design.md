# SP-2.1 — Per-Tile DAG Scheduling

## Goal

Replace openjp2k's barrier-synchronized per-tile decode (T2 → T1 →
DWT-H → DWT-V → MCT) with a task graph (DAG) where stage transitions
become explicit task dependencies. Tasks run as their predecessors
complete, not when the main thread joins a barrier. Targets the
−20.82% gmean gap vs grok at t=8 measured in [SP-2.0](2026-05-26-sp2-0-mt-baseline-outcome.md);
grok uses Taskflow internally (per the bench's grok adapter notes)
and scales 2.47× at t=8 vs our 1.86×.

This is **SP-2.1** — the first deliverable in Sub-project 2
(codeblock threading + tile-graph scheduling) per the decode-perf
roadmap (`2026-05-16-decode-perf-design.md` §4.3). SP-2.2 (cross-tile
pipelining + parser back-pressure) layers on top later; SP-2.1 stays
strictly per-tile.

Predicted: openjp2k speedup at t=8 from 1.86× → ~2.2-2.5× (target
grok parity). Iter overall t=8 from −6.14% vs openjpeg to ≥+5%; from
−20.82% vs grok to ≥−10%. Slowest-scaling slices (8-bit lossy,
medical) see the largest wins since they currently scale 1.65-2.04×.

## Background

SP-2.0 measured openjp2k MT scaling against openjpeg and grok at
t=1,2,4,8 on the 8-core dev host. Key findings:

- openjp2k tracks openjpeg's scaling almost identically (~1.86× at
  t=8); both inherit the same barrier-synchronized pool.
- Grok scales 2.47× at t=8 via Taskflow-based DAG scheduling.
- The −20.82% openjp2k-vs-grok gap at t=8 is the project's largest
  unrealized competitive deficit.
- Worst-scaling openjp2k slices (1.65-2.04× at t=8): 8-bit lossy,
  conformance, archival, medical. These have fewer codeblocks per
  tile or smaller tiles, so the existing pool's codeblock-level
  parallelism runs out before all cores are saturated, and stage
  barriers force core idle during transitions.

The existing thread pool (`src/lib/openjp2/thread.c`) parallelizes
WITHIN a stage: codeblocks within T1 are dispatched as thread-pool
jobs. But stages are SERIALIZED: `tcd_decode_tile` calls `opj_t2_decode`,
then waits, then calls `opj_t1_decode_cblks` (which submits jobs and
waits), then calls `opj_dwt_decode`, then `opj_mct_decode`. Every
core idles during each `wait`.

The DAG approach replaces those waits with task dependencies: a T1
job for codeblock C becomes runnable as soon as the T2 task for C's
band completes; a DWT-H task for row R becomes runnable as soon as
all T1 jobs writing to row R complete; and so on. Cores keep working
through stage transitions.

## Design

### Backend selection

Two switches:
- **Build-time:** `OPJ_ENABLE_TASK_GRAPH` cmake option (default ON).
  When OFF, `task_graph.cpp` is not compiled, no C++ runtime
  dependency, the library is pure C, and `tcd.c` only compiles the
  barrier path. Mirrors the `OPJ_ENABLE_AVX2` opt-in pattern from
  SP3.1.
- **Runtime:** `OPJ_DAG` env var (default 1) when the build has
  `OPJ_ENABLE_TASK_GRAPH=ON`. `OPJ_DAG=0` falls back to the barrier
  path in the same binary. Mirrors D7's `OPJ_T1_FAST` pattern and
  enables paired-ratio bench gates.

### Task graph C ABI

New header `src/lib/openjp2/task_graph.h`:

```c
typedef struct opj_tg opj_tg_t;
typedef void (*opj_tg_task_fn)(void* arg);

opj_tg_t* opj_tg_create(int num_threads);
void      opj_tg_destroy(opj_tg_t* tg);

/* Add a task to the graph; returns an opaque task handle. */
typedef int opj_tg_task_id;
opj_tg_task_id opj_tg_add_task(opj_tg_t* tg, opj_tg_task_fn fn, void* arg);

/* Declare succ runs only after pred completes. */
void opj_tg_add_dep(opj_tg_t* tg, opj_tg_task_id pred, opj_tg_task_id succ);

/* Execute the graph; blocks until all tasks complete. Returns
 * OPJ_TRUE on success. If any task sets an error via a side channel
 * the caller is responsible for surfacing it. */
OPJ_BOOL opj_tg_run(opj_tg_t* tg);

/* Convenience: enabled check (consults env var + build flag). */
OPJ_BOOL opj_tg_enabled(void);
```

The runtime check `opj_tg_enabled()` mirrors D7's `opj_t1_fast_enabled()`:
returns 1 by default, 0 if `OPJ_DAG=0`/`off`/`false`, cached after
first call.

### Taskflow integration

`src/lib/openjp2/task_graph.cpp` wraps Taskflow:

```cpp
#include <taskflow/taskflow.hpp>

struct opj_tg {
    tf::Taskflow flow;
    tf::Executor executor;
    std::vector<tf::Task> tasks;  // indexed by opj_tg_task_id
    explicit opj_tg(int num_threads)
        : executor(num_threads > 0 ? num_threads : std::thread::hardware_concurrency()) {}
};

extern "C" {
opj_tg_t* opj_tg_create(int num_threads) {
    return new opj_tg{num_threads};
}
opj_tg_task_id opj_tg_add_task(opj_tg_t* tg, opj_tg_task_fn fn, void* arg) {
    auto t = tg->flow.emplace([fn, arg]{ fn(arg); });
    tg->tasks.push_back(t);
    return (opj_tg_task_id)(tg->tasks.size() - 1);
}
void opj_tg_add_dep(opj_tg_t* tg, opj_tg_task_id pred, opj_tg_task_id succ) {
    tg->tasks[pred].precede(tg->tasks[succ]);
}
OPJ_BOOL opj_tg_run(opj_tg_t* tg) {
    tg->executor.run(tg->flow).wait();
    return OPJ_TRUE;
}
void opj_tg_destroy(opj_tg_t* tg) { delete tg; }
}  // extern "C"
```

Vendored Taskflow lives at `third_party/taskflow/taskflow.hpp`
(single-header, MIT-licensed, C++17). Total dependency footprint:
~3000 LOC of header, ~50 KB to libopenjp2.so size.

### tcd.c restructure

The existing `opj_tcd_decode_tile` calls `opj_tcd_t2_decode`,
`opj_tcd_t1_decode`, `opj_tcd_dwt_decode`, `opj_tcd_mct_decode` in
sequence (each function internally blocks until its stage completes).
SP-2.1 adds a parallel path:

```c
OPJ_BOOL opj_tcd_decode_tile(opj_tcd_t* p_tcd, ...) {
    if (!opj_tg_enabled()) {
        /* existing barrier-synchronized path, unchanged */
        return opj_tcd_decode_tile_legacy(p_tcd, ...);
    }
    return opj_tcd_decode_tile_dag(p_tcd, ...);
}
```

`opj_tcd_decode_tile_dag` builds a DAG with:

- **T2 tasks** (one per band per tile-part): parse codestream
  bitstreams into per-cblk segments. Predecessors: none. Successors:
  T1 tasks for each cblk in that band.
- **T1 tasks** (one per codeblock): invoke `opj_t1_decode_cblk` (the
  D7.1-gated dispatch already lives there). Predecessors: T2 of the
  cblk's band. Successors: a DWT "input ready" task for the cblk's
  resolution.
- **DWT-resolution tasks** (one per resolution): run the entire
  `opj_dwt_decode` per-resolution lift (current per-resolution
  implementation kept as-is). Predecessors: ALL T1 tasks of cblks in
  this resolution AND the DWT task of the previous (coarser)
  resolution. Successors: the next resolution's DWT, or MCT.
- **MCT task** (one per tile): invoke `opj_mct_decode`. Predecessors:
  DWT of resolution 0 for every used component. Successors: none.

Intra-resolution structure (DWT-H per row, DWT-V per col-stripe) is
NOT broken open by SP-2.1 — the existing per-row/per-stripe thread
dispatch inside `dwt.c` continues to use the thread pool, which is
now driven by Taskflow's executor instead of `opj_thread_pool`.

### Thread-pool unification

When `OPJ_DAG=1`, the existing `opj_thread_pool_*` is bypassed:
`opj_tcd_create` allocates the legacy pool only when `!opj_tg_enabled()`.
The DAG path uses Taskflow's executor as the single worker pool.
This avoids oversubscription where two pools each try to use 8 cores
on an 8-core machine.

Code paths inside `t1.c` and `dwt.c` that currently submit to
`opj_thread_pool` need a runtime check: if a Taskflow executor
context exists for this tile, post jobs to it; otherwise use the
legacy pool. Minimal abstraction: a `opj_dispatch_job(tcd, fn, arg)`
helper in `tcd.c` that picks the right pool. Existing callers in
t1.c and dwt.c switch to this helper.

### Files Modified / Added

**Added:**
- `third_party/taskflow/taskflow.hpp` — vendored single-header
  Taskflow (~3000 LOC, MIT). Gitignored only if we choose to fetch
  via cmake `FetchContent`; otherwise checked in for build
  reproducibility (prefer checked-in for now).
- `src/lib/openjp2/task_graph.h` — C ABI (~50 LOC).
- `src/lib/openjp2/task_graph.cpp` — Taskflow C++ wrapper (~200-300 LOC).

**Modified:**
- `src/lib/openjp2/tcd.c` — add `opj_tcd_decode_tile_dag`; dispatch
  at `opj_tcd_decode_tile` entry; small refactor of cblk/DWT/MCT
  callers to use `opj_dispatch_job` helper. ~200-300 LOC delta.
- `src/lib/openjp2/t1.c` — switch the thread-pool dispatch in
  `opj_t1_decode_cblks` to the `opj_dispatch_job` abstraction.
  ~20 LOC delta.
- `src/lib/openjp2/dwt.c` — same switch for the per-row/stripe job
  dispatch. ~30 LOC delta.
- `src/lib/openjp2/CMakeLists.txt` — add `task_graph.cpp` (compiled
  as C++17), conditional on `OPJ_ENABLE_TASK_GRAPH`. Link
  `-lstdc++`. ~10 LOC delta.

**Not modified:** `t2.c`, `mct.c`, `mqc.c` — the stage functions
themselves are unchanged; only the orchestration around them changes.

Total estimate: ~3500 LOC added (mostly Taskflow), ~600 LOC of
project changes across 5 files. Expected implementation: **4-6 weeks**
of careful work.

## Scope Boundaries

- **Per-tile only.** Cross-tile pipelining + parser back-pressure
  is SP-2.2. SP-2.1's `tcd_decode_tile` stays per-tile; the caller's
  per-tile loop is untouched.
- **Stage barriers only.** Intra-stage structure (per-cblk T1,
  per-row DWT-H, per-stripe DWT-V) preserved. Strip-based DWT is a
  separate deliverable.
- **No T2 restructure.** T2 decoding stays single-threaded per
  tile-part; SP-2.1 just lets T2 of band B feed T1 of cblks in B
  before T2 of band B+1 completes.
- **C library remains buildable without C++.** `OPJ_ENABLE_TASK_GRAPH=OFF`
  removes the C++ dependency entirely.
- **Runtime fallback is mandatory.** Even with `OPJ_ENABLE_TASK_GRAPH=ON`,
  `OPJ_DAG=0` must produce byte-identical decode using the legacy
  barrier path.

## Verification

### Conformance

- Build `OPJ_ENABLE_TASK_GRAPH=ON`, `OPJ_ENABLE_AVX2=ON`, default
  `OPJ_DAG`: 545/553 NR-DEC pass, exactly the 8 pre-existing failures.
- Same build with `OPJ_DAG=0`: same 8 failures.
- Build `OPJ_ENABLE_TASK_GRAPH=OFF`, `OPJ_ENABLE_AVX2=ON`: same 8
  failures, no `task_graph.cpp` compiled, no C++ dependency.

### Threading sanity

- ThreadSanitizer build (`-fsanitize=thread -DOPJ_TSAN=1`): smoke
  decode of all 90 synthetic files at `--threads 4`, `OPJ_DAG=1`.
  Zero races reported. Re-run with `--threads 1` to confirm the
  serial-execution path doesn't have its own races.
- Stress test: 5× repeated decode of the 90-file smoke at `--threads 8`,
  compare each output to the `OPJ_DAG=0` reference (byte-cmp).
  Any byte differences ≡ a missing dependency edge in the DAG.

### Manual byte-cmp

Three files (8-bit, 12-bit, 16-bit lossless) × {DAG-on, DAG-off,
AVX2-OFF reference} × {full-tile, center-50% ROI} = 18 cmp pairs. All
identical to the AVX2-OFF reference.

### Bench gate

Bench harness now spawns fresh `jp2k-bench` process per `--threads`
value (cornish/openjp2k-bench `c07c9c3`), so grok numbers are correct
at every thread count. Same-day baselines.

Two builds:
- **Baseline:** current main (D7.1 tip, `2310d339`).
- **Head:** SP-2.1 branch tip with default `OPJ_DAG=1`.

Run iter at `--threads 1,2,4,8` against each.

| Slice | t | Pass | Target |
|---|---|---|---|
| Iter overall | 1 | within ±0.5% | parity (no t=1 regression) |
| Iter overall | 4 | ≥ +3% | ≥ +5% |
| Iter overall | 8 | ≥ +5% | ≥ +10% |
| 8-bit lossy iter | 8 | ≥ +10% | ≥ +20% |
| Medical iter | 8 | ≥ +10% | ≥ +20% |
| 12-bit lossy iter | 8 | ≥ +8% | ≥ +15% |
| Conformance (`OPJ_ENABLE_AVX2=ON`, `OPJ_DAG=1`) | — | 8 pre-existing | exact |
| Conformance (`OPJ_DAG=0` override) | — | 8 pre-existing | exact |

Project-level goal: close ≥50% of the t=8 grok gap. Pass = openjp2k
at ≥−10% vs grok at t=8 overall; target = grok parity.

## Risks

1. **Race conditions in DAG topology.** Each task must declare ALL
   its predecessors. Missing an edge produces silent data races,
   manifesting as flaky conformance failures or wrong pixels.
   Mitigation: TSan stress test in CI; edges encoded in a single
   helper (`opj_tcd_build_decode_dag`) rather than scattered through
   `opj_tcd_decode_tile_dag`; review checklist enumerates every
   producer-consumer pair.

2. **Taskflow overhead on tiny tiles.** Building and tearing down a
   DAG with N tasks costs O(N). For tiles with one cblk per band, the
   DAG cost might exceed the savings. Mitigation: tile-size threshold
   below which `opj_tcd_decode_tile_dag` falls back to the barrier
   path even with `OPJ_DAG=1`. Threshold empirical, set during
   implementation. Probably ~16-32 cblks total per tile as the
   crossover.

3. **Thread-pool double-allocation / oversubscription.** Existing
   code uses `opj_thread_pool_*` for cblk dispatch. Taskflow has its
   own executor. Running both concurrently means 16 threads on an
   8-core machine. Mitigation: when `OPJ_DAG=1`, the legacy pool is
   not created (or its thread count is 0); when `OPJ_DAG=0` the
   Taskflow executor is not created. The `opj_dispatch_job` helper
   picks the right pool based on which is active.

4. **C++ runtime in libopenjp2.so.** Adds ~50 KB to .so size and
   pulls in libstdc++. Some embedded distributors care. Mitigation:
   `OPJ_ENABLE_TASK_GRAPH=OFF` build keeps the library pure C with
   only the legacy barrier path. Document in BUILDING.md.

5. **Conformance flakes from output-buffer races / false sharing.**
   Even with a correct DAG, two tasks writing adjacent rows of
   `tilec->data` could false-share cache lines, producing bench
   noise (not wrong output). Mitigation: align cblk-output writes to
   cache-line boundaries where possible; bench at warmup=2 to absorb
   first-iteration anomalies.

6. **Bench-gate noise floor.** Expected gain (+5-10% overall iter)
   is within cross-day drift on this host. Same-day baselines
   mandatory (per the D7 retrospective). Plan: baseline + head iter
   runs back-to-back within the same hour, paired ratios.

7. **Scope creep into SP-2.2.** Within-tile DAG must not pre-implement
   parser back-pressure or cross-tile dependencies. Hard rule: the
   per-tile loop in the caller of `tcd_decode_tile` is touched only
   if absolutely necessary, and ANY change to it is a code-review
   red flag.

8. **Realistic upper bound is finite.** Amdahl's law: grok hits
   2.47× at t=8 — well below the 8× theoretical limit. SP-2.1's
   best case is grok parity (~2.5×). Beating grok meaningfully would
   require SP-2.2 (cross-tile pipelining) or restructuring
   intra-stage parallelism.

9. **Taskflow C++17 compiler requirement.** Some downstream
   distributors may pin older toolchains. Build with `OPJ_ENABLE_TASK_GRAPH=OFF`
   for them. Document in CONTRIBUTING.md.

10. **Long implementation timeline.** 4-6 weeks of careful work
    increases risk of mid-flight scope changes, integration drift
    with main, and reviewer fatigue. Mitigation: ship as several
    sub-deliverables (SP-2.1a = task_graph module + barrier-path
    fallback; SP-2.1b = T2→T1 DAG; SP-2.1c = T1→DWT DAG; SP-2.1d =
    full DAG end-to-end). Each lands behind `OPJ_DAG=1` with passing
    conformance before the next starts.

## Open Questions

1. **Sub-deliverable split (SP-2.1a/b/c/d).** Risk #10 suggests
   splitting the implementation into incremental landings. Worth
   deciding before writing the plan. My recommendation: split into
   four steps as above. Each step independently testable and
   bench-able. SP-2.1a is mostly infrastructure (task_graph module,
   build wiring, runtime gate); the perf gains arrive in 2.1b-d.

2. **Taskflow version pin.** Latest stable as of 2026-05-26 is
   v3.7.x (per upstream). Pin to a specific tag in `third_party/`
   for reproducibility. Decide which release at implementation time.

3. **Should the task graph be reusable across tiles?** Per-tile
   `opj_tg_create`/`destroy` costs allocations. Reusing one DAG
   across tiles (clearing + re-adding tasks) is a perf win but
   complicates state management. Defer to implementation
   measurement.

## Decision Gate

After implementation:

- **Iter t=8 overall**: ≥+5% (pass), ≥+10% (target) vs same-day D7.1
  baseline.
- **Iter t=1 overall**: within ±0.5% (no single-threaded regression).
- **8-bit lossy iter t=8**: ≥+10% pass / ≥+20% target.
- **Medical iter t=8**: ≥+10% pass / ≥+20% target.
- **Conformance**: 8 pre-existing failures under all
  (`OPJ_ENABLE_TASK_GRAPH`, `OPJ_DAG`) combinations.
- **TSan stress test**: zero races at `--threads 4` and 8.
- **Byte-cmp**: 18/18 pairs identical.
- **Build**: passes with `OPJ_ENABLE_TASK_GRAPH={ON,OFF}` ×
  `OPJ_ENABLE_AVX2={ON,OFF}` = 4 combinations.

If iter t=8 overall < +3%, the DAG isn't actually relieving the
barrier waits — investigate before tagging. If t=1 regresses > 0.5%,
DAG construction overhead is bleeding into the single-threaded path;
fix or add a single-threaded short-circuit.

## Cumulative project framing

SP-2.1 is the project's largest single bet to date. Pre-SP-2.1
positions:
- Iter t=1: **−0.10% vs openjpeg** (parity).
- Iter t=8: −6.14% vs openjpeg, **−20.82% vs grok**.

Successful SP-2.1 outcome would be the first time openjp2k is
defensibly competitive across both single and multi-thread
workloads. Failed SP-2.1 (no measurable gain) means single-threaded
parity remains the project's ceiling — at which point SP-2.2 and the
roadmap's cloud / random-access sub-projects become more attractive
relative to in-tile threading.
