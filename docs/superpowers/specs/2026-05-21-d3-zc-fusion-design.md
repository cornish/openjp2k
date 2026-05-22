# D3 — ZC Context Fusion (Cache Packed State at Context Slot)

## Goal

Eliminate one indirect memory access per MQ decode in the fast T1 path by storing the packed MQ state directly at each context slot instead of an index that must be resolved via a second `opj_mqc_states_packed[]` lookup.

This is the next perf deliverable picked from the
[D2 post-D1 profile](../profile/2026-05-21-post-d1-decode-profile.md). The profile
identified `opj_t1_dec_sigpass_mqc_fast_*` and `opj_t1_dec_clnpass_fast_*` as
57–97% of decode cycles across the corpus; the per-decode load pattern
`getctxno_*(flags) → ctxs_idx[ctxno] → pktbl[state_idx]` has three serially
dependent loads. Collapsing the second and third loads removes one load from
every MQ decode in the fast path — not only ZC but also SC, MR, UNI, and AGG
contexts (the change is a data-layout swap that benefits all contexts uniformly;
ZC just fires most often, hence the deliverable's name).

## Background

Post-D1 fast path (current main):

```c
/* opj_mqc_t (mqc.h) */
typedef struct opj_mqc {
    ...
    OPJ_UINT32 ctxs_idx[MQC_NUMCTXS];   /* per-context state index 0..93 */
    OPJ_UINT32 *curctx_idx;             /* pointer to active slot */
    ...
} opj_mqc_t;

/* opj_mqc_fast_decode_macro body (mqc_fast.h) */
OPJ_UINT32 _pkd = pktbl[*curidx];        /* load 1: pktbl[ ctxs_idx[ctxno] ] */
/* ... derive qeval, mps, ... */
*(curidx) = OPJ_MQC_PACK_NMPS_IDX(_pkd); /* writeback: store index */
```

The actual access chain in assembly (per the D2 profile, §4.2):
1. `mov  lut_zc[flags], ctxt`         (byte load from per-orientation LUT)
2. `mov  ctxs_idx[ctxt], state_idx`   (u32 load from opj_mqc_t)
3. `mov  pktbl[state_idx], packed`    (u32 load from rodata)

Three loads, each depending on the previous. The MPS-only fast path is hottest
(no transition), and on that path we do load #1 + #2 + #3, derive `_qe` and
`_mps`, then write nothing.

## Design

### Data layout change

Replace the per-context state-index array with a per-context packed-state array:

```c
/* opj_mqc_t (mqc.h) — after */
typedef struct opj_mqc {
    ...
    OPJ_UINT32 ctxs_packed[MQC_NUMCTXS]; /* per-context packed MQ state */
    OPJ_UINT32 *curctx_packed;           /* pointer to active slot */
    ...
} opj_mqc_t;
```

Semantically: `mqc->ctxs_packed[ctxno]` holds the same `uint32_t` value that
`opj_mqc_states_packed[mqc->ctxs_idx[ctxno]]` holds today. The accessor macros
`OPJ_MQC_PACK_QEVAL/MPS/SWITCH/NMPS_IDX/NLPS_IDX` operate on it unchanged.

Memory footprint: unchanged (19 × 4 bytes = 76 bytes, same as `ctxs_idx[]`).

### Decode macro change

```c
/* mqc_fast.h opj_mqc_fast_decode_macro — after */
OPJ_UINT32 _pkd = *(curpacked);           /* load 1 (was 2) */
OPJ_UINT32 _qe  = OPJ_MQC_PACK_QEVAL(_pkd);
OPJ_UINT32 _mps = OPJ_MQC_PACK_MPS(_pkd);
(a) -= _qe;
if (((c) >> 16) < _qe) {
    /* LPS path — transition */
    OPJ_UINT32 _next_idx = OPJ_MQC_PACK_NLPS_IDX(_pkd);
    *(curpacked) = pktbl[_next_idx];      /* writeback: load next packed + store */
    opj_mqc_fast_renormd_macro(...);
    ...
} else {
    (c) -= _qe << 16;
    if (((a) & 0x8000) == 0) {
        /* MPS exchange + renorm — transition */
        OPJ_UINT32 _next_idx = OPJ_MQC_PACK_NMPS_IDX(_pkd);
        *(curpacked) = pktbl[_next_idx];  /* writeback */
        opj_mqc_fast_renormd_macro(...);
        ...
    } else {
        (d) = _mps;                       /* pure MPS — no writeback */
    }
}
```

Cost accounting per decode:

| path | current | after | delta |
|---|---|---|---|
| pure MPS (common) | 3 loads | **2 loads** | −1 load |
| LPS | 3 loads + 1 store | 3 loads + 1 store | 0 |
| MPS + renorm | 3 loads + 1 store | 3 loads + 1 store | 0 |

The pure-MPS path is dominant on natural-image content (high-probability
contexts where the model rarely mispredicts). The 1-load saving on that path
is the proposed win.

### Setup change

`opj_mqc_resetstates` and `opj_mqc_setstate` (mqc.c) currently maintain
`ctxs_idx[]` in parallel with `ctxs[]`. After this change they instead maintain
`ctxs_packed[]`, initialized via:

```c
/* mqc.c — after */
void opj_mqc_resetstates(opj_mqc_t *mqc)
{
    OPJ_UINT32 i;
    for (i = 0; i < MQC_NUMCTXS; i++) {
        mqc->ctxs[i] = mqc_states;
        mqc->ctxs_packed[i] = opj_mqc_states_packed[0];  /* state-0 packed value */
    }
    mqc->curctx_packed = &mqc->ctxs_packed[0];
}

void opj_mqc_setstate(opj_mqc_t *mqc, OPJ_UINT32 ctxno, OPJ_UINT32 msb,
                      OPJ_INT32 prob)
{
    OPJ_UINT32 idx = msb + (OPJ_UINT32)(prob << 1);
    mqc->ctxs[ctxno] = &mqc_states[idx];
    mqc->ctxs_packed[ctxno] = opj_mqc_states_packed[idx];
}
```

`opj_mqc_setcurctx` macro updates to set `curctx_packed = &ctxs_packed[ctxno]`.

### t1_fast.c

`opj_t1_setcurctx_fast` macro and `DOWNLOAD_MQC_FAST_VARIABLES /
UPLOAD_MQC_FAST_VARIABLES` are renamed from `curidx` to `curpacked` to reflect
the semantic change. No functional change in the cloned sigpass / refpass /
clnpass clones beyond mechanical rename.

### Files touched

| file | change |
|---|---|
| `src/lib/openjp2/mqc.h` | rename field; update `opj_mqc_setcurctx` macro |
| `src/lib/openjp2/mqc.c` | rewrite `opj_mqc_resetstates` / `opj_mqc_setstate` per above |
| `src/lib/openjp2/mqc_fast.h` | rewrite `opj_mqc_fast_decode_macro` body; rename `DOWNLOAD/UPLOAD` locals |
| `src/lib/openjp2/t1_fast.c` | rename `curidx` → `curpacked` in the clones |
| `tests/unit/test_mqc_packed_equivalence.c` | no change (still verifies packed table structure) |
| `tests/unit/test_mqc_dump.c` | no change |
| `scripts/run_diff_test.sh` | no change |

## Verification

Per-task: smoke diff-test (90 files, ~9 min) + ctest. Same harness as D1.

Per-deliverable gate:
- Full conformance (8 pre-existing failures unchanged).
- 30-file worst-loser diff-test (same list as D1).
- Bench smoke legacy-vs-fast: target gmean ≥ 1.02 (2% wall-clock improvement);
  pass threshold gmean > 1.00 (any positive movement).
- Iter bench openjp2k-vs-openjpeg: comparison against vanilla, recorded in
  spec but not gating.

## Scope Boundaries

- ZC context fusion only. Sign-bit packing (combining sc_ctxt + spb into a
  single LUT entry) is a separate follow-up.
- Fast path only. Legacy path is unchanged.
- No new files.
- No new build options.

## Risks

1. **The compiler may already hoist load #2 across consecutive decodes on the
   same context** (e.g., when ctxs_idx[ctxt] is loaded once and reused while
   the state stays put). The D1.3 GOT-fix had this dynamic — the predicted
   ~5% win was already partially captured by the compiler. The estimated
   2-4% wall-clock win is a ceiling, not a floor.
2. **Transition path is slightly slower** (one extra pktbl load to resolve
   next packed state). If transitions dominate (low-probability content,
   noisy images), net could be neutral or negative. Bench gates against this.
3. **Same-binary A/B not available.** The runtime fast/legacy switch via
   `OPJ_T1_FAST` still applies, but there is no "fast-no-fusion vs fast-with-
   fusion" comparison in the same binary because the swap is in-place. If
   bench shows a regression, revert the commit and investigate.

## Decision Gate

Bench gate after implementation. If smoke gmean ≥ 1.00 → tag and merge.
If gmean < 1.00 → revert, report, escalate to user for next direction.

---

## Outcome (2026-05-22): reverted

Implementation landed on `feat/d3-zc-fusion` (commit `67b9b5ac`) per the
plan at [`docs/superpowers/plans/2026-05-21-d3-zc-fusion.md`](../plans/2026-05-21-d3-zc-fusion.md).
Correctness held — 90/90 smoke + 30/30 worst-loser byte-identical, 8
pre-existing conformance failures unchanged. **Perf gate failed:**
smoke gmean openjp2k_fast/openjp2k_legacy = **0.9933** (Pre-D3 D1 baseline
was 1.0045 — D3 made things 1.1% worse). WSI 1024×1024 subset showed a
small win (+0.46%), so the effect was workload-dependent, not a clean
regression.

Disassembly diff (`opj_t1_dec_sigpass_mqc_fast_64x64_novsc`):

| | D1 | D3 |
|---|---|---|
| instructions | 1238 | 1283 (+3.6%) |
| `opj_mqc_states_packed` refs | 8 | 32 |
| `lea` ops | 32 | 56 |

The 24 extra references come from 4 inlined step-macro instances × 4
transition writeback sites, each emitting its own `lea base-of-pktbl`
because the compiler couldn't keep one register pinned across the
inlined writebacks. Each writeback site issues `pktbl[next_idx]` to
resolve the next packed state — work the read path saved is added back
on every transition. Pure-MPS-heavy workloads (WSI) win slightly;
transition-heavy workloads (synthetic mono16 lossless) lose more.

**Why "Option C" (pre-resolve NMPS/NLPS inline) doesn't help:** the MQ
state machine is recursive — to make transition O(1) without a LUT,
each slot would need to contain the full state graph reachable from
it. The only finite encoding of that is a pointer to the next state,
which **is exactly what the legacy decoder already does** (`*curctx =
(*curctx)->nmps` writes a pointer; `(*curctx)->qeval` reads through it).
D3's packed-index approach is structurally inferior to the legacy
pointer dispatch on the transition path, and the inlining-cost trade
overwhelmed the read-path savings.

Reverted by abandoning `feat/d3-zc-fusion`. Branch preserved for
historical reference; not merged. Pivoting to **D6.1 (TCD buffer-pool)**
which the D2 profile identified as 11.71% on archival workloads — a
different optimization class (allocator pressure, not inner-loop
arithmetic), where the D1/D3 "compiler-already-good" pattern doesn't
apply.
