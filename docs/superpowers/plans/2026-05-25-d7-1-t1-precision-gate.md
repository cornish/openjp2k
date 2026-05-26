# D7.1 — Fast T1 Precision Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend D7's conditional T1 fast/legacy dispatch with a second AND gate on `prec ≤ 12`. Disables D1's "fast" T1 path on 14/16-bit content (DICOM CT/MR), where the bisect data shows the fast path consistently costs ~6% vs openjpeg. Predicted: iter 16-bit lossless from −6.33% to ~+2.5% vs openjpeg.

**Architecture:** Mirror D7's plumbing for a new field. Add `OPJ_UINT32 prec;` to `opj_t1_t` (alongside `whole_tile_decoding`). Add the same field to the per-cblk job struct. Compute from `image_comp.prec` in `opj_t1_decode_cblks`. Copy job → t1 in the worker. AND into the dispatch at `t1.c:1934`.

**Tech Stack:** C99, OPJ_UINT32 typedef.

---

## File Structure

**Modified:**
- `src/lib/openjp2/t1.h` — add `OPJ_UINT32 prec;` to `opj_t1_t` (next to `whole_tile_decoding` at line 210).
- `src/lib/openjp2/t1.c` — four edits:
  1. Add `OPJ_UINT32 prec;` to the per-cblk job struct (line 1509-1520, alongside the existing `whole_tile_decoding`).
  2. In `opj_t1_decode_cblks` (around line 1886), compute compno + prec from the per-component image_comp and set on the job.
  3. In `opj_t1_clbl_decode_processor` (around line 1607), copy `t1->prec = job->prec;` alongside the existing `whole_tile_decoding` copy.
  4. In `opj_t1_decode_cblk` at line 1934, change the dispatch to AND in `t1->prec <= 12`.

**Not modified:** any other file.

---

## Task 1: Implement the precision gate (single atomic commit)

All edits land together — the fields and dispatch must match.

**Files:**
- Modify: `src/lib/openjp2/t1.h`
- Modify: `src/lib/openjp2/t1.c`

### Step 1: Verify the propagation pattern

Recall from D7 the propagation chain for `whole_tile_decoding`:

```bash
cd /home/cornish/GitHub/openjp2k
grep -n 'whole_tile_decoding' src/lib/openjp2/t1.h src/lib/openjp2/t1.c
```

You should see hits at:
- `t1.h:210` (field on opj_t1_t)
- `t1.c:1509` (job struct field)
- `t1.c:1546` (unrelated — partial-decode memory-alloc check, pre-existing)
- `t1.c:1607` (worker copy: t1->X = job->X)
- `t1.c:1861` (unrelated — partial-decode check)
- `t1.c:1886` (dispatcher set: job->X = tcd->X)
- `t1.c:1934` (dispatch AND)

The new `prec` field follows the exact same shape, except line 1886 reads from `tcd->image->comps[compno].prec` (not directly from a tcd field — there's no `tcd->prec`).

### Step 2: Add `prec` field to `opj_t1_t` in `t1.h`

Open `src/lib/openjp2/t1.h` near line 210 (the existing `whole_tile_decoding` declaration with its comment block). Add immediately after the `whole_tile_decoding;` line:

```c
    /* Source-component precision, propagated from image_comp.prec.
     * Used alongside whole_tile_decoding to gate the fast T1 path:
     * for prec > 12 the fast path's per-codeblock setup cost
     * outweighs the inner-loop savings (16-bit DICOM CT/MR
     * workload). */
    OPJ_UINT32   prec;
```

If the existing "/* The N variables below are only used by the decoder */" comment counts variables, bump N by 1 (e.g., 4 → 5). Check by inspection.

### Step 3: Add `prec` to the per-cblk job struct in `t1.c`

Locate the job struct at `t1.c:1508-1520` (the one containing `whole_tile_decoding`, `mustuse_cblkdatabuffer`, etc.). Add immediately after `whole_tile_decoding`:

```c
    OPJ_UINT32 prec;
```

No comment needed — the field's purpose is documented on `opj_t1_t` (Step 2).

### Step 4: Set the job's `prec` in `opj_t1_decode_cblks`

Locate `t1.c:1885-1886`:

```c
                    job->whole_tile_decoding = tcd->whole_tile_decoding;
```

The compno is needed to index into the image's component array. Add immediately before line 1885:

```c
                    {
                        OPJ_UINT32 compno = (OPJ_UINT32)(tilec -
                            tcd->tcd_image->tiles->comps);
                        assert(compno < tcd->image->numcomps);
                        job->prec = tcd->image->comps[compno].prec;
                    }
```

Then keep the existing `job->whole_tile_decoding = ...` line as-is.

**Important:** compno calculation must be inside the per-cblk loop (where `tilec` is in scope) and must match how `tilec` was obtained. Check the surrounding code: `tilec` is the function's first-parameter input (per `opj_t1_decode_cblks` signature in t1.h:239), so it's the same `tilec` across all cblks in this call. Could be computed once outside the loop for efficiency, but per-cblk is also fine (the compno computation is cheap) and matches the existing per-job-init pattern.

Optimization (optional): if you want to compute it once per call rather than per-cblk, declare `OPJ_UINT32 _compno_prec;` at function scope, compute once after `tilec` is established, and reference it in the job init. Not required for correctness.

### Step 5: Copy job → t1 in `opj_t1_clbl_decode_processor`

Locate `t1.c:1607`:

```c
    t1->whole_tile_decoding = job->whole_tile_decoding;
```

Add immediately after it:

```c
    t1->prec = job->prec;
```

### Step 6: Change the dispatch at `t1.c:1934`

Find:

```c
    int use_fast = opj_t1_fast_enabled() && t1->whole_tile_decoding;
```

Change to:

```c
    int use_fast = opj_t1_fast_enabled()
                   && t1->whole_tile_decoding
                   && t1->prec <= 12;
```

### Step 7: Build AVX2-ON

```bash
cd /home/cornish/GitHub/openjp2k
cmake -S . -B build-d7-1 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=ON -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-d7-1 -j 2>&1 | tail -5
```

Expected: clean build, no warnings. The new `OPJ_UINT32 prec;` field in `opj_t1_t` and the job struct shouldn't trigger any field-initialization warnings (job is `opj_calloc`'d, opj_t1_t is allocated via `opj_t1_create` and used after Step 5's copy). If `-Werror=missing-field-initializers` fires, check that no designated-initializer for the job struct (or opj_t1_t) exists in either file.

### Step 8: Build AVX2-OFF

```bash
cmake -S . -B build-d7-1-noavx2 -DCMAKE_BUILD_TYPE=Release -DOPJ_ENABLE_AVX2=OFF -DBUILD_TESTING=ON 2>&1 | tail -3
cmake --build build-d7-1-noavx2 -j 2>&1 | tail -5
```

Expected: clean. The fast-path code is compiled either way; only the AVX2 5/3 / 9/7 DWT kernels are gated.

### Step 9: Conformance with `OPJ_ENABLE_AVX2=ON`

```bash
scripts/run-conformance.sh -B build-d7-1 -- -R NR-DEC -j4 2>&1 | tail -15
```

Expected: 545/553 pass, exactly the same 8 pre-existing NR-DEC-md5 failures. The byte-exactness invariant guarantees identical output regardless of dispatch.

Any 9th failure → STOP. Likely cause: prec was read from the wrong place (e.g., the compno offset arithmetic is wrong, leading to reading another component's prec or out-of-bounds).

### Step 10: Conformance with `OPJ_T1_FAST=0`

```bash
OPJ_T1_FAST=0 scripts/run-conformance.sh -B build-d7-1 -- -R NR-DEC -j4 2>&1 | tail -10
```

Expected: same 8 failures.

### Step 11: Manual byte-cmp on three precision tiers

```bash
F8=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_mon_enone.jp2
F12=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono12_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k
F16=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono16_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k

for F in "$F8" "$F12" "$F16"; do
  build-d7-1/bin/opj_decompress -i "$F" -o /tmp/on.raw > /dev/null 2>&1
  build-d7-1-noavx2/bin/opj_decompress -i "$F" -o /tmp/off.raw > /dev/null 2>&1
  if cmp -s /tmp/on.raw /tmp/off.raw; then
    echo "OK   $(basename "$F")"
  else
    echo "FAIL $(basename "$F")"
  fi
done
```

Expected: 3 OK. These specifically test 8-bit (fast path active), 12-bit (fast path active at threshold), and 16-bit (fast path now disabled by D7.1). Byte-cmp must hold for all three. Any FAIL means dispatch is wrong; STOP.

### Step 12: Stage and commit

```bash
git add src/lib/openjp2/t1.h src/lib/openjp2/t1.c
git status --short
git diff --cached --stat
```

Expected: 2 files, ~8-12 LOC added total.

```bash
git commit -m "$(cat <<'EOF'
T1: precision gate for fast path — disable on prec > 12 (D7.1)

D7 gated the fast T1 path on whole_tile_decoding.  The
2026-05-25 iter bisect surfaced a second workload where fast is
the regression source: 16-bit lossless full-tile decode shows
openjp2k -6.33% slower than openjpeg, stable since D1's merge
(May 21).  POC confirmed disabling fast on 16-bit closes the
gap and flips position to ~+2.5% ahead of openjpeg.

D7.1 plumbs the source-component precision into the T1
dispatch and ANDs `prec <= 12` into the existing fast-enabled
+ whole_tile_decoding check.  Mirror of D7's propagation chain:
new field on opj_t1_t and the per-cblk job struct, set from
image_comp.prec via compno offset arithmetic in
opj_t1_decode_cblks, copied to t1 in the worker.

OPJ_T1_FAST=0 still forces legacy everywhere.  Default
behavior now: fast on (whole-tile AND prec<=12), legacy
otherwise.

Conformance unchanged (T1 fast and legacy are byte-exact in
either branch).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Bench gate

Two bench runs (full-tile smoke + iter) against the D7.1 head, plus
same-day baseline reruns of D7 for paired-ratio comparison.

**Files:** None modified.

### Step 1: Set up the D7-baseline worktree

```bash
cd /home/cornish/GitHub/openjp2k
git worktree add /home/cornish/GitHub/openjp2k-d7-baseline v0.9.0-d7-t1-conditional-dispatch
```

### Step 2: Rebuild bench against D7 baseline, run full-tile smoke + iter

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source /home/cornish/GitHub/openjp2k-d7-baseline 2>&1 | tail -3
nohup ./scripts/run_smoke.sh > results/smoke_d7_sameday_$(date +%Y%m%d_%H%M%S).jsonl 2> results/smoke_d7_sameday_$(date +%Y%m%d_%H%M%S).log
nohup ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt corpus/public/ > results/iter_d7_sameday_$(date +%Y%m%d_%H%M%S).jsonl 2> results/iter_d7_sameday_$(date +%Y%m%d_%H%M%S).log
echo D7_BASELINE_DONE
```

Wall time: ~9 min smoke + ~50 min iter = ~60 min. Use background dispatch if running interactively.

### Step 3: Rebuild bench against D7.1, run same two benches

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source /home/cornish/GitHub/openjp2k 2>&1 | tail -3
nohup ./scripts/run_smoke.sh > results/smoke_d7_1_$(date +%Y%m%d_%H%M%S).jsonl 2> results/smoke_d7_1_$(date +%Y%m%d_%H%M%S).log
nohup ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt corpus/public/ > results/iter_d7_1_$(date +%Y%m%d_%H%M%S).jsonl 2> results/iter_d7_1_$(date +%Y%m%d_%H%M%S).log
echo D7_1_DONE
```

Same wall time. Two builds back-to-back guarantee same-day measurement.

### Step 4: Paired-ratio analysis

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

def gm(xs):
    xs = [x for x in xs if x and x>0]
    return math.exp(sum(map(math.log,xs))/len(xs)) if xs else None

def analyse(label, base_p, head_p):
    base, m_b = load(base_p)
    head, m_h = load(head_p)
    sl = defaultdict(list); abs_b = defaultdict(list); abs_h = defaultdict(list)
    for f in set(base) & set(head):
        a, b = base[f], head[f]
        if not all(d in a and d in b for d in ('openjp2k','openjpeg')): continue
        r_b = a['openjpeg']/a['openjp2k']
        r_h = b['openjpeg']/b['openjp2k']
        delta = r_h / r_b
        bd, _ = m_h[f]
        mode = 'lossless' if 'lossless' in f else ('lossy' if 'lossy' in f else '?')
        for k in ('overall', f'{bd}-bit', f'{bd}-bit {mode}'):
            sl[k].append(delta); abs_b[k].append(r_b); abs_h[k].append(r_h)
    print(f"\n=== {label} ===")
    print(f"{'Slice':28} {'n':>3}   D7.1 delta              baseline oj2k v oje  D7.1 oj2k v oje")
    print('-'*98)
    for k in ['overall',
              '8-bit lossless','12-bit lossless','16-bit lossless',
              '8-bit lossy','12-bit lossy','16-bit lossy']:
        d=sl.get(k,[]); b=abs_b.get(k,[]); h=abs_h.get(k,[])
        gd,gb,gh = gm(d), gm(b), gm(h)
        pd = f"{(gd-1)*100:+6.2f}% (gm {gd:.4f})" if gd else '—'
        pb = f"{(gb-1)*100:+6.2f}%" if gb else '—'
        ph = f"{(gh-1)*100:+6.2f}%" if gh else '—'
        print(f"{k:28} {len(d):>3}   {pd:25} {pb:>9}            {ph:>9}")

# Auto-pick newest pair
smoke_base = sorted(glob.glob('results/smoke_d7_sameday_*.jsonl'))[-1]
smoke_head = sorted(glob.glob('results/smoke_d7_1_*.jsonl'))[-1]
iter_base = sorted(glob.glob('results/iter_d7_sameday_*.jsonl'))[-1]
iter_head = sorted(glob.glob('results/iter_d7_1_*.jsonl'))[-1]
analyse("Full-tile smoke (D7.1 vs same-day D7)", smoke_base, smoke_head)
analyse("Iter (D7.1 vs same-day D7)", iter_base, iter_head)
```

### Step 5: Verify the gate

| Gate row | Expected | Pass / Target |
|---|---:|---|
| Iter 16-bit lossless | ~+5 to +8% | ≥+5% / ≥+6% |
| Iter overall | ~+0.3 to +0.6% | ≥+0.3% / ≥+0.5% |
| Iter 12-bit lossless | ~0% | within ±0.5% |
| Iter 8-bit lossless | ~0% | within ±0.5% |
| Full-tile smoke 16-bit lossless | ~+5 to +6% | ≥+5% / ≥+6% |
| Full-tile smoke overall | ~+0.5 to +1% | within ±0.5% (slight positive OK) |

If iter 16-bit lossless < +3%, STOP and investigate. Likely: the prec field isn't reaching the dispatch (Step 5 was missed) or the compno computation is wrong (Step 4).

If iter 12-bit lossless improves > +1%, the threshold was set too high — bench data is telling us prec>8 was the right cutoff. Document in outcome and consider a D7.2 follow-up.

---

## Task 3: Outcome + merge + tag

**Files:**
- Modify: `docs/superpowers/specs/2026-05-25-d7-1-t1-precision-gate-design.md`

### Step 1: Append outcome to the design spec

Mirror the D7 outcome section (`2026-05-25-d7-t1-conditional-dispatch-design.md`
Outcome §). Sections:
- **Outcome (date)** — paragraph with commit SHAs.
- **Implementation note** — any surprises (e.g., compno arithmetic
  worked first try, no plumbing surprises).
- **Conformance** — exactly 8 failures, byte-cmp 3/3 pairs.
- **Bench** — table from Task 2 Step 5, full slice breakdown.
- **Cumulative position** — note the new openjp2k-vs-openjpeg
  absolute positions (iter overall, 16-bit lossless slice).
- **Carry-over** — any new follow-ups (per-cblk-size sub-dispatch
  if patterns suggest, code-organization debt from the AND chain).

### Step 2: Commit the outcome

```bash
git add docs/superpowers/specs/2026-05-25-d7-1-t1-precision-gate-design.md
git commit -m "$(cat <<'EOF'
Spec: D7.1 outcome — <fill in 16-bit headline %>

Records paired same-day bench results vs D7 baseline.  16-bit
lossless: <fill>.  Overall iter: <fill>.  Conformance: 8 pre-
existing failures only.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Step 3: Merge feature branch to main + tag

```bash
git checkout main
git merge --ff-only feat/d7-1-t1-precision-gate
git tag -a v0.10.0-d7-1-t1-precision-gate -m "D7.1 — Fast T1 precision gate; +<fill>% iter 16-bit lossless"
git push origin main
git push origin v0.10.0-d7-1-t1-precision-gate
```

### Step 4: Memory hygiene

Save `d7-1-retrospective.md` next to `d7-retrospective.md` with what
landed and lessons (the bisect methodology, the per-precision sub-
dispatch pattern, any future-debt notes about the AND chain). Add a
one-line pointer to MEMORY.md.

### Step 5: Clean up

```bash
git branch -d feat/d7-1-t1-precision-gate
git worktree remove /home/cornish/GitHub/openjp2k-d7-baseline
git worktree list   # should show only the main worktree
```

---

## Summary checklist

- [ ] Task 1: D7.1 implementation atomic commit; AVX2-ON + AVX2-OFF builds clean; conformance shows 8 baseline failures (with and without OPJ_T1_FAST=0); byte-cmp passes on 3/3 (8/12/16-bit) files.
- [ ] Task 2: D7-baseline and D7.1 smoke + iter runs complete, paired-ratio analysis run; iter 16-bit lossless ≥+5%, iter overall ≥+0.3%, no non-target regression >0.5%.
- [ ] Task 3: Outcome appended + committed; merged to main; tag `v0.10.0-d7-1-t1-precision-gate` published; retrospective memory saved; branch + worktree cleaned up.
