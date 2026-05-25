# SP3.2b — Int16 5/3 DWT for Partial-Tile (ROI) Decode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend SP3.2's int16 5/3 IDWT path from full-tile decode to partial-tile (ROI) decode. Adds an int16 mirror of the int32 sparse array, an int16 partial-tile orchestrator, and the partial-tile int16 kernels. Same `OPJ_ENABLE_AVX2` + `prec ≤ 8` gate.

**Architecture:** A new `sparse_array_int16` translation unit mirrors `sparse_array.c` with the element-type swap. In `dwt.c`, a new `opj_dwt_decode_partial_tile_int16` orchestrator parallels the existing int32 partial-tile orchestrator: it converts `tilec->data` (int32, T1 output) to int16, populates the int16 sparse array, runs windowed lift per resolution entirely in int16 (using new partial-tile int16 kernels that mirror `opj_dwt_decode_partial_1` and `opj_dwt_decode_partial_1_parallel`), then unpacks back to int32 when writing `tilec->data_win`. `opj_dwt_decode` chooses the orchestrator based on `image_comp.prec` and `__AVX2__` availability. The int32 partial path is untouched.

**Tech Stack:** C99, x86 AVX2/SSE2 intrinsics (`__m256i` with 16 int16 lanes), `OPJ_ENABLE_AVX2` cmake gate from SP3.1, CMake.

---

## File Structure

**Added:**
- `src/lib/openjp2/sparse_array_int16.h` (~140 LOC) — int16 API mirror of `sparse_array.h`. Public-to-the-library only; not in `openjpeg.h`. Head comment documents the parallel-with-int32 invariant.
- `src/lib/openjp2/sparse_array_int16.c` (~350 LOC) — structural mirror of `sparse_array.c` with `OPJ_INT32` → `OPJ_INT16` substitution.

**Modified:**
- `src/lib/openjp2/dwt.c` — add ~500 LOC of partial-tile int16 path:
  - 1 init helper (`opj_dwt_init_sparse_array_int16`) — mirror of `opj_dwt_init_sparse_array` (dwt.c:3750), packs `tilec->data` int32 → int16 via SP3.2's `opj_dwt_pack_int32_to_int16` helper while populating the int16 sparse array.
  - 1 horizontal kernel `opj_dwt_decode_partial_1_int16` — mirror of `opj_dwt_decode_partial_1` (dwt.c:3470), operates on 1 int16 row with windowed bounds.
  - 1 vertical kernel `opj_dwt_decode_partial_1_parallel_int16` — mirror of `opj_dwt_decode_partial_1_parallel` (dwt.c:3555), processes **8 int16 columns** per call (vs 4 int32 columns in the original) using AVX2.
  - 1 orchestrator `opj_dwt_decode_partial_tile_int16` — mirror of `opj_dwt_decode_partial_tile` (dwt.c:3806), allocates int16 scratch, drives per-resolution windowed lift, unpacks final result via SP3.2's `opj_dwt_unpack_int16_to_int32` helper into `tilec->data_win`.
  - Dispatch hook in `opj_dwt_decode` (dwt.c:2847) — adds AVX2 + `prec ≤ 8` gate to the `else` (partial) branch, mirroring the gate already in the `whole_tile_decoding` (full-tile) branch added by SP3.2.

- `src/lib/openjp2/CMakeLists.txt` — register `sparse_array_int16.c` and `sparse_array_int16.h` in the openjp2 source list, next to the existing `sparse_array.c` / `sparse_array.h` entries.

**Not modified:** `t1.c`, `mqc.c`, `mct.c`, `j2k.c`, `tcd.c`, `tcd.h`, `sparse_array.c`, `sparse_array.h`, `openjpeg.c`, `openjpeg.h`, any other file. The int16 partial path activates entirely inside the existing `OPJ_ENABLE_AVX2` envelope.

---

## Task 1: Implement the int16 5/3 partial-tile IDWT path (single atomic commit)

All changes are interdependent — the sparse_array_int16 module, the partial-tile kernels, the orchestrator, the dispatch hook, and the CMakeLists.txt registration must land together. The new sparse_array_int16 functions are otherwise unreferenced and would fail `-Werror=unused-function`. One commit.

**Files:**
- Create: `src/lib/openjp2/sparse_array_int16.h`
- Create: `src/lib/openjp2/sparse_array_int16.c`
- Modify: `src/lib/openjp2/dwt.c`
- Modify: `src/lib/openjp2/CMakeLists.txt`

### Step 1: Read the existing int32 sparse-array module for line-accurate mirroring

```bash
cd /home/cornish/GitHub/openjp2k
wc -l src/lib/openjp2/sparse_array.h src/lib/openjp2/sparse_array.c
sed -n '1,141p' src/lib/openjp2/sparse_array.h   # full header — 141 LOC, mirror entirely
sed -n '1,346p' src/lib/openjp2/sparse_array.c   # full implementation — 346 LOC, mirror entirely
```

Internalize:
- `struct opj_sparse_array_int32` (sparse_array.c:35-43) — block-tiled 2D array, `OPJ_INT32* data_blocks` flat array, block size `(block_width × block_height) × sizeof(OPJ_INT32)`.
- `_create / _free` — straight allocator.
- `_is_region_valid` — bounds check.
- `_read_or_write` (the worker, sparse_array.c:105-310) — 2D copy between sparse blocks and a dense user buffer.
- `_read` / `_write` — thin wrappers over `_read_or_write`.

### Step 2: Read the partial-tile int32 entry points for line-accurate mirroring

```bash
sed -n '3470,3554p' src/lib/openjp2/dwt.c        # opj_dwt_decode_partial_1 (horizontal)
sed -n '3555,3700p' src/lib/openjp2/dwt.c        # opj_dwt_decode_partial_1_parallel (vertical, 4-col SSE2 stripe)
sed -n '3750,3805p' src/lib/openjp2/dwt.c        # opj_dwt_init_sparse_array
sed -n '3806,4040p' src/lib/openjp2/dwt.c        # opj_dwt_decode_partial_tile (orchestrator)
sed -n '2845,2866p' src/lib/openjp2/dwt.c        # opj_dwt_decode dispatch hook (full-tile gate already there)
```

Also re-read the SP3.2 helpers in dwt.c that we'll reuse:
- `opj_dwt_pack_int32_to_int16` — converts an int32 buffer to int16 via `_mm256_packs_epi32` with `_mm256_permute4x64_epi64(_, 0xD8)` cross-lane fixup.
- `opj_dwt_unpack_int16_to_int32` — converts back via `_mm256_cvtepi16_epi32`.
- `VREG_S16` / `LOAD_S16` / `LOADU_S16` / `ADD_S16` / `SUB_S16` / `SAR_S16` macros (added at dwt.c:~750 in SP3.2).

### Step 3: Create `src/lib/openjp2/sparse_array_int16.h`

```c
/*
 * SPDX-License-Identifier: BSD-2-Clause AND Apache-2.0
 *
 * Copyright (c) 2017, IntoPix SA <contact@intopix.com>
 * Copyright (c) 2026, Toby Cornish <tcornish@gmail.com>
 *
 * Structural mirror of sparse_array.h with the element type swapped to
 * OPJ_INT16. Used by the partial-tile int16 5/3 IDWT path (SP3.2b).
 * When updating, keep this in lock-step with sparse_array.h.
 *
 * The original BSD-2-Clause notice on sparse_array.h applies to the
 * mirror structure. New code added in the SP3.2b deliverable is
 * Apache-2.0 (see CONTRIBUTING.md).
 */

#ifndef OPJ_SPARSE_ARRAY_INT16_H
#define OPJ_SPARSE_ARRAY_INT16_H

/* int16_t */
#include <stdint.h>

/** @file sparse_array_int16.h */

typedef struct opj_sparse_array_int16 opj_sparse_array_int16_t;

opj_sparse_array_int16_t* opj_sparse_array_int16_create(OPJ_UINT32 width,
        OPJ_UINT32 height,
        OPJ_UINT32 block_width,
        OPJ_UINT32 block_height);

void opj_sparse_array_int16_free(opj_sparse_array_int16_t* sa);

OPJ_BOOL opj_sparse_array_int16_is_region_valid(
    const opj_sparse_array_int16_t* sa,
    OPJ_UINT32 x0,
    OPJ_UINT32 y0,
    OPJ_UINT32 x1,
    OPJ_UINT32 y1);

OPJ_BOOL opj_sparse_array_int16_read(const opj_sparse_array_int16_t* sa,
                                     OPJ_UINT32 x0, OPJ_UINT32 y0,
                                     OPJ_UINT32 x1, OPJ_UINT32 y1,
                                     int16_t* dest,
                                     OPJ_UINT32 dest_col_stride,
                                     OPJ_UINT32 dest_line_stride,
                                     OPJ_BOOL forgiving);

OPJ_BOOL opj_sparse_array_int16_write(opj_sparse_array_int16_t* sa,
                                      OPJ_UINT32 x0, OPJ_UINT32 y0,
                                      OPJ_UINT32 x1, OPJ_UINT32 y1,
                                      const int16_t* src,
                                      OPJ_UINT32 src_col_stride,
                                      OPJ_UINT32 src_line_stride,
                                      OPJ_BOOL forgiving);

#endif /* OPJ_SPARSE_ARRAY_INT16_H */
```

### Step 4: Create `src/lib/openjp2/sparse_array_int16.c`

Structural copy of `sparse_array.c` with `OPJ_INT32` → `int16_t` and `_int32` → `_int16` substitutions. Three rules:
1. Block-tiled struct layout identical. Only `int16_t* data_blocks` differs.
2. The `_read_or_write` worker is a long 2D-copy routine with unrolling. Substitute element type uniformly; the unroll factors and the block-edge handling logic are unchanged.
3. SPDX header pattern matches the mirrored .h file.

```c
/*
 * SPDX-License-Identifier: BSD-2-Clause AND Apache-2.0
 *
 * Copyright (c) 2017, IntoPix SA <contact@intopix.com>
 * Copyright (c) 2026, Toby Cornish <tcornish@gmail.com>
 *
 * Structural mirror of sparse_array.c with OPJ_INT32 -> int16_t.
 * Keep in lock-step with sparse_array.c when either changes.
 */

#include "opj_includes.h"
#include "sparse_array_int16.h"

struct opj_sparse_array_int16 {
    OPJ_UINT32 width;
    OPJ_UINT32 height;
    OPJ_UINT32 block_width;
    OPJ_UINT32 block_height;
    OPJ_UINT32 block_count_hor;
    OPJ_UINT32 block_count_ver;
    int16_t** data_blocks;
};

/* Body: copy sparse_array.c entirely, applying the substitutions:
 *   OPJ_INT32    -> int16_t
 *   _int32_      -> _int16_
 *   opj_sparse_array_int32_t -> opj_sparse_array_int16_t
 *   opj_sparse_array_is_region_valid -> opj_sparse_array_int16_is_region_valid
 * The unroll factors, block boundary handling, and bound checks remain
 * identical.  Verify the diff vs sparse_array.c shows only those
 * substitutions plus the SPDX header before committing.
 */
```

Implementation note: write the file by reading sparse_array.c top-to-bottom and applying the substitution in each function body. The file should be ~340 LOC (a few lines shorter than the int32 version, since the `int16_t` keyword is shorter than `OPJ_INT32` in declarations). When done, run:

```bash
diff <(sed -E 's/OPJ_INT32/int16_t/g; s/_int32_/_int16_/g; s/opj_sparse_array_int32_t/opj_sparse_array_int16_t/g; s/opj_sparse_array_is_region_valid/opj_sparse_array_int16_is_region_valid/g' src/lib/openjp2/sparse_array.c) src/lib/openjp2/sparse_array_int16.c
```

Only the SPDX header lines should differ. If anything else differs, recheck the substitution.

### Step 5: Register the new files in `src/lib/openjp2/CMakeLists.txt`

```bash
grep -n 'sparse_array' src/lib/openjp2/CMakeLists.txt
```

Add `sparse_array_int16.c` next to `sparse_array.c` in the `OPENJPEG_SRCS` (or equivalent) variable, and `sparse_array_int16.h` next to `sparse_array.h` in the headers list. Exact edit depends on the existing format — typically:

```cmake
  ${CMAKE_CURRENT_SOURCE_DIR}/sparse_array.c
  ${CMAKE_CURRENT_SOURCE_DIR}/sparse_array_int16.c
```

and

```cmake
  ${CMAKE_CURRENT_SOURCE_DIR}/sparse_array.h
  ${CMAKE_CURRENT_SOURCE_DIR}/sparse_array_int16.h
```

### Step 6: Add the int16 init helper to dwt.c

Just below `opj_dwt_init_sparse_array` (currently ending around line 3805), add the int16 mirror:

```c
/* SP3.2b: int16 mirror of opj_dwt_init_sparse_array (dwt.c:3750).
 * Reads tilec->data (int32, output of T1), packs to int16 via the
 * SP3.2 helper, and populates the int16 sparse array. */
static opj_sparse_array_int16_t* opj_dwt_init_sparse_array_int16(
    opj_tcd_tilecomp_t* tilec,
    OPJ_UINT32 numres)
{
    opj_tcd_resolution_t* tr_max = &(tilec->resolutions[numres - 1]);
    OPJ_UINT32 w = (OPJ_UINT32)(tr_max->x1 - tr_max->x0);
    OPJ_UINT32 h = (OPJ_UINT32)(tr_max->y1 - tr_max->y0);
    OPJ_UINT32 resno, bandno, precno, cblkno;

    opj_sparse_array_int16_t* sa = opj_sparse_array_int16_create(
                                       w, h,
                                       opj_uint_min(w, 64),
                                       opj_uint_min(h, 64));
    if (sa == NULL) {
        return NULL;
    }

    for (resno = 0; resno < numres; ++resno) {
        opj_tcd_resolution_t* res = &tilec->resolutions[resno];
        for (bandno = 0; bandno < res->numbands; ++bandno) {
            opj_tcd_band_t* band = &res->bands[bandno];
            for (precno = 0; precno < res->pw * res->ph; ++precno) {
                opj_tcd_precinct_t* precinct = &band->precincts[precno];
                for (cblkno = 0; cblkno < precinct->cw * precinct->ch; ++cblkno) {
                    opj_tcd_cblk_dec_t* cblk = &precinct->cblks.dec[cblkno];
                    if (cblk->decoded_data != NULL) {
                        OPJ_UINT32 x = (OPJ_UINT32)(cblk->x0 - band->x0);
                        OPJ_UINT32 y = (OPJ_UINT32)(cblk->y0 - band->y0);
                        OPJ_UINT32 cblk_w = (OPJ_UINT32)(cblk->x1 - cblk->x0);
                        OPJ_UINT32 cblk_h = (OPJ_UINT32)(cblk->y1 - cblk->y0);

                        if (band->bandno & 1) {
                            opj_tcd_resolution_t* pres = &tilec->resolutions[resno - 1];
                            x += (OPJ_UINT32)(pres->x1 - pres->x0);
                        }
                        if (band->bandno & 2) {
                            opj_tcd_resolution_t* pres = &tilec->resolutions[resno - 1];
                            y += (OPJ_UINT32)(pres->y1 - pres->y0);
                        }

                        /* Pack cblk_w * cblk_h int32 values into an int16
                         * scratch buffer, then write to the sparse array.
                         * For small cblks the cost is negligible; for
                         * larger ones the AVX2 helper amortizes. */
                        {
                            int16_t* tmp = (int16_t*)opj_aligned_32_malloc(
                                              (size_t)cblk_w * cblk_h * sizeof(int16_t));
                            if (!tmp) {
                                opj_sparse_array_int16_free(sa);
                                return NULL;
                            }
                            opj_dwt_pack_int32_to_int16(tmp, cblk->decoded_data,
                                                        (size_t)cblk_w * cblk_h);
                            if (!opj_sparse_array_int16_write(sa, x, y,
                                                              x + cblk_w, y + cblk_h,
                                                              tmp, 1, cblk_w,
                                                              OPJ_TRUE)) {
                                opj_aligned_free(tmp);
                                opj_sparse_array_int16_free(sa);
                                return NULL;
                            }
                            opj_aligned_free(tmp);
                        }
                    }
                }
            }
        }
    }
    return sa;
}
```

Verify the int32 init helper structure matches by re-reading dwt.c:3750-3805 — if upstream's version differs in detail (e.g., differently-named cblk fields), copy that detail exactly. The only behavioral difference here is the per-cblk pack-to-int16 + int16 write.

### Step 7: Add the int16 horizontal partial kernel to dwt.c

Just below `opj_dwt_decode_partial_1` (currently around line 3470-3554), add `opj_dwt_decode_partial_1_int16` as a structural mirror. The kernel is a 1-row windowed lift:

```c
/* SP3.2b: int16 mirror of opj_dwt_decode_partial_1 (dwt.c:3470).
 * Same 5/3 lift math, same windowing logic; the only change is the
 * element type and the AVX2 widening (8 int32 -> 16 int16 lanes). */
static void opj_dwt_decode_partial_1_int16(int16_t *a, OPJ_INT32 dn, OPJ_INT32 sn,
        OPJ_INT32 cas,
        OPJ_INT32 win_l_x0,
        OPJ_INT32 win_l_x1,
        OPJ_INT32 win_h_x0,
        OPJ_INT32 win_h_x1)
{
    /* Body: copy the int32 version line-by-line, substituting:
     *   OPJ_INT32 *a -> int16_t *a
     *   OPJ_S, OPJ_D, OPJ_S_, OPJ_D_ macros remain unchanged (they index
     *   the array; element type follows the pointer)
     * The naive scalar fallback path needs no other change.  The SSE2
     * inner loop (line 3598-3640) — if present in this kernel — should
     * be left as-is, since the partial-1 horizontal kernel is row-at-a-
     * time and the SSE2 trick was only in the vertical _parallel kernel.
     */
}
```

Read dwt.c:3470-3554 line-by-line and write the mirror verbatim with element-type substitution. The kernel is single-row, so no SIMD widening is needed inside it — the int16 win is at the orchestrator's call site (more rows fit in the same cache, and the vertical kernel processes more columns per stripe).

### Step 8: Add the int16 vertical partial-multicol kernel to dwt.c

Below `opj_dwt_decode_partial_1_parallel` (dwt.c:3555-~3700), add `opj_dwt_decode_partial_1_parallel_int16`. **This is where the AVX2 win lives** — process 8 int16 columns per stripe (vs 4 int32 columns in the SSE2 original):

```c
/* SP3.2b: int16 mirror of opj_dwt_decode_partial_1_parallel (dwt.c:3555).
 * Processes 8 int16 columns per stripe using AVX2 (vs 4 int32 columns
 * via SSE2 in the int32 original).  The stripe stride is 8 elements per
 * row (= 16 bytes per row, same total bytes as 4-int32 stripe — easier
 * cache reasoning).  Windowing logic is identical. */
static void opj_dwt_decode_partial_1_parallel_int16(int16_t *a,
        OPJ_UINT32 nb_cols,
        OPJ_INT32 dn, OPJ_INT32 sn,
        OPJ_INT32 cas,
        OPJ_INT32 win_l_x0,
        OPJ_INT32 win_l_x1,
        OPJ_INT32 win_h_x0,
        OPJ_INT32 win_h_x1)
{
    /* Body: copy the int32 version, replacing the SSE2 8-byte stripe block
     * with an AVX2 16-byte (8 int16) stripe.  Element-access macros
     * (OPJ_S_off, OPJ_D_off, etc.) take an `off` parameter that selects
     * within the stripe — bump the stride from 8 (= 4 int32 elements
     * per row) to 8 (= 8 int16 elements per row); same byte count, twice
     * the elements.
     *
     *   __m128i / _mm_load_si128 / _mm_add_epi32 / _mm_srai_epi32 / ...
     *      -> __m128i / _mm_load_si128 / _mm_add_epi16 / _mm_srai_epi16 / ...
     *   _mm_set1_epi32(2)        -> _mm_set1_epi16(2)
     *
     * Constants (the +2 rounding addend, the shift amounts) are identical
     * to the int32 version — 5/3 lift math is integer with shifts only.
     *
     * Naive scalar fallback (the #else branch) substitutes int32 -> int16
     * uniformly; no other change.
     */
}
```

This step is the most error-prone. After writing, sanity-check by re-reading the int32 kernel's SSE2 block side-by-side and confirming every intrinsic was substituted (no leftover `_mm_*_epi32` in the int16 path).

### Step 9: Add the int16 partial-tile orchestrator to dwt.c

Just below `opj_dwt_decode_partial_tile` (dwt.c:3806-~4045), add:

```c
/* SP3.2b: int16 mirror of opj_dwt_decode_partial_tile (dwt.c:3806).
 * Activated when image_comp->prec <= 8 in opj_dwt_decode's else branch. */
static OPJ_BOOL opj_dwt_decode_partial_tile_int16(
    opj_tcd_tilecomp_t* tilec,
    OPJ_UINT32 numres)
{
    /* Body: structural mirror of opj_dwt_decode_partial_tile, with:
     *
     * 1. opj_sparse_array_int32_t -> opj_sparse_array_int16_t
     * 2. opj_dwt_init_sparse_array(...) -> opj_dwt_init_sparse_array_int16(...)
     * 3. h.mem and v.mem switch from OPJ_INT32* to int16_t*; allocation
     *    size uses sizeof(int16_t).  The 4x multiplier for column
     *    stride doubles to 8x because we process 8 int16 columns per
     *    stripe.
     * 4. opj_dwt_decode_partial_1 / _parallel calls -> _int16 variants.
     * 5. Final write to tilec->data_win: read int16 from the sparse
     *    array into an int16 scratch, then unpack via
     *    opj_dwt_unpack_int16_to_int32 into tilec->data_win.
     *
     * Single-threaded — no thread-pool argument, matching the int32
     * partial path.  whole_tile_decoding == OPJ_FALSE precondition is
     * checked by the dispatcher in opj_dwt_decode.
     */
}
```

For the final write step (point 5 above), the int32 partial orchestrator currently does:

```c
opj_sparse_array_int32_read(sa, ..., tilec->data_win, ...);
```

The int16 mirror needs an extra unpack:

```c
{
    OPJ_UINT32 win_w = tr_max->win_x1 - tr_max->win_x0;
    OPJ_UINT32 win_h = tr_max->win_y1 - tr_max->win_y0;
    size_t n = (size_t)win_w * win_h;
    int16_t* scratch = (int16_t*)opj_aligned_32_malloc(n * sizeof(int16_t));
    if (!scratch) {
        opj_sparse_array_int16_free(sa);
        opj_aligned_free(h.mem);
        return OPJ_FALSE;
    }
    OPJ_BOOL ok = opj_sparse_array_int16_read(sa,
                  tr_max->win_x0 - (OPJ_UINT32)tr_max->x0,
                  tr_max->win_y0 - (OPJ_UINT32)tr_max->y0,
                  tr_max->win_x1 - (OPJ_UINT32)tr_max->x0,
                  tr_max->win_y1 - (OPJ_UINT32)tr_max->y0,
                  scratch, 1, win_w, OPJ_TRUE);
    if (ok) {
        opj_dwt_unpack_int16_to_int32(tilec->data_win, scratch, n);
    }
    opj_aligned_free(scratch);
    if (!ok) {
        opj_sparse_array_int16_free(sa);
        opj_aligned_free(h.mem);
        return OPJ_FALSE;
    }
}
```

### Step 10: Add the dispatch hook in `opj_dwt_decode`

Locate dwt.c:2845-2866 (the `opj_dwt_decode` function). It currently looks like:

```c
OPJ_BOOL opj_dwt_decode(opj_tcd_t *p_tcd, opj_tcd_tilecomp_t* tilec,
                        OPJ_UINT32 numres)
{
    if (p_tcd->whole_tile_decoding) {
#ifdef __AVX2__
        /* SP3.2: int16 path for <=8-bit precision components. */
        {
            const OPJ_UINT32 compno = (OPJ_UINT32)(tilec -
                                      p_tcd->tcd_image->tiles->comps);
            const opj_image_comp_t *image_comp = &p_tcd->image->comps[compno];
            if (image_comp->prec <= 8) {
                return opj_dwt_decode_tile_int16(p_tcd->thread_pool, tilec, numres);
            }
        }
#endif
        return opj_dwt_decode_tile(p_tcd->thread_pool, tilec, numres);
    } else {
        return opj_dwt_decode_partial_tile(tilec, numres);
    }
}
```

Replace the `else` branch with the symmetric int16 gate:

```c
    } else {
#ifdef __AVX2__
        /* SP3.2b: int16 path for <=8-bit precision components. */
        {
            const OPJ_UINT32 compno = (OPJ_UINT32)(tilec -
                                      p_tcd->tcd_image->tiles->comps);
            const opj_image_comp_t *image_comp = &p_tcd->image->comps[compno];
            if (image_comp->prec <= 8) {
                return opj_dwt_decode_partial_tile_int16(tilec, numres);
            }
        }
#endif
        return opj_dwt_decode_partial_tile(tilec, numres);
    }
```

### Step 11: Build with `OPJ_ENABLE_AVX2=ON` and confirm no warnings

```bash
cd /home/cornish/GitHub/openjp2k
cmake -S . -B build-sp32b -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=ON -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-sp32b -j 2>&1 | tail -10
```

Expected: clean build, no warnings. If `-Werror=unused-function` fires on any new int16 symbol, check that the symbol is referenced in the orchestrator or the dispatch hook. If it fires on an int32 symbol, that means we accidentally broke a call site — re-read the diff against `git diff src/lib/openjp2/dwt.c`.

### Step 12: Build with `OPJ_ENABLE_AVX2=OFF` and confirm it still works

```bash
cmake -S . -B build-noavx2 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=OFF -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-noavx2 -j 2>&1 | tail -5
```

Expected: clean build. The new int16 path is `#ifdef __AVX2__`-gated, so AVX2-OFF compiles the int32 partial path only. If `sparse_array_int16.c` causes a warning when nothing references it under AVX2-OFF, mark its definitions with `#ifdef __AVX2__` at function bodies — but the prefered approach is to leave the int16 sparse-array module fully compiled (it's not AVX2-dependent itself, only its caller is). Easiest: check `-Wunused-function` is silenced on these static-but-conditionally-called symbols by adding `OPJ_LOCAL` or `(void)opj_dwt_decode_partial_tile_int16;` if needed; otherwise rely on the link-pruner.

### Step 13: Run conformance with `OPJ_ENABLE_AVX2=ON`

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run-conformance.sh -B build-sp32b -- -R NR-DEC -j4 2>&1 | tail -15
```

Expected: 99% pass (553 tests), exactly **8 failures**, identical to the SP3.2 baseline:
- NR-DEC-_00042.j2k-2-decode-md5
- NR-DEC-kodak_2layers_lrcp.j2c-31-decode-md5
- NR-DEC-kodak_2layers_lrcp.j2c-32-decode-md5
- NR-DEC-issue205.jp2-43-decode-md5
- NR-DEC-issue135.j2k-68-decode-md5
- NR-DEC-issue208.jp2-69-decode-md5
- NR-DEC-issue226.j2k-74-decode
- NR-DEC-issue226.j2k-74-decode-md5

**Any 9th failure → revert + re-investigate.** Common new-failure causes: wrong stripe stride in the vertical kernel, off-by-one in the window-edge handling, or skipping a `_mm256_permute4x64_epi64` cross-lane fixup in the int16 pack.

### Step 14: Manual byte-cmp — three 8-bit files × three regions

Pick three 8-bit input files; for each, decode three regions with `OPJ_ENABLE_AVX2=ON` and `OFF`, and byte-cmp the raw outputs.

```bash
# Inputs
F1=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_mon_enone.jp2
F2=$HOME/GitHub/openjp2k-data/corpus/archival/loc-maps/07481_1926-0001.jp2
F3=$HOME/GitHub/openjp2k-data/corpus/medical/J2KR_US1_J2KR.j2k

for F in "$F1" "$F2" "$F3"; do
  for REGION in "" "-d 256,256,768,768" "-d 0,0,512,512"; do
    build-sp32b/bin/opj_decompress -allow-partial $REGION -i "$F" -o /tmp/avx2on.raw  > /dev/null 2>&1
    build-noavx2/bin/opj_decompress -allow-partial $REGION -i "$F" -o /tmp/avx2off.raw > /dev/null 2>&1
    if cmp -s /tmp/avx2on.raw /tmp/avx2off.raw; then
      echo "OK   $(basename "$F")  region='$REGION'"
    else
      echo "FAIL $(basename "$F")  region='$REGION'   ($(cmp /tmp/avx2on.raw /tmp/avx2off.raw 2>&1 | head -1))"
    fi
  done
done
```

Expected: all 9 lines say `OK`. The `-d` coordinates here are absolute tile coordinates; for files larger than 1024×1024 they decode a 512×512 region. Region values may need adjustment if a file is smaller than 512² (e.g., `J2KR_US1_J2KR` is 640×480 — use `-d 160,120,480,360` for the center-50% of it). Adjust per file; the principle is the same.

**If any FAIL:** the int16 path is producing different bytes than int32. The lift math is integer with shifts and *must* be byte-exact. Likely culprits: signed/unsigned conversion in the pack, incorrect 4x64-epi64 lane permute, or a bug in the vertical 8-column stripe. Bisect by checking which region (full vs center vs top-left) fails first.

### Step 15: Stage and commit

```bash
git add src/lib/openjp2/sparse_array_int16.h \
        src/lib/openjp2/sparse_array_int16.c \
        src/lib/openjp2/dwt.c \
        src/lib/openjp2/CMakeLists.txt

git status --short
git diff --cached --stat
```

Expected stat: 2 new files (~140 + ~340 LOC), dwt.c +~500 LOC, CMakeLists.txt +~2 LOC.

```bash
git commit -m "$(cat <<'EOF'
DWT: int16 partial-tile (ROI) path for <=8-bit precision (SP3.2b)

Extends SP3.2's int16 5/3 path from the full-tile orchestrator to the
partial-tile orchestrator.  Adds:

- sparse_array_int16.{h,c}: int16 element-type mirror of the existing
  int32 sparse array, used by the partial-tile data store.  Keep in
  lock-step with sparse_array.c.
- dwt.c: opj_dwt_init_sparse_array_int16, opj_dwt_decode_partial_1_int16
  (windowed row), opj_dwt_decode_partial_1_parallel_int16 (8-col AVX2
  stripe — twice the elements of the int32 SSE2 4-col stripe in the
  same vector bytes), and opj_dwt_decode_partial_tile_int16 orchestrator.

Dispatch gate in opj_dwt_decode mirrors the full-tile gate (SP3.2):
prec<=8 + __AVX2__ -> int16 partial path; else the int32 partial path
runs unchanged.

The int32 partial path is untouched.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: ROI smoke bench — baseline + measurement

Single new script in the bench repo, plus two bench runs (SP3.2 baseline + SP3.2b) for paired-ratio analysis.

**Files:**
- Create: `~/GitHub/openjp2k-bench/scripts/run_smoke_roi.sh`

### Step 1: Write the ROI smoke wrapper

```bash
cat > ~/GitHub/openjp2k-bench/scripts/run_smoke_roi.sh <<'EOF'
#!/usr/bin/env bash
# SP3.2b ROI smoke variant: same 90-file synthetic-iter manifest as
# run_smoke.sh, but with a fixed center-50% region (512x512@256,256 on
# the 1024^2 rasters).  Used by the SP3.2b decision gate.
#
# Usage: ./scripts/run_smoke_roi.sh [extra args forwarded to run_bench.sh]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MANIFEST="${ROOT}/corpus/synthetic-iter.txt"
if [ ! -r "$MANIFEST" ]; then
  echo "missing $MANIFEST; regenerate via scripts/select_synthetic_iter.py" >&2
  exit 1
fi
exec "$ROOT/scripts/run_bench.sh" --include-from "$MANIFEST" \
     --roi 512x512@256,256 "$@"
EOF
chmod +x ~/GitHub/openjp2k-bench/scripts/run_smoke_roi.sh
```

### Step 2: Rebuild the bench against the SP3.2 baseline worktree

The SP3.1-baseline worktree from the SP3.2 session was already removed. Create a new SP3.2-baseline worktree at the SP3.2 outcome commit `bb8c376e` (the head of the SP3.2 branch before SP3.2b):

```bash
cd /home/cornish/GitHub/openjp2k
git worktree add /home/cornish/GitHub/openjp2k-sp32-baseline bb8c376e
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source ~/GitHub/openjp2k-sp32-baseline 2>&1 | tail -5
```

Expected: clean bench rebuild against the SP3.2 baseline source.

### Step 3: Run the ROI smoke against the SP3.2 baseline

```bash
cd ~/GitHub/openjp2k-bench
nohup ./scripts/run_smoke_roi.sh > results/roi_smoke_sp32_baseline_$(date +%Y%m%d_%H%M%S).jsonl 2> results/roi_smoke_sp32_baseline_$(date +%Y%m%d_%H%M%S).log &
wait %1 && echo DONE
```

Expected: ~10-15 minutes (90 files × 3 decoders × 20 iters, center-50% ROI). The JSONL has ~270 result rows.

### Step 4: Rebuild bench against the SP3.2b head and re-run

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source ~/GitHub/openjp2k 2>&1 | tail -5
nohup ./scripts/run_smoke_roi.sh > results/roi_smoke_sp32b_$(date +%Y%m%d_%H%M%S).jsonl 2> results/roi_smoke_sp32b_$(date +%Y%m%d_%H%M%S).log &
wait %1 && echo DONE
```

### Step 5: Compute the paired-ratio SP3.2b delta

Save this script as a one-off (or run via `python3 <<PY`). It mirrors the SP3.2 analysis pattern: per-file `openjpeg/openjp2k` within-run ratios, then ratio-of-ratios across runs.

```python
import json, math, glob
from collections import defaultdict

def load(p):
    out, meta = {}, {}
    for l in open(p):
        if '"type"' not in l: continue
        r = json.loads(l)
        if r.get('type') != 'result' or r.get('threads', 1) != 1: continue
        if r.get('error') or r['timing_s']['min'] <= 0: continue
        out.setdefault(r['file'], {})[r['decoder']] = r['timing_s']['min'] * 1000.0
        meta[r['file']] = (r['bit_depth'], r['channels'])
    return out, meta

# Auto-pick the newest pair
base_path = sorted(glob.glob('results/roi_smoke_sp32_baseline_*.jsonl'))[-1]
head_path = sorted(glob.glob('results/roi_smoke_sp32b_*.jsonl'))[-1]
print(f"baseline: {base_path}\nhead:     {head_path}\n")

base, m_b = load(base_path)
head, m_h = load(head_path)

def gm(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(map(math.log, xs)) / len(xs)) if xs else None

slices = defaultdict(list)
abs_b, abs_h = defaultdict(list), defaultdict(list)
for f in set(base) & set(head):
    a_b, a_h = base[f], head[f]
    if not all(d in a_b and d in a_h for d in ('openjp2k', 'openjpeg')): continue
    r_b = a_b['openjpeg'] / a_b['openjp2k']
    r_h = a_h['openjpeg'] / a_h['openjp2k']
    delta = r_h / r_b
    bd, _ = m_h[f]
    mode = 'lossless' if 'lossless' in f else ('lossy' if 'lossy' in f else '?')
    for k in ('overall', f'{bd}-bit', f'{bd}-bit {mode}'):
        slices[k].append(delta)
        abs_b[k].append(r_b); abs_h[k].append(r_h)

print(f"{'Slice':28} {'n':>3}   SP3.2b delta              sp32-vs-openjpeg  sp32b-vs-openjpeg")
print('-' * 102)
for k in ['overall',
          '8-bit', '8-bit lossless', '8-bit lossy',
          '12-bit', '12-bit lossless', '12-bit lossy',
          '16-bit', '16-bit lossless', '16-bit lossy']:
    d, b, h = slices.get(k, []), abs_b.get(k, []), abs_h.get(k, [])
    gd, gb, gh = gm(d), gm(b), gm(h)
    pd = f"{(gd-1)*100:+6.2f}% (gm {gd:.4f})" if gd else '—'
    pb = f"{(gb-1)*100:+6.2f}%" if gb else '—'
    ph = f"{(gh-1)*100:+6.2f}%" if gh else '—'
    print(f"{k:28} {len(d):>3}   {pd:25} {pb:>10}        {ph:>10}")
```

Expected pattern:
- **8-bit lossless delta**: ≥ +1% (gate pass), ideally ≥ +2% (target).
- **Other slices**: within ±1% (no regression), possibly slight unexpected wins per SP3.2 retro.
- **Smoke overall**: positive small number or near 0%; > +1% in the *positive* direction is also OK and gets documented as an accidental cross-slice win in the outcome.

If 8-bit lossless lands below +0.5%, **stop and investigate** before tagging — could be sparse-array overhead dominating on small windows, or a kernel that didn't actually vectorize.

### Step 6: Commit the bench wrapper

```bash
cd ~/GitHub/openjp2k-bench
git add scripts/run_smoke_roi.sh
git commit -m "$(cat <<'EOF'
scripts: add run_smoke_roi.sh for SP3.2b partial-decode bench

Center-50% (512x512@256,256) ROI variant of run_smoke.sh, using the
same 90-file synthetic-iter manifest.  Powers the SP3.2b decision
gate's "ROI smoke 8-bit lossless" row.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push origin main 2>&1 | tail -3
```

---

## Task 3: Outcome spec + merge + tag

**Files:**
- Modify: `docs/superpowers/specs/2026-05-24-sp3-2b-int16-partial-dwt-design.md`

### Step 1: Append an Outcome section to the SP3.2b design spec

Mirror the structure of the SP3.2 outcome (`docs/superpowers/specs/2026-05-24-sp3-2-int16-53-dwt-design.md` Outcome §). Sections:
- **Outcome (date)** — one paragraph naming the commits.
- **Bench — SP3.2b isolated vs SP3.2 final** — the table from Task 2 Step 5.
- **Spec deviations** — any (likely none; if the cross-slice surprise reappears, note it).
- **Carry-over** — cumulative project state, next candidates.

Use the SP3.2 outcome section as a template (read it first):

```bash
sed -n '/^## Outcome/,/^## /p' docs/superpowers/specs/2026-05-24-sp3-2-int16-53-dwt-design.md | head -100
```

### Step 2: Commit the outcome spec

```bash
git add docs/superpowers/specs/2026-05-24-sp3-2b-int16-partial-dwt-design.md
git commit -m "$(cat <<'EOF'
Spec: SP3.2b outcome — <fill in headline numbers from Task 2 Step 5>

Records paired-ratio bench results vs SP3.2 final.  ROI 8-bit lossless:
<actual %>.  Conformance: 8 pre-existing failures only with
OPJ_ENABLE_AVX2=ON.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Step 3: Tag and push

Per the SP3 versioning convention (`v0.8.0-sp3-2`, predecessor), tag the SP3.2b deliverable `v0.9.0-sp3-2b-int16-partial-dwt`:

```bash
git tag -a v0.9.0-sp3-2b-int16-partial-dwt -m "SP3.2b — int16 5/3 partial-tile DWT for <=8-bit precision (<headline %>)"
git push origin main
git push origin v0.9.0-sp3-2b-int16-partial-dwt
```

### Step 4: Update the auto-memory retrospective

Append an SP3.2b retrospective entry to the memory index. Use the SP3.2 retrospective as a template:

```bash
ls /home/cornish/.claude/projects/-home-cornish-GitHub-openjp2k/memory/sp3-2-retrospective.md
```

Save a new `sp3-2b-retrospective.md` next to it with: what landed, what we learned (sparse-array mirror cost, partial-decode T1-vs-DWT fraction, any cross-slice surprise reproducibility data), and add a one-line pointer in `MEMORY.md`.

### Step 5: Clean up the SP3.2 baseline worktree

```bash
git worktree remove /home/cornish/GitHub/openjp2k-sp32-baseline
git worktree list   # should show only the main worktree
```

---

## Summary checklist

- [ ] Task 1: SP3.2b implementation atomic commit landed; AVX2-ON + AVX2-OFF builds clean; conformance shows 8 baseline failures only; 9-of-9 byte-cmp passes.
- [ ] Task 2: ROI smoke wrapper added to bench repo, two ROI smoke runs completed (baseline + head), paired-ratio analysis run, gate verdict known.
- [ ] Task 3: Outcome spec appended + committed; tag `v0.9.0-sp3-2b-int16-partial-dwt` published; retrospective memory saved; baseline worktree cleaned up.
