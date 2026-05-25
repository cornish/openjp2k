# D7 — T1 Conditional Fast/Legacy Dispatch Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Gate D1's "fast" T1 entropy-decode path on `whole_tile_decoding` so it runs only for full-tile decode, not partial-tile (ROI) decode. The fast path's per-codeblock setup cost amortizes well on full-tile (many cblks per tile) but dominates over its faster inner loop on partial-tile (fewer cblks per tile). Predicted: ROI smoke overall +1.9%, ROI 16-bit lossless +6.3%; full-tile unchanged.

**Architecture:** Add `OPJ_BOOL whole_tile_decoding;` field to `opj_t1_t` (alongside `mustuse_cblkdatabuffer`). Propagate from `tcd->whole_tile_decoding` through `opj_t1_decode_cblks` → per-cblk job struct → per-worker `t1` struct, mirroring the existing `mustuse_cblkdatabuffer` flow. Change the dispatch at `t1.c:1933` to AND the existing fast-enabled check with the new flag. Env var `OPJ_T1_FAST=0` still forces legacy everywhere (preserves existing semantics for callers who set it).

**Tech Stack:** C99, OPJ_BOOL typedef from openjpeg.h.

---

## File Structure

**Modified:**
- `src/lib/openjp2/t1.h` — add `OPJ_BOOL whole_tile_decoding;` to the `opj_t1` struct (next to `mustuse_cblkdatabuffer` at line 204).
- `src/lib/openjp2/t1.c` — three edits:
  1. Add `OPJ_BOOL whole_tile_decoding;` to the per-cblk job struct (locate where `mustuse_cblkdatabuffer` is declared on the job — search around line 1515).
  2. In `opj_t1_decode_cblks` (around line 1895), set the job's new flag from `tcd->whole_tile_decoding` at the same site that already sets `job->mustuse_cblkdatabuffer`.
  3. In `opj_t1_clbl_decode_processor` (around line 1606), copy `t1->whole_tile_decoding = job->whole_tile_decoding;` alongside the existing `t1->mustuse_cblkdatabuffer = job->mustuse_cblkdatabuffer;`.
  4. In `opj_t1_decode_cblk` at line 1933, change `int use_fast = opj_t1_fast_enabled();` to `int use_fast = opj_t1_fast_enabled() && t1->whole_tile_decoding;`.
- `src/lib/openjp2/tcd.c` — no changes (the existing call to `opj_t1_decode_cblks(p_tcd, ...)` at line 2155 already passes `p_tcd`; the new propagation reads `tcd->whole_tile_decoding` inside `opj_t1_decode_cblks`, no new caller-side wiring).

**Not modified:** any other file. The deliverable is dispatch-only.

---

## Task 1: Implement the conditional dispatch (single atomic commit)

All three propagation hops must land together — the field declarations must match across struct definitions or build fails. One commit.

**Files:**
- Modify: `src/lib/openjp2/t1.h`
- Modify: `src/lib/openjp2/t1.c`

### Step 1: Read the canonical propagation pattern (`mustuse_cblkdatabuffer`)

```bash
cd /home/cornish/GitHub/openjp2k
grep -n 'mustuse_cblkdatabuffer' src/lib/openjp2/t1.h src/lib/openjp2/t1.c
```

You should see five hits:
- `t1.h:204` — field declaration on `opj_t1_t`
- `t1.c:1515` — field declaration on the per-cblk job struct (look at the surrounding `struct` block to identify the struct name; likely `opj_t1_cblk_decode_processing_job_t` or similar)
- `t1.c:1606` — per-worker handler copying `t1->X = job->X`
- `t1.c:1895` — per-tile setup in `opj_t1_decode_cblks` setting `job->X`
- `t1.c:1972` — read site (not relevant to D7)

Read 10-20 lines around each. The pattern is: job struct field → set in dispatcher → copy to worker t1 → read in worker. The new `whole_tile_decoding` field follows the same path.

### Step 2: Add `whole_tile_decoding` to `opj_t1_t` in `t1.h`

Open `src/lib/openjp2/t1.h` near line 204 and locate:

```c
    OPJ_BOOL     mustuse_cblkdatabuffer;
```

Add immediately after it:

```c
    /* D7: whether the surrounding tile is being whole-tile-decoded.
     * Set by opj_t1_decode_cblks() from tcd->whole_tile_decoding.
     * Used by the dispatch in opj_t1_decode_cblk() to gate D1's
     * "fast" T1 path: fast amortizes on full-tile but its per-cblk
     * setup cost dominates on partial-tile. */
    OPJ_BOOL     whole_tile_decoding;
```

### Step 3: Add the field to the per-cblk job struct in `t1.c`

Locate the job struct definition around `t1.c:1515` (the line with `OPJ_BOOL mustuse_cblkdatabuffer;` outside the `opj_t1_t` struct). Identify the enclosing `struct` (the per-cblk job descriptor). Add immediately after the existing flag:

```c
    OPJ_BOOL whole_tile_decoding;
```

No need for a comment here — the field's purpose is documented on `opj_t1_t`.

### Step 4: Set the job's `whole_tile_decoding` in `opj_t1_decode_cblks`

Near `t1.c:1895`, find:

```c
                    job->mustuse_cblkdatabuffer = opj_thread_pool_get_thread_count(tp) > 1;
```

Add immediately after it:

```c
                    job->whole_tile_decoding = tcd->whole_tile_decoding;
```

(`tcd` is the first parameter to `opj_t1_decode_cblks`; it's in scope.)

### Step 5: Copy job → t1 in `opj_t1_clbl_decode_processor`

Near `t1.c:1606`, find:

```c
    t1->mustuse_cblkdatabuffer = job->mustuse_cblkdatabuffer;
```

Add immediately after it:

```c
    t1->whole_tile_decoding = job->whole_tile_decoding;
```

### Step 6: Change the dispatch at `t1.c:1933`

Find:

```c
    int use_fast = opj_t1_fast_enabled();
```

Change to:

```c
    int use_fast = opj_t1_fast_enabled() && t1->whole_tile_decoding;
```

### Step 7: Build AVX2-ON

```bash
cd /home/cornish/GitHub/openjp2k
cmake -S . -B build-d7 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=ON -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-d7 -j 2>&1 | tail -5
```

Expected: clean build, no warnings. If `-Werror=missing-field-initializers` fires on the job struct, check that all initializers (designated or positional) are updated. If `-Werror=missing-prototypes` fires, the new field on `opj_t1_t` shouldn't trigger it (struct fields aren't subject to this), but the job struct may need an updated initializer expression elsewhere — search for `mustuse_cblkdatabuffer =` to find all the places the job is initialized and confirm they're either struct-zeroed or set the new field explicitly.

### Step 8: Build AVX2-OFF

```bash
cmake -S . -B build-d7-noavx2 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=OFF -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-d7-noavx2 -j 2>&1 | tail -5
```

Expected: clean build. With AVX2-OFF, the fast path code is still compiled (it's gated at runtime by `opj_t1_fast_enabled()`, not at compile time), so the new flag still wires through.

### Step 9: Conformance with `OPJ_ENABLE_AVX2=ON`

```bash
scripts/run-conformance.sh -B build-d7 -- -R NR-DEC -j4 2>&1 | tail -15
```

Expected: 545/553 pass, exactly 8 failures, the same fingerprint listed in the SP3.2 conformance Step 13 (`_00042`, `kodak_2layers`×2, `issue205`, `issue135`, `issue208`, `issue226`×2).

Any 9th failure → STOP. The byte-exactness invariant says fast and legacy produce identical output for any single cblk; conditional dispatch shouldn't change behavior, only timing. If conformance moves, the change is wrong.

### Step 10: Conformance with `OPJ_T1_FAST=0` (override path sanity)

```bash
OPJ_T1_FAST=0 scripts/run-conformance.sh -B build-d7 -- -R NR-DEC -j4 2>&1 | tail -10
```

Expected: same 8 failures. This confirms the existing env-var override semantics still work after the AND change.

### Step 11: Manual byte-cmp on three 8-bit files, full + ROI regions

Three files × two regions × {AVX2-ON, AVX2-OFF} = 12 decode invocations. Confirm every (file, region) pair is byte-exact.

```bash
F1=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_mon_enone.jp2
F2=$HOME/GitHub/openjp2k-data/corpus/archival/loc-maps/07481_1926-0001.jp2
F3=$HOME/GitHub/openjp2k-data/corpus/medical/J2KR_US1_J2KR.j2k

for F in "$F1" "$F2" "$F3"; do
  for REGION in "" "-d 256,256,768,768"; do
    build-d7/bin/opj_decompress -allow-partial $REGION -i "$F" -o /tmp/on.raw > /dev/null 2>&1
    build-d7-noavx2/bin/opj_decompress -allow-partial $REGION -i "$F" -o /tmp/off.raw > /dev/null 2>&1
    if cmp -s /tmp/on.raw /tmp/off.raw; then
      echo "OK   $(basename "$F")  region='$REGION'"
    else
      echo "FAIL $(basename "$F")  region='$REGION'"
    fi
  done
done
```

Expected: 6 OK lines (or 9 if you keep the SP3.2b 3-region sweep). Any FAIL → STOP.

### Step 12: Stage and commit

```bash
git add src/lib/openjp2/t1.h src/lib/openjp2/t1.c
git status --short
git diff --cached --stat
```

Expected: 2 files, ~10-15 LOC added total.

```bash
git commit -m "$(cat <<'EOF'
T1: conditional fast/legacy dispatch on whole-tile-decoding (D7)

D1's "fast" T1 entropy-decode path (commits 512eb96d..f5978a62)
amortizes well on full-tile decode (many cblks per tile) but its
per-cblk setup cost dominates on partial-tile (ROI) decode (fewer
cblks per tile, same fixed setup).  This addresses
cornish/openjp2k#3 by gating the fast path on whole-tile-decoding.

Propagation mirrors the existing mustuse_cblkdatabuffer flow:
- whole_tile_decoding field added to opj_t1_t (t1.h:204) and to
  the per-cblk job struct (t1.c).
- opj_t1_decode_cblks copies tcd->whole_tile_decoding to job.
- opj_t1_clbl_decode_processor copies job to t1.
- opj_t1_decode_cblk:1933 ANDs the existing fast-enabled check
  with the new flag.

OPJ_T1_FAST=0 still forces legacy everywhere (existing semantics
preserved for callers that explicitly set it).  Default behavior
changes silently from "fast everywhere" to "fast on full-tile,
legacy on partial-tile" — a workload-driven choice based on the
profile + paired-bench investigation captured in the spec.

Conformance unchanged (T1 fast and legacy are byte-exact in
either branch).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Bench gate

Single new bench run pair (ROI + full-tile smokes against the D7 head), then paired-ratio analysis vs the existing baselines.

**Files:** None modified — bench-only.

### Step 1: Rebuild bench against the D7 head

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source ~/GitHub/openjp2k 2>&1 | tail -5
```

### Step 2: Run ROI smoke

```bash
nohup ./scripts/run_smoke_roi.sh > results/roi_smoke_d7_$(date +%Y%m%d_%H%M%S).jsonl 2> results/roi_smoke_d7_$(date +%Y%m%d_%H%M%S).log &
wait %1 && echo DONE
```

### Step 3: Run full-tile smoke

```bash
nohup ./scripts/run_smoke.sh > results/smoke_d7_$(date +%Y%m%d_%H%M%S).jsonl 2> results/smoke_d7_$(date +%Y%m%d_%H%M%S).log &
wait %1 && echo DONE
```

### Step 4: Paired-ratio analysis

Save as a one-off Python script (or run via `python3 <<PY`):

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

# Auto-pick newest pair vs the standing baselines
roi_d7 = sorted(glob.glob('results/roi_smoke_d7_*.jsonl'))[-1]
ful_d7 = sorted(glob.glob('results/smoke_d7_*.jsonl'))[-1]
roi_base = 'results/roi_smoke_sp32_baseline_20260525_085632.jsonl'
ful_base = 'results/smoke_sp3_2_20260524_143416.jsonl'

def gm(xs):
    xs = [x for x in xs if x and x>0]
    return math.exp(sum(map(math.log,xs))/len(xs)) if xs else None

def analyse(label, base_p, head_p):
    base, m_b = load(base_p)
    head, m_h = load(head_p)
    sl = defaultdict(list)
    for f in set(base) & set(head):
        a, b = base[f], head[f]
        if not all(d in a and d in b for d in ('openjp2k','openjpeg')): continue
        delta = (b['openjpeg']/b['openjp2k']) / (a['openjpeg']/a['openjp2k'])
        bd, _ = m_h[f]
        mode = 'lossless' if 'lossless' in f else ('lossy' if 'lossy' in f else '?')
        for k in ('overall', f'{bd}-bit', f'{bd}-bit {mode}'):
            sl[k].append(delta)
    print(f"\n=== {label}: D7 vs SP3.2 final ===")
    print(f"{'Slice':28} {'n':>3}   D7 delta")
    print('-'*52)
    for k in ['overall','8-bit','8-bit lossless','8-bit lossy',
              '12-bit','12-bit lossless','12-bit lossy',
              '16-bit','16-bit lossless','16-bit lossy']:
        d = sl.get(k, [])
        gd = gm(d)
        pd = f"{(gd-1)*100:+6.2f}% (gm {gd:.4f})" if gd else '—'
        print(f"{k:28} {len(d):>3}   {pd}")

analyse("ROI smoke", roi_base, roi_d7)
analyse("Full-tile smoke", ful_base, ful_d7)
```

### Step 5: Verify the gate

Compare results against the design spec's Decision Gate:

| Gate row | Expected from investigation | Pass / Target |
|---|---:|---|
| ROI smoke overall | ~+1.9% | ≥+1% / ≥+1.5% |
| ROI smoke 16-bit lossless | ~+6.3% | ≥+3% / ≥+5% |
| ROI smoke 12-bit lossless | ~+2.6% | ≥+1% / ≥+2% |
| Full-tile smoke overall | ~0% | within ±0.5% |
| Full-tile smoke 8-bit lossless | ~0% | within ±0.5% |

If ROI smoke overall < +0.5%, the new flag isn't reaching the workers — STOP and check that Steps 4 + 5 of Task 1 landed in the right places. Likely culprit: the per-cblk job struct has a designated initializer or memset-to-zero pattern that overrides the explicit set.

If full-tile smoke regresses > 0.5%, the AND change accidentally disabled fast on full-tile too — STOP, check Step 6.

---

## Task 3: Outcome spec + merge + tag

**Files:**
- Modify: `docs/superpowers/specs/2026-05-25-d7-t1-conditional-dispatch-design.md`

### Step 1: Append an Outcome section to the design spec

Use the SP3.2 outcome section as a template (`2026-05-24-sp3-2-int16-53-dwt-design.md` Outcome §). Sections:
- **Outcome (date)** — paragraph with commit SHAs.
- **Bench — D7 isolated vs SP3.2 final** — the table from Task 2 Step 4.
- **Spec deviations** — any (likely none).
- **Carry-over** — cumulative project state, follow-ups (notably: the 16-bit-lossless full-tile +2.5% legacy preference flagged in the spec's Risk #2).

### Step 2: Commit outcome + close issue #3

```bash
git add docs/superpowers/specs/2026-05-25-d7-t1-conditional-dispatch-design.md
git commit -m "$(cat <<'EOF'
Spec: D7 outcome — ROI <fill in>%, full-tile parity (closes #3)

Records paired-ratio bench results vs SP3.2 final.  Headline: ROI
smoke overall <fill>, ROI 16-bit lossless <fill>; full-tile within
±0.5%.  Closes cornish/openjp2k#3 (partial-decode perf gap).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Step 3: Merge feature branch to main

```bash
git checkout main
git merge --ff-only feat/d7-t1-conditional-dispatch
```

### Step 4: Tag and push

Per the SP3 versioning convention (`v0.8.0` was SP3.2), tag D7 as `v0.9.0`:

```bash
git tag -a v0.9.0-d7-t1-conditional-dispatch -m "D7 — T1 conditional fast/legacy dispatch on whole-tile-decoding (<headline %> ROI, parity full-tile)"
git push origin main
git push origin v0.9.0-d7-t1-conditional-dispatch
```

### Step 5: Memory hygiene

- Delete `~/.claude/projects/-home-cornish-GitHub-openjp2k/memory/issue-3-investigation-in-progress.md` (it was a breadcrumb; investigation is now closed).
- Update `MEMORY.md` to remove the entry pointing to that file.
- Save a new `d7-retrospective.md` with what landed, the workload-dependent dispatch lesson, and the 16-bit-lossless follow-up note.

### Step 6: Clean up the feature branch

```bash
git branch -d feat/d7-t1-conditional-dispatch
git worktree list   # should show only the main worktree
```

---

## Summary checklist

- [ ] Task 1: D7 implementation atomic commit landed; AVX2-ON + AVX2-OFF builds clean; conformance shows 8 baseline failures only (with and without OPJ_T1_FAST=0); byte-cmp passes on 6/9 (file, region) pairs.
- [ ] Task 2: ROI + full-tile smokes run, paired-ratio analysis complete; ROI overall ≥+1%, ROI 16-bit lossless ≥+3%, full-tile within ±0.5%.
- [ ] Task 3: Outcome spec appended + committed; merged to main; tag `v0.9.0-d7-t1-conditional-dispatch` published; memory breadcrumb removed; retrospective saved.
