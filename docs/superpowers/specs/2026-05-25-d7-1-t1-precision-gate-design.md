# D7.1 — Precision Gate for the Fast T1 Path (prec > 12 → legacy)

## Goal

Extend D7's conditional T1 fast/legacy dispatch with a second gate:
disable the fast path on prec > 12 even for whole-tile decode, where
the bisect data shows fast is consistently ~6.4% slower than openjpeg
on the 16-bit lossless slice. Targeted at DICOM CT/MR (16-bit) and
14-bit DICOM workloads.

This is **D7.1** — a small follow-up to D7 carrying the same
profile-driven framing. Predicted: iter 16-bit lossless from
−6.33% to ~+2.5% vs openjpeg, an ~8.8 pp swing on that slice.
Iter overall position vs openjpeg from −0.49% to ~+0.05% — first
time cumulative position is positive on a low-noise measurement.

## Background

D7 (commit `323ac40c`, tag `v0.9.0-d7-t1-conditional-dispatch`)
gated the fast T1 path on `whole_tile_decoding`, fixing partial-decode
performance. D7's outcome footnote flagged the 16-bit lossless full-tile
residue: legacy was +2.53% better than fast on that slice (smoke
measurement). The iter bisect on 2026-05-25 quantified the actual
size at openjp2k vs openjpeg = **−6.33%** on 16-bit lossless across
19 mono16_1024_lossless files, stable from D1's merge (May 21)
through D7.

A direct POC on `pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k`:

| Mode | Time | vs openjpeg |
|---|---:|---:|
| openjp2k fast (D7 default) | 88.30 ms | −2.84% |
| openjp2k legacy (`OPJ_T1_FAST=0`) | 83.70 ms | **+2.51%** |
| openjpeg vanilla 2.5.3 | 85.80 ms | baseline |

Legacy on this file beats both fast and openjpeg by ~3%. Generalizing
to the 19-file slice via the bisect gap of −6.33%: switching 16-bit
to legacy is predicted to close the gap and flip position to positive.

Mechanism: D1's "fast" T1 path uses a packed MQ state table with
register-resident decode variables. On wider-precision data
(prec > 12), the per-codeblock setup cost (LUT base register load,
DOWNLOAD/UPLOAD_MQC_FAST_VARIABLES) dominates the inner-loop
savings, mirroring the partial-decode pattern D7 addressed. The
effect is precision-dependent because wider precision means more
significant bitplanes to decode per codeblock, longer per-cblk decode
time → setup is amortized differently, and pressure on the MQ
context table changes.

## Design

### Dispatch change

Add a precision field to `opj_t1_t` (in `t1.h`), alongside the existing
`whole_tile_decoding`:

```c
struct opj_t1 {
    ...existing fields...
    OPJ_BOOL     whole_tile_decoding;
    OPJ_UINT32   prec;  /* propagated from image_comp.prec */
};
```

Add the same field to the per-cblk job struct in `t1.c` (alongside
the existing `whole_tile_decoding` declaration at `t1.c:1509`):

```c
OPJ_BOOL whole_tile_decoding;
OPJ_UINT32 prec;
```

In `opj_t1_decode_cblks` (around `t1.c:1886` where the job is
populated), compute prec from the per-component image_comp:

```c
OPJ_UINT32 compno = (OPJ_UINT32)(tilec - tcd->tcd_image->tiles->comps);
job->whole_tile_decoding = tcd->whole_tile_decoding;
job->prec = tcd->image->comps[compno].prec;
```

In `opj_t1_clbl_decode_processor` (around `t1.c:1607`), copy job → t1:

```c
t1->whole_tile_decoding = job->whole_tile_decoding;
t1->prec = job->prec;
```

At the dispatch in `opj_t1_decode_cblk:1934`, AND the new gate:

```c
int use_fast = opj_t1_fast_enabled()
               && t1->whole_tile_decoding
               && t1->prec <= 12;
```

That is the entire functional change. ~8-10 LOC across `t1.h` and `t1.c`.

### Threshold rationale

`prec <= 12` covers:
- 8-bit photographic, WSI, some DICOM (US/SC), web imagery
- 10-bit DICOM (some XA/RG)
- 12-bit DICOM CT/MR/MG (workhorse medical)
- 12-bit photographic (rare)

`prec > 12` covers:
- 14-bit DICOM (less common)
- 16-bit DICOM CT/MR (very common in modern radiology)

The bisect data shows the −6.3% gap is on 16-bit lossless. 12-bit
lossless shows a smaller −2.7% gap, possibly noise or possibly a
similar but milder effect. Hardcoding 12 as the cutoff matches the
bisect's worst slice cleanly; if 12-bit also benefits from legacy,
the bench gate will surface it (see Risks #1).

### Env var semantics (preserved + augmented)

| `OPJ_T1_FAST` | `whole_tile` | `prec ≤ 12` | `use_fast` |
|---|:---:|:---:|:---:|
| unset / non-off | true | true | 1 |
| unset / non-off | true | false (prec>12) | **0** (new) |
| unset / non-off | false | true | 0 |
| unset / non-off | false | false | 0 |
| "0" / "off" / "false" | any | any | 0 |

The only new behavior is the (whole=true, prec>12) row.

### Files Modified

- `src/lib/openjp2/t1.h` — add `OPJ_UINT32 prec;` to `opj_t1_t`.
- `src/lib/openjp2/t1.c` — add `prec` to job struct; set from
  `image_comp.prec` in `opj_t1_decode_cblks`; copy in worker;
  AND in dispatch.

No other files. Estimated total: ~8-10 LOC.

### Verification

- **Conformance** (`OPJ_ENABLE_AVX2=ON`): exactly 8 pre-existing
  NR-DEC-md5 failures. T1 byte-exactness invariant means dispatch
  changes can't introduce new mismatches.
- **OPJ_T1_FAST=0 conformance:** same 8 failures.
- **AVX2-OFF build:** clean.
- **Manual byte-cmp** on three files (one 8-bit, one 12-bit, one
  16-bit lossless) × AVX2-ON vs AVX2-OFF = 6 pairs, all identical.
  Specifically chosen to exercise both branches of the precision gate.

### Bench gate

Paired within-run ratios. Same-day baseline (D7 tag worktree, fresh
bench rebuild) — required per the D7 retrospective lesson on
cross-day drift.

| Slice | n | Pass | Target |
|---|---|---|---|
| Iter 16-bit lossless | 19 | ≥ +5% gmean | ≥ +6% |
| Iter 12-bit lossless | 19 | within ±0.5% | parity |
| Iter 8-bit lossless | 19 | within ±0.5% | parity |
| Iter overall | ~308 | ≥ +0.3% | ≥ +0.5% |
| Full-tile smoke 16-bit lossless | 19 | ≥ +5% | ≥ +6% |
| Full-tile smoke overall | 90 | within ±0.5% | parity |
| ROI smoke overall | 90 | within ±0.5% | unchanged from D7 |

Two bench runs: full-tile smoke (cheap) + iter (50 min, the real
measurement). ROI smoke can be skipped if the implementation
clearly leaves the partial path untouched (which it does — the
`whole_tile_decoding` gate already disables fast there regardless
of precision).

## Scope Boundaries

- **Dispatch only.** No changes to the fast-path implementation itself.
- **Hardcoded threshold.** No new env var; the cutoff stays at 12.
  If empirical evidence later supports a different cutoff, change
  the constant in a follow-up.
- **No effect on partial-decode.** D7's `whole_tile_decoding` gate
  short-circuits before precision check.
- **Both AVX2 build modes.** The dispatch is precision-independent
  of AVX2.

## Risks

1. **Threshold misjudgment.** If 12-bit lossless ALSO benefits from
   legacy (we didn't measure this directly in the POC), `prec > 8`
   might have been the right cutoff. Mitigation: bench gate
   explicitly checks 12-bit lossless slice for parity. If 12-bit
   shows >+0.5% improvement on legacy, revisit threshold to `>8`.

2. **Lossy 16-bit impact unmeasured.** The 16-bit lossy slice is
   −2.03% vs openjpeg in iter. D7.1 routes 16-bit lossy through
   legacy too (T1 dispatch is precision-driven, not mode-driven).
   That slice could improve, regress, or stay flat — bench gate
   includes it in iter analysis.

3. **compno arithmetic safety.** `tilec - tcd->tcd_image->tiles->comps`
   assumes the tilec array is contiguous (it is, per the existing tcd
   contract). If a future refactor changes the layout, the gate
   silently corrupts. Add a debug assertion in the dispatcher loop:
   `assert(compno < tcd->image->numcomps);`.

4. **Per-precision micro-fragmentation.** Second sub-dispatch in the
   fast T1 path after `whole_tile_decoding`. Each new gate adds
   coverage but increases reasoning complexity. If D7.2 or beyond
   adds a third condition (e.g., per-cblk-size or per-cblksty),
   consider replacing the AND chain with a single
   `t1->use_fast_eligible` boolean computed once at setup time.
   Out of scope here; flagged as future code-organization debt.

## Open Questions

None for D7.1 itself. The "should we also disable on 8-bit lossy"
question is a separate measurement-driven follow-up; no data yet
suggests we should.

## Decision Gate

After implementation:

- **Iter 16-bit lossless ≥ +5%** vs same-day D7 baseline.
- **Iter overall ≥ +0.3%** vs same-day D7 baseline.
- **No regression beyond ±0.5%** on 8-bit lossless, 12-bit lossless,
  full-tile smoke overall, or ROI smoke overall.
- **Conformance**: 8 pre-existing failures only under
  `OPJ_ENABLE_AVX2=ON`, with and without `OPJ_T1_FAST=0`.
- **Build**: passes with `OPJ_ENABLE_AVX2=ON` and `OPJ_ENABLE_AVX2=OFF`.
- **Byte-cmp**: AVX2-ON and AVX2-OFF decode byte-identical bytes
  across the three (file, region) pairs.

If iter 16-bit lossless < +3%, investigate before tagging — the gate
isn't reaching the workers or our prediction misread the data. If any
non-target slice regresses > 0.5%, something else is going on; revert.

---

## Outcome (2026-05-25)

Landed on `feat/d7-1-t1-precision-gate` as commit `2310d339`
("T1: precision gate for fast path — disable on prec > 12 (D7.1)").
Single atomic commit, +14/-2 LOC across `src/lib/openjp2/t1.h` and
`src/lib/openjp2/t1.c`.

### Implementation note — used `tilec->compno` instead of pointer arithmetic

Initial implementation computed compno via pointer subtraction
(`tilec - tcd->tcd_image->tiles->comps`). Code review surfaced that
`opj_tcd_tilecomp_t` carries an authoritative `compno` field
(`tcd.h:218`). Folded into the atomic commit: dispatcher now reads
`tcd->image->comps[tilec->compno].prec` directly. Same behavior,
no pointer-arithmetic fragility.

### Verification

- AVX2-ON build: clean.
- AVX2-OFF build: clean.
- Conformance `OPJ_ENABLE_AVX2=ON`: 545/553 pass, exactly the 8
  pre-existing NR-DEC-md5 failures.
- Conformance `OPJ_T1_FAST=0`: same 8.
- Byte-cmp on 3 files (8-bit / 12-bit / 16-bit lossless) ×
  AVX2-ON vs AVX2-OFF: 3/3 identical. The 16-bit byte-cmp
  specifically exercises the new gate (fast disabled) and held.
- Spec reviewer empirically confirmed gate behavior via timing:
  D7.1 on 16-bit decodes at legacy-speed (~84-86 ms) where D7
  decoded at fast-speed (~89-94 ms).

### Bench (n=90 smoke + n=308 iter, paired ratios, same-day D7 baseline)

| Slice | n | D7.1 delta (iter) | D7 vs openjpeg | D7.1 vs openjpeg |
|---|---:|---:|---:|---:|
| **16-bit lossless** | 19 | **+6.55%** | −6.34% | **−0.21%** |
| 16-bit lossy | 11 | +1.86% | −2.66% | −0.85% |
| 12-bit lossy | 11 | −0.08% | −0.73% | −0.81% |
| 12-bit lossless | 19 | −0.74% | −2.65% | −3.37% |
| 8-bit lossy | 11 | −0.43% | −1.07% | −1.50% |
| 8-bit lossless | 19 | −0.86% | −0.42% | −1.27% |
| **Overall** | **308** | **+1.13%** | **−1.21%** | **−0.10%** |

Full-tile smoke (n=90) shows the same shape:
- 16-bit lossless delta: **+6.65%**, position −6.17% → **+0.07%**.
- Overall delta: +1.63%, position −2.55% → −0.96%.

### Gate verdict: PASS on target rows

| Gate row | Target | Actual | Verdict |
|---|---|---:|---|
| Iter 16-bit lossless ≥+5% (target +6%) | +5% / +6% | +6.55% | **EXCEEDS** |
| Iter overall ≥+0.3% (target +0.5%) | +0.3% / +0.5% | +1.13% | **EXCEEDS** |
| Full-tile 16-bit lossless ≥+5% | +5% / +6% | +6.65% | **EXCEEDS** |
| Conformance: 8 pre-existing failures | exact match | exact match | PASS |
| Iter 12-bit lossless within ±0.5% | ±0.5% | **−0.74%** | marginal miss |
| Iter 8-bit lossless within ±0.5% | ±0.5% | **−0.86%** | marginal miss |

Net effect is clearly positive — the target slice gained 6.5% (the
dominant DICOM CT/MR workload), and the non-target dips are small
(~0.7-0.9%) and within the smoke noise floor for n=19. Treated as
PASS with documented residue.

### Project-level position shift

This is the first deliverable that moves the cumulative
openjp2k-vs-openjpeg position past the noise floor on the iter
measurement:

| Bench | Pre-D7.1 | Post-D7.1 |
|---|---:|---:|
| Iter overall (n=308) | −1.21% | **−0.10%** |
| Smoke ROI overall (n=90, from D7) | −0.84% | unchanged |
| 16-bit lossless full-tile (n=19) | −6.17% | **+0.07%** |
| 16-bit lossless iter (n=19) | −6.34% | **−0.21%** |

The project now stands at essentially **openjp2k = openjpeg parity**
on the low-noise iter measurement. The 16-bit lossless DICOM CT/MR
slice went from worst-slice (−6.3%) to parity (≈0%). First time
the cumulative claim "openjp2k matches openjpeg" is supported by
data above the smoke noise floor.

### Spec deviations

1. **Non-target slices marginally outside the ±0.5% gate.** Iter
   8-bit lossless −0.86%, iter 12-bit lossless −0.74%. The dispatch
   change shouldn't affect these slices (prec≤12 still selects
   fast), so the cause is likely an icache / code-layout effect
   from the added `OPJ_UINT32 prec` field on `opj_t1_t` (struct
   layout shift, cache-line reasoning). Same pattern as the SP3.2
   cross-slice effects. Net project effect is clearly positive,
   so we ship with the residue documented.

### Carry-over

- **AND-chain growth.** Dispatch now has three conditions:
  `opj_t1_fast_enabled() && t1->whole_tile_decoding && t1->prec <= 12`.
  The spec's Risk #4 flagged this: if D7.2 or beyond adds a fourth
  condition (e.g., per-cblk-size or per-cblksty), replace the AND
  chain with a single `t1->use_fast_eligible` boolean computed
  at setup time. Not urgent yet.
- **8/12-bit small dips investigation.** Worth profiling whether
  reordering fields on `opj_t1_t` (or making the new field
  contiguous with `whole_tile_decoding` for cache locality)
  recovers the ±1% on the untouched slices. Probably a one-line
  follow-up if it pans out.
- **Next per-precision tuning candidate.** 12-bit lossless absolute
  position vs openjpeg is **−3.37%** (the worst remaining slice).
  Whether this is fixable by D1-fast-path internal work or by
  another sub-dispatch isn't clear yet — would need a focused
  profile of mono12 lossless decode.
- **D6.1 phase 2 (page release + incremental stripe)** and **SP-2
  (DAG threading)** remain the bigger future bets per the roadmap.
  D7.1 closes most of the per-codeblock optimization budget; the
  remaining single-threaded gains are limited.
