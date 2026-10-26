# Kakadu Cleanroom Research

Technical description of the **Kakadu JPEG 2000 codec**'s classic (MQ-EBCOT)
decode path, prepared as a **cleanroom reference** for openjp2k contributors —
the Kakadu counterpart to `../grok-cleanroom-research/`.

## Why this exists

openjp2k is Apache 2.0. Kakadu is **proprietary, commercial, closed-source**.
openjp2k cannot lift, port, or derive code from Kakadu.

openjp2k *can* benefit from understanding the design choices that made Kakadu
the long-time performance reference for JPEG 2000. These documents describe
those choices at the level of **architecture and published-standard technique**,
so implementers can pursue the same ideas from the public standard without ever
touching Kakadu.

## Provenance — read this carefully

Unlike the Grok folder (which was written by reading AGPL *source*), these
documents were produced by **decompiling a shipped Kakadu binary** — the
**Kakadu 6.4** core statically linked into a third-party application
(`TiffComp.ocx`), analyzed with Ghidra. This is a stricter situation, so:

- The documents describe **only** architecture, data-structure shape, and
  algorithmic technique — all of which are **implementations of the published
  JPEG 2000 standard** (ISO/IEC 15444-1 Annexes C/D, EBCOT + MQ) and of
  techniques described in **Taubman & Marcellin's textbook** and the original
  EBCOT papers, which the Kakadu binary itself cites.
- They contain **no decompiled Kakadu code**, no verbatim routines, and no
  literal tuning constants beyond those defined by the standard itself.
- Addresses/offsets quoted (e.g. `0x180552aa0`) are evidence anchors into the
  analyzed binary for traceability; they are not portable and carry no code.

## Cleanroom rules for openjp2k contributors

- ✅ Read these documents freely.
- ✅ Read OpenJPEG source (BSD-2-Clause — Apache-compatible).
- ✅ Read OpenJPH / public standards (ISO/IEC 15444-1/-15) and public papers
  (Taubman & Marcellin; EBCOT; PCRD-opt).
- ❌ **Do not** obtain, decompile, or read Kakadu source or binaries yourself if
  you intend to contribute production code to openjp2k. Work only from these
  descriptions and the public references above.

## Contents

| Document | What it covers |
|---|---|
| `kakadu-decode-notes.md` | The five classic-T1 fast-path techniques (column-packed state words, zero-column skip, register-resident MQ, context quads, pass×mode-specialized kernels), line-based DWT, region decode, threading. Evidence anchors + license boundary. |
| `openjp2k-vs-kakadu-t1-diff.md` | **Start here for action.** Diff of openjp2k's current `t1.c`/`dwt.c` against those techniques. Finding: **4 of 5 T1 techniques are already in openjp2k's base** (the modern OpenJPEG `opj_flag_t` design). Real open gaps = inverse-DWT threading and region-selective decode. Revised priority list. |
| `kakadu-version-decode-speed.md` | Whether newer Kakadu (7.x/8.x) beats the analyzed 6.4 on *classic* decode. Short answer: no algorithmic gain (serial MQ ceiling) — only multicore scaling + newer-ISA SIMD; the 10–30× leap was HTJ2K, a different block coder. |

## Bottom line for implementers

The diff found openjp2k is **not behind on any hidden Kakadu algorithm** — the
column-state-word T1 design is already present. Spend effort on (1) threading
the currently-serial inverse DWT into the pool, (2) a precinct/region-selective
decode path for the viewer, and (3) a small T1 zero-run-skip refinement. Do
**not** add SIMD to the T1 decode passes — MQ-EBCOT is inherently serial and
neither Kakadu nor OpenJPEG vectorizes it.

> Not yet benchmarked against this repo's `bench/` harness — the relative-speed
> claims are structural inferences. Validate the DWT-threading hypothesis on a
> real large tile before committing effort.

Source analysis: rea_wsi RE workspace, 2026-10-24 (Kakadu 6.4 in TiffComp.ocx,
sha256 ceec0056…; openjp2k `t1.c`/`dwt.c` @ `master`).
