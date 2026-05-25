# SP3.2b — Int16 5/3 DWT Path for Partial-Tile (ROI) Decode

## Goal

Extend SP3.2's int16 5/3 inverse DWT path from the full-tile orchestrator
(`opj_dwt_decode_tile`, shipped in commit `67c3376b`) to the partial-tile
orchestrator (`opj_dwt_decode_partial_tile`). Targets the same `prec ≤ 8`
workloads — DICOM 8-bit, photographic 8-bit, WSI tiles — when the decoder
is invoked with a region-of-interest window rather than the full tile.

Real-world consumers driving this: geospatial viewers panning over GeoJP2,
DICOM viewers zooming into a radiograph, WSI viewers tiling a slide.
CLAUDE.md explicitly calls partial-region decode out as cross-cutting
opportunity vanilla openjpeg leaves on the table.

This is **SP3.2b** — the second-half completion of SP3.2 (which deferred
partial-tile to its own deliverable). AVX2 only, gated by
`OPJ_ENABLE_AVX2`.

## Background

`opj_dwt_decode_partial_tile` (dwt.c:3806) is structurally distinct from
`opj_dwt_decode_tile`:

- Tile data lives in an `opj_sparse_array_int32_t` (sparse subregion
  store), not the dense `tilec->data` buffer.
- Per-resolution scratch `h.mem` is 4-column-stride int32 (vs
  PARALLEL_COLS_53 for the full-tile mcols kernels).
- Kernels (`opj_dwt_decode_partial_1`, friends) include row-windowing
  logic to handle the window edges, which the full-tile kernels skip.
- Single-threaded (no thread-pool argument).

The lift math is identical to the full-tile path — the difference is the
data store and the per-row windowing. So SP3.2's pack/unpack helpers and
the AVX2 lift-kernel idioms transfer cleanly; the orchestrator and the
windowed row kernels do not.

## Design

### Activation

At `opj_dwt_decode_partial_tile` entry, gate on the source component's
precision, mirroring SP3.2's check:

```c
} else {
#ifdef __AVX2__
    const OPJ_UINT32 compno = (OPJ_UINT32)(tilec -
                              p_tcd->tcd_image->tiles->comps);
    const opj_image_comp_t *image_comp = &p_tcd->image->comps[compno];
    if (image_comp->prec <= 8) {
        return opj_dwt_decode_partial_tile_int16(p_tcd, tilec, numres);
    }
#endif
    return opj_dwt_decode_partial_tile(tilec, numres);
}
```

The `p_tcd` argument needs to reach the partial branch (currently only
the full branch receives it), a small signature touch-up at the
`opj_dwt_decode` call site.

Signedness reasoning is identical to SP3.2: dequantized coefficients are
always signed regardless of `image_comp.sgnd` because of the DC level
shift, so the magnitude bound is what matters.

### Data flow (int16 partial path)

```
opj_dwt_decode_partial_tile_int16(tilec) {
    sa16 = opj_sparse_array_int16_create(rw, rh, ...)
    opj_dwt_init_sparse_array_int16(sa16, tilec)   // T1 int32 → int16 pack

    int16_t* scratch = aligned_alloc(max_resolution_size * sizeof(int16_t) * STRIPE)

    for (resno = 1; resno < numres; ++resno) {
        for each window row:
            opj_idwt53_h_int16_partial(scratch, sa16, row, win_x0, win_x1)
        for each window col-stripe:
            opj_idwt53_v_int16_partial_mcols(scratch, sa16, col_stripe, win_y0, win_y1)
    }

    opj_sparse_array_int16_read(sa16, win, tilec_data_win_int16)
    opj_dwt_unpack_int16_to_int32(tilec->data_win, tilec_data_win_int16, win_area)

    opj_sparse_array_int16_free(sa16)
    aligned_free(scratch)
}
```

Conversion structure mirrors SP3.2:
- 1× int32→int16 pack at `init_sparse_array_int16` (once per tile).
- 1× int16→int32 unpack at the final write to `tilec->data_win`.
- All DWT lift levels run pure int16; no per-level conversion.

### New files

- `src/lib/openjp2/sparse_array_int16.h` — int16 API mirroring
  `sparse_array.h`: `opj_sparse_array_int16_create / _free / _read /
  _write / _is_region_valid`. Same block-tiling structure as the int32
  version. ~140 LOC.
- `src/lib/openjp2/sparse_array_int16.c` — implementation mirroring
  `sparse_array.c` with the element-type swap. ~350 LOC structural
  mirror.

The int16 sparse array lives in its own translation unit so the int32
version is untouched. A future macroized refactor (SP3.5?) could
parameterize the element type and de-duplicate; out of scope here.

### New dwt.c additions

| Function | Mirrors |
|---|---|
| `opj_dwt_init_sparse_array_int16` | `opj_dwt_init_sparse_array` (dwt.c:3750) |
| `opj_dwt_decode_partial_tile_int16` | `opj_dwt_decode_partial_tile` (dwt.c:3806) |
| `opj_idwt53_h_int16_partial` | `opj_dwt_decode_partial_1` (horizontal windowed) |
| `opj_idwt53_v_int16_partial_mcols` | `opj_dwt_decode_partial_1_parallel` (vertical 4-col → 8-col int16 stripe) |

Estimated net add to `dwt.c`: ~500 LOC. Combined with sparse_array_int16
files (~490 LOC), total new code ~990 LOC. Same order of magnitude as
SP3.2 (962 LOC added in dwt.c).

The pack/unpack helpers (`opj_dwt_pack_int32_to_int16`,
`opj_dwt_unpack_int16_to_int32`) and the 5/3 lift idioms
(`_mm256_add_epi16`, `_mm256_srai_epi16`, `_mm256_sub_epi16`) carry over
unchanged from SP3.2.

### Verification

- **Conformance:** `OPJ_ENABLE_AVX2=ON` build must show the same 8
  pre-existing NR-DEC-md5 failures, no new mismatches. NR-DEC tests
  that exercise partial decode via `-d x0,y0,x1,y1` will hit the new
  path automatically on 8-bit files.
- **Manual byte-cmp:** three 8-bit files — one smoke-synthetic rgb8,
  one archival LoC map (`07481_1926-0001.jp2`), one DICOM J2KR
  (`J2KR_US1_J2KR.j2k`) — decoded with three regions each: full tile,
  center-50% (`-d w/4,h/4,3w/4,3h/4`), and top-left quadrant
  (`-d 0,0,w/2,h/2`). `OPJ_ENABLE_AVX2=ON` vs `OFF` decoded planes
  must byte-cmp identical for each (file, region) pair.
- **AVX2-OFF build:** must compile and pass conformance with
  `OPJ_ENABLE_AVX2=OFF`. The new code is `#ifdef __AVX2__`-gated.

### Bench gate

New `scripts/run_smoke_roi.sh` in the bench repo wrapping `run_bench.sh`
with a fixed center-50% ROI on the existing synthetic-iter manifest
(same 90 files, same iters/warmup as full-tile smoke). If the
`jp2k-bench` binary doesn't already accept a region flag, that's a
one-line argv-parser add — verified before SP3.2b implementation
proper begins.

Paired within-run ratios against an SP3.2 baseline build (HEAD
`886e1532`), per the SP3.2 retrospective lesson. Cross-run absolute
timings are noise-dominated and unreliable.

| Slice | n | Pass | Target |
|---|---|---|---|
| ROI smoke 8-bit lossless | ~19 | ≥ +1% gmean | ≥ +2% |
| ROI smoke overall (negative-control slices) | 90 | within ±1% gmean | — |
| Conformance (`OPJ_ENABLE_AVX2=ON`) | 553 | exactly 8 pre-existing | — |

The overall row is a no-regression tripwire on slices SP3.2b doesn't
activate (>8-bit, lossy 9/7). If it lands outside ±1% in the *positive*
direction — as SP3.2 unexpectedly did across the board — that's an
accidental cross-slice win, documented in the outcome but not blocking.
If it regresses by ≥1%, investigate before tagging.

## Files Modified / Added

- **Added:** `src/lib/openjp2/sparse_array_int16.h` (~140 LOC)
- **Added:** `src/lib/openjp2/sparse_array_int16.c` (~350 LOC)
- **Modified:** `src/lib/openjp2/dwt.c` — new partial int16 orchestrator,
  init helper, and two kernels; dispatch gate at the `else` branch of
  `opj_dwt_decode`; signature touch-up to pass `p_tcd` to the partial
  branch (~500 LOC added).
- **Added (bench repo):** `scripts/run_smoke_roi.sh`. Possibly a one-line
  argv-parser change to `src/main.cpp` in the bench if no region flag
  exists yet.

No changes to T1, sparse_array.c (int32), or any other source file.

## Scope Boundaries

- **5/3 lossless only.** Partial 9/7 lossy is unchanged.
- **prec ≤ 8 only.** 9-bit and above stays on the existing int32
  partial path. 12-bit deferred — same reasoning as SP3.2: intermediate
  values can exceed int16 range through 5 decomposition levels.
- **AVX2 only.** Non-AVX2 builds run the existing int32 partial path.
  SSE2 int16 partial deferred.
- **Single-threaded partial path stays single-threaded.** The full-tile
  thread-pool dispatch doesn't apply here; matching the existing
  partial-tile behavior keeps the surface minimal.
- **No new build options.** Activates automatically inside the existing
  `OPJ_ENABLE_AVX2` envelope.

## Risks

1. **Overflow on >8-bit data.** Same defense as SP3.2: strict gate at
   `prec ≤ 8` + Debug assertion inside `pack_int32_to_int16`.

2. **Conversion overhead dominates on tiny windows.** Partial decode is
   often used with small windows (DICOM thumbnail, GeoJP2 minimap), where
   the O(N) pack/unpack over the window area can eat the win. Mitigation:
   bench-measure; if measurable regression on small windows appears, add
   a `window_area ≥ T` guard (T empirical).

3. **Sparse-array surface duplication.** Two parallel sparse-array
   implementations (int32 + int16) until SP3.5 macroization. Accept the
   duplication; the int16 file is a structural mirror of the int32 file
   and will track its evolution mechanically. Document the parallel in
   `sparse_array_int16.h` head comment so future edits stay synchronized.

4. **Realized win smaller than SP3.2's.** For partial decode, T1 entropy
   is a larger fraction of total time than DWT (fewer codeblocks decoded,
   but each costs the same as in full-tile). So a +4-5% DWT win
   translates to a smaller end-to-end speedup. Anything positive is
   acceptable for SP3.2b's gate.

5. **Cross-slice icache surprise (symmetric to SP3.2).** Adding the
   int16 partial functions to `dwt.c` may shift code layout enough to
   affect the int32 partial path. Measured by the ROI smoke overall row.

## Open Questions

None for SP3.2b itself. The 12-bit extension (for 12-bit ROI decode in
DICOM CT/MR) deserves its own deliverable once SP3.2 + SP3.2b ship and
we can measure the actual narrow-precision DICOM win. The
sparse_array_int16 + partial int16 work here lays the foundation; the
12-bit story is incremental on top.

## Decision Gate

After implementation:

- **Smoke ROI 8-bit lossless speedup**: ≥ +1% (pass), ≥ +2% (target).
- **No smoke ROI regression beyond ±1%** on slices SP3.2b doesn't touch.
- **Conformance**: 8 pre-existing failures only under
  `OPJ_ENABLE_AVX2=ON`.
- **Build**: passes with `OPJ_ENABLE_AVX2=ON` and `OPJ_ENABLE_AVX2=OFF`.
- **Byte-cmp**: AVX2-ON and AVX2-OFF decode identical bytes across
  three 8-bit files × three ROI windows.

If ROI 8-bit lossless improves < 0.5%, investigate before tagging. If
ROI smoke overall regresses > 1%, revert.

---

## Outcome (2026-05-25) — **Negative result, reverted**

Implemented and benched on `feat/sp3-2b-int16-partial-dwt` (commit
`55ff6ac6`); branch deleted without merging to main after measurement.
Spec + plan retained on main as a record of the attempt.

### Verification (all passed)

- AVX2-ON and AVX2-OFF builds: clean, no warnings.
- Conformance `OPJ_ENABLE_AVX2=ON`: 545/553 pass, exactly the 8
  pre-existing NR-DEC-md5 failures — no new mismatches.
- Manual byte-cmp (3 files × 3 regions, AVX2-ON vs AVX2-OFF):
  **9/9 identical.**

The implementation is correct. The problem was performance.

### Bench (ROI smoke, center-50% window, n=90, paired-ratio vs SP3.2 baseline)

Three-way decomposition isolated the contributions. **A** = SP3.2
baseline (no SP3.2b code in binary). **B** = SP3.2b code in binary but
dispatch disabled via an investigation env var (int32 partial path
runs for prec≤8). **C** = SP3.2b shipped (int16 partial path runs for
prec≤8).

| Slice | n | Icache cost (B/A) | int16 path delta (C/B) | Combined (C/A) |
|---|---:|---:|---:|---:|
| Overall | 90 | **−0.99%** | +0.01% | −0.97% |
| **8-bit lossless** (SP3.2b target) | 19 | **−0.96%** | **+0.27%** | **−0.69%** |
| 8-bit lossy | 11 | −0.78% | +0.69% | −0.10% |
| 12-bit lossless | 19 | −0.95% | +0.12% | −0.84% |
| 12-bit lossy | 11 | −0.55% | −1.31% | −1.85% |
| **16-bit lossless** | 19 | **−1.41%** | +0.13% | **−1.28%** |
| 16-bit lossy | 11 | −1.01% | −0.16% | −1.16% |

Decision gate verdict: **FAIL**.

- Target slice (8-bit lossless ROI) gate: ≥+1% pass / ≥+2% target.
  Actual: −0.69%.
- Smoke overall tripwire (±1%): actual −0.97%, just inside the revert
  threshold but in the wrong direction.

### Diagnosis

Two findings, both load-bearing for the decision:

1. **The int16 partial path itself barely helps.** On the 8-bit
   lossless target slice, switching from int32 to int16 lift in the
   partial-tile orchestrator delivers only **+0.27%**. The lift cycle
   savings are nearly cancelled by the per-cblk `int32→int16` pack
   overhead in `opj_dwt_init_sparse_array_int16` and the conversion
   round-trip at the boundary. For partial decode, T1 entropy is a
   larger fraction of total time than DWT — there isn't enough DWT
   work per resolution to amortize the conversion. Risk #2 in the
   design was real and dominant.

2. **The icache cost of adding the new code is the dominant negative
   effect (~−1% uniformly).** Even with the int16 dispatch disabled,
   merely having the SP3.2b code in the binary regresses every slice
   by about 1%. The worst-hit slice is **16-bit lossless** at **−1.41%**
   — a workload (medical CT/MR) SP3.2b doesn't even activate on. This
   is the SP3.2 cross-slice surprise running in reverse direction:
   SP3.2 full-tile addition gave a +4% accidental win; SP3.2b partial
   addition gives a −1% accidental loss. Code layout / icache effects
   from adding ~1000 LOC to `dwt.c` are real and direction-dependent.

### Why no salvage path is viable

- **Window-size guard (Risk #2's suggested mitigation):** unhelpful.
  Our 512×512 measurement window is already a generous fraction of
  the 1024² synthetic tile — if this size doesn't show a win, smaller
  windows definitely won't.
- **Even a magically-zero-cost int16 path** would only deliver +0.27%
  on the target slice — still below the +1% pass threshold. The whole
  premise (int16 SIMD widening dominates partial-decode wall clock)
  is wrong for this workload.
- **Reducing code size to lower icache cost:** the ~990 added LOC are
  legitimate kernels and the sparse-array surface. Can't shrink
  meaningfully without losing correctness.

### Action taken

- Branch `feat/sp3-2b-int16-partial-dwt` deleted; commit `55ff6ac6`
  abandoned. The investigation env-var hack on dwt.c was reverted
  before deletion.
- This spec + the implementation plan are retained on main as the
  archive of the attempt — useful negative result.
- No tag cut. The project state stays at SP3.2 final
  (`v0.8.0-sp3-2-int16-53-dwt`).

### Carry-over

- **Partial-decode perf gap is pre-existing and worth its own work.**
  Even with no SP3.2b code in the binary (sp32_baseline), openjp2k
  is **−2.48% slower than openjpeg overall** on ROI smoke, with
  −5.96% on 16-bit lossless. The 5/3 int32 partial path itself
  has perf headroom unrelated to SIMD widening.
- **Cross-slice icache effects are real and direction-dependent.**
  SP3.2 (+4% on untouched slices) and SP3.2b (−1% on untouched slices)
  are both empirical evidence. Future deliverables that add code to
  `dwt.c` should expect to measure this effect and may need
  function-layout / alignment / `OPJ_LOCAL` work to manage it.
- **Methodology lesson for future SP-style deliverables:** the design
  spec's "investigate if target < 0.5%" tripwire fired. The three-way
  decomposition (no-code / code-present-but-disabled / code-active)
  via an env-var dispatch off-switch is the right pattern for separating
  icache cost from the active-path delta. Use it whenever the active
  path is a small fraction of the binary's net change.
- Next candidates per the SP3 plan: SP3.4 (Highway runtime dispatch —
  unaffected by this result, still valuable), or a new D-tier deliverable
  targeted at the partial-path perf gap surfaced here, or pathological
  J2KR slowdowns (cornish/openjp2k#2).
