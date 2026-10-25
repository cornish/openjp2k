# openjp2k T1/DWT vs the Kakadu techniques — actual diff

Comparison of `cornish/openjp2k` (`src/lib/openjp2/t1.c`, `dwt.c` at `master`, 2026-10-24)
against the Kakadu 6.4 decode techniques in `kakadu-decode-notes.md`. Purpose: stop the
fork from reimplementing things it already inherited from the modern OpenJPEG T1 rewrite,
and point optimization effort at the gaps that are actually open.

## Headline

**Four of the five Kakadu "T1 fast-path" techniques are already present in openjp2k's
base.** The 2016 Even-Rouault OpenJPEG T1 rewrite independently arrived at the same
column-packed-flag design Kakadu uses. The Kakadu report's T1 checklist was written
before reading your source and overstated the opportunity — corrected here.

## Point-by-point

| # | Kakadu technique | openjp2k `t1.c` today | Verdict |
|---|---|---|---|
| 1 | Column-packed state word, 4 samples × 3-bit lanes, mask-indexed context | **Present.** `opj_flag_t *flagsp`, one word per 4-row column; `T1_FLAGS(x,y) = flags[x+1 + ((y/4)+1)*(w+2)]`; per-sample state at `<< (ci*3U)` with `T1_SIGMA_THIS`/`T1_PI_THIS`/`T1_SIGMA_NEIGHBOURS` | **Already done** — equivalent to Kakadu |
| 2 | Zero-column fast-skip bypassing the MQ coder | **Present but weaker.** `opj_flag_t flags = *flagsp; if (flags != 0) {…}` in `opj_t1_dec_sigpass_mqc_internal` / `_refpass_mqc_internal` / `_clnpass_internal` — skips one column at a time | **Partial gap** (see below) |
| 3 | Pass × mode specialized kernels, no inner `switch` | **Present, arguably more complete.** `opj_t1_dec_sigpass_mqc_64x64_{novsc,vsc}`, `_generic_{novsc,vsc}`, `_raw`; refpass `_64x64`/`_generic`/`_raw`; clnpass `_64x64_{novsc,vsc}`/`_generic_{novsc,vsc}`; routers branch on tile size + VSC once, outside the loop | **Already done** |
| 4 | MQ registers held across the pass, renorm inlined | **Present.** `opj_mqc_decode_macro(v,mqc,curctx,a,c,ct)` with `DOWNLOAD_MQC_VARIABLES`/`UPLOAD_MQC_VARIABLES` bracketing each pass loop; A/C/CT in locals | **Already done** |
| 5 | Context state as contiguous quad, branch-free swap | Equivalent-cost different layout: OpenJPEG `curctx` points into the 47-entry `mqc_states` table ({qeval,mps,nmps,nlps}); swap is a pointer reassign | **Already done** (cosmetic difference, no win) |

## The one T1 micro-gap worth a look (#2)

OpenJPEG tests `if (flags != 0)` **per column** — it still visits and branches on every
column word. Kakadu, on hitting a zero word, enters a tight **run-skip**:

```
if ((state & 0x12480000) == 0) {       // no sample codable this pass in this column
  param_6 += 2;
  while (*param_6 == 0) { param_6 += 2; param_5 += 2; }   // sprint over empty columns
}
```

i.e. it advances through *runs* of empty columns without re-entering the per-sample
dispatch. On high bitplanes of large WSI tiles (mostly-insignificant), this turns a
per-column branch into a pointer sprint. **This is the only T1-level change with a
plausible (small) win** — and it's a localized edit to the three `*_internal` loops, not
a redesign. Measure before/after on a real 40× SVS tile; expect low-single-digit percent,
concentrated in the first passes.

Note Kakadu's skip tests a *"codable this pass"* mask (`0x12480000`), not merely
`flags != 0`. OpenJPEG's `flags != 0` is slightly coarser (a column can be non-zero from
neighbour-propagation bits yet have nothing to code this pass). Matching Kakadu means
testing the pass-relevant lanes, not the whole word.

## SIMD in T1 is a non-goal — for both codecs

Neither Kakadu 6.4 nor OpenJPEG vectorizes the T1 decode passes, because MQ-EBCOT is an
inherently **serial** arithmetic decode (each symbol depends on the previous coder
state). openjp2k's T1 has no SIMD and shouldn't grow any; the 8-way unrolling seen in the
Kakadu magref kernel is *address arithmetic* (computing a sample offset), not sample
parallelism. Don't chase this. (HTJ2K/FBCOT is the block coder that *is* parallelizable —
separate track, already in your stack.)

## Where the real Kakadu-vs-openjp2k gap actually is: DWT + decode scope

`dwt.c` is where openjp2k still differs structurally from Kakadu:

- **Whole-tile-component inverse DWT.** `opj_dwt_decode_tile` / `opj_idwt53_h` /
  `opj_idwt53_v` allocate and transform the **entire** tile-component buffer. Kakadu
  streams lifting over a handful of line buffers (`kdu_synthesis`/`kdu_multi_synthesis`
  + `kdu_sample_allocator`). The gap is **memory footprint and cache residency on
  gigapixel tiles**, not raw ALU —
- **…because openjp2k's DWT is already heavily SIMD'd**, which Kakadu 6.4's isn't to the
  same degree: `opj_idwt53_v_cas0_mcols_SIMD` with `__m256i`/`__m512i`/`__m128i`/NEON
  `int32x4_t`, plus a fixed-point **int16 Q1.15** 9/7 path (`opj_dwt_decode_tile_97_int16`,
  `MULHRS_Q15`, `opj_v8_t`/`VREG`). So on throughput you may already match or beat 2009
  Kakadu; the win from line-based processing would be **footprint** (fewer cache misses,
  less allocation) on huge tiles, and it's a real redesign — only worth it if profiling
  shows DWT memory traffic dominating.
- **Inverse DWT is essentially serial.** `opj_dwt_decode_tile` takes an
  `opj_thread_pool_t* tp` but the inverse transform runs mostly on one thread (the
  `opj_dwt_encode_{h,v}_job_t` jobs are encode-side only). Kakadu parallelizes synthesis
  per resolution/stripe. **Threading the inverse DWT (per-row-group jobs into your
  taskflow pool) is a more promising win than anything in T1.**
- **Decode scope.** OpenJPEG offers `opj_set_decode_area` (tile granularity) +
  `opj_set_decoded_resolution_factor`. Kakadu's `kdu_region_decompressor` is
  precinct-selective with rational resampling to a render canvas — finer-grained, and the
  right model for openscope's viewport-driven decode. This is an API/architecture gap, not
  a kernel one.

## Recommended priority for openjp2k (revised)

1. **Thread the inverse DWT** at code-block-row / resolution-level granularity into your
   taskflow pool — biggest likely decode speedup, DWT is currently serial.
2. **Region+resolution-selective decode** path (precinct-level) for the viewer — matches
   openscope's access pattern; avoids decoding full tiles for a viewport.
3. **T1 zero-run skip (#2)** with a pass-relevant lane mask — small, localized, safe.
4. **Line/stripe-blocked inverse DWT** — only if profiling shows DWT memory traffic
   dominating; large redesign, footprint (not ALU) win given existing SIMD.
5. Do **not** reimplement T1 items #1/#3/#4/#5 (already equivalent) or add T1 SIMD
   (serial coder).

Cross-refs: Kakadu techniques + evidence anchors in `kakadu-decode-notes.md`; whether
newer Kakadu versions beat 6.4 on classic decode in `kakadu-version-decode-speed.md`
(short answer: no algorithmic gain — only multicore scaling + newer-ISA SIMD, both
already on this list); codec selection in `bif-format.md` §ImageScope. openjp2k reading: `t1.c`, `dwt.c` @ master,
fetched 2026-10-24 (not benchmarked — claims about relative speed are structural
inferences, verify with your bench harness in `bench/`).
