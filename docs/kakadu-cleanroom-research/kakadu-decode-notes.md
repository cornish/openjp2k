# Kakadu JPEG 2000 decode — techniques worth borrowing for openjp2k

Decompilation study of the **Kakadu 6.4** core statically linked into Aperio's
`TiffComp.ocx` (sha256 ceec0056…, Ghidra via REA; snapshot
`snapshots\TiffComp.ocx.rea-snapshot`). Goal: identify what a performance-oriented
OpenJPEG fork (`github.com/cornish/openjp2k`, feeding wsitools' cgo jp2k codec and
openscope) can learn. 2026-10-24.

> **License boundary — read first.** Kakadu is proprietary (commercial licence). The
> *techniques* below are implementations of **published, standard-specified** algorithms
> (ISO/IEC 15444-1 Annex C/D EBCOT+MQ; Taubman's "EBCOT" papers and the Taubman &
> Marcellin book Ch. 6/8/17 — the binary literally cites it). This report documents
> **architecture and measurable design choices only**, as a checklist. Implement from the
> standard + Taubman, **not** by transcribing decompiled routines. Do not copy Kakadu code
> or paste decompiled bodies into an Apache/BSD repo. Where OpenJPEG already does the same
> thing, it's noted.

## What's in the binary

Full Kakadu C++ API is exported with symbols: `kdu_codestream`, `kdu_tile`,
`kdu_subband`, `kdu_block` / `kd_block_decoder`, `kdu_decoder`, `kdu_synthesis`,
`kdu_multi_synthesis`, `kdu_region_decompressor`, `kdu_thread_entity`/`kdu_thread_env`.
The embedded parameter docs confirm it's the real KDU core (default code-blocks 64×64,
Part-1/Part-2, ROI, resilient mode). Decode entry for T1 is the vtable slot of
`kd_block_decoder` → the ~2 KB dispatcher `FUN_180552aa0`, which calls five
pass-specialized kernels (two of which, `0x1805546a0` magref and `0x180553580` cleanup,
were read in full).

## The five techniques that make Kakadu's T1 fast

T1 (the embedded block coder) is 60–80% of JPEG 2000 decode time, so this is where a
fork wins or loses. Every item below was **observed in the decompiled magref/cleanup
kernels**, not inferred.

### 1. One packed 32-bit *state word* per 4-sample stripe column — the核心 trick
Kakadu stores code-block samples in **stripe-columns of 4 rows** (`local_264 =
ceil(width/4)`), and keeps a parallel array of 32-bit **neighbourhood-state words**, one
per column. Each word packs the significance + already-coded + sign state of all 4
samples, at **3-bit spacing** (sample *k* occupies bits `19+3k…`; the pass tests
`uVar18>>0x13`, `>>0x16`, `>>0x19`, `>>0x1c`). Neighbour context for a sample is formed
by **masking** that one word (masks `0x1ef`, `0xf78`, `0x7bc0`, `0x3de00` select each
sample's 8-neighbour bits) — no per-pixel neighbour gather, no conditionals. This single
representation is the biggest structural difference from classic OpenJPEG's
per-sample `flags`/`data` arrays.
→ **openjp2k action**: adopt a column-packed state word with fixed bit-lanes so context
formation is `(state & mask)`-indexed table lookups. This is also what OpenJPEG's own
later `opj_t1` rewrite moved toward (`flagsp` column words) — worth diffing your fork
against current `t1.c` to see how far you already are.

### 2. Zero-column fast-skip without touching the arithmetic coder
Before decoding a column the kernel tests `if ((state & 0x12480000) == 0)` (the
"any-sample-codable-this-pass" bits) and, when zero, **runs a tight loop advancing two
columns at a time over all-zero state words** (`while (uVar18==0) param_6+=2`). Entire
insignificant regions are skipped with pointer arithmetic, never entering the MQ decoder.
→ **action**: a per-column "needs work" mask test + run-skip is cheap and large-image WSI
tiles are mostly-insignificant at high bitplanes — big real win.

### 3. MQ decoder state lives in registers across the whole pass
The MQ A-interval / C-code registers (`iVar4`,`iVar16`,`iVar19`) and the byte-source
cursor are loaded once at pass entry, kept in locals through the entire stripe loop, and
written back to the block struct only on return. Renormalization is the inline
`do { … } while (interval < 0x800000)` with FF/bit-stuff handling (`uVar15==0xff` →
`0x8f` threshold) folded in. No per-symbol function calls, no per-symbol state reload.
→ **action**: ensure your MQ decoder is fully inlined into each pass kernel (not a
`mqc_decode()` call per symbol). OpenJPEG inlines via macros; confirm your fork keeps A/C
in locals, not a struct, across the pass.

### 4. Context state + transition stored as a 4-word quad, swapped branch-free
Each context is a 16-byte record at a table offset (`0x100` significant / `0xf0`
run, or `0x110` for the ≥4 refinement case). On an LPS/MPS update the kernel loads the
next-state **quad** (`*puVar2`…`puVar2[3]` vs `puVar2[4]`…`[7]`) in one shot and stores
all four words back — the probability estimate, MPS sense, and next-context pointer swap
together with no branch on symbol value (the `1 - uVar11` sign exchange is arithmetic).
→ **action**: lay out context records so the state transition is a contiguous load+store,
not several dependent updates.

### 5. Pass- and mode-specialized kernels (no generic inner loop)
The dispatcher selects among **separate compiled kernels** per coding pass
(sigprop / magref / cleanup) **and** per mode (causal vs non-causal / "vertically-causal"
`local_268`). Each kernel hard-codes its pass's context rules; there is no runtime
`switch(pass_type)` inside the hot loop. The dispatcher also decodes
`(maxbits)*3 − 2` passes per block and handles the three MQ bypass/termination modes
(`param_2+0x1c` bits 0/1/2/4 = bypass/reset/restart/causal).
→ **action**: template/duplicate the pass kernels per (pass × mode) rather than
branching inside. Costs code size, buys branch-prediction and register allocation.

## Beyond T1 — DWT, region decode, threading

- **DWT is line-based** (`kdu_synthesis`/`kdu_multi_synthesis` take a `kdu_resolution`
  and stream lifting over line buffers via a `kdu_sample_allocator`), not whole-tile
  component buffers. For WSI tiles this is the memory-footprint and cache win. OpenJPEG's
  `dwt.c` is largely whole-tile; a line/stripe-blocked inverse DWT is a legitimate fork
  improvement. (Architecture only observed here — no DWT kernel read.)
- **`kdu_region_decompressor`** decodes only the precincts intersecting a requested
  region at a chosen resolution, with rational up/down-sampling to a render canvas
  (`expand_numerator`/`denominator`, `get_rendered_image_dims`). For a slide **viewer**
  this matters more than raw throughput — it's finer-grained than OpenJPEG's
  `opj_set_decode_area`. Worth designing openscope's decode calls around a
  region+resolution API rather than full-tile decode.
- **Threading** is a job pool (`kdu_thread_entity::add_queue`/`add_jobs`/`process_jobs`/
  `synchronize`/`register_synchronized_job`). Granularity: per-code-block T1 jobs
  (embarrassingly parallel) with DWT synthesis as synchronized stages. You already vendor
  taskflow — the lesson is **where the cut points are**: fan out T1 at code-block
  granularity, join at each DWT resolution level, double-buffer between.

## The honest strategic caveat

This is **2009-era** Kakadu. The field's biggest decode speedup since is **HTJ2K
(ISO 15444-15 / the FBCOT block coder)** — a ground-up T1 replacement that is
~10–30× faster than MQ-EBCOT and is where modern Kakadu's headline numbers come from.
Your stack already has an htj2k codec (openscope `decoders`, wsitools `all_htj2k.go`).
So:
- For **throughput on new data**: HTJ2K-first, or a J2K→HTJ2K transcode path, beats
  anything learnable from classic EBCOT here.
- For **decoding existing SVS/JP2/QPTIFF archives** (all classic MQ-EBCOT — our whole
  WSI corpus): the five T1 techniques above are exactly the right target, and KDU 6.4 is
  a faithful, complete reference for them.

## Checklist for openjp2k (classic-J2K decode path)

> **Superseded for T1 by `openjp2k-vs-kakadu-t1-diff.md` (2026-10-24).** After reading the
> fork's `t1.c`, items 1/3/4/5 below are **already implemented** in openjp2k's base (the
> modern OpenJPEG T1 uses the same column-packed `opj_flag_t` design). Item 2 is present
> but coarser. The real open gaps are DWT threading and region-selective decode. Keep this
> list as the Kakadu-side reference; act from the diff report.

1. [x] Column-packed 32-bit state words, fixed 3-bit sample lanes, mask-indexed contexts.
   — **already in openjp2k** (`opj_flag_t`, `T1_FLAGS`, `<<(ci*3U)`).
2. [~] Per-column zero-skip run loop that bypasses the MQ coder. — **partial**: openjp2k
   does per-column `if (flags != 0)`; Kakadu sprints over *runs* of empty columns with a
   pass-relevant lane mask. Small localized win.
3. [x] MQ A/C registers held in locals across each full pass; renorm + bit-unstuff inlined.
   — **already in openjp2k** (`opj_mqc_decode_macro`, `DOWNLOAD/UPLOAD_MQC_VARIABLES`).
4. [x] Context records as contiguous quads; branch-free MPS/LPS state+pointer swap.
   — **already in openjp2k** (equivalent cost via `mqc_states` + `curctx` pointer).
5. [x] Separate kernels per (pass × causal-mode); no `switch` in the inner loop.
   — **already in openjp2k** (`*_64x64_{novsc,vsc}` / `*_generic_*` / `*_raw`).
6. [ ] Line/stripe-blocked inverse 5/3 and 9/7 DWT instead of whole-tile buffers.
   — **open**, but openjp2k's DWT is already SIMD+int16; win is footprint, not ALU.
7. [ ] Region+resolution decode API (precinct-selective) for the viewer path. — **open**.
8. [ ] Code-block-granularity T1 jobs; DWT-level synchronized joins (taskflow).
   — T1 pool exists; **inverse DWT is serial** — threading it is the biggest open win.
9. [ ] ~~SIMD T1~~ — **non-goal**: MQ-EBCOT is serial; neither codec vectorizes T1 decode.

Evidence anchors (TiffComp.ocx snapshot): T1 dispatcher `0x180552aa0`; magref kernel
`0x1805546a0`; cleanup kernel `0x180553580`; sigprop/other kernels `0x180555130`,
`0x180555ed0`, `0x180554e20`; `kd_block_decoder` vtable `0x18069d370`;
`kdu_region_decompressor`/`kdu_thread_entity` via exported symbols. Codec selection
(KDU vs OpenJPEG 1.4 vs Matrox MIL) documented in reports/bif-format.md §ImageScope
(`compCodec=MIL` switch, "JPEG2000 codec selected/unavailable").
