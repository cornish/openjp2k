# SP3.1 — AVX2 9/7 Float Lift

## Goal

Promote the 9/7 (lossy float) IDWT lift kernels from `__m128` (SSE2, 4 floats per
op) to `__m256` (AVX2, 8 floats per op) on AVX2-capable hardware. Targets the
~16% of decode wall-time that the D2 post-D1 profile attributed to
`opj_dwt_decode_real` on lossy archival workloads.

This is **SP3.1** — the first deliverable in a decomposition of the original
spec's Sub-project 3 (IDWT polish). The remaining sub-deliverables (SP3.2
int16 5/3 path, SP3.3 DWT scratch pool, SP3.4 Highway dispatch) are tracked
separately and not in scope here.

## Background

The 5/3 (lossless int) DWT in `src/lib/openjp2/dwt.c` already has AVX-512,
AVX2, and SSE2/NEON paths (see `VREG_INT_COUNT` selection at dwt.c:72-81).
The 9/7 (lossy float) DWT — `opj_v8dwt_decode_step1_sse` (dwt.c:3245-3259)
and `opj_v8dwt_decode_step2_sse` (dwt.c:3261-...) — uses **only `__m128`**.
There is no AVX2 (or AVX-512) variant of the 9/7 lift.

The D2 profile (`docs/superpowers/profile/2026-05-21-post-d1-decode-profile.md`)
measured the archival workload (loc-maps lossy 9/7, 1120 tiles):

| function | self-time % |
|---|---|
| `opj_dwt_decode_real` (wrapper + lift) | 10.92 |
| `opj_v8dwt_interleave_h` | 2.51 |
| `opj_v8dwt_decode_step2_sse` | 1.84 |
| `opj_v8dwt_decode` | 0.79 |
| **DWT total** | **~16%** |

The step1/step2 lift kernels themselves are 2-3% directly; the rest of the
"DWT total" lives in the wrapper / interleave / partial-region path which
SP3.1 does not target. So the achievable upper bound from widening the lift
alone is bounded by the kernel's share — likely 2-5% wall-clock on this
workload.

## Design

### Data layout (unchanged)

`opj_v8_t` (dwt.c:96-100) is a union holding 8 `OPJ_FLOAT32` = 32 bytes,
which is the natural AVX2 alignment. The existing SSE2 code treats each
record as `__m128*[4]` (4 chunks of 4 floats each, processed pairwise). AVX2
treats it as `__m256*[2]` (2 chunks of 8 floats). No struct or buffer-layout
change is required.

### Compile-time dispatch

Gate the new kernels with `#ifdef __AVX2__`. When the build target has AVX2
(which `-march=native` provides on the bench host), the AVX2 versions
replace the SSE2 versions. Otherwise the existing SSE2/NEON code path is
unchanged.

This matches the existing pattern: the 5/3 DWT picks vector width via
`#ifdef __AVX512F__ / __AVX2__` ladder (dwt.c:72-81). No runtime dispatch,
no function pointers; the compiler resolves the path at build time.

### Lift kernel translations

`opj_v8dwt_decode_step1_sse` (current SSE2):

```c
static void opj_v8dwt_decode_step1_sse(opj_v8_t* w, OPJ_UINT32 start,
                                       OPJ_UINT32 end, const __m128 c)
{
    __m128* OPJ_RESTRICT vw = (__m128*) w;
    OPJ_UINT32 i = start;
    vw += 4 * start;   /* 4 = NB_ELTS_V8 / 4-floats-per-__m128 */
    for (; i < end; ++i, vw += 4) {
        vw[0] = _mm_mul_ps(vw[0], c);
        vw[1] = _mm_mul_ps(vw[1], c);
    }
}
```

AVX2 variant:

```c
static void opj_v8dwt_decode_step1_avx2(opj_v8_t* w, OPJ_UINT32 start,
                                        OPJ_UINT32 end, const __m256 c)
{
    __m256* OPJ_RESTRICT vw = (__m256*) w;
    OPJ_UINT32 i = start;
    vw += 2 * start;   /* 2 = NB_ELTS_V8 / 8-floats-per-__m256 */
    for (; i < end; ++i, vw += 2) {
        vw[0] = _mm256_mul_ps(vw[0], c);
        vw[1] = _mm256_mul_ps(vw[1], c);
    }
}
```

`opj_v8dwt_decode_step2_sse` translates analogously: each `__m128`
operation pair becomes a single `__m256` operation (since the two paired
SSE ops were processing the lower and upper halves of the same opj_v8_t).
For example, the prologue:

```c
/* SSE2 */
vw[-2] = _mm_add_ps(vw[-2], _mm_mul_ps(_mm_add_ps(vl[0], vw[0]), c));
vw[-1] = _mm_add_ps(vw[-1], _mm_mul_ps(_mm_add_ps(vl[1], vw[1]), c));

/* AVX2 */
vw[-1] = _mm256_add_ps(vw[-1], _mm256_mul_ps(_mm256_add_ps(vl[0], vw[0]), c));
```

### Byte-exact maintained

IEEE 754 guarantees lane-wise SIMD ops produce results identical to the
equivalent scalar operations on each lane. `_mm256_add_ps` on two stacked
4-float halves produces the same bits as two `_mm_add_ps` calls. **No FMA**
is used — we keep the `mul + add` as two distinct ops, matching the SSE2
rounding behaviour byte-for-byte.

Verification: the upstream conformance MD5 tests (`NR-DEC-*-md5`) compare
decoded pixel output against precomputed reference hashes. If the AVX2 lift
matches the SSE2 lift bit-for-bit, the conformance hashes match. The same 8
pre-existing failures from D1/D6.1 should remain the only failures.

### `c` argument broadcast

The current SSE2 kernels take `__m128 c` (a 4-wide broadcast of a scalar
lift coefficient like `-1.586134342f`). AVX2 needs `__m256 c` (8-wide). The
callers in `opj_v8dwt_decode` construct these via `_mm_set1_ps(...)` and
will need an `#ifdef __AVX2__` switch to `_mm256_set1_ps(...)`. Trivial
change at the call sites.

## Files Modified

- `src/lib/openjp2/dwt.c` — add `opj_v8dwt_decode_step1_avx2` and
  `opj_v8dwt_decode_step2_avx2` kernels; gate the existing `_sse` variants
  to compile only when `__AVX2__` is undefined; update the lift-coefficient
  broadcast sites (`_mm_set1_ps` → `_mm256_set1_ps` when AVX2). Net add:
  ~80-100 lines of new AVX2 code, balanced by `#ifdef` gating of existing
  code.

No changes to TCD, T1, MCT, j2k, mqc, image, or any spec/plan documents
outside this design doc.

## Verification

### Per-task
- Build clean with AVX2 enabled (the bench host's `-march=native` default).
- Build clean with AVX2 disabled (`-mno-avx2` or older `-march=core2`) to
  verify the SSE2 fallback still compiles.
- `ctest -j8`: 8 pre-existing NR-DEC-md5 failures only, no new MD5
  mismatches.
- Smoke diff-test: 90/90 (sanity — both fast and legacy go through the
  same DWT, so this confirms the AVX2 lift doesn't introduce decode
  drift).
- Spot-check: dump pixels from one lossy 9/7 image with the AVX2 build
  and the SSE2 build, cmp bytes.

### Deliverable gate
- Bench archival workload (loc-maps): compare the post-D6.1 baseline (main
  before SP3.1) vs the SP3.1 branch. Target: ≥ 2% gmean improvement on
  lossy 9/7 files. Pass threshold: ≥ 1% improvement.
- Smoke bench: gmean ratio within ±1% (smoke is single-tile synthetic;
  some files use lossy 9/7 too).
- WSI bench (the 30-file subset used at D6.1 Task 4): no regression.

## Scope Boundaries

- **9/7 float only.** The 5/3 integer DWT already has AVX2/AVX-512 paths —
  no further work needed there.
- **AVX2 only.** AVX-512 9/7 lift is a possible future deliverable (call it
  SP3.1b) but excluded here. The bench host doesn't have AVX-512, so
  there's no measurement story for it yet.
- **Lift kernels only.** The `opj_v8dwt_interleave_h/v` functions and the
  `opj_dwt_decode_partial_*` paths stay unchanged. The profile shows
  interleave at 2.51% — sizable but a separate deliverable.
- **No new runtime dispatch.** Compile-time `#ifdef __AVX2__` only.

## Risks

1. **Memory bandwidth ceiling.** Lift is partly memory-bound at high
   resolutions (large tile-components). AVX2 may yield less than 2×
   throughput over SSE2 if memory is the limit. Bench is the truth.

2. **Profile attribution dilution.** The "16% DWT" headline includes the
   wrapper, interleave, and partial-region code. The step1/step2 kernels
   themselves are 2-3%. Best case 2× speedup on the kernels recovers
   1-1.5% of total decode. Pass threshold of 1% on archival is consistent
   with this estimate.

3. **AVX/SSE transition penalty.** Mixed AVX and legacy SSE code can incur
   per-transition stalls on some Intel CPUs. The fix is to use AVX (256-bit
   `vmovaps` etc.) consistently in the entire function, not mix with
   plain SSE `movaps`. Compiler intrinsic naming makes this straightforward:
   stay in `_mm256_*` once entered.

4. **Compiler vectorization regression.** The existing SSE2 code is
   already well-optimized; the compiler may auto-vectorize parts. Adding
   explicit AVX2 intrinsics could (in pathological cases) prevent further
   compiler optimization. Bench will catch.

5. **Tail handling.** `opj_v8_t` packs exactly 8 floats — perfectly
   aligned for AVX2's 256-bit registers. No scalar tail needed within a
   record. Multi-record loops process whole records, so no tail issue at
   that level either.

## Decision Gate

Bench gate after implementation:
- **Archival improvement**: ≥ 1% (pass), ≥ 2% (target).
- **No smoke or WSI regression** beyond ±1%.
- **Conformance**: 8 pre-existing failures only.
- **Build**: both AVX2 and AVX2-disabled builds compile.

If archival improves < 0.5%, investigate before tagging. If smoke or WSI
regresses > 1%, revert.

## Open Questions

None. The design is concrete; the implementation is mechanical translation
of two SSE2 kernels into AVX2 plus call-site coefficient-broadcast tweaks.
The verification path reuses existing infrastructure (conformance, smoke
diff-test, archival bench).
