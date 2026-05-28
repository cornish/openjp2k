# D8 — AVX2 DC Level Shift (Float Path) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Vectorize the per-pixel float clamp + level shift loop at `tcd.c:2419-2436` (inside `opj_tcd_dc_level_shift_decode`, the float-path branch). The SP-2.1a profile localized 9% of single-thread runtime on 8-bit lossy decode to this scalar loop; grok's equivalent post-processing is ~2%. Target: ~7-pp shift on lossy slices.

**Architecture:** Add a static AVX2 helper `opj_tcd_dc_level_shift_decode_float_avx2` near the existing scalar function in `tcd.c`. Replace the scalar float-path body with `#ifdef __AVX2__` dispatch to the helper. Scalar fallback preserved for `OPJ_ENABLE_AVX2=OFF` builds. SIMD strategy: pre-clamp float to `[lmin-dc_shift, lmax-dc_shift]` (safe range after add) → `_mm256_cvtps_epi32` (MXCSR rounding matches `opj_lrintf`) → `_mm256_add_epi32(dc_shift)` → store. Skips int64 overflow guard the scalar needs.

**Tech Stack:** C99, AVX2 intrinsics (`<immintrin.h>`), CMake (no new options — rides on existing `OPJ_ENABLE_AVX2` from SP3.1).

---

## File Structure

**Modified:**
- `src/lib/openjp2/tcd.c` — add the AVX2 helper (~70 LOC) above `opj_tcd_dc_level_shift_decode`; replace the float-path scalar body inside the function with `#ifdef __AVX2__` dispatch (~10 LOC delta). ~80 LOC total added.

**Not modified:** any other file. No new CMake options. No new header API (helper is static).

---

## Task 1: Implement the AVX2 helper + dispatch (atomic commit)

The helper and the dispatch must land together — adding the helper without the dispatch leaves it unreferenced (`-Werror=unused-function`).

**Files:**
- Modify: `src/lib/openjp2/tcd.c`

### Step 1: Read the existing scalar function for context

```bash
cd /home/cornish/GitHub/openjp2k
sed -n '2352,2441p' src/lib/openjp2/tcd.c
```

Note specifically:
- Function header: `static OPJ_BOOL opj_tcd_dc_level_shift_decode(opj_tcd_t *p_tcd)` at line 2352.
- `l_min` / `l_max`: computed at lines 2398-2402 based on `l_img_comp->prec` and signedness.
- `l_tccp->m_dc_level_shift`: the additive shift, OPJ_INT32.
- Float-path branch starts at line 2419 (`} else {`); ends at line 2437. This is what D8 replaces.
- Per-pixel `l_current_ptr` is `OPJ_INT32*` but the loop reads it as `OPJ_FLOAT32` via `*((OPJ_FLOAT32 *) l_current_ptr)`.
- `l_stride` is the gap between rows (in int32 elements). Row advance: `l_current_ptr += l_stride;` at line 2435.

### Step 2: Verify `OPJ_ENABLE_AVX2` already sets `__AVX2__`

```bash
grep -n 'OPJ_ENABLE_AVX2\|__AVX2__' src/lib/openjp2/CMakeLists.txt src/lib/openjp2/dwt.c 2>/dev/null | head -10
```

Confirm SP3.1's wiring: `OPJ_ENABLE_AVX2=ON` adds `-mavx2` to the compile flags, which causes the compiler to define `__AVX2__` automatically. The new helper uses `#ifdef __AVX2__` as the build-time gate (same pattern as the SP3.2 int16 5/3 helpers in `dwt.c`).

### Step 3: Add the AVX2 helper at the top of the function block

Open `src/lib/openjp2/tcd.c` and locate line 2352 (`static OPJ_BOOL opj_tcd_dc_level_shift_decode`). Add the helper as a separate `static` function immediately ABOVE it:

```c
#ifdef __AVX2__
#include <immintrin.h>

/* D8: AVX2 inner loop for the float-path branch of
 * opj_tcd_dc_level_shift_decode (used by 9/7 lossy decode).
 *
 * Equivalence to the scalar:
 *   pre-clamp float to [(float)(lmin-dc_shift), (float)(lmax-dc_shift)]
 *   _mm256_cvtps_epi32 (MXCSR rounding; default round-to-nearest-even
 *                       matches opj_lrintf semantics)
 *   add dc_shift as int32 (no overflow: pre-clamp keeps the result
 *                          in [lmin, lmax] post-add)
 *   store
 *
 * Eight pixels per iteration; scalar tail handles the remaining <8.
 * The pre-clamp absorbs the scalar's three branches (>INT_MAX,
 * <INT_MIN, in-range) into a single min/max because for any
 * reasonable JP2K dc_shift (≤ ±32768 for 16-bit prec), the
 * float-domain pre-clamp bounds are exactly representable in IEEE-754
 * single precision and the post-add result is identical to the
 * scalar's three-branch dispatch. */
static void opj_tcd_dc_level_shift_decode_float_avx2(
    OPJ_INT32* data,
    OPJ_UINT32 width,
    OPJ_UINT32 height,
    OPJ_UINT32 stride,
    OPJ_INT32 dc_shift,
    OPJ_INT32 lmin,
    OPJ_INT32 lmax)
{
    const __m256 vfmin = _mm256_set1_ps((float)lmin - (float)dc_shift);
    const __m256 vfmax = _mm256_set1_ps((float)lmax - (float)dc_shift);
    const __m256i vdc  = _mm256_set1_epi32(dc_shift);
    OPJ_UINT32 j;

    for (j = 0; j < height; ++j) {
        OPJ_INT32* row = data + (OPJ_SIZE_T)j * (width + stride);
        OPJ_UINT32 i = 0;
        for (; i + 8 <= width; i += 8) {
            __m256 v = _mm256_loadu_ps((const float*)(row + i));
            v = _mm256_min_ps(_mm256_max_ps(v, vfmin), vfmax);
            __m256i vi = _mm256_cvtps_epi32(v);
            vi = _mm256_add_epi32(vi, vdc);
            _mm256_storeu_si256((__m256i*)(row + i), vi);
        }
        /* Scalar tail: <8 remaining pixels on this row. */
        for (; i < width; ++i) {
            OPJ_FLOAT32 v = *((OPJ_FLOAT32*)(row + i));
            if (v > (OPJ_FLOAT32)INT_MAX) {
                row[i] = lmax;
            } else if (v < INT_MIN) {
                row[i] = lmin;
            } else {
                OPJ_INT64 vi64 = (OPJ_INT64)opj_lrintf(v);
                row[i] = (OPJ_INT32)opj_int64_clamp(vi64 + dc_shift, lmin, lmax);
            }
        }
    }
}
#endif /* __AVX2__ */
```

**Important detail on row stride:** the scalar loop advances `l_current_ptr` element by element and adds `l_stride` after each row (`l_current_ptr += l_stride;` at line 2435). The helper computes `row = data + j * (width + stride)` to land on the start of row `j`. Mathematically equivalent: each row advances `width + stride` elements from the previous row's start, matching the scalar's `width` per-element steps + `stride` row-end addition.

### Step 4: Replace the float-path scalar body with the dispatch

Locate `tcd.c:2419-2437` (the `} else {` float branch and its closing `}`). Replace the body inside that else block with:

```c
        } else {
#ifdef __AVX2__
            opj_tcd_dc_level_shift_decode_float_avx2(
                l_current_ptr, l_width, l_height, l_stride,
                l_tccp->m_dc_level_shift, l_min, l_max);
#else
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
                                             l_value_int + l_tccp->m_dc_level_shift, l_min, l_max);
                    }
                    ++l_current_ptr;
                }
                l_current_ptr += l_stride;
            }
#endif
        }
```

The `#else` branch is the unchanged scalar loop (literally copy-pasted from the current code at lines 2420-2436). When `__AVX2__` is defined, only the helper call compiles in; the scalar is preserved verbatim for `OPJ_ENABLE_AVX2=OFF` builds.

### Step 5: Build AVX2-ON

```bash
cmake -S . -B build-d8 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=ON -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-d8 -j 2>&1 | tail -5
```

Expected: clean build, no warnings. The helper is static, used by exactly one caller — no `-Werror=unused-function`.

### Step 6: Build AVX2-OFF

```bash
cmake -S . -B build-d8-noavx2 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=OFF -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-d8-noavx2 -j 2>&1 | tail -5
```

Expected: clean build. The helper is not compiled (under `#ifdef __AVX2__`); scalar runs in the `#else` branch.

### Step 7: Conformance with `OPJ_ENABLE_AVX2=ON`

```bash
scripts/run-conformance.sh -B build-d8 -- -R NR-DEC -j4 2>&1 | tail -15
```

Expected: 545/553 pass, exactly the 8 pre-existing failures (`_00042`, `kodak_2layers_lrcp`×2, `issue205`, `issue135`, `issue208`, `issue226`×2).

**Any 9th failure → STOP.** Most likely cause: rounding-mode mismatch between `_mm256_cvtps_epi32` (MXCSR) and `opj_lrintf` (libc), producing 1-LSB differences. Diagnose by computing the byte-cmp delta on a known-good file and inspecting which bytes differ — if a uniform ±1 offset on some pixels, it's rounding.

Fix if rounding-mismatch surfaces: replace `_mm256_cvtps_epi32` with explicit `_mm256_round_ps(v, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC)` followed by `_mm256_cvttps_epi32` (truncating cast after explicit rounding).

### Step 8: Manual byte-cmp matrix

Three **lossy** files (D8 changes their decode path) × two builds (AVX2-ON vs OFF):

```bash
F8=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossy_l1_mon_enone.jp2
F12=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono12_1024/pLRCP_d5_b64_t1024x1024_lossy_l1_moff_enone.j2k
F16=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono16_1024/pLRCP_d5_b64_t1024x1024_lossy_l1_moff_enone.j2k

for F in "$F8" "$F12" "$F16"; do
  build-d8/bin/opj_decompress -i "$F" -o /tmp/on.raw > /dev/null 2>&1
  build-d8-noavx2/bin/opj_decompress -i "$F" -o /tmp/off.raw > /dev/null 2>&1
  cmp -s /tmp/on.raw /tmp/off.raw && echo "OK   $(basename "$F")" || echo "FAIL $(basename "$F")"
done
```

Expected: 3/3 OK.

Then three **lossless** files (D8 should NOT affect their decode — int path stays scalar):

```bash
L8=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_mon_enone.jp2
L12=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono12_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k
L16=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono16_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k

for F in "$L8" "$L12" "$L16"; do
  build-d8/bin/opj_decompress -i "$F" -o /tmp/on.raw > /dev/null 2>&1
  build-d8-noavx2/bin/opj_decompress -i "$F" -o /tmp/off.raw > /dev/null 2>&1
  cmp -s /tmp/on.raw /tmp/off.raw && echo "OK   $(basename "$F")" || echo "FAIL $(basename "$F")"
done
```

Expected: 3/3 OK (unchanged behavior; the int-path branch wasn't touched).

Total: 6/6 byte-cmp pass.

### Step 9: Quick perf re-check on the worst-gap file

Confirm the regression localized in SP-2.1a's profile is closed:

```bash
F=/home/cornish/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d1_b32_t1024x1024_lossy_l1_moff_esop_eph.jp2
for label in "openjp2k D8" "openjpeg vanilla"; do
  case $label in
    "openjp2k D8") BIN=build-d8/bin/opj_decompress ;;
    "openjpeg vanilla") BIN=/tmp/openjpeg-vanilla/bin/opj_decompress ;;
  esac
  echo -n "$label: "
  { for i in $(seq 1 30); do $BIN -i $F -o /tmp/_p.raw 2>&1 | grep 'decode time' | awk '{print $3}'; done; } | sort -n | head -10 | awk '{s+=$1; n++} END {printf "%.2f ms (mean of 10 best)\n", s/n}'
done
```

(`build-perf/bin/opj_decompress` from earlier sessions had `-g`; use `build-d8/bin/opj_decompress` for the actual D8 release timing.)

Expected: openjp2k D8 around 12 ms (matches openjpeg or beats it), vs the pre-D8 ~14 ms. If still ~14 ms, the SIMD helper isn't engaging — check the dispatch.

### Step 10: Stage and commit

```bash
git add src/lib/openjp2/tcd.c
git status --short
git diff --cached --stat
```

Expected: 1 file, ~80-90 LOC added.

```bash
git commit -m "$(cat <<'EOF'
tcd: AVX2 inner loop for DC level shift float path (D8)

Vectorizes the per-pixel float clamp + level shift loop at
tcd.c:2419-2437, the final post-processing step for lossy (9/7)
decode.  SP-2.1a's openjp2k-vs-grok profile localized 9% of
single-thread runtime to this scalar loop; grok's equivalent
postProcessBlock is ~2%.

SIMD strategy: pre-clamp float to [lmin-dc_shift, lmax-dc_shift]
so the post-add int32 result is guaranteed in [lmin, lmax] with no
overflow — skips the scalar's int64 overflow guard.
_mm256_cvtps_epi32 uses MXCSR rounding (default round-to-nearest-
even) which matches opj_lrintf's semantics, so output is
byte-identical to the scalar.

8 pixels per iteration; scalar tail handles the remaining <8.
Rides on the existing OPJ_ENABLE_AVX2 cmake opt-in from SP3.1
(via __AVX2__).  AVX2-OFF builds run the unchanged scalar.

Lossless (int-path) decode unchanged; D9 may follow if its
profile justifies vectorization.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Bench gate + outcome + merge + tag

Same-day baseline, paired ratios, iter at t=1,2,4,8. Per the run_bench.sh fix from earlier (`c07c9c3`), --threads 1,2,4,8 spawns one bench process per thread count automatically — grok numbers correct at every t.

**Files:**
- Modify: `docs/superpowers/specs/2026-05-27-d8-dc-level-shift-avx2-design.md`

### Step 1: Set up the SP-2.1a baseline worktree

```bash
cd /home/cornish/GitHub/openjp2k
git worktree add /home/cornish/GitHub/openjp2k-sp21a-baseline v0.11.0-sp2-1a-task-graph-infra
```

### Step 2: Rebuild bench against baseline, run iter at t=1,2,4,8

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source /home/cornish/GitHub/openjp2k-sp21a-baseline 2>&1 | tail -3
nohup ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt --threads 1,2,4,8 corpus/public/ > results/d8_baseline_$(date +%Y%m%d_%H%M%S).jsonl 2> results/d8_baseline_$(date +%Y%m%d_%H%M%S).log
echo D8_BASELINE_DONE
```

Wall time: ~3-4 hours (iter × 4 thread counts).

### Step 3: Rebuild against D8 head, run the same

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source /home/cornish/GitHub/openjp2k 2>&1 | tail -3
nohup ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt --threads 1,2,4,8 corpus/public/ > results/d8_head_$(date +%Y%m%d_%H%M%S).jsonl 2> results/d8_head_$(date +%Y%m%d_%H%M%S).log
echo D8_HEAD_DONE
```

### Step 4: Paired-ratio analysis

```python
import json, math, glob
from collections import defaultdict

def load(p):
    fd = defaultdict(lambda: defaultdict(dict))
    meta = {}
    for l in open(p):
        if '"type"' not in l: continue
        r = json.loads(l)
        if r.get('type') != 'result': continue
        if r.get('error') or r['timing_s']['min'] <= 0: continue
        fd[r['file']][r['threads']][r['decoder']] = r['timing_s']['min'] * 1000.0
        meta[r['file']] = (r['bit_depth'], r['channels'])
    return fd, meta

def gm(xs):
    xs = [x for x in xs if x and x>0]
    return math.exp(sum(map(math.log, xs))/len(xs)) if xs else None

base, m = load(sorted(glob.glob('results/d8_baseline_*.jsonl'))[-1])
head, _ = load(sorted(glob.glob('results/d8_head_*.jsonl'))[-1])

print(f"{'Slice':22} {'t':>2}  {'n':>3}  {'D8 delta':>18}  {'base v oje':>11}  {'head v oje':>11}")
for t in (1, 2, 4, 8):
    for slice_name in ['overall', '8-bit lossy', '12-bit lossy', '16-bit lossy',
                       '8-bit lossless', '12-bit lossless', '16-bit lossless']:
        deltas, abs_b, abs_h = [], [], []
        for f in set(base) & set(head):
            if t not in base[f] or t not in head[f]: continue
            a, b = base[f][t], head[f][t]
            if not all(d in a and d in b for d in ('openjp2k', 'openjpeg')): continue
            bd, _ = m[f]
            mode = 'lossless' if 'lossless' in f else ('lossy' if 'lossy' in f else '?')
            if slice_name != 'overall' and slice_name != f'{bd}-bit {mode}': continue
            r_b = a['openjpeg']/a['openjp2k']
            r_h = b['openjpeg']/b['openjp2k']
            deltas.append(r_h/r_b); abs_b.append(r_b); abs_h.append(r_h)
        gd, gb, gh = gm(deltas), gm(abs_b), gm(abs_h)
        if gd:
            print(f"{slice_name:22} t={t}  {len(deltas):>3}  "
                  f"{(gd-1)*100:+6.2f}% (gm {gd:.4f})  "
                  f"{(gb-1)*100:+7.2f}%  {(gh-1)*100:+7.2f}%")
    print()
```

### Step 5: Verify the gate

| Gate row | Expected | Pass |
|---|---:|---|
| 8-bit lossy iter t=1 | ~+5% | ≥ +3% |
| 8-bit lossy iter t=8 | ~+8-10% | ≥ +5% |
| 12-bit lossy iter t=8 | ~+5% | ≥ +3% |
| 16-bit lossy iter t=8 | parity-positive | within ±1% |
| Lossless slices any t | unchanged | within ±0.5% |
| Iter overall t=8 | ~+3% | ≥ +2% |

If 8-bit lossy iter t=8 improves < +3%, the dispatch isn't engaging or the SIMD helper has hidden overhead — verify with the perf re-check (Step 9 of Task 1) on the bench's installed library. If any lossless slice regresses > 0.5%, the dispatch leaked into the int path — re-read the diff against the original `} else {` branch.

### Step 6: Append outcome to the D8 spec

Mirror the D7.1 / SP-2.1a outcome pattern. Sections:
- **Outcome (date)** — commit SHA.
- **Implementation notes** — any surprises (rounding-mode held? Stride math right? Anything in conformance unexpected?).
- **Bench** — full slice table from Step 4 + 5.
- **Project-level position shift** — what does iter overall t=8 look like absolute vs openjpeg, vs grok?
- **Carry-over** — D9 (int path) candidate measurements? Anything to chase next?

### Step 7: Commit outcome + merge + tag

```bash
git add docs/superpowers/specs/2026-05-27-d8-dc-level-shift-avx2-design.md
git commit -m "Spec: D8 outcome — <fill in headline %>"
git checkout main
git merge --ff-only feat/d8-dc-level-shift-avx2
git tag -a v0.12.0-d8-dc-level-shift-avx2 -m "D8 — AVX2 DC level shift float path; <headline %> on 8-bit lossy iter t=8"
git push origin main
git push origin v0.12.0-d8-dc-level-shift-avx2
```

### Step 8: Memory + cleanup

- Save `d8-retrospective.md` in `~/.claude/projects/-home-cornish-GitHub-openjp2k/memory/` with what landed + lessons.
- Add a one-line entry in MEMORY.md.
- `git branch -d feat/d8-dc-level-shift-avx2`
- `git worktree remove /home/cornish/GitHub/openjp2k-sp21a-baseline`

---

## Summary checklist

- [ ] Task 1: D8 implementation atomic commit; AVX2-ON + AVX2-OFF builds clean; conformance shows 8 pre-existing failures; byte-cmp 6/6 (3 lossy + 3 lossless); perf re-check shows ~12 ms (was ~14 ms) on the worst-gap reference file.
- [ ] Task 2: Iter at t=1,2,4,8 baseline + head; 8-bit lossy iter t=8 ≥ +5%; no lossless regression; outcome appended + committed; tag `v0.12.0-d8-dc-level-shift-avx2` published; memory + cleanup done.
