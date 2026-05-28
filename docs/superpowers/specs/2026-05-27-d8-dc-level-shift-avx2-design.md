# D8 — AVX2 DC Level Shift (Float Path) for Lossy Decode

## Goal

Vectorize the per-pixel float clamp + level shift loop at
`tcd.c:2419-2436` in `opj_tcd_dc_level_shift_decode`. The
profile-driven follow-up to SP-2.1a localized **9% of single-thread
runtime on 8-bit lossy decode** to this scalar loop — it's the final
post-processing step after T1 → DWT → MCT for the lossy (9/7) path,
running per-pixel with conditional branches that prevent the compiler
from vectorizing. Grok's equivalent post-processing
(`postProcessBlock`) consumed ~2% on the same workload, suggesting
they vectorize this step. SIMD-ifying the float path targets a
~7 percentage point shift on lossy slices at every thread count.

This is **D8** — a small, profile-driven deliverable. Lossless (5/3)
slices use the int-path branch in the same function and are not
touched by D8. A follow-up (D9) may vectorize the int path if a
focused profile justifies it.

## Background

The SP-2.1a outcome's profile (single-thread, 8-bit lossy file
`pCPRL_d1_b32_t1024x1024_lossy_l1_moff_esop_eph.jp2`) found:
- T1 inner-loop fractions are equal across openjp2k / openjpeg / grok
  (~18% each) — T1 is not the differentiator.
- `opj_tcd_decode_tile` self-time at **9.02%** in openjp2k vs grok's
  equivalent at ~2%. The 7-pp gap localized via perf annotate to
  `tcd.c:2423` — the `if (l_value > (OPJ_FLOAT32)INT_MAX)` test on
  the float clamp path.
- Grok's lead on lossy slices at t=8 in SP-2.0:
  8-bit lossy −44%, 12-bit lossy −25%, 16-bit lossy −7%. Largest on
  exactly the workloads where this clamp loop runs per-pixel.

The scalar loop:

```c
for (j = 0; j < l_height; ++j) {
    for (i = 0; i < l_width; ++i) {
        OPJ_FLOAT32 l_value = *((OPJ_FLOAT32 *) l_current_ptr);
        if (l_value > (OPJ_FLOAT32)INT_MAX) {
            *l_current_ptr = l_max;
        } else if (l_value < INT_MIN) {
            *l_current_ptr = l_min;
        } else {
            /* Do addition on int64 to avoid overflows */
            OPJ_INT64 l_value_int = (OPJ_INT64)opj_lrintf(l_value);
            *l_current_ptr = (OPJ_INT32)opj_int64_clamp(
                                 l_value_int + l_tccp->m_dc_level_shift,
                                 l_min, l_max);
        }
        ++l_current_ptr;
    }
    l_current_ptr += l_stride;
}
```

Per-pixel branching kills auto-vectorization. For our 1024² × 3
test file: ~3M pixels × 5 instructions per iteration = ~15M
instructions in the hot path, scalar. AVX2 processes 8 pixels per
iteration of a branch-free clamp → cvt → add pattern, a textbook
4-6× speedup pattern.

## Design

### SIMD strategy: pre-clamp to range that's safe after the add

Naive vectorization (clamp float to `[INT_MIN, INT_MAX]`, cvt, add
shift, clamp to `[lmin, lmax]`) needs the int add in int64 to avoid
overflow when the cvt output is near `INT_MAX/MIN` and dc_shift
pushes past the boundary.

Better: pre-clamp the float to bounds chosen so the add can't
overflow. Specifically, clamp to `[(float)(lmin - dc_shift),
(float)(lmax - dc_shift)]`. After `cvtps_epi32` produces a value in
`[lmin - dc_shift, lmax - dc_shift]`, adding `dc_shift` lands in
`[lmin, lmax]` with no overflow. Skips the int64 path entirely.

This is equivalent to the scalar version because:
- For float values cleanly in range: the float pre-clamp is a no-op,
  and the rest of the chain (cvt → add → store) matches the scalar
  in-range branch exactly.
- For float values outside `[INT_MIN, INT_MAX]` (the scalar's "clamp
  to l_min/l_max without adding shift" branches): pre-clamp to
  `lmax - dc_shift` (still within `INT_MAX` for any reasonable
  dc_shift), cvt, add → `lmax`. Same end result.
- The pre-clamp bounds in float (`lmax - dc_shift`,
  `lmin - dc_shift`) are safe in IEEE-754 single precision for any
  reasonable JP2K precision (lmin, lmax fit in int32; dc_shift is
  typically `±2^(prec-1)` ≤ ±32768 for 16-bit). No representation
  loss for the comparison.

### AVX2 inner loop

```c
__m256 vfmin = _mm256_set1_ps((float)lmin - (float)dc_shift);
__m256 vfmax = _mm256_set1_ps((float)lmax - (float)dc_shift);
__m256i vdc  = _mm256_set1_epi32(dc_shift);

for (j = 0; j < height; ++j) {
    OPJ_UINT32 i;
    OPJ_INT32* row = data + j * (width + stride);
    /* Vector body: 8 pixels per iteration. */
    for (i = 0; i + 8 <= width; i += 8) {
        __m256 v  = _mm256_loadu_ps((const float*)(row + i));
        v         = _mm256_min_ps(_mm256_max_ps(v, vfmin), vfmax);
        __m256i vi = _mm256_cvtps_epi32(v);  /* MXCSR rounding (default RNE) */
        vi        = _mm256_add_epi32(vi, vdc);
        _mm256_storeu_si256((__m256i*)(row + i), vi);
    }
    /* Scalar tail: < 8 remaining pixels. */
    for (; i < width; ++i) {
        OPJ_FLOAT32 v = *((OPJ_FLOAT32*)(row + i));
        if (v > (OPJ_FLOAT32)INT_MAX) row[i] = lmax;
        else if (v < INT_MIN)        row[i] = lmin;
        else {
            OPJ_INT64 vi64 = (OPJ_INT64)opj_lrintf(v);
            row[i] = (OPJ_INT32)opj_int64_clamp(vi64 + dc_shift, lmin, lmax);
        }
    }
}
```

### Rounding-mode match

`_mm256_cvtps_epi32` uses the MXCSR rounding mode, which on
Linux/x86-64 defaults to round-to-nearest-even (IEEE 754 default).
`opj_lrintf` rounds to nearest using the current rounding mode (also
round-to-nearest-even by default). They match for all non-tie
values; ties round to even in both. Byte-exact equivalence holds.

If anyone has ever called `fesetround` with a non-default mode
before our decode, both scalar and SIMD see the same change. No
behavior divergence.

### Dispatch

Inside `opj_tcd_dc_level_shift_decode` at the float-path branch
(currently `tcd.c:2419-2436`), replace the scalar body with:

```c
} else {
#ifdef __AVX2__
    opj_tcd_dc_level_shift_decode_float_avx2(
        l_current_ptr, l_width, l_height, l_stride,
        l_tccp->m_dc_level_shift, l_min, l_max);
#else
    /* existing scalar body */
    for (j = 0; j < l_height; ++j) { ... }
#endif
}
```

`__AVX2__` defines automatically when `OPJ_ENABLE_AVX2=ON` (per
SP3.1's `-mavx2` flag). When `OPJ_ENABLE_AVX2=OFF`, scalar runs.

### Files Modified

- `src/lib/openjp2/tcd.c` — add the AVX2 helper (~80 LOC), dispatch
  at the existing scalar site (~10 LOC delta).

No other files. No new build options. ~90 LOC total.

## Verification

### Conformance

- `OPJ_ENABLE_AVX2=ON`: 8 pre-existing NR-DEC-md5 failures.
- `OPJ_ENABLE_AVX2=OFF`: same 8 failures (scalar path runs in this
  build).
- Note: D8 changes decoded **values** only for lossy decode (the
  float path) — if rounding-mode equivalence holds, output is
  byte-identical to scalar. Manual byte-cmp verifies.

### Manual byte-cmp

Three lossy files at each precision tier × two builds:

```bash
F8=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossy_l1_mon_enone.jp2
F12=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono12_1024/pLRCP_d5_b64_t1024x1024_lossy_l1_moff_enone.j2k
F16=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono16_1024/pLRCP_d5_b64_t1024x1024_lossy_l1_moff_enone.j2k
for F in $F8 $F12 $F16; do
  build-d8/bin/opj_decompress -i "$F" -o /tmp/on.raw
  build-d8-noavx2/bin/opj_decompress -i "$F" -o /tmp/off.raw
  cmp -s /tmp/on.raw /tmp/off.raw && echo "OK $(basename $F)" || echo "FAIL"
done
```

Expected: 3/3 OK. Any FAIL → rounding-mode mismatch or tail bug;
STOP and fix before measuring perf.

Also: byte-cmp on 3 **lossless** files (8/12/16-bit lossless), to
confirm D8 doesn't accidentally affect the int-path branch. All
should be unchanged.

### Profile re-check

After landing, re-run perf on the same 8-bit lossy file and confirm:
- `opj_tcd_decode_tile` self-time drops from ~9% to ~2-3%.
- New AVX2 helper appears at low percentage (the SIMD work itself
  is fast).

### Bench gate (paired iter vs SP-2.1a tip)

Same-day baseline (D7 retrospective lesson). Bench harness at
threads 1,2,4,8 — fresh process per thread count (the
`run_bench.sh` fix from earlier).

| Slice | t | Pass | Target |
|---|---|---|---|
| 8-bit lossy iter | 1 | ≥ +3% | ≥ +5% |
| 8-bit lossy iter | 8 | ≥ +5% | ≥ +10% |
| 12-bit lossy iter | 8 | ≥ +3% | ≥ +5% |
| 16-bit lossy iter | 8 | within ±1% | parity-positive |
| 8-bit lossless iter | any | within ±0.5% | unchanged |
| 12-bit lossless iter | any | within ±0.5% | unchanged |
| 16-bit lossless iter | any | within ±0.5% | unchanged |
| Iter overall | 8 | ≥ +2% | ≥ +3% |
| Conformance | — | 8 pre-existing | exact |

Predicted absolute position shift on iter overall t=8: from current
−6.14% vs openjpeg to roughly −3 to −4% — meaningful single-deliverable
movement toward MT parity.

## Scope Boundaries

- **Float path only.** Int path stays scalar in D8; defer to D9 if a
  focused profile shows it's hot.
- **AVX2 only.** No SSE2 fallback, no NEON / AVX-512 / SVE. The
  pattern translates trivially, but each target is a separate
  deliverable (Sub-project 3.4 Highway dispatch territory).
- **No new build options.** Rides on existing `OPJ_ENABLE_AVX2`.
- **Float clamp + level shift only.** Not the broader MCT
  vectorization (that's Sub-project 4).

## Risks

1. **Rounding-mode mismatch (most likely concern).** Default
   round-to-nearest-even should match `opj_lrintf`, but a caller
   could theoretically change MXCSR before our decode. Byte-cmp
   catches any difference. If it surfaces, switch to
   `_mm256_round_ps(_, _MM_FROUND_TO_NEAREST_INT)` + `_mm256_cvttps_epi32`
   for explicit round-then-truncate, matching the C standard
   library's nearbyint semantics.

2. **Tail handling.** Width may not be a multiple of 8. Scalar tail
   loop handles the remainder. For widths < 8 entirely (degenerate
   case), the SIMD loop runs zero iterations and tail runs everything.

3. **Stride.** `l_current_ptr` is `int32_t*`. `_mm256_loadu_ps` /
   `_mm256_storeu_si256` (unaligned variants) handle any alignment;
   the perf cost on modern x86 is essentially zero for unaligned
   loads in L1.

4. **Lossless integer path accidentally affected.** Mitigation: byte-cmp
   on lossless files confirms the int-path branch is untouched. The
   change is purely under the `} else {` of the int-vs-float test.

5. **Pre-clamp arithmetic overflow in float bounds.** `lmax - dc_shift`
   for unusual inputs (e.g., dc_shift very negative + lmax very
   positive). For JP2K, dc_shift is small (typically `±2^(prec-1)`,
   ≤ ±32768 for 16-bit), lmin/lmax fit in int32. Float representation
   of the difference is exact for these magnitudes.

6. **opj_tcd_dc_level_shift_decode runs for both encode and decode.**
   The function is shared but the float branch (and D8's change)
   only executes when reading float coefficients — that's the decode
   path. Encode writes raw int input data. No encode impact.
   Verified by reading the function context before implementing.

## Decision Gate

After implementation:

- **8-bit lossy iter t=8** ≥ +5% (pass), ≥ +10% (target) vs same-day
  SP-2.1a baseline.
- **8-bit lossy iter t=1** ≥ +3% (pass), ≥ +5% (target).
- **No lossless regression beyond ±0.5%** on 8/12/16-bit lossless
  slices.
- **Conformance**: 8 pre-existing failures under both
  `OPJ_ENABLE_AVX2={ON,OFF}`.
- **Byte-cmp**: AVX2-ON and AVX2-OFF decode identical bytes on 6
  files (3 lossy + 3 lossless × {full-tile}).
- **Profile re-check**: `opj_tcd_decode_tile` self-time drops to
  ~2-3% on the 8-bit lossy reference file.

If 8-bit lossy iter t=8 improves < +3%, the gate isn't engaged or
the SIMD helper has hidden overhead — investigate before tagging.
If any lossless slice regresses > 0.5%, the dispatch accidentally
hit the int path; revert.

## Cumulative project framing

D8 is the first deliverable explicitly targeting a single-thread
gap surfaced by direct openjp2k-vs-grok profile data. Predicted to
move the MT iter t=8 position from −6.14% to roughly −3% vs
openjpeg (cumulative project state would be: single-thread parity
post-D7.1, multi-thread within −3% of openjpeg, still ~−15-17%
behind grok). The remaining MT gap to grok requires SP-2.2-style
cross-tile work plus the per-cblk architectural improvements
(memory layout, lower overhead) that grok has and we don't.

## Open Questions

None for D8 itself. The int-path D9 follow-up depends on whether
the post-D8 profile shows int-path overhead big enough to matter —
worth measuring after D8 lands but not pre-committing.
