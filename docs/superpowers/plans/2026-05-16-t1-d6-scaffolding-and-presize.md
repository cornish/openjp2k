# T1 D6 Scaffolding + Pre-Sizing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the structural prerequisites for Sub-project 1 of the decode performance roadmap: (a) a parallel `t1_fast.c` translation unit and the `OPJ_T1_LEGACY_ONLY` build switch so future fast paths can land behind a diff-test, (b) removal of the dead `opj_t1_t::encoder` field, (c) pre-sized `data`/`flags` buffers at decode entry so per-codeblock realloc is eliminated.

**Architecture:** Pure refactor. No behavior changes. New file `src/lib/openjp2/t1_fast.c` (Apache-2.0) initially holds only a header comment and is empty otherwise — it's a landing pad for D1+. CMake gains an `OPJ_T1_LEGACY_ONLY` option that defines a preprocessor macro of the same name; dispatch sites added in D1+ will check this macro. The `encoder` field is removed from `opj_t1_t` and from `opj_t1_create`. Pre-sizing is done by calling `opj_t1_allocate_buffers` once at decode-processor entry with the tile-component's max codeblock dims (`1 << tccp->cblkw` × `1 << tccp->cblkh`), making the per-codeblock call a no-op.

**Tech Stack:** C99, CMake, CTest. No new dependencies.

**Scope deferred to separate plans:**
- **D6.3 (job pool):** thread-pool join semantics need investigation; jobs are allocated on the main thread and freed on workers in unknown order.
- **D6.4 (decoded_data buffer reuse):** changes ownership semantics of `cblk->decoded_data` (currently freed by tcd.c cleanup paths at tcd.c:1378 and tcd.c:2389).

**Prerequisites for *merging* this plan's commits:**
- None. This plan makes no performance claims; the gate in spec §2.7 does not apply (no cycle reduction asserted). Standard CTest pass on `main` is sufficient.
- Sub-projects 0 (bench scaffolding) and 0.5 (cleanroom report) remain prerequisites for **D1 onward**, not for D6.

---

## File Structure

**Create:**
- `src/lib/openjp2/t1_fast.c` — empty Apache-2.0 translation unit; landing pad for D1+ optimized variants.
- `src/lib/openjp2/t1_fast.h` — Apache-2.0 header; initially empty interior, declares nothing yet. Created so the CMake build picks it up consistently with sibling pairs.

**Modify:**
- `src/lib/openjp2/CMakeLists.txt` — add `t1_fast.c` / `t1_fast.h` to `OPENJPEG_SRCS`; add `option(OPJ_T1_LEGACY_ONLY ...)` block.
- `src/lib/openjp2/t1.h` — remove `OPJ_BOOL encoder;` from `opj_t1_t`.
- `src/lib/openjp2/t1.c` — remove `l_t1->encoder = isEncoder;` from `opj_t1_create`; add pre-size call in `opj_t1_clbl_decode_processor`.

---

## Task 1: Establish baseline test pass rate

**Files:** none modified.

This is the reference point every later task is compared against. Without it we cannot tell whether a regression is from our change or pre-existing.

- [ ] **Step 1: Configure a Release build**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
```
Expected: CMake configures without error; `build/` populated.

- [ ] **Step 2: Build**

Run:
```bash
cmake --build build -j
```
Expected: build succeeds. If it does not, fix the environment before continuing — the rest of the plan assumes a working tree.

- [ ] **Step 3: Capture baseline test results**

Run:
```bash
ctest --test-dir build --output-on-failure 2>&1 | tee /tmp/openjp2k-baseline-tests.log
tail -5 /tmp/openjp2k-baseline-tests.log
```
Expected: summary line like `XX% tests passed, YY tests failed out of ZZZ`. Record the pass/fail counts. These numbers are the gate for every subsequent task: no new failures may be introduced.

- [ ] **Step 4: No commit**

No source changes were made.

---

## Task 2: Create `t1_fast.c` / `t1_fast.h` skeleton

**Files:**
- Create: `src/lib/openjp2/t1_fast.c`
- Create: `src/lib/openjp2/t1_fast.h`

- [ ] **Step 1: Create `t1_fast.h`**

Write `src/lib/openjp2/t1_fast.h` with this exact content:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026, Toby Cornish
 *
 * Optimized T1 (entropy decode) variants for openjp2k.
 *
 * This header is intentionally empty in the initial scaffolding commit.
 * Declarations land here as Sub-project 1 deliverables D1-D5 are implemented.
 * The legacy decoder in t1.c remains the correctness reference.
 */

#ifndef OPJ_T1_FAST_H
#define OPJ_T1_FAST_H

/* No declarations yet. */

#endif /* OPJ_T1_FAST_H */
```

- [ ] **Step 2: Create `t1_fast.c`**

Write `src/lib/openjp2/t1_fast.c` with this exact content:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026, Toby Cornish
 *
 * Optimized T1 (entropy decode) variants for openjp2k.
 *
 * This translation unit is intentionally empty in the initial scaffolding
 * commit. Implementations land here as Sub-project 1 deliverables D1-D5
 * are completed. While OPJ_T1_LEGACY_ONLY is defined (the current default),
 * none of the optimized variants are wired in and the legacy decoder in
 * t1.c is used.
 */

#include "opj_includes.h"
#include "t1_fast.h"

/* No definitions yet. */
```

- [ ] **Step 3: Add both files to `CMakeLists.txt`**

In `src/lib/openjp2/CMakeLists.txt`, locate the line listing `t1.h` (line 43, just after `t1.c`). Insert the two new entries immediately after it, so the block reads:

```cmake
  ${CMAKE_CURRENT_SOURCE_DIR}/t1.c
  ${CMAKE_CURRENT_SOURCE_DIR}/t1.h
  ${CMAKE_CURRENT_SOURCE_DIR}/t1_fast.c
  ${CMAKE_CURRENT_SOURCE_DIR}/t1_fast.h
  ${CMAKE_CURRENT_SOURCE_DIR}/t2.c
```

- [ ] **Step 4: Rebuild**

Run:
```bash
cmake --build build -j
```
Expected: build succeeds. New files compile (`t1_fast.c` will compile to an empty object — fine). If a "no symbols" linker warning appears on macOS for `t1_fast.o`, that is expected and harmless; do not silence it with a dummy symbol.

- [ ] **Step 5: Run the test suite**

Run:
```bash
ctest --test-dir build --output-on-failure 2>&1 | tee /tmp/openjp2k-task2-tests.log
tail -5 /tmp/openjp2k-task2-tests.log
```
Expected: pass/fail counts identical to baseline from Task 1, Step 3. Any new failure is a regression and must be diagnosed before proceeding.

- [ ] **Step 6: Commit**

```bash
git add src/lib/openjp2/t1_fast.c src/lib/openjp2/t1_fast.h src/lib/openjp2/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add t1_fast.{c,h} scaffolding for Sub-project 1

Empty Apache-2.0 translation unit and header. Landing pad for D1-D5
optimized T1 variants. No declarations or definitions yet; legacy
decoder in t1.c remains the only decode path.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add `OPJ_T1_LEGACY_ONLY` CMake option

**Files:**
- Modify: `src/lib/openjp2/CMakeLists.txt` (the `option(...)` block around lines 74-77)

The macro is currently unused — no code site checks it yet. Adding it now means D1's plan can land the first `#ifndef OPJ_T1_LEGACY_ONLY` dispatch in a single commit instead of needing a CMake change at the same time.

- [ ] **Step 1: Add the option block**

In `src/lib/openjp2/CMakeLists.txt`, locate the existing block:

```cmake
option(OPJ_DISABLE_TPSOT_FIX "Disable TPsot==TNsot fix. See https://github.com/uclouvain/openjpeg/issues/254." OFF)
if(OPJ_DISABLE_TPSOT_FIX)
  add_definitions(-DOPJ_DISABLE_TPSOT_FIX)
endif()
```

Insert immediately after it:

```cmake
option(OPJ_T1_LEGACY_ONLY "Force the legacy T1 decoder path (disable t1_fast variants). Used by CI diff-tests and as a correctness fallback." OFF)
if(OPJ_T1_LEGACY_ONLY)
  add_definitions(-DOPJ_T1_LEGACY_ONLY)
endif()
```

- [ ] **Step 2: Reconfigure with the option OFF (default)**

Run:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
```
Expected: build succeeds. The option exists but defines nothing extra (default is OFF, so `-DOPJ_T1_LEGACY_ONLY` is not added).

- [ ] **Step 3: Reconfigure with the option ON to verify it builds**

From the repo root, run:
```bash
cmake -S . -B build-legacy -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DOPJ_T1_LEGACY_ONLY=ON
cmake --build build-legacy -j
```
Expected: build succeeds. `OPJ_T1_LEGACY_ONLY` is now defined globally for the libopenjp2 compilation. No code checks the macro yet, so there is no behavior change — this verifies the wiring, not any effect.

- [ ] **Step 4: Run tests in the legacy build**

Run:
```bash
ctest --test-dir build-legacy --output-on-failure 2>&1 | tee /tmp/openjp2k-task3-legacy-tests.log
tail -5 /tmp/openjp2k-task3-legacy-tests.log
```
Expected: identical pass/fail counts to baseline. (They must be identical because no code path is yet gated by the macro.)

- [ ] **Step 5: Remove the legacy build directory**

Run:
```bash
rm -rf build-legacy
```
The directory is not needed for ongoing work; CI will re-create it on demand once D1 lands.

- [ ] **Step 6: Commit**

```bash
git add src/lib/openjp2/CMakeLists.txt
git commit -m "$(cat <<'EOF'
Add OPJ_T1_LEGACY_ONLY CMake option

Default OFF. When ON, defines OPJ_T1_LEGACY_ONLY for libopenjp2; future
D1+ dispatch sites in t1.c will check this macro to force the legacy
decoder path for diff-testing. No code site checks it yet.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Remove dead `opj_t1_t::encoder` field

**Files:**
- Modify: `src/lib/openjp2/t1.h:201`
- Modify: `src/lib/openjp2/t1.c:1559`

The field is set in `opj_t1_create` (t1.c:1559) and read nowhere — verified by `grep -nE "->encoder" src/lib/openjp2/{t1.c,t1.h}` returning only the assignment line. Encode and decode paths are already separated by which function the caller invokes (`opj_t1_encode_cblks` vs `opj_t1_decode_cblks`); the boolean is a vestige.

- [ ] **Step 1: Re-verify the field has no readers**

Run:
```bash
grep -nE "->encoder|\.encoder" src/lib/openjp2/*.c src/lib/openjp2/*.h
```
Expected output: one line only:
```
src/lib/openjp2/t1.c:1559:    l_t1->encoder = isEncoder;
```
If any other line is reported, **stop** — the field is read somewhere this plan did not anticipate. Restart the task with that read site investigated.

- [ ] **Step 2: Remove the field declaration in `t1.h`**

In `src/lib/openjp2/t1.h`, remove line 201 (the `OPJ_BOOL encoder;` line) from the `opj_t1_t` struct. Leave the surrounding lines unchanged. The struct should now read:

```c
typedef struct opj_t1 {

    /** MQC component */
    opj_mqc_t mqc;

    OPJ_INT32  *data;
    /** Flags used by decoder and encoder.
     * Such that flags[1+0] is for state of col=0,row=0..3,
       flags[1+1] for col=1, row=0..3, flags[1+flags_stride] for col=0,row=4..7, ...
       This array avoids too much cache trashing when processing by 4 vertical samples
       as done in the various decoding steps. */
    opj_flag_t *flags;

    OPJ_UINT32 w;
    OPJ_UINT32 h;
    OPJ_UINT32 datasize;
    OPJ_UINT32 flagssize;

    /* The 3 variables below are only used by the decoder */
    /* set to TRUE in multithreaded context */
    OPJ_BOOL     mustuse_cblkdatabuffer;
    /* Temporary buffer to concatenate all chunks of a codebock */
    OPJ_BYTE    *cblkdatabuffer;
    /* Maximum size available in cblkdatabuffer */
    OPJ_UINT32   cblkdatabuffersize;
} opj_t1_t;
```

- [ ] **Step 3: Remove the field assignment in `t1.c`**

In `src/lib/openjp2/t1.c`, delete line 1559 (the `l_t1->encoder = isEncoder;` line). The `isEncoder` parameter of `opj_t1_create` is now formally unused.

- [ ] **Step 4: Silence the unused-parameter warning**

The signature of `opj_t1_create` must stay `opj_t1_t* opj_t1_create(OPJ_BOOL isEncoder)` because callers in tcd.c pass it. Add an explicit `(void)isEncoder;` cast as the first line of the function body. The function should now read:

```c
opj_t1_t* opj_t1_create(OPJ_BOOL isEncoder)
{
    opj_t1_t *l_t1 = 00;

    (void)isEncoder;

    l_t1 = (opj_t1_t*) opj_calloc(1, sizeof(opj_t1_t));
    if (!l_t1) {
        return 00;
    }

    return l_t1;
}
```

- [ ] **Step 5: Rebuild**

Run:
```bash
cmake --build build -j
```
Expected: build succeeds with no new warnings. If the compiler still warns about `isEncoder`, the `(void)` cast was placed in the wrong spot — fix and rebuild.

- [ ] **Step 6: Run the test suite**

Run:
```bash
ctest --test-dir build --output-on-failure 2>&1 | tee /tmp/openjp2k-task4-tests.log
tail -5 /tmp/openjp2k-task4-tests.log
```
Expected: pass/fail counts identical to baseline. The `encoder` field was dead, so removing it must have no behavioral effect.

- [ ] **Step 7: Commit**

```bash
git add src/lib/openjp2/t1.h src/lib/openjp2/t1.c
git commit -m "$(cat <<'EOF'
Remove dead opj_t1_t::encoder field

The field was set in opj_t1_create and read nowhere. Encode vs decode
paths are already separated by which entry point the caller invokes
(opj_t1_encode_cblks vs opj_t1_decode_cblks); the boolean was a vestige.

The isEncoder parameter of opj_t1_create is retained for ABI/callsite
stability and marked (void) to suppress the unused-parameter warning.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Pre-size `data`/`flags` at decode-processor entry

**Files:**
- Modify: `src/lib/openjp2/t1.c` around line 1689 (inside `opj_t1_clbl_decode_processor`).

Goal: call `opj_t1_allocate_buffers` once per worker thread with the tile-component's max codeblock dimensions (`1u << tccp->cblkw` and `1u << tccp->cblkh`) before any codeblock-specific work. The existing per-codeblock call at t1.c:2019-2022 then becomes a no-op (its grow check `if (datasize > t1->datasize)` is never true), eliminating the cold-codeblock realloc.

This is safe because:
- `tccp->cblkw` and `tccp->cblkh` are uniform per tile-component (per `opj_tccp_t` defn in j2k.h:188-190).
- `opj_t1_allocate_buffers` already grows monotonically via the same `if (datasize > t1->datasize)` checks (t1.c:1469, t1.c:1494); calling it with the max once is identical in observable behavior to calling it with progressively-smaller dims many times.
- Worst-case over-allocation is bounded: data buffer holds `(1<<cblkw) * (1<<cblkh)` INT32; with the J2K-mandated maximum `cblkw=cblkh=6`, that's 4096 INT32 ≈ 16 KB per worker thread. Flags buffer is comparable.

- [ ] **Step 1: Locate the insertion point**

Open `src/lib/openjp2/t1.c` and read lines 1685-1700. The block should look like:

```c
            opj_free(job);
            return;
        }
    }
    t1->mustuse_cblkdatabuffer = job->mustuse_cblkdatabuffer;

    if ((tccp->cblksty & J2K_CCP_CBLKSTY_HT) != 0) {
```

The pre-size call goes between line 1689 (`t1->mustuse_cblkdatabuffer = ...;`) and the HT branch on line 1691.

- [ ] **Step 2: Insert the pre-size call**

After the line `t1->mustuse_cblkdatabuffer = job->mustuse_cblkdatabuffer;`, insert:

```c
    /* D6.2: pre-size t1 buffers to the tile-component's max codeblock dims so
     * the per-codeblock opj_t1_allocate_buffers call (below, around line 2019)
     * is a no-op. tccp->cblkw / cblkh are log2 exponents per the J2K spec.
     */
    if (!opj_t1_allocate_buffers(t1, 1u << tccp->cblkw, 1u << tccp->cblkh)) {
        opj_event_msg(job->p_manager, EVT_ERROR,
                      "Cannot pre-size T1 buffers for tile component\n");
        *(job->pret) = OPJ_FALSE;
        opj_free(job);
        return;
    }
```

The resulting block should read:

```c
    t1->mustuse_cblkdatabuffer = job->mustuse_cblkdatabuffer;

    /* D6.2: pre-size t1 buffers to the tile-component's max codeblock dims so
     * the per-codeblock opj_t1_allocate_buffers call (below, around line 2019)
     * is a no-op. tccp->cblkw / cblkh are log2 exponents per the J2K spec.
     */
    if (!opj_t1_allocate_buffers(t1, 1u << tccp->cblkw, 1u << tccp->cblkh)) {
        opj_event_msg(job->p_manager, EVT_ERROR,
                      "Cannot pre-size T1 buffers for tile component\n");
        *(job->pret) = OPJ_FALSE;
        opj_free(job);
        return;
    }

    if ((tccp->cblksty & J2K_CCP_CBLKSTY_HT) != 0) {
```

- [ ] **Step 3: Verify `opj_t1_allocate_buffers` signature and return type**

Run:
```bash
grep -n "opj_t1_allocate_buffers" src/lib/openjp2/t1.c | head -5
```
Confirm the function is declared `static OPJ_BOOL opj_t1_allocate_buffers(opj_t1_t *t1, OPJ_UINT32 w, OPJ_UINT32 h)` and is in the same translation unit as the call site (no forward decl needed if it appears before line 1689; if not, move the function up or add a forward decl). Read the function body at t1.c:1451-1520 to confirm it returns `OPJ_TRUE` on success and `OPJ_FALSE` on allocation failure.

If the function is defined *after* line 1689 (file ordering), add a forward declaration just above `opj_t1_clbl_decode_processor`:

```c
static OPJ_BOOL opj_t1_allocate_buffers(opj_t1_t *t1, OPJ_UINT32 w, OPJ_UINT32 h);
```

- [ ] **Step 4: Rebuild**

Run:
```bash
cmake --build build -j
```
Expected: build succeeds.

- [ ] **Step 5: Run the test suite**

Run:
```bash
ctest --test-dir build --output-on-failure 2>&1 | tee /tmp/openjp2k-task5-tests.log
tail -5 /tmp/openjp2k-task5-tests.log
```
Expected: pass/fail counts identical to baseline. The pre-size is observationally a no-op (the lazy grow path would have grown the buffers to the same max anyway, just incrementally).

- [ ] **Step 6: Spot-check that the per-codeblock call has become a no-op**

This is a sanity check, not a formal verification. Temporarily add an `assert(0 && "should not grow")` inside the `if (datasize > t1->datasize)` branch at t1.c:1469, rebuild, and run a single small test (e.g. one ctest item that exercises decode). The assertion must not fire. Then **remove the assert** before continuing.

Run:
```bash
ctest --test-dir build -R "decode" -j1 --output-on-failure 2>&1 | head -40
```
If the assert fires, the pre-size call is not being reached on that path — investigate before the next step.

- [ ] **Step 7: Remove the spot-check assert**

If the assert was added in Step 6, remove it now. Rebuild and re-run the test suite to confirm baseline pass/fail unchanged.

- [ ] **Step 8: Commit**

```bash
git add src/lib/openjp2/t1.c
git commit -m "$(cat <<'EOF'
Pre-size T1 data/flags buffers at decode-processor entry (D6.2)

Allocate t1->data and t1->flags up-front to the tile-component's max
codeblock dimensions (1<<tccp->cblkw, 1<<tccp->cblkh) on first
opj_t1_clbl_decode_processor call for a given worker. The per-codeblock
opj_t1_allocate_buffers call later in the function becomes a no-op
because the grow checks (datasize > t1->datasize, flagssize > t1->flagssize)
are no longer satisfied.

Eliminates the cold-codeblock realloc path. Observable behavior unchanged.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Final verification

**Files:** none modified.

- [ ] **Step 1: Clean rebuild from scratch**

Run:
```bash
rm -rf build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j
```
Expected: build succeeds cleanly.

- [ ] **Step 2: Run the full test suite**

Run:
```bash
ctest --test-dir build --output-on-failure 2>&1 | tee /tmp/openjp2k-final-tests.log
tail -5 /tmp/openjp2k-final-tests.log
```
Expected: pass/fail counts match the baseline captured in Task 1, Step 3. Any deviation must be diagnosed.

- [ ] **Step 3: Verify the OPJ_T1_LEGACY_ONLY build still works**

Run:
```bash
cmake -S . -B build-legacy -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DOPJ_T1_LEGACY_ONLY=ON
cmake --build build-legacy -j
ctest --test-dir build-legacy --output-on-failure 2>&1 | tail -5
rm -rf build-legacy
```
Expected: identical pass/fail counts. The macro is defined but not yet checked by any code path, so behavior is identical to the default build.

- [ ] **Step 4: Confirm commit history is clean**

Run:
```bash
git log --oneline main..HEAD
```
Expected: exactly four commits, in order:
1. `Add t1_fast.{c,h} scaffolding for Sub-project 1`
2. `Add OPJ_T1_LEGACY_ONLY CMake option`
3. `Remove dead opj_t1_t::encoder field`
4. `Pre-size T1 data/flags buffers at decode-processor entry (D6.2)`

If the count or order differs, investigate before opening a PR.

- [ ] **Step 5: No final commit**

The work is complete. PR creation is a separate human decision; this plan does not push or open one.

---

## Notes for follow-up plans

- **D6.1 (struct split) — partial.** This plan landed only the dead-field
  removal portion of D6.1 (the `encoder` boolean). The full struct split
  (`opj_t1_t` → `opj_t1_dec_t` + `opj_t1_enc_t`) was deferred because the
  marginal cache-footprint win is small (~16 bytes per worker) and the
  decoder-only field set will be clearer after D1 work surfaces what the
  fast paths actually need on the struct. Revisit when planning D1 or D4.
- **D6.3 (job pool)** — needs a survey of `opj_thread_pool_submit_job` semantics in `thread.c`: who owns the job pointer between submission and consumption, whether jobs may be cancelled, and whether the main thread blocks on completion. The shape of the freelist (per-pool with locking vs per-thread with handoff) depends on those answers.

  *(Investigated 2026-05-17. Findings: the thread pool takes ownership of an internal wrapper struct but the user_data — our job struct — is fully released to user code after the callback runs and is freed explicitly by the T1 worker at 8 sites in `opj_t1_clbl_decode_processor`. Wait is `opj_thread_pool_wait_completion(tp, 0)` in tcd.c:2075, called once after the loop that invokes `opj_t1_decode_cblks` per component. No cancellation. The cleanest design is **a per-decode-call arena** (chunked bump allocator or pre-counted single calloc) allocated by tcd.c, threaded through `opj_t1_decode_cblks` via a signature change — t1.h is internal-only, so the ABI change is acceptable. Cost: significant API surface change touching tcd.c/t1.c/t1.h plus arena infrastructure. Estimated win at 2–5% wallclock per tile decode on heavy-codeblock workloads. **Recommendation: defer until the bench is live and we can measure the actual win before paying the refactor cost.**)*
- **D6.4 (decoded_data buffer reuse)** — needs a redesign of `opj_tcd_cblk_dec_t::decoded_data` ownership. Currently the buffer is freed by tcd.c cleanup paths (tcd.c:1378, tcd.c:2389), which assumes per-cblk ownership. A reusable per-thread buffer would change that to "data is copied out of decoded_data and into tilec->data_win before the next codeblock reuses the buffer." Need to audit whether anything outside the t1 processor reads `cblk->decoded_data` between codeblocks.

  *(Investigated 2026-05-17. Findings: `cblk->decoded_data` is read by **dwt.c:2812-2890** in `opj_dwt_init_sparse_array` to populate a sparse array that feeds the partial-tile IDWT — AFTER the T1 worker thread has returned. The buffer is freed at tile teardown (tcd.c:2389), not at end-of-decode-call. Making it reusable requires either (a) moving sparse-array build into the T1 worker, (b) adding a copy that defeats the purpose, or (c) restructuring the T1→DWT data handoff. None are small. **Recommendation: defer until SP-1's fast paths land** — the new T1 dispatch may want different decoded_data routing anyway, and re-engineering twice is wasteful.)*
- **D1 (branchless MQ + packed state LUT)** — first deliverable that needs the cleanroom Grok report and the bench harness. Plan that deliverable only after Sub-projects 0 and 0.5 have landed.
