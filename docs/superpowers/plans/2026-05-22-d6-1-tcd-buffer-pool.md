# D6.1 (phase 1) — TCD Tile-Component Buffer Pool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the per-tile `malloc`/`free` churn on `opj_tcd_tilecomp_t::data` by pooling buffers inside the `opj_tcd_t` object (which already persists across all tiles of a single image decode).

**Architecture:** Add a per-component buffer pool to `opj_tcd_t`. `opj_alloc_tile_component_data` checks the pool first and grows the slot's buffer on demand (monotonic). At tile teardown, the tilec relinquishes the buffer back to the pool slot instead of freeing it. The pool is destroyed only when the TCD itself is destroyed. Disambiguator between caller-owned, self-owned, and pool-owned data is a sentinel-pointer check (`tilec->data == pool->slots[comp_no].buf`).

**Tech Stack:** C99 / OpenJPEG-fork conventions. No new external deps. `opj_image_data_alloc` / `opj_image_data_free` remain the underlying allocator.

---

## File Structure

**Modified:**
- `src/lib/openjp2/tcd.h` — add `opj_tcd_pool_slot_t` and `opj_tcd_buffer_pool_t` types; add `data_pool` field to `opj_tcd_t`; add `comp_no` and `parent_tcd` fields to `opj_tcd_tilecomp_t`.
- `src/lib/openjp2/tcd.c` — implement pool init / destroy / handout / return; rewrite `opj_alloc_tile_component_data`; rewrite teardown branch in `opj_tcd_decode_tile`; populate `comp_no` and `parent_tcd` in `opj_tcd_init_tile`.
- `docs/superpowers/specs/2026-05-22-d6-1-tcd-buffer-pool-design.md` — append bench-result Outcome section after Task 3.

**Not modified:**
- `src/lib/openjp2/j2k.c` — call sites pass `l_tilec` only; no signature change.
- `src/lib/openjp2/t1.c`, `t1_fast.c`, `dwt.c`, `mct.c` — pool change is invisible to these.

---

## Task 1: Add pool types and tilec fields

**Files:**
- Modify: `src/lib/openjp2/tcd.h`

- [ ] **Step 1: Read the current `opj_tcd_tilecomp_t` and `opj_tcd_t` definitions.**

```bash
cd /home/cornish/GitHub/openjp2k
grep -n "^typedef struct opj_tcd_tilecomp\|^} opj_tcd_tilecomp_t\|^typedef struct opj_tcd \|^} opj_tcd_t" src/lib/openjp2/tcd.h
```

Note the line range of each struct.

- [ ] **Step 2: Add the pool slot and pool types.**

In `tcd.h`, immediately before the `opj_tcd_tilecomp_t` typedef (around line 195-200, but before `typedef struct opj_tcd_tilecomp`), insert:

```c
/* D6.1: per-component pool slot for the tile-component data buffer.
 * The pool lives on opj_tcd_t and survives individual tile decodes,
 * so the first malloc per component is amortized across the whole
 * image's tiles. */
typedef struct opj_tcd_pool_slot {
    OPJ_INT32 *buf;        /* pooled buffer (NULL if slot empty)             */
    OPJ_SIZE_T size;       /* allocated size of buf in bytes (0 if NULL)     */
    OPJ_BOOL   lent;       /* TRUE while a tilec currently holds this buffer */
} opj_tcd_pool_slot_t;

typedef struct opj_tcd_buffer_pool {
    opj_tcd_pool_slot_t *slots;  /* numcomps slots; NULL until pool inited   */
    OPJ_UINT32 numcomps;         /* matches image->numcomps when initialized */
} opj_tcd_buffer_pool_t;
```

- [ ] **Step 3: Add `comp_no` and `parent_tcd` fields to `opj_tcd_tilecomp_t`.**

Forward-declare the TCD type (the tilec is defined before `opj_tcd_t`):

```c
/* Forward declaration so opj_tcd_tilecomp_t can hold a back-pointer. */
struct opj_tcd;
```

Place this above the `opj_tcd_tilecomp_t` typedef start.

Then inside the `opj_tcd_tilecomp_t` struct body, append (just before the closing `}`):

```c
    /* D6.1: back-pointers used by opj_alloc_tile_component_data to
     * reach the parent TCD's data_pool. Set once in opj_tcd_init_tile. */
    OPJ_UINT32 comp_no;
    struct opj_tcd *parent_tcd;
```

- [ ] **Step 4: Add the `data_pool` field to `opj_tcd_t`.**

Inside the `opj_tcd_t` struct body, before its closing `}`, insert:

```c
    /* D6.1: per-component tile-component buffer pool. Lifetime is tied
     * to this TCD; reset to all-NULL slots at opj_tcd_init, populated
     * lazily by opj_alloc_tile_component_data, freed at opj_tcd_destroy. */
    opj_tcd_buffer_pool_t data_pool;
```

- [ ] **Step 5: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j > /tmp/d61-t1-build.log 2>&1 && echo BUILT || tail -30 /tmp/d61-t1-build.log
```

Expected: BUILT. Struct additions are no-ops behaviourally; nothing reads the new fields yet.

- [ ] **Step 6: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/tcd.h
git commit -m "$(cat <<'EOF'
TCD: add buffer-pool types + tilec back-pointers (D6.1 step 1)

opj_tcd_pool_slot_t and opj_tcd_buffer_pool_t added to tcd.h. data_pool
field appended to opj_tcd_t. comp_no and parent_tcd back-pointers
added to opj_tcd_tilecomp_t so opj_alloc_tile_component_data can reach
the parent TCD's pool from a tilec.

Struct layout change only; nothing reads the new fields yet.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Pool init, destroy, and tilec back-pointer population

**Files:**
- Modify: `src/lib/openjp2/tcd.c`

- [ ] **Step 1: Locate the three modification sites.**

```bash
cd /home/cornish/GitHub/openjp2k
grep -n "^OPJ_BOOL opj_tcd_init\b\|^void opj_tcd_destroy\b\|^static INLINE OPJ_BOOL opj_tcd_init_tile" src/lib/openjp2/tcd.c
```

`opj_tcd_init` is around line 714, `opj_tcd_destroy` around line 744, `opj_tcd_init_tile` around line 791.

- [ ] **Step 2: Initialize the pool in `opj_tcd_init`.**

Read the current body of `opj_tcd_init` to confirm where `p_tcd->image` is bound and where the function returns OPJ_TRUE. Add this block immediately before the return-true (or before the final closing brace if the function ends with `return OPJ_TRUE`):

```c
    /* D6.1: initialize the per-component data buffer pool. */
    p_tcd->data_pool.numcomps = p_image->numcomps;
    p_tcd->data_pool.slots = (opj_tcd_pool_slot_t *) opj_calloc(
        p_image->numcomps, sizeof(opj_tcd_pool_slot_t));
    if (p_tcd->data_pool.slots == NULL) {
        opj_event_msg(p_manager, EVT_ERROR,
                      "Failed to allocate TCD buffer pool slots\n");
        return OPJ_FALSE;
    }
```

If `opj_tcd_init` doesn't take a `p_manager` arg, drop the `opj_event_msg` line — just `return OPJ_FALSE`. Check the signature:

```bash
sed -n '714,720p' src/lib/openjp2/tcd.c
```

Adapt the error path accordingly. The point is: a calloc failure must propagate.

- [ ] **Step 3: Destroy the pool in `opj_tcd_destroy`.**

Insert this block at the top of `opj_tcd_destroy` (or after the NULL-check on the tcd argument, before any other cleanup):

```c
    /* D6.1: free pool buffers and the slot array. */
    if (tcd->data_pool.slots != NULL) {
        OPJ_UINT32 i;
        for (i = 0; i < tcd->data_pool.numcomps; ++i) {
            if (tcd->data_pool.slots[i].buf != NULL) {
                opj_image_data_free(tcd->data_pool.slots[i].buf);
            }
        }
        opj_free(tcd->data_pool.slots);
        tcd->data_pool.slots = NULL;
        tcd->data_pool.numcomps = 0;
    }
```

Confirm the argument is named `tcd` (not `p_tcd`); adapt if different.

- [ ] **Step 4: Populate `comp_no` and `parent_tcd` in `opj_tcd_init_tile`.**

Read `opj_tcd_init_tile`:

```bash
sed -n '791,910p' src/lib/openjp2/tcd.c
```

Find the per-component for-loop where `l_tilec` (or `tilec`) is being initialized. Add inside that loop:

```c
    l_tilec->comp_no    = compno;
    l_tilec->parent_tcd = p_tcd;
```

The loop induction variable might be named differently (`compno`, `i`, `j`); use the actual name in scope.

- [ ] **Step 5: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j > /tmp/d61-t2-build.log 2>&1 && echo BUILT || tail -30 /tmp/d61-t2-build.log
```

Expected: BUILT, no warnings.

- [ ] **Step 6: Smoke diff-test still 90/0.**

```bash
cd /home/cornish/GitHub/openjp2k
if [ ! -s /tmp/diff-smoke.txt ]; then
    grep -v '^#' /home/cornish/GitHub/openjp2k-bench/corpus/synthetic-iter.txt \
      | grep -v '^[[:space:]]*$' \
      | sed "s|^|/home/cornish/GitHub/openjp2k-bench/|" \
      > /tmp/diff-smoke.txt
fi
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `diff-test: 90 ok, 0 failed (of 90)`. The pool is initialized but nothing reads it yet — behaviour unchanged.

- [ ] **Step 7: Quick ctest sanity.**

```bash
cd /home/cornish/GitHub/openjp2k/build && ctest -j8 2>&1 | tail -5
```

Expected: 8 pre-existing NR-DEC-md5 failures only.

- [ ] **Step 8: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/tcd.c
git commit -m "$(cat <<'EOF'
TCD: pool init + destroy + tilec back-pointer population (D6.1 step 2)

opj_tcd_init allocates the per-component slot array (calloc, all-NULL).
opj_tcd_destroy frees the pool buffers and slot array.
opj_tcd_init_tile populates each tilec's comp_no and parent_tcd
back-pointers so opj_alloc_tile_component_data can reach the pool.

Pool is wired but unused; behaviour unchanged. Smoke diff-test 90/90,
ctest pre-existing failures only.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Rewrite alloc/teardown to use the pool

This is the behavior-change commit. Atomic — alloc and teardown must change together or the pool ownership gets confused.

**Files:**
- Modify: `src/lib/openjp2/tcd.c`

- [ ] **Step 1: Read current `opj_alloc_tile_component_data`.**

```bash
sed -n '760,790p' src/lib/openjp2/tcd.c
```

The current function lives at line 760-787.

- [ ] **Step 2: Replace `opj_alloc_tile_component_data` with the pool-aware version.**

Replace the entire function body (lines 760-787) with:

```c
OPJ_BOOL opj_alloc_tile_component_data(opj_tcd_tilecomp_t *l_tilec)
{
    opj_tcd_pool_slot_t *slot;

    /* Fast path: the tilec already holds a buffer big enough for the
     * current need. Happens e.g. when a tile is re-decoded without
     * teardown. */
    if (l_tilec->data != 00 && l_tilec->data_size_needed <= l_tilec->data_size) {
        return OPJ_TRUE;
    }

    /* If the tilec previously self-allocated (ownsData), release that
     * buffer before we hand it a pool buffer. */
    if (l_tilec->ownsData && l_tilec->data != 00) {
        opj_image_data_free(l_tilec->data);
        l_tilec->data     = 00;
        l_tilec->ownsData = OPJ_FALSE;
        l_tilec->data_size = 0;
    }

    /* Pool path. The tilec back-pointers must have been set by
     * opj_tcd_init_tile; the pool itself by opj_tcd_init. */
    if (l_tilec->parent_tcd == NULL ||
        l_tilec->parent_tcd->data_pool.slots == NULL ||
        l_tilec->comp_no >= l_tilec->parent_tcd->data_pool.numcomps) {
        /* Pool not available — fall back to legacy direct allocation. */
        l_tilec->data = (OPJ_INT32 *) opj_image_data_alloc(l_tilec->data_size_needed);
        if (l_tilec->data == 00 && l_tilec->data_size_needed != 0) {
            return OPJ_FALSE;
        }
        l_tilec->data_size = l_tilec->data_size_needed;
        l_tilec->ownsData  = OPJ_TRUE;
        return OPJ_TRUE;
    }

    slot = &l_tilec->parent_tcd->data_pool.slots[l_tilec->comp_no];
    if (slot->buf == NULL || slot->size < l_tilec->data_size_needed) {
        /* Grow or first-allocate. */
        opj_image_data_free(slot->buf);
        slot->buf  = (OPJ_INT32 *) opj_image_data_alloc(l_tilec->data_size_needed);
        if (slot->buf == NULL && l_tilec->data_size_needed != 0) {
            slot->size = 0;
            return OPJ_FALSE;
        }
        slot->size = l_tilec->data_size_needed;
    }

    l_tilec->data      = slot->buf;
    l_tilec->data_size = slot->size;
    l_tilec->ownsData  = OPJ_FALSE;   /* pool owns it */
    slot->lent         = OPJ_TRUE;
    return OPJ_TRUE;
}
```

C90 compliance: all locals declared at top (`opj_tcd_pool_slot_t *slot`); no mid-block declarations.

- [ ] **Step 3: Update the teardown branch.**

Find the existing teardown block at `tcd.c:1989` (the `if (l_tile_comp->ownsData && l_tile_comp->data) { ... }`). Replace it with:

```c
        if (l_tile_comp->data != 00) {
            /* Distinguish caller-owned vs pool-owned vs self-owned:
             *   ownsData == TRUE  → self-owned: free.
             *   ownsData == FALSE && data == pool slot buf → pool-owned: return.
             *   ownsData == FALSE && data != pool slot buf → caller-owned
             *                       (e.g. whole-tile decode into image-pixels):
             *                       leave alone; the caller frees.
             */
            if (l_tile_comp->ownsData) {
                opj_image_data_free(l_tile_comp->data);
            } else if (l_tile_comp->parent_tcd != NULL &&
                       l_tile_comp->parent_tcd->data_pool.slots != NULL &&
                       l_tile_comp->comp_no < l_tile_comp->parent_tcd->data_pool.numcomps) {
                opj_tcd_pool_slot_t *slot =
                    &l_tile_comp->parent_tcd->data_pool.slots[l_tile_comp->comp_no];
                if (slot->buf == l_tile_comp->data) {
                    /* Pool-owned: return to the pool. Buffer stays
                     * allocated; the next tile will reuse it. */
                    slot->lent = OPJ_FALSE;
                }
                /* else caller-owned: do not free. */
            }
            l_tile_comp->data              = 00;
            l_tile_comp->ownsData          = 0;
            l_tile_comp->data_size         = 0;
            l_tile_comp->data_size_needed  = 0;
        }
```

C90 compliance: `opj_tcd_pool_slot_t *slot` is at the top of its inner block, before any statement in that block — valid.

- [ ] **Step 4: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j > /tmp/d61-t3-build.log 2>&1 && echo BUILT || tail -50 /tmp/d61-t3-build.log
```

Expected: BUILT.

- [ ] **Step 5: Smoke diff-test.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `diff-test: 90 ok, 0 failed`. The smoke corpus is single-tile-per-file synthetic, so it exercises one pool slot per file but doesn't stress the across-tile reuse. Correctness check passes if pool init / destroy / handout / return all wire correctly.

If any file mismatches, STOP and report BLOCKED. Capture:

```bash
F=<failing file>
OPJ_T1_FAST=0 build/bin/test_mqc_dump "$F" > /tmp/legacy.bin
OPJ_T1_FAST=1 build/bin/test_mqc_dump "$F" > /tmp/fast.bin
cmp -l /tmp/legacy.bin /tmp/fast.bin | head -10
```

Likely cause: a teardown path that didn't null out `data` properly, leaving stale state on the next handout.

- [ ] **Step 6: ctest.**

```bash
cd /home/cornish/GitHub/openjp2k/build && ctest -j8 2>&1 | tail -15
```

Expected: only the 8 pre-existing NR-DEC-md5 failures.

- [ ] **Step 7: Multi-tile sanity — pick an archival file and decode.**

```bash
cd /home/cornish/GitHub/openjp2k
ARCH=/home/cornish/GitHub/openjp2k-bench/corpus/public/archival/loc-maps/01964_1912-0001.jp2
[ -f "$ARCH" ] && ls -la "$ARCH"
build/bin/test_mqc_dump "$ARCH" > /tmp/d61-arch-fast.bin 2> /tmp/d61-arch.err
echo "exit=$?  size=$(wc -c < /tmp/d61-arch-fast.bin)"
```

Expected: exit 0, non-zero dump size. (We'll bench it in Task 4; this is just a smoke test that the multi-tile path doesn't crash.)

- [ ] **Step 8: Run valgrind on the archival file to confirm no double-free or use-after-free.**

```bash
cd /home/cornish/GitHub/openjp2k
valgrind --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
    build/bin/test_mqc_dump "$ARCH" > /dev/null 2> /tmp/d61-valgrind.log
echo "valgrind exit=$?"
grep -E "ERROR SUMMARY|definitely lost" /tmp/d61-valgrind.log
```

Expected: `ERROR SUMMARY: 0 errors`. `definitely lost: 0 bytes`.

If valgrind reports errors, STOP and report BLOCKED with the log excerpt. The new ownership semantics are the prime suspect — likely a missing `data = NULL` after a teardown branch, or a double-route through `opj_image_data_free` for a pool-owned buffer.

If valgrind is not installed (`which valgrind` returns nothing), skip this step and document in the report.

- [ ] **Step 9: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/tcd.c
git commit -m "$(cat <<'EOF'
TCD: route tilec->data through the buffer pool (D6.1 step 3)

opj_alloc_tile_component_data now consults the per-TCD pool slot for
the tilec's component. Misses + grows trigger the underlying
opj_image_data_alloc; hits hand back the existing slot buffer with
ownsData=FALSE. The teardown branch in opj_tcd_decode_tile
distinguishes self-owned (free), pool-owned (return slot->lent=FALSE
and keep the buffer alive), and caller-owned (leave alone) cases via
a sentinel-pointer check against the slot's buffer.

Smoke diff-test 90/90 byte-identical, ctest pre-existing failures
only, valgrind clean on a multi-tile archival decode.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Archival + smoke bench gate

The smoke corpus is single-tile-per-file; the perf win lives in multi-tile archival workloads. Bench both: archival must improve, smoke must not regress.

**Files:** none modified; measurement only.

- [ ] **Step 1: Sync openjp2k source into the bench's third_party clone.**

```bash
cd /home/cornish/GitHub/openjp2k-bench
for f in mqc.h mqc.c mqc_fast.h mqc_fast.c t1.c t1_fast.c t1_fast.h t1_inl.h tcd.c tcd.h CMakeLists.txt; do
    cp /home/cornish/GitHub/openjp2k/src/lib/openjp2/$f third_party/openjp2k/src/lib/openjp2/
done
rm -f build/openjp2k_ext-prefix/src/openjp2k_ext-stamp/openjp2k_ext-build
rm -f build/openjp2k_ext-prefix/src/openjp2k_ext-stamp/openjp2k_ext-install
cmake --build build --target openjp2k_ext -- -j > /tmp/d61-bench-build.log 2>&1 && \
cmake --build build --target jp2k-bench -- -j >> /tmp/d61-bench-build.log 2>&1 && \
echo BENCH_BUILT
```

Also update the bench's third_party openjp2k git tree so its provenance reflects this branch:

```bash
cd /home/cornish/GitHub/openjp2k-bench/third_party/openjp2k
git fetch /home/cornish/GitHub/openjp2k feat/d6-1-tcd-buffer-pool 2>&1 | tail -3
git checkout -q $(cd /home/cornish/GitHub/openjp2k && git rev-parse HEAD) 2>&1 | tail -1
git log --oneline -1
```

- [ ] **Step 2: Build archival + smoke file lists.**

```bash
# Archival list — multi-tile LoC scans
ls /home/cornish/GitHub/openjp2k-bench/corpus/public/archival/loc-maps/*.jp2 > /tmp/d61-archival.txt 2>/dev/null
wc -l /tmp/d61-archival.txt

# Smoke list — already exists from prior gates
ls -la /tmp/diff-smoke.txt 2>/dev/null
if [ ! -s /tmp/diff-smoke.txt ]; then
    grep -v '^#' /home/cornish/GitHub/openjp2k-bench/corpus/synthetic-iter.txt \
      | grep -v '^[[:space:]]*$' \
      | sed "s|^|/home/cornish/GitHub/openjp2k-bench/|" \
      > /tmp/diff-smoke.txt
fi
```

- [ ] **Step 3: Bench archival twice (cool down between).**

The pool only helps the second-and-later tiles within a single decode, so `--iters 10` per file already exercises the pool. Bench at HEAD of feat/d6-1-tcd-buffer-pool. For an apples-to-apples comparison we need the same binary against the same baseline; the cleanest baseline is `main` immediately before this branch's first commit.

```bash
cd /home/cornish/GitHub/openjp2k-bench
sleep 60

# Bench with pool (current branch)
build/jp2k-bench --iters 10 --warmup 2 --threads 1 --jsonl \
    --decoder openjp2k $(cat /tmp/d61-archival.txt) > /tmp/d61-archival-pool.log 2>&1
echo "pool run done"

# Switch back to main's tcd source temporarily, rebuild, bench again
cd /home/cornish/GitHub/openjp2k-bench/third_party/openjp2k
git checkout main -- src/lib/openjp2/tcd.c src/lib/openjp2/tcd.h 2>&1 | tail -1
cd /home/cornish/GitHub/openjp2k-bench
rm -f build/openjp2k_ext-prefix/src/openjp2k_ext-stamp/openjp2k_ext-build
cmake --build build --target openjp2k_ext -- -j > /tmp/d61-rebuild-main.log 2>&1 && \
cmake --build build --target jp2k-bench -- -j >> /tmp/d61-rebuild-main.log 2>&1 && echo REBUILT

sleep 60
build/jp2k-bench --iters 10 --warmup 2 --threads 1 --jsonl \
    --decoder openjp2k $(cat /tmp/d61-archival.txt) > /tmp/d61-archival-main.log 2>&1
echo "main run done"

# Restore third_party/openjp2k to feat/d6-1 source so subsequent tasks have a clean state
cd /home/cornish/GitHub/openjp2k-bench/third_party/openjp2k
git checkout $(cd /home/cornish/GitHub/openjp2k && git rev-parse HEAD) -- src/lib/openjp2/tcd.c src/lib/openjp2/tcd.h 2>&1 | tail -1
cd /home/cornish/GitHub/openjp2k-bench
rm -f build/openjp2k_ext-prefix/src/openjp2k_ext-stamp/openjp2k_ext-build
cmake --build build --target openjp2k_ext -- -j > /tmp/d61-restore-build.log 2>&1
cmake --build build --target jp2k-bench -- -j >> /tmp/d61-restore-build.log 2>&1 && echo RESTORED
```

If `--decoder openjp2k` is not a valid flag, drop it and rely on the bench's default behavior (which still produces openjp2k entries in the JSONL).

- [ ] **Step 4: Compute archival speedup.**

```bash
python3 << 'EOF'
import json, math, re
from collections import defaultdict

def load(p):
    d = defaultdict(dict)
    for line in open(p):
        m = re.match(r'^\{', line)
        if not m: continue
        try: r = json.loads(line)
        except: continue
        if r.get('type') != 'result' or r.get('error'): continue
        d[r['file']][r['decoder']] = r['megapixels_per_sec']
    return d

main = load('/tmp/d61-archival-main.log')
pool = load('/tmp/d61-archival-pool.log')
common = [f for f in main if f in pool and 'openjp2k' in main[f] and 'openjp2k' in pool[f]]
ratios = sorted([(pool[f]['openjp2k'] / main[f]['openjp2k'], f) for f in common])
n = len(ratios)
vals = [r for r, _ in ratios]
gm = math.exp(sum(math.log(v) for v in vals) / n)

print(f'archival files: {n}')
print(f'min={vals[0]:.4f}  p50={vals[n//2]:.4f}  max={vals[-1]:.4f}  gmean={gm:.4f}')
print(f'pool effect on archival: {(gm-1.0)*100:+.2f}%')

for r, f in ratios[:3]:
    print(f'  slowest: {r:.4f}  {f}')
for r, f in ratios[-3:]:
    print(f'  fastest: {r:.4f}  {f}')
EOF
```

Target: archival gmean ≥ 1.05 (the spec said "improvement of ≥5% on the loc-maps subset, capturing half of the profile's predicted 11.71% allocator self-time").

- [ ] **Step 5: Bench smoke fast-vs-legacy with the pool build.**

```bash
cd /home/cornish/GitHub/openjp2k-bench
sleep 60

OPJ_T1_FAST=0 scripts/run_smoke.sh > /tmp/d61-smoke-legacy.log 2>&1
echo "legacy done"

sleep 60

OPJ_T1_FAST=1 scripts/run_smoke.sh > /tmp/d61-smoke-fast.log 2>&1
echo "fast done"
```

- [ ] **Step 6: Compute smoke ratio.**

```bash
python3 << 'EOF'
import json, math
from collections import defaultdict

def load(p):
    d = defaultdict(dict)
    for line in open(p):
        if not line.startswith('{'): continue
        try: r = json.loads(line)
        except: continue
        if r.get('type') != 'result' or r.get('error'): continue
        d[r['file']][r['decoder']] = r['megapixels_per_sec']
    return d

L = load('/tmp/d61-smoke-legacy.log')
F = load('/tmp/d61-smoke-fast.log')
common = [f for f in L if f in F and 'openjp2k' in L[f] and 'openjp2k' in F[f]]
ratios = [F[f]['openjp2k']/L[f]['openjp2k'] for f in common]
n = len(ratios)
gm = math.exp(sum(math.log(v) for v in ratios)/n)
print(f'smoke files: {n}')
print(f'gmean fast/legacy: {gm:.4f}')
print(f'smoke effect: {(gm-1.0)*100:+.2f}%')
EOF
```

Target: smoke gmean within ±1% of D1's 1.0045 (the pool shouldn't affect smoke since each synthetic file is single-tile).

- [ ] **Step 7: Worst-loser diff-test on the same 30-file list used by D1/D3.**

```bash
cd /home/cornish/GitHub/openjp2k-bench
LATEST=$(ls -t results/full_corpus_*.jsonl | head -1)
python3 << EOF > /tmp/worst-losers.txt
import json, os
from collections import defaultdict
ROOT = '/home/cornish/GitHub/openjp2k-bench'
mps = defaultdict(dict)
for line in open('$LATEST'):
    r = json.loads(line)
    if r.get('type') != 'result' or r.get('error'): continue
    mps[r['file']][r['decoder']] = r['megapixels_per_sec']
ratios = []
for f, d in mps.items():
    if 'openjp2k' in d and 'openjpeg' in d:
        ratios.append((d['openjp2k']/d['openjpeg'], f))
ratios.sort()
for _, f in ratios[:30]:
    print(f if f.startswith('/') else os.path.join(ROOT, f))
EOF

cd /home/cornish/GitHub/openjp2k
while IFS= read -r p; do [ -f "$p" ] && echo "$p"; done < /tmp/worst-losers.txt > /tmp/worst-existing.txt
scripts/run_diff_test.sh --include-from /tmp/worst-existing.txt
```

Expected: `0 failed`.

- [ ] **Step 8: Full conformance.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run-conformance.sh > /tmp/d61-conformance.log 2>&1
grep -E "tests? failed|tests? passed" /tmp/d61-conformance.log | tail -3
```

Expected: 8 pre-existing failures, no new ones.

- [ ] **Step 9: Record results for Task 5.**

```bash
cat > /tmp/d61-bench-results.txt << EOF
D6.1 bench results:
  archival gmean (pool / main-without-pool): <VAL>
  archival effect: <SIGNED %>
  smoke gmean fast/legacy: <VAL>
  worst-loser diff-test: <N> ok / <M> failed
  conformance: <pass>/<total>, pre-existing 8 only
EOF
```

Fill in real values.

- [ ] **Step 10: Verdict.**

| archival gmean | smoke effect | verdict |
|---|---|---|
| ≥ 1.05 | within ±1% | PASS — proceed to Task 5 |
| 1.00 ≤ archival < 1.05 | within ±1% | MARGINAL — report DONE_WITH_CONCERNS, discuss before tagging |
| archival < 1.00 OR smoke regresses >1% | — | FAIL — report BLOCKED, revert candidate |

---

## Task 5: Spec update + tag

**Files:**
- Modify: `docs/superpowers/specs/2026-05-22-d6-1-tcd-buffer-pool-design.md`

- [ ] **Step 1: Append an Outcome section.**

Open the spec and append below the existing Decision Gate:

```markdown
---

## Outcome (2026-05-22)

Landed on `feat/d6-1-tcd-buffer-pool`. Archival decode gmean
pool-vs-main: <VAL>. Smoke gmean fast/legacy: <VAL> (D1 baseline
was 1.0045). Worst-loser diff-test 30/30 byte-identical, conformance
8 pre-existing failures unchanged.
```

Substitute the actual values from `/tmp/d61-bench-results.txt`.

- [ ] **Step 2: Commit the spec update.**

```bash
cd /home/cornish/GitHub/openjp2k
git add docs/superpowers/specs/2026-05-22-d6-1-tcd-buffer-pool-design.md
git commit -m "$(cat <<'EOF'
Spec: D6.1 phase 1 landed with bench numbers

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 3: Tag if archival ≥ 1.05.**

```bash
cd /home/cornish/GitHub/openjp2k
git tag -a v0.6.0-d6-1-tcd-buffer-pool -m "$(cat <<'EOF'
D6.1 phase 1 — TCD Tile-Component Buffer Pool

Per-component pool inside opj_tcd_t replaces per-tile malloc/free
churn on tilec->data. First tile-per-component triggers a malloc;
subsequent tiles reuse with monotonic growth. Pool is freed at
opj_tcd_destroy.

Archival workload speedup: <VAL>x (target was ≥1.05).
Smoke unchanged (within ±1%); conformance 8 pre-existing failures
only; worst-loser diff-test 30/30 byte-identical.

Phase 2 (data_win pooling) and phase 3 (tilec-struct persistence)
remain as separate follow-ups.

Cumulative project state: D6.5 + partial D6.1 + D1.0-D1.3 + D6.1-phase-1.
EOF
)"
git push origin feat/d6-1-tcd-buffer-pool v0.6.0-d6-1-tcd-buffer-pool
```

If archival < 1.05 but ≥ 1.00, do NOT tag yet. Report DONE_WITH_CONCERNS with the values and let the controller ask the user.

If archival < 1.00, do NOT commit the spec update. Report BLOCKED.

---

## Self-Review

1. **Spec coverage:** Pool types (spec §Design > Data structure) → Task 1. Pool init/destroy (spec §Design > Lifecycle) → Task 2. Allocator and teardown rewrite (spec §Design > Buffer handout + Buffer return) → Task 3. Component-index plumbing (spec §Design > Component index plumbing) → Task 1 (fields) + Task 2 (population). Caller-owned disambiguation (spec §Design > "Caller-owned data" path) → Task 3 (sentinel check in alloc + teardown). Bench gate (spec §Verification + §Decision Gate) → Task 4. All risks addressed: subtile/ROI (sentinel check), tile reinit (fast-path early return + ownsData defensive free in alloc), memory peak (monotonic growth), buffer staleness (no zeroing; matches legacy `opj_image_data_alloc` semantics).

2. **Placeholder scan:** Task 4 Step 3 has `<VAL>` and `<N>` placeholders inside the bench-results template — these are intended to be filled in at runtime by the implementer. Same for Task 5 Step 1 and Step 3's tag message. These are correct uses, not plan failures. Otherwise the plan contains no TBD/TODO; every C code change is shown verbatim.

3. **Type consistency:** `opj_tcd_pool_slot_t` (Task 1) is referenced in Task 3 as `opj_tcd_pool_slot_t *slot`. `opj_tcd_buffer_pool_t` (Task 1) holds `slots` and `numcomps`, referenced in Tasks 2 and 3 as `.slots` and `.numcomps`. `data_pool` field (Task 1) is accessed in Tasks 2 (init/destroy) and 3 (alloc/teardown). `comp_no` and `parent_tcd` (Task 1, populated in Task 2) are read in Task 3. All consistent.

4. **Task 4 baseline comparison:** the plan does fast-path-vs-fast-path comparison (pool-build vs main-build, both with `OPJ_T1_FAST` defaulted). That's the right comparison because the pool change is orthogonal to the fast/legacy switch — it lives below T1 in the decode stack. Smoke gmean check at Step 6 confirms the fast/legacy delta is unchanged.

5. **One open detail to call out:** Task 2 Step 2 says "the `opj_event_msg` line — adapt if `opj_tcd_init` doesn't take a `p_manager` arg". This is necessary because I haven't verified the current signature. The step instructs the implementer to check and conditionally adapt, with concrete behavior on both branches. Not a placeholder; explicit fork.
