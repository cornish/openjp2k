# D6.1 (phase 1) — TCD Tile-Component Buffer Pool

## Goal

Eliminate the per-tile `malloc`/`free` churn on `tilec->data` that the post-D1
profile flagged as **11.71% of decode time on a 1120-tile archival workload**
(3360 malloc/free pairs averaging ~256 KB each).

Replace it with a per-component buffer pool scoped to the `opj_tcd_t` object
(which already persists across all tiles of an image decode). First-tile-per-
component triggers a malloc; subsequent tiles reuse and grow monotonically.

This is **phase 1**. After landing, evaluate whether to expand to:
- `tilec->data_win` (the ROI/subtile buffer) — same allocation pattern.
- `opj_tcd_tilecomp_t` struct persistence — bigger refactor; pools the struct
  itself across tiles instead of just the data buffer.

## Background

`opj_tcd_decode_tile` decodes one tile of a multi-tile image. It allocates
the tile-component pixel buffers via `opj_alloc_tile_component_data`
(`tcd.c:760`) which currently looks like:

```c
if ((l_tilec->data == 00) || ...) {
    l_tilec->data = (OPJ_INT32 *) opj_image_data_alloc(l_tilec->data_size_needed);
    ...
    l_tilec->ownsData = OPJ_TRUE;
}
```

At tile teardown (`tcd.c:1989`):

```c
if (l_tile_comp->ownsData && l_tile_comp->data) {
    opj_image_data_free(l_tile_comp->data);
    l_tile_comp->data = 00;
    l_tile_comp->ownsData = 0;
    ...
}
```

There's already a high-water-mark amortization inside `opj_alloc_tile_component_data`
— but it's defeated because the entire `opj_tcd_tilecomp_t` array is freed at
tile teardown (`tcd.c:2002`: `opj_free(l_tile->comps)`) and re-allocated by the
next `opj_tcd_init_decode_tile`. Each new tilec arrives with `data == NULL,
data_size == 0`, so the allocator fires unconditionally.

The `opj_tcd_t` object (`p_j2k->m_tcd`) persists for the full image decode;
it's the right home for the pool.

## Design

### Data structure

Add to `opj_tcd_t` (in `tcd.h`):

```c
typedef struct opj_tcd_pool_slot {
    OPJ_INT32 *buf;        /* pooled buffer (NULL if slot empty) */
    OPJ_SIZE_T size;       /* allocated size of buf in bytes (0 if NULL) */
    OPJ_BOOL   lent;       /* TRUE while a tilec holds this buffer */
} opj_tcd_pool_slot_t;

typedef struct opj_tcd_buffer_pool {
    opj_tcd_pool_slot_t *slots;     /* numcomps slots; NULL until pool inited */
    OPJ_UINT32 numcomps;            /* matches image->numcomps */
} opj_tcd_buffer_pool_t;
```

Add `opj_tcd_buffer_pool_t data_pool;` to `opj_tcd_t`.

### Lifecycle

**Pool init.** In `opj_tcd_init` (around `tcd.c:714`), after the image is
bound to the TCD: allocate `slots = calloc(image->numcomps, sizeof(slot))`.
All slots start `{buf=NULL, size=0, lent=FALSE}`.

**Pool destroy.** In `opj_tcd_destroy` (around `tcd.c:744`): walk the slots,
`opj_image_data_free(slot->buf)` for each non-NULL buf, then `opj_free(slots)`.

**Buffer handout.** Replace the existing branch in `opj_alloc_tile_component_data`:

```c
OPJ_BOOL opj_alloc_tile_component_data(opj_tcd_tilecomp_t *l_tilec)
{
    /* Fast path: the tilec already holds a buffer big enough. */
    if (l_tilec->data != 00 && l_tilec->data_size_needed <= l_tilec->data_size) {
        return OPJ_TRUE;
    }

    /* New path: check the pool. The pool is in the parent TCD; the tilec
     * needs to reach it. tilec already has a back-pointer to the parent
     * tile, which has the component index. We thread the pool reference
     * via a new arg to opj_alloc_tile_component_data (see "API change"). */

    opj_tcd_pool_slot_t *slot = &pool->slots[component_index];
    if (slot->buf == NULL || slot->size < l_tilec->data_size_needed) {
        /* Grow or first-allocate. */
        opj_image_data_free(slot->buf);
        slot->buf = (OPJ_INT32 *) opj_image_data_alloc(l_tilec->data_size_needed);
        if (slot->buf == NULL) { slot->size = 0; return OPJ_FALSE; }
        slot->size = l_tilec->data_size_needed;
    }
    /* Free any old data the tilec had (corner case: tile got reinit
     * without passing through teardown). Pool-borrowed buffer cannot be
     * free'd via image_data_free since the pool still tracks it. */
    if (l_tilec->ownsData && l_tilec->data) {
        opj_image_data_free(l_tilec->data);
    }
    l_tilec->data      = slot->buf;
    l_tilec->data_size = slot->size;
    l_tilec->ownsData  = OPJ_FALSE;   /* pool owns it */
    slot->lent         = OPJ_TRUE;
    return OPJ_TRUE;
}
```

**Buffer return.** Replace the teardown block at `tcd.c:1989`:

```c
/* If tilec borrowed from the pool, just clear the back-ref. The pool
 * keeps the buffer for the next tile. */
if (!l_tile_comp->ownsData && l_tile_comp->data) {
    /* Find the pool slot and clear `lent`. We need the component index;
     * see "Component index" below. */
    opj_tcd_pool_slot_t *slot = &pool->slots[component_index];
    if (slot->buf == l_tile_comp->data) {
        slot->lent = OPJ_FALSE;
    }
    l_tile_comp->data = 00;
    l_tile_comp->data_size = 0;
}
/* Legacy: if ownsData (no pool, e.g. caller-provided buffer for whole-tile
 * decode into image-pixels), free as before. */
if (l_tile_comp->ownsData && l_tile_comp->data) {
    opj_image_data_free(l_tile_comp->data);
    l_tile_comp->data = 00;
    l_tile_comp->ownsData = 0;
    l_tile_comp->data_size = 0;
    l_tile_comp->data_size_needed = 0;
}
```

### Component index plumbing

`opj_alloc_tile_component_data` currently takes `opj_tcd_tilecomp_t *` and
nothing else. It doesn't know which component this tilec is, and the tilec
struct doesn't store its own index.

**Option A (recommended):** Add a `comp_no` field to `opj_tcd_tilecomp_t` at
init time. `opj_tcd_init_tile` already loops `for (compno = 0; ...)` so the
index is in scope. Set `l_tilec->comp_no = compno` and `l_tilec->parent_tcd
= p_tcd` once during init.

**Option B:** Change the signature of `opj_alloc_tile_component_data` to take
the TCD and component index. Slightly invasive — there are call sites in `j2k.c`
that would need updating.

Going with A — minimal disruption, adds two fields to the tilec struct.

### "Caller-owned data" path (whole-tile decode)

`j2k.c:12692` sets `l_tilec->ownsData = OPJ_FALSE` for the whole-tile case
where `tilec->data` points at the image's output pixel buffer (no allocation
happens for the tilec itself). That path predates this pool work and must
keep working unchanged: `ownsData = FALSE` and `data != NULL` means
"caller-owned", not "pool-owned." The teardown must distinguish.

Disambiguator: a third flag `OPJ_BOOL data_from_pool` on the tilec. Or a
sentinel value (e.g., pool buffers always start at slot->buf, so equality
check `tilec->data == pool->slots[comp_no].buf` is the disambiguator). The
sentinel form has fewer struct changes; use it.

Caller-owned: `ownsData == FALSE && data != pool->slots[comp_no].buf`.
Pool-owned:   `ownsData == FALSE && data == pool->slots[comp_no].buf`.
Self-owned:   `ownsData == TRUE` (legacy, e.g., subtile non-shared case).

### Files modified

| file | change |
|---|---|
| `src/lib/openjp2/tcd.h` | add `opj_tcd_pool_slot_t` + `opj_tcd_buffer_pool_t`; field on `opj_tcd_t`; add `comp_no` / `parent_tcd` fields on `opj_tcd_tilecomp_t` |
| `src/lib/openjp2/tcd.c` | pool init in `opj_tcd_init`; pool destroy in `opj_tcd_destroy`; rewrite `opj_alloc_tile_component_data`; rewrite teardown branch in `opj_tcd_decode_tile`; set `comp_no`/`parent_tcd` in `opj_tcd_init_tile` |

No changes to `j2k.c`, `t1.c`, `t1_fast.c`, `dwt.c`, `mct.c`, `openjpeg.h/.c`.
ABI surface unchanged (the `opj_tcd_*` types are private to the library).

## Verification

### Per-task
- Smoke diff-test (90/90 byte-identical). The pool change is invisible to
  T1/DWT/MCT — the buffers it hands out are the same memory the legacy path
  would have allocated.
- `ctest -j8` — same 8 pre-existing NR-DEC-md5 failures unchanged.
- Valgrind run on one representative multi-tile file to confirm no
  double-free or use-after-free (the new ownership semantics warrant this).

### Deliverable gate
- **Bench focus: archival workload** (the profile-flagged 11.71%). Smoke is
  synthetic-single-tile and won't exercise the pool. Use a small archival
  + multi-tile subset (loc-maps + 1-2 multi-tile DICOMs).
- Compare D3-revert baseline (current `main`) vs branch with pool. Pass:
  decode wall time on archival improves measurably; smoke is unchanged
  (within ±1% noise floor).
- Conformance pass.
- Worst-loser diff-test on the same 30 files used by D1/D3.

## Scope Boundaries

- Phase 1 only: `tilec->data`. `data_win` and tilec-struct persistence are
  separate follow-up phases evaluated after this lands.
- No public API change.
- No new build flags.
- No changes to encoding path (TCD is shared but encoding uses different
  alloc patterns; left for a future audit).

## Risks

1. **Subtile/ROI decode path.** When the user requests a sub-region decode,
   `tilec->data` may be aliased to the user's output buffer (`ownsData = FALSE`,
   buffer is caller-provided). The pool must not touch that. Mitigated by the
   sentinel check `tilec->data == slot->buf`.

2. **Tile reinit without teardown.** If `opj_tcd_init_decode_tile` is called
   twice without an intervening teardown (does this happen?), the pool slot
   could be re-handed-out while marked `lent`. Audit `j2k.c` for this pattern;
   if found, add a defensive `slot->lent = FALSE` in init.

3. **Memory peak.** The pool retains the largest-seen buffer per component
   for the full decode. For images with one huge tile + many small tiles,
   peak memory equals the huge tile's footprint × numcomps. Same peak as
   legacy (which allocates the huge tile anyway); no regression.

4. **Pool buffer staleness.** When a buffer is handed back to the pool and
   then re-handed-out for the next tile, the buffer's prior contents are
   stale. T1 writes every codeblock pixel it touches; DWT reads only the
   populated region. So stale data in tail bytes is harmless. (Legacy
   `opj_image_data_alloc` doesn't zero either — same behavior.)

## Decision Gate

Bench gate after implementation:
- **Archival decode wall time:** improvement of ≥5% on the loc-maps subset
  (the profile predicted 11.71% allocator self-time; capture half of that
  as a conservative target).
- **Smoke gmean:** unchanged (within ±1%).
- **Conformance:** no new failures.

If archival improves <2%, investigate before tagging. If smoke regresses
>1%, revert.
