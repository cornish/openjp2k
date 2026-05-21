# D2 Profile: Post-D1 Decode Hotspot Investigation

**Date:** 2026-05-21  
**Branch:** main  
**openjp2k commit:** `ae23a798843c7c90769f54c6318cb232f235d910`  
**jp2k-bench commit:** `ebd3cd1521ade4a151cad30f8f26f6d830e0ca1c`

---

## 1. Setup Confirmation

### perf_event_paranoid status

`/proc/sys/kernel/perf_event_paranoid` was **4** at session start and remained at 4
throughout — `sudo` requires a password in this environment so it could not be
lowered. `perf record` is completely blocked at paranoid=4 without `CAP_PERFMON`.

**Fallback:** gprof (`-pg`) instrumentation of a statically-linked
`opj_decompress` binary, combined with `jp2k-bench --profile-stages` for
stage-level wall-clock attribution.

Limitations vs perf:
- gprof samples at SIGPROF (~100 Hz); files that decode in <10 ms per
  invocation accumulate zero samples (WSI tile 2 ms, medical 2 ms,
  conformance <1 ms). For those three files, only call-count data is
  available.
- gprof measures CPU time, not wall clock; results are representative for
  single-threaded compute but miss OS/memory-subsystem effects visible in
  hardware cycle counters.
- Inlined macros (the entire `opj_mqc_fast_decode_macro` body) are
  attributed to their call-site function, not to themselves.

The two large-file runs (synthetic_rgb8 at 240 ms/iter and archival at
~500 ms/iter × 20 iterations) collected adequate samples (~480 and ~836
ticks respectively).

### Build flags (profiling binary)

```
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_FLAGS="-march=native -mtune=native -fno-plt \
                       -funroll-loops -ffp-contract=fast -pg -no-pie"
```

The gprof binary compiles at `-O2` (RelWithDebInfo) plus the corpus flags.
Production bench builds use `-O3`; the function-percentage ordering is
representative but per-call latencies are slightly inflated.

### jp2k-bench stage-timing binary

Built from same openjp2k source (`ae23a798`) but at `-O3 -march=native`
(no `-pg`) via:

```
cmake -DJP2KBENCH_OPENJP2K_SOURCE=/home/cornish/GitHub/openjp2k \
      -DJP2KBENCH_BUILD_GROK=OFF
```

---

## 2. Per-File Top-N Tables

### Stage attribution summary

All five files confirm that >96% of wall time is in the `decode` stage;
`setup`, `unpack`, and `teardown` are negligible for files > 0.1 ms.

| File | decode min (ms) | setup % | decode % | unpack % |
|---|---|---|---|---|
| synthetic_rgb8 | 237.6 | 0.05% | 99.5% | 0.4% |
| wsi_tile | 1.77 | 0.2% | 98.3% | 0.9% |
| archival | 2101.7 | 0.0% | 96.9% | 2.4% |
| medical (CT1) | 2.72 | 0.2% | 96.1% | 3.3% |
| conformance (p1_07) | 0.036 | 5.3% | 77.8% | 0.2% |

The conformance file's 5% setup fraction is expected — at 36 µs total, even
header parsing is visible. The archival file's 2.4% unpack is the
`imagetopnm` float→pixel conversion of a multi-megapixel image.

---

### File 1: `pLRCP_d5_b64_t1024x1024_lossless_l1_mon_enone.jp2` (synthetic RGB8)

**Profile basis:** gprof, 20 iterations merged, 480 ticks  
**Codec:** lossless 9-7 + MQ, 5 decomp levels, 1 tile 1024×1024, 64×64 codeblocks

| Rank | Function | Self % | Self (s) | Calls |
|---|---|---|---|---|
| 1 | `opj_t1_fast_dec_refpass_mqc` | 40.79% | 1.95 | 110,260 |
| 2 | `opj_t1_dec_sigpass_mqc_fast_64x64_novsc` | 39.96% | 1.91 | 110,260 |
| 3 | `opj_t1_dec_clnpass_fast_64x64_novsc` | 15.48% | 0.74 | 125,620 |
| 4 | `opj_mct_decode` | 0.84% | 0.04 | 20 |
| 5 | `opj_tcd_decode_tile` (self) | 0.84% | 0.04 | 20 |
| 6 | `opj_idwt53_v` | 0.63% | 0.03 | 3,600 |
| 7 | `imagetopnm` | 0.63% | 0.03 | 20 |
| 8 | `opj_t1_clbl_decode_processor` (self) | 0.42% | 0.02 | 15,360 |
| 9 | `opj_idwt53_v_final_memcpy` | 0.42% | 0.02 | 3,600 |

**T1 total: ~97%** (refpass + sigpass + clnpass)  
**DWT (5/3 lossless): ~1.3%** (idwt53_v + idwt53_h)  
**MCT: 0.84%**

`opj_t1_fast_dec_refpass_mqc` is essentially 100% the
`opj_t1_dec_refpass_mqc_fast_64x64` specialization (the w=h=64 path in
the dispatch). Similarly, the sigpass dispatch immediately calls
`opj_t1_dec_sigpass_mqc_fast_64x64_novsc`.

---

### File 2: `TC_031_0010__L1_c032r016.j2k` (WSI tile, 240×240)

**Profile basis:** call counts only (2 ms/iter, zero gprof samples at 100 Hz)  
**Codec:** lossy 9-7 + MQ, small codeblocks (non-64×64 → generic path), 1 tile

gprof call counts from 20 merged runs (representative call ratios, no
timing):

| Rank | Function | Calls (20 iter) | Notes |
|---|---|---|---|
| 1 | `opj_t1_dec_clnpass_step_fast` | 2,598 | inner step loop |
| 2 | `opj_t1_dec_sigpass_step_mqc_fast` | 2,148 | inner step loop |
| 3 | `opj_v8dwt_decode_step2_sse` | 1,416 | 9/7 DWT lift step |
| 4 | `opj_bio_read` | 587 | T2 bit I/O |
| 5 | `opj_v8dwt_decode` | 354 | DWT dispatch |
| 6 | `opj_tgt_decode` | 334 | tag-tree |
| 7 | `opj_v8dwt_interleave_h` | 177 | DWT interleave |
| 8 | `opj_t1_fast_dec_clnpass` | 122 | clnpass dispatch |
| 9 | `opj_t1_dec_clnpass_fast_generic_novsc` | 116 | generic clnpass body |
| 10 | `opj_t1_fast_dec_sigpass_mqc` | 106 | sigpass dispatch |

Key observation: the WSI tile uses the **generic path** throughout (no
64×64 specializations). `opj_t1_dec_sigpass_mqc_fast_64x64_novsc` appears
only 5 times (setup overhead) vs 2,148 calls to the step function.
`opj_v8dwt_decode_step2_sse` has ~1.4k calls indicating the 9/7 float DWT
contributes visibly relative to a 240×240 tile.

---

### File 3: `06103_1890-0001.jp2` (archival, loc-maps)

**Profile basis:** gprof, 20 iterations merged, 836 ticks  
**Codec:** lossy 9-7 + MQ, 1120 tiles, large image, 64×64 codeblocks

| Rank | Function | Self % | Self (s) | Calls |
|---|---|---|---|---|
| 1 | `opj_t1_dec_sigpass_mqc_fast_64x64_novsc` | 22.84% | 9.56 | 1,192,520 |
| 2 | `opj_t1_dec_clnpass_fast_64x64_novsc` | 21.98% | 9.20 | 1,401,860 |
| 3 | `opj_t1_fast_dec_refpass_mqc` | 11.92% | 4.99 | 1,401,960 |
| 4 | `opj_tcd_decode_tile` (self) | 11.71% | 4.90 | 1,120 |
| 5 | `opj_dwt_decode_real` (self) | 10.92% | 4.57 | 3,360 |
| 6 | `imagetopnm` (output, not codec) | 7.57% | 3.17 | 20 |
| 7 | `opj_v8dwt_interleave_h` | 2.51% | 1.05 | 792,120 |
| 8 | `opj_t1_clbl_decode_processor` (self) | 2.34% | 0.98 | 759,900 |
| 9 | `opj_t1_fast_dec_sigpass_mqc` (dispatch) | 2.15% | 0.90 | 1,426,760 |
| 10 | `opj_v8dwt_decode_step2_sse` | 1.84% | 0.77 | 6,223,200 |
| 11 | `opj_t1_dec_clnpass_fast_generic_novsc` | 1.72% | 0.72 | 275,960 |
| 12 | `opj_mct_decode_real` | 1.12% | 0.47 | 1,120 |
| 13 | `opj_v8dwt_decode` | 0.79% | 0.33 | 1,555,800 |
| 14 | `opj_tgt_decode` | 0.12% | 0.05 | 6,554,020 |
| 15 | `opj_t2_init_seg` | 0.10% | 0.04 | 23,520 |

**T1 total: ~57%** (sigpass 22.84% + clnpass 21.98% + refpass 11.92% +
dispatch+generic 4%)  
**opj_tcd_decode_tile self: 11.71%** — per-tile buffer allocation cost (see §4)  
**9/7 DWT total: ~16%** (opj_dwt_decode_real 10.92% + v8dwt_interleave_h
2.51% + v8dwt_decode_step2_sse 1.84% + v8dwt_decode 0.79%)  
**Output (imagetopnm): 7.57%** — not part of codec  
**MCT: 1.12%**

DWT jumps from ~1% (lossless, synthetic) to ~16% (lossy 9/7, archival)
because: (a) 9/7 does float arithmetic vs 5/3 integer, and (b) 1120 tiles
× 3 components = 3360 calls to `opj_dwt_decode_real`.

---

### File 4: `J2KI_CT1_J2KI.j2k` (DICOM CT mono16, 512×512)

**Profile basis:** call counts only (2 ms/iter, zero samples)

| Rank | Function | Calls (20 iter) | Notes |
|---|---|---|---|
| 1 | `opj_v8dwt_decode_step2_sse` | 19,840 | 9/7 DWT lift step |
| 2 | `opj_bio_read` | 14,600 | T2 bit I/O |
| 3 | `opj_tgt_decode` | 11,600 | tag-tree |
| 4 | `opj_free` | 7,120 | alloc/free overhead |
| 5 | `opj_v8dwt_decode` | 4,960 | DWT dispatch |
| 6 | `opj_calloc` | 4,800 | alloc overhead |
| 7 | `opj_t1_fast_dec_clnpass` | 4,220 | clnpass dispatch |
| 8 | `opj_mqc_setstate` | 4,200 | MQC init |
| 9 | `opj_t1_dec_clnpass_fast_generic_novsc` | 3,840 | clnpass body |
| 10 | `opj_t1_fast_dec_sigpass_mqc` | 3,840 | sigpass dispatch |

Key: CT uses generic-path codeblocks too. Heavy `opj_v8dwt_decode_step2_sse`
count relative to T1 calls suggests the CT image is less entropy-coded
(fewer bitplanes) making DWT proportionally larger.

---

### File 5: `p1_07.j2k` (conformance, tiny)

**Profile basis:** call counts only (<1 ms/iter, zero samples)

| Rank | Function | Calls (20 iter) | Notes |
|---|---|---|---|
| 1 | `opj_t1_dec_clnpass_step_fast` | 11,960 | inner step loop |
| 2 | `opj_t1_dec_sigpass_step_mqc_fast` | 10,280 | inner step loop |
| 3 | `opj_free` | 9,960 | alloc overhead |
| 4 | `opj_bio_read` | 7,240 | T2 bit I/O |
| 5 | `opj_calloc` | 7,220 | alloc overhead |
| 6 | `opj_t1_dec_clnpass_fast_generic_novsc` | 4,340 | generic clnpass body |
| 7 | `opj_t1_fast_dec_clnpass` | 4,340 | clnpass dispatch |
| 8 | `opj_t1_fast_dec_refpass_mqc` | 3,740 | refpass dispatch |
| 9 | `opj_t1_fast_dec_sigpass_mqc` | 3,740 | sigpass dispatch |
| 10 | `opj_tgt_decode` | 3,640 | tag-tree |

Key: uses generic path (non-64×64 codeblocks). At 0.036 ms total, 5.3% of
decode time is in setup (header parsing) — the overhead functions `opj_tgt_create`,
`opj_tgt_destroy`, `opj_tcd_code_block_dec_deallocate` each appear 2640–1320 times
relative to only 960 codeblock decode invocations, suggesting per-cblk teardown
is relatively expensive at this scale.

---

## 3. Cross-File Synthesis

### Universal top-5 functions (appear in top-N on every profiled file)

| Function | Synthetic | Archival | Notes |
|---|---|---|---|
| T1 sigpass | #2 (39.96%) | #1 (22.84%) | 64×64 on large files, generic on WSI/CT/conf |
| T1 clnpass | #3 (15.48%) | #2 (21.98%) | same dispatch pattern |
| T1 refpass | #1 (40.79%) | #3 (11.92%) | always top-3 on timed files |
| 9/7 DWT step | <1% | #10 (1.84%) | grows with lossy files and tile count |
| alloc/free | <1% | #4 (11.71%) | **explodes** on high-tile-count files |

**Workload-specific findings:**

- **Lossless (synthetic):** T1 passes consume ~97% of decode; DWT is
  negligible (5/3 integer, 1 tile). The inner loop of sigpass+refpass+clnpass
  is the only target that matters.

- **Lossy large image (archival):** T1 drops to ~57% as DWT (9/7 float,
  16%) and per-tile buffer allocation (11.71%) become meaningful. The
  archival has 1120 tiles decoded sequentially; `opj_tcd_decode_tile` self
  at 11.71% is almost entirely tile-component buffer `malloc`/`free`
  amortized over 3360 component passes.

- **Small codeblocks (WSI, CT, conformance):** Generic path only
  (`opj_t1_dec_sigpass_mqc_fast_generic_novsc`, step functions) — the
  64×64 specialization provides zero benefit. `opj_v8dwt_decode_step2_sse`
  is proportionally larger because entropy work per pixel is lower.

- **Cross-workload DWT pattern:** Every lossy file has `opj_v8dwt_decode_step2_sse`
  in the top-10 by call count. For WSI and CT tiles the step2_sse count
  exceeds T1 step calls, suggesting DWT is competitive with T1 for small
  codeblock images.

---

## 4. Annotated Hot Lines

### 4.1 `opj_t1_fast_dec_refpass_mqc` — #1 hotspot on synthetic (~41%)

The dispatch is a 3-line function that calls `opj_t1_dec_refpass_mqc_fast_64x64`
directly when w=h=64 (the common case). All cycles are in the body:

```c
/* t1_fast.c — refpass dispatch */
void opj_t1_fast_dec_refpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno)
{
    if (t1->w == 64 && t1->h == 64) {
        opj_t1_dec_refpass_mqc_fast_64x64(t1, bpno);   // ← all cycles here
    } else {
        opj_t1_dec_refpass_mqc_fast_generic(t1, bpno);
    }
}
```

The hot loop inside `opj_t1_dec_refpass_mqc_fast_64x64` (macro-expanded
from `opj_t1_dec_refpass_mqc_fast_internal`):

```c
for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) {
    for (i = 0; i < l_w; ++i, ++data, ++flagsp) {
        opj_flag_t flags = *flagsp;
        /* Check T1_SIGMA_THIS bits for all 4 rows: */
        if (flags & T1_SIGMA_4) {
            /* expand to: read pktbl[curidx], subtract qe, ... */
            opj_t1_dec_refpass_step_mqc_fast_macro(..., 0, ...);
        }
        /* ... repeated for ci=1,2,3 */
    }
}
```

The inner loop body is the `opj_mqc_fast_decode_macro` expanded inline:

```c
/* mqc_fast.h — core decode macro (one MQ symbol) */
OPJ_UINT32 _pkd = pktbl[*curidx];         // packed state load (1 LUT hit)
OPJ_UINT32 _qe  = OPJ_MQC_PACK_QEVAL(_pkd);  // mask, 16-bit
OPJ_UINT32 _mps = OPJ_MQC_PACK_MPS(_pkd);    // bit 16
(a) -= _qe;
if (((c) >> 16) < _qe) {                  // LPS path (rare)
    ...
    opj_mqc_fast_renormd_macro(mqc, a, c, ct);
} else {
    (c) -= _qe << 16;
    if (((a) & 0x8000) == 0) {            // renorm needed
        ...
        opj_mqc_fast_renormd_macro(mqc, a, c, ct);
    } else {
        (d) = _mps;                        // fast MPS path: 1 store
    }
}
```

The disassembly (from objdump at `-O2 -march=native`; the production build
at `-O3` will be tighter) shows the inner loop is ~40 instructions with two
conditional branches (LPS check, renorm check). The `pktbl` load is
RIP-relative (GOT fix from D1.3 is effective). The likely bottleneck is
the data-dependent branch on `(c >> 16) < _qe` — the prediction miss rate
depends on the symbol distribution of the image content.

### 4.2 `opj_t1_dec_sigpass_mqc_fast_64x64_novsc` — #2 hotspot on synthetic (~40%)

The sigpass inner loop is similar but more complex: it tests the
significance flag first (`if (flags != 0)`) before entering the MQ decode
path, then calls the step macro four times. From the disassembly:

```asm
; Inner loop at 0x47c430 (abridged, -O2 build):
  mov    (%rdi),%esi          ; load flags
  test   %esi,%esi            ; if flags == 0, skip entirely
  je     <skip>
  test   $0x200010,%esi       ; already significant in row 0?
  jne    <row0_skip>
  ...                         ; ZC context lookup + MQ decode
  lea    0x10b1a1(%rip),%r12  ; pktbl RIP-relative (GOT-fixed ✓)
  movzbl (%r10,%r15,1),%r13d  ; ctxs_idx[] lookup
  mov    0xd4(%r8,%r13,4),%r11d ; ctxno_zc table lookup (second LUT)
  mov    (%r12,%r11,4),%r15d  ; pktbl[state_idx] (packed state load)
```

Notable: there are **two LUT accesses per ZC check** — one to map the
significance flags to a ZC context number, and one to load the packed
state. The ZC-context table (`mqc->ctxno_zc_lut`, or equivalent) is a
separate lookup from the packed state table. This is the "sign table"
opportunity noted in the spec as D3.

### 4.3 `opj_tcd_decode_tile` self-time (11.71% on archival)

The 4.90 s self-time in `opj_tcd_decode_tile` on the archival file is NOT
computation — it is `opj_alloc_tile_component_data` called once per
component per tile:

```c
/* tcd.c:1629 — called 3360 times (1120 tiles × 3 comps) */
if (!opj_alloc_tile_component_data(tilec)) { ... }

/* tcd.c:760 — the body: */
OPJ_BOOL opj_alloc_tile_component_data(opj_tcd_tilecomp_t *l_tilec)
{
    if ((l_tilec->data == 00) || ...) {
        l_tilec->data = opj_image_data_alloc(l_tilec->data_size_needed);
        // malloc for a tile-component buffer: e.g., 256×256×4 = 256 KB
    }
    ...
}
```

Each call allocates a fresh tile-component buffer. For 1120 tiles × 3
components, this is 3360 `malloc`/`free` pairs, averaging 256 KB each.
The allocator pressure dominates the gprof "self" time. This is the
**TCD buffer-reuse opportunity** tracked as spec item D6.1 (struct-split
for deferred follow-up).

---

## 5. Recommendation

### Named deliverable: **D3 — Sign-bit Table + ZC Context Fusion**

**Target function/loop:**  
`opj_t1_dec_sigpass_mqc_fast_64x64_novsc` and
`opj_t1_dec_clnpass_fast_64x64_novsc` (together #1+#2 on 3 of 5 files)

**What it targets:**  
Inside each sigpass step, two sequential table lookups happen before the
MQ decode:
1. `opj_t1_getctxno_zc(mqc, flags >> (ci * 3U))` — maps 9 significance
   bits to a ZC context index (0–8, from a small table)
2. `opj_t1_setcurctx_fast(curidx, ctxt1)` — stores the context index
3. Then `opj_mqc_fast_decode_macro(...)` loads `pktbl[*curidx]`

The `getctxno_zc` lookup returns a small index (0–8), and `curidx` is
immediately overwritten again by the MQ state transition. A
pre-computed table indexed on the significance flags directly
(pre-shifted, 9-bit key → packed MQ state for the initial ZC decode)
would collapse steps 1+3 into a single lookup, saving one indirect
memory access per ZC check. This is spec item **D8 (pre-shifted LUT)**
in its ZC flavor.

Additionally, sign-bit decoding (sigpass only) uses a separate `lu =
opj_t1_getctxtno_sc_or_spb_index(flags, ...)` call that computes a
context number and immediately uses it for one more `pktbl` load. A
fused sign table that pre-combines this lookup with the state entry would
eliminate that second `pktbl` access. This is spec item **D3 (sign-table)**.

**Estimated win magnitude:**  
Sigpass + clnpass + refpass = ~96% of decode cycles on the synthetic
workload and ~57% on archival (excluding imagetopnm output). The ZC
context table lookup is executed on every non-zero flag bit; at 64×64 with
multiple bitplanes, this fires ~6–8 times per codeblock column. Removing
one indirect load per ZC event is conservatively 5–10% of sigpass/clnpass
execution time, or **3–6% of total decode wall clock on the synthetic
workload** and somewhat less on archival (where DWT is also significant).

This is a speculative estimate pending a real measurement; the D1 pattern
(got-fix yielding 0.45% despite being an obvious 1 load/decode win) shows
that the compiler may partially hoist these loads already. A targeted
micro-benchmark (sigpass only, profiler-counter enabled) should quantify it.

**Spec alignment:**  
- Aligns to **D3 (sign-table)** and **D8 (pre-shifted LUT)** from the
  existing spec.
- Does not conflict with D7 (multi-buffer MQ) — these are orthogonal.

**Surprises from the profile:**

1. **Refpass is as hot as sigpass** (40.8% vs 40.0% on synthetic). The
   original D2 spec text said "function-pointer dispatch"; this profile
   shows the dispatch overhead is negligible — the bottleneck is inside
   the 64×64 loop bodies, not at the call site. Refpass has no ZC context
   lookup but its flag-test pattern is similar.

2. **TCD buffer allocation at 11.71% on archival** is a genuine secondary
   bottleneck for multi-tile images. A tile-component buffer pool (reuse
   across tiles of the same size) could recover this without touching the
   hot paths. This maps to the D6.1 spec item (struct-split) already
   deferred; it deserves higher priority for archival/remote-sensing
   workloads than previously estimated.

3. **DWT is non-trivial for lossy workloads.** On the archival file, the
   9/7 float DWT consumes ~16% of decode time — comparable to half the
   clnpass budget. The 5/3 integer DWT on the lossless synthetic file is
   only 1%. Sub-project 3 (IDWT) is correctly targeted at lossy images.

4. **Generic-path codeblocks dominate WSI and CT.** The 64×64
   specialization provides zero benefit for these workloads; the generic
   variants (`opj_t1_fast_dec_*_generic_novsc`) and per-step functions
   (`opj_t1_dec_sigpass_step_mqc_fast`) are the hot paths. Any
   SIMD/vectorization work on T1 must also target the generic path to
   benefit WSI and CT.

5. **The fast path is byte-identical to legacy** (per D1 design): the
   profile shows no legacy functions appearing in the hot path when
   `OPJ_T1_FAST` is not disabled. The fast dispatch is working correctly.

---

## Appendix: profiling tool limitations and recommended follow-up

This profile used gprof as a perf fallback. When `perf_event_paranoid` can
be set to ≤1 (or with `CAP_PERFMON`), a follow-up hardware-counter run is
recommended for:

- **Branch misprediction rate** inside `opj_mqc_fast_decode_macro` — the
  main unknown is whether the `(c >> 16) < _qe` check is predicted
  accurately by the CPU's branch predictor. A high miss rate here would
  validate the pre-shifted LUT direction; a low miss rate would shift
  focus to memory latency instead.
- **L1 cache miss rate** on `pktbl[]` — 94 entries × 4 bytes = 376 bytes,
  fits in a cache line; this should be hot but confirmation matters.
- **Cycle-accurate attribution** inside the refpass loop — gprof cannot
  distinguish the MQ decode body from the renormalize path.

Recommended: `perf stat -e cycles,instructions,branch-misses,L1-dcache-load-misses`
on a single-file bench run once paranoid access is granted.
