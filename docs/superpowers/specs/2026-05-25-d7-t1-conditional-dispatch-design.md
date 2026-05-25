# D7 — T1 Conditional Fast/Legacy Dispatch Based on Whole-Tile-Decoding

## Goal

Make D1's "fast" T1 entropy decode path run only on whole-tile decode,
not on partial-tile (ROI) decode. The fast path's per-codeblock setup
cost amortizes well across many full-size codeblocks (full-tile) but
dominates over its faster inner loop when fewer codeblocks are
decoded (partial-tile). Disabling it for partial decode recovers the
pre-existing partial-decode perf gap surfaced by
[cornish/openjp2k#3](https://github.com/cornish/openjp2k/issues/3).

This is **D7** — the next D-tier (profile-driven) deliverable after
D6.1. Predicted wins: ROI smoke overall +1.9%, ROI 16-bit lossless
+6.3%; full-tile smoke unchanged.

## Background

D1 (commits `512eb96d`..`f5978a62`, tag `v0.4.0-d1-mq-tightening`)
cloned the three hot T1 entropy-decode passes — sigpass, refpass,
clnpass — into `src/lib/openjp2/t1_fast.c` as a "fast" variant using
a packed MQ state table (`opj_mqc_states_packed`) and macro-driven
register-resident MQ variables. Dispatch is gated at `t1.c:1933`:

```c
int use_fast = opj_t1_fast_enabled();
```

`opj_t1_fast_enabled()` returns 1 by default, 0 if env var
`OPJ_T1_FAST` is set to `0` / `off` / `false`.

The [D1 retrospective](../../../.claude/projects/-home-cornish-GitHub-openjp2k/memory/d1-retrospective.md)
recorded "parity not a win" on the full-tile smoke (smoke gmean
fast/legacy = 1.0045) and didn't bench partial-decode. The SP3.2b
investigation surfaced a pre-existing **−2.48%** openjp2k-vs-openjpeg
gap on ROI smoke (−5.96% on 16-bit lossless), and a follow-up
profile + paired smoke runs localized the regression to the fast
T1 path:

| Slice | T1_legacy vs T1_fast (ROI) | T1_legacy vs T1_fast (Full) |
|---|---:|---:|
| Overall | **+1.90%** (legacy wins) | −2.69% (fast wins) |
| 16-bit lossless | **+6.29%** (legacy wins) | +2.53% (legacy slightly wins) |
| 12-bit lossless | +2.61% (legacy wins) | −1.91% (fast wins) |
| 8-bit lossless | +0.38% | −4.15% (fast wins) |
| 12-bit lossy | −1.57% | −6.77% (fast wins) |
| 16-bit lossy | +1.79% (legacy wins) | −3.96% (fast wins) |

The mechanism: the fast path's per-codeblock setup (packed-state LUT
init, `DOWNLOAD_MQC_FAST_VARIABLES` / `UPLOAD_MQC_FAST_VARIABLES`,
ctxs_idx setup) amortizes over the per-cblk decode loop. Full-tile
decode visits many codeblocks per tile, so the setup amortizes; the
faster inner loop dominates. Partial-tile decode visits only those
codeblocks overlapping the window — fewer cblks per tile, same fixed
setup cost per cblk, less amortization. The fast path's setup cost
becomes net-negative for partial decode.

## Design

### Dispatch change

Add a single boolean field to `opj_t1_t` (declared in `t1.h`):

```c
struct opj_t1 {
    ...existing fields...
    OPJ_BOOL whole_tile_decoding;
};
```

In `tcd.c`, at the per-tile T1 setup site that constructs / configures
the worker `opj_t1_t` struct (the site that currently sets
`mustuse_cblkdatabuffer`), set the new field from
`p_tcd->whole_tile_decoding`:

```c
t1->whole_tile_decoding = p_tcd->whole_tile_decoding;
```

In `t1.c:1933`, change the dispatch from:

```c
int use_fast = opj_t1_fast_enabled();
```

to:

```c
int use_fast = opj_t1_fast_enabled() && t1->whole_tile_decoding;
```

That is the entire functional change. ~5-10 lines across `t1.h`,
`t1.c`, and `tcd.c`.

### Env var semantics (preserved + augmented)

| `OPJ_T1_FAST` | `whole_tile_decoding` | `use_fast` |
|---|:---:|:---:|
| unset / "1" / "true" / anything not in the off-set | true | 1 |
| unset / "1" / "true" / anything not in the off-set | false | **0** (new) |
| "0" / "off" / "false" | true | 0 |
| "0" / "off" / "false" | false | 0 |

The "off" cases preserve existing behavior. The new behavior is
unset/on + partial-tile, which now selects legacy.

### Threading

In multi-threaded decode, T1 work is dispatched per-codeblock across
thread-pool workers. Each worker owns its own `opj_t1_t` struct.
`whole_tile_decoding` is a per-tile configuration value (does not
change across codeblocks within a tile), set at tile entry. The
setup must happen *before* the worker thread-pool jobs are queued,
so all workers see the flag.

The exact tcd.c setup site is `opj_tcd_t1_decode_job_init` or
`opj_t1_decode_cblks` (to be confirmed during implementation). The
flag must be propagated to whatever worker-local `t1` instance is
created. Look for the existing `mustuse_cblkdatabuffer` assignment
as the canonical place that already does this kind of per-worker
config propagation.

### Files Modified

- `src/lib/openjp2/t1.h` — add `OPJ_BOOL whole_tile_decoding;` to
  the `opj_t1` struct.
- `src/lib/openjp2/t1.c` — change dispatch at line 1933.
- `src/lib/openjp2/tcd.c` — set the flag at the per-tile T1 setup
  site (and per-worker setup if needed).

Estimated total: ~10-15 LOC across 3 files. No new files.

### Verification

- **Conformance:** `OPJ_ENABLE_AVX2=ON` build must show the same 8
  pre-existing NR-DEC-md5 failures, no new mismatches. T1 produces
  byte-identical output in fast vs legacy (D1's invariant), so
  conditional dispatch is byte-exact in either branch.
- **OPJ_T1_FAST=0 conformance run:** same 8 failures, confirms the
  global-override path still works.
- **AVX2-OFF build:** clean, conformance same.

### Bench gate

Paired within-run ratios against current SP3.2 final (`2f37e529`).
Baselines already on-disk:
- ROI: `roi_smoke_sp32_baseline_20260525_085632.jsonl`
- Full-tile: `smoke_sp3_2_20260524_143416.jsonl`

| Slice | n | Pass | Target |
|---|---|---|---|
| ROI smoke overall | 90 | ≥ +1% gmean | ≥ +1.5% |
| ROI smoke 16-bit lossless | 19 | ≥ +3% gmean | ≥ +5% |
| ROI smoke 12-bit lossless | 19 | ≥ +1% gmean | ≥ +2% |
| Full-tile smoke overall | 90 | within ±0.5% | parity |
| Full-tile smoke 8-bit lossless | 19 | within ±0.5% | parity |

Predicted deltas (from the already-measured T1_legacy ROI / fast
full-tile pair, since conditional dispatch is the per-workload
union of legacy-on-ROI + fast-on-full): ROI overall +1.9%, 16-bit
lossless +6.3%; full-tile unchanged.

## Scope Boundaries

- **T1 dispatch only.** Don't touch the fast-path implementation
  itself — D7 is a runtime dispatch fix, not a re-implementation.
  Improving the fast path's setup cost so it amortizes on partial
  decode is a separate (and harder) deliverable.
- **No env-var surface change.** Keep `OPJ_T1_FAST=0` as the
  documented global override. Don't introduce `=auto` or new values.
- **Both AVX2 build modes.** The flag is independent of AVX2.

## Risks

1. **Wrong tcd.c setup site.** If `whole_tile_decoding` is set on
   `t1` *after* T1 worker dispatch, MT workers won't see the flag.
   Mitigation: locate the site that creates the per-worker `t1`
   structs (likely `opj_t1_decode_cblks` or `opj_tcd_t1_decode_job_init`),
   set the flag at the same scope as the existing
   `mustuse_cblkdatabuffer` assignment. Verified by re-bench: if
   ROI numbers don't shift, the flag isn't reaching the workers.

2. **16-bit lossless full-tile leaves +2.5% on the table.** The
   full-tile smoke showed legacy is +2.53% on 16-bit lossless —
   the one slice where legacy beats fast on full-tile. Conditional
   dispatch keeps fast there, so we leave that win unrealized.
   Acceptable tradeoff (other full-tile slices win 2-7% with fast).
   Worth its own follow-up deliverable — possibly a per-precision
   sub-dispatch ("fast for prec≤12 full-tile, legacy otherwise").

3. **Env-var semantic shift is silent.** Previously
   `OPJ_T1_FAST=unset` meant "fast everywhere." After D7 it means
   "fast on full-tile, legacy on partial-tile." Callers who never
   set the var see this change without notice. Document in the
   commit message and outcome spec. Callers who explicitly set
   `=0` keep their current behavior.

4. **Performance is workload-dependent and corpus-dependent.** Our
   measurement is on the synthetic-iter smoke (90 1024² rasters).
   The crossover behavior on real-world corpora — large remote-
   sensing tiles, DICOM J2KR, WSI — may differ. Document the
   expectation that the conditional dispatch is a workload-driven
   bet and may be tunable later.

## Open Questions

None for D7 itself. The "improve the fast path so it amortizes on
partial decode too" question is real but out of scope here.

## Decision Gate

After implementation:

- **ROI smoke overall**: ≥ +1% (pass), ≥ +1.5% (target).
- **ROI smoke 16-bit lossless**: ≥ +3% (pass), ≥ +5% (target).
- **No full-tile smoke regression** beyond ±0.5% on overall or 8-bit lossless.
- **Conformance**: 8 pre-existing failures only under
  `OPJ_ENABLE_AVX2=ON`, with and without `OPJ_T1_FAST=0`.
- **Build**: passes with `OPJ_ENABLE_AVX2=ON` and `OPJ_ENABLE_AVX2=OFF`.

If ROI overall improves < +0.5%, the flag isn't reaching the
workers — investigate before tagging. If full-tile regresses > 0.5%,
something else changed; revert.
