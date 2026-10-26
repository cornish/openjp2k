# Did newer Kakadu speed up *classic* JPEG 2000 decoding?

Decision-support note for `cornish/openjp2k`, companion to
`openjp2k-vs-kakadu-t1-diff.md` and `kakadu-decode-notes.md`. Question: the Kakadu core
we decompiled from TiffComp is **6.4** (~2009). Do newer Kakadu releases (7.x, 8.x)
decode *classic* MQ-EBCOT JPEG 2000 meaningfully faster — i.e. is openjp2k chasing a
moving target?

> **Evidence basis.** This is **not** decompilation-grounded — only 6.4 was analyzed
> firsthand. The below is from public sources + domain knowledge; benchmark figures are
> machine/workload specific and some ("Speed-Pack ~50%") are vendor marketing. Confidence
> flagged per claim.

## Answer: no, not the block decoder itself

The MQ arithmetic decoder is **inherently serial** (each symbol depends on the prior
coder state), and Kakadu 6.4 was already a tight, cache-aware implementation — exactly
what the decompilation showed (register-resident A/C registers, column state words,
zero-column skip). The standard does not permit a dramatically faster single-threaded
classic-J2K block decode, so no newer Kakadu version delivers one. **Confidence: high**
(architectural, not version-specific).

**Key implication for openjp2k: the fork is not behind on any hidden algorithm.** The
per-core classic-decode ceiling applies to Kakadu too.

## What newer Kakadu *did* improve (classic path), by real-world impact

1. **Multithreading scaling.** Headline Kakadu benchmarks (e.g. ~1.79 s → 0.345 s) are
   **8-core thread scaling, not a faster per-core decoder**. Note 6.4 *already had* the
   full `kdu_thread_entity`/`kdu_thread_env` framework we decompiled; 7.x refined it
   ("almost free from critical sections"), it was not new. **Confidence: high.**
   → Maps to openjp2k's #1 open win: the inverse DWT is currently serial — threading it
   is the same lever Kakadu's numbers actually pull.
2. **SIMD on the non-T1 stages** (DWT, dequantization, colour / multi-component), extended
   per instruction set (SSE→AVX→AVX2→AVX512). The classic block coder stays scalar.
   **Confidence: high.** → openjp2k's DWT is already AVX2/AVX512/NEON + int16 Q1.15, so
   this is largely captured.
3. **"Speed-Pack" paid add-on**, marketed ~50% faster — tuned sample-processing / block
   coding with newer SIMD, not an algorithmic change. **Confidence: low** (vendor figure,
   workload-dependent).

## The one large leap is *not* classic J2K

HTJ2K / FBCOT (Kakadu v8, ~2019+) is ~10–30× on the block-coding stage and is
parallelizable — but it is a **different block coder** requiring HT-encoded data, not a
speedup of existing MQ-EBCOT streams. openjp2k/openscope already have an HTJ2K path; it's
a separate track, irrelevant to decoding existing SVS/JP2/QPTIFF archives (all MQ-EBCOT).
**Confidence: high.**

## Net conclusion

Newer Kakadu's advantage over 6.4 on classic decode is almost entirely **(a) multicore
scaling** + **(b) newer-ISA SIMD on sample stages** — both available to openjp2k with no
proprietary knowledge and both already named in the diff report's priority list. There is
no algorithmic classic-decode speedup to reverse-engineer. Chasing throughput beyond this
means HTJ2K, not better MQ-EBCOT.

Sources (fetched 2026-10-24):
- Kakadu, *Decoding HTJ2K on a GPU* (ICIP 2019): https://kakadusoftware.com/wp-content/uploads/ICIP2019_GPU.pdf
- JPEG WG1, *HTJ2K performance case study* (laptop/desktop): https://ds.jpeg.org/documents/jpeg2000/wg1n100269-096-COM-JPEG_Case_Study_HTJ2K_performance_on_laptop_desktop_PCs.pdf
- Chris Hafey, *JPEG2000 Decoding Performance*: http://chafey.blogspot.com/2016/09/jpeg2000-decoding-performance.html
- Kakadu enhanced-server platform overview: https://www.slideshare.net/xband/kakadu-software-highqualityjpeg2000softwareenhancedserverplatform
