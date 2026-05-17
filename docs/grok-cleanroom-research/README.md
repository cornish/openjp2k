# Grok Cleanroom Research

This folder contains a four-document technical description of the
**Grok JPEG 2000 codec** (https://github.com/GrokImageCompression/grok),
prepared as a **cleanroom reference** for openjp2k contributors.

## Why this exists

Grok is AGPL v3. openjp2k is Apache 2.0. The two licenses are
incompatible: openjp2k cannot lift, port, or derive code from Grok.

However, openjp2k *can* benefit from understanding the design choices
that make Grok the fastest open-source JPEG 2000 codec in published
benchmarks. The standard solution is a two-team cleanroom split:

1. A **describer** reads Grok's AGPL source freely and writes
   high-level technical descriptions in their own words.
2. **Implementers** working on openjp2k production code read the
   describer's output, but **never read Grok source themselves**.

The four documents in this folder are the describer's output. They
were produced by an author who read Grok's source. They contain no
verbatim Grok code, no copied struct/function definitions, and no
literal constants beyond mathematical facts defined by the JPEG 2000
standard itself.

## Cleanroom rules for openjp2k contributors

**To preserve cleanroom isolation in your work:**

- ✅ Read these documents freely.
- ✅ Read OpenJPEG source (BSD-2-Clause — fully Apache-compatible).
- ✅ Read OpenJPH source (BSD-2-Clause — embed it for HTJ2K).
- ✅ Read public standards: ISO/IEC 15444-1, -4, -15.
- ✅ Read public papers: Taubman & Marcellin's textbook, the
  original EBCOT and PCRD-opt papers.
- ❌ **Do not read Grok source** if you intend to contribute
  production code to openjp2k.
- ❌ Do not import literal constants from these documents that
  the document itself flags as "Grok-specific tuning" (e.g. the
  PCRD slope-estimator's α=0.75 padding factor). Re-derive them
  by experiment in openjp2k.

## The four documents

Read in this order:

### 1. `grok-performance-innovations.md` — start here (~7,900 words)

The main spec. Subsystem-by-subsystem walk through what Grok does
that's different from stock OpenJPEG, and why it's fast:

- §1 Headline findings — the 10 architectural choices behind Grok's
  measured ~2× speedup over OpenJPEG.
- §3 Taskflow-based per-tile DAG scheduler.
- §4 Incremental stripe compositing (the memory-bound innovation).
- §5 DWT subsystem including the 16-bit fast path, overflow-safe
  SIMD averaging identities, Highway dispatch, cascade synthesis.
- §6 Tier-1 block coder with incremental convex-hull early
  termination (the encoder speedup).
- §7 Tier-2 with TLM/PLT-driven `SelectiveFetchRanges` (the
  cloud-decode enabler) and the progressive PCRD slope estimator.
- §8 Two-tier cache (in-memory LRU + disk spill) and
  network-fetcher integration.
- §10 **Prioritized 15-step implementation order** for openjp2k —
  read this if nothing else.
- §11 Scope exclusions.

Each major section ends with a **"vs OpenJPEG"** subsection that
contrasts Grok's design with OpenJPEG 2.5.3, citing the exact
OpenJPEG file:line. Useful for deciding subsystem-by-subsystem
whether to fork OpenJPEG or implement greenfield from the spec.

### 2. `grok-encoder-deep-dive.md` (~3,800 words)

The main spec leans toward decoder concerns. This document fills
in encoder-specific material: compress pipeline orchestration,
forward DWT (5/3 and 9/7), quantization, the full PCRD-opt
bisection loop, T2 packet emission with tag-tree encoding,
marker writing with TLM backfill, HTJ2K encode via OpenJPH.

Concentrates the encoder-side performance items in §13.

### 3. `grok-public-api.md` (~3,100 words)

Describes Grok's public C API (`grok.h`) and the design choices
behind it. Useful for deciding whether openjp2k should:

- be source-compatible with Grok (drop-in replacement), or
- design a fresh, cleaner API, or
- ship both (fresh API + `grok_compat.h` shim).

Includes a summary table of API subsystems with per-subsystem
"keep / redesign" recommendations.

### 4. `grok-conformance-and-benchmarks.md` (~2,100 words)

How Grok verifies correctness and measures performance:

- ISO/IEC 15444-4 conformance suite (and the fact that Grok
  enables only a fraction of it — a public differentiation
  opportunity).
- Per-platform stratified MD5 baseline pattern for non-regression
  tests.
- OSS-Fuzz integration shape with two fuzzers (decode + compress-
  param mutation).
- CI matrix.
- Benchmark methodology (and Grok's documented weak spot here —
  another opportunity).

Includes a recommendations summary in §10.

## Sources

The describer worked from these public sources:

- Grok master branch as of mid-May 2026
  (`https://github.com/GrokImageCompression/grok`).
- Grok's own architecture docs in `doc/` (TileCache.md,
  16BitDWT.md, IncrementalStripeCompositing.md, S3.md) — these
  are extraordinarily well-written and are cited directly where
  they describe behavior, with no clean-room concern (they are
  documentation, not code).
- OpenJPEG 2.5.3 source (`https://github.com/uclouvain/openjpeg`)
  for the comparison subsections — BSD-2-Clause, freely citable.
- ISO/IEC 15444-1 (Part 1), 15444-4 (Part 4 conformance), and
  15444-15 (Part 15 HTJ2K).
- Taubman & Marcellin, *JPEG 2000: Image Compression Fundamentals,
  Standards and Practice*, Springer 2002.

## Status

These are research/reference documents. They describe Grok as it
exists today; they are not openjp2k design docs and they do not
prescribe what openjp2k must build. The openjp2k team decides
scope and priority; these documents inform those decisions.

If you find an error or omission, file an issue. If a Grok
behavior is described inaccurately — verify against Grok's own
public documentation or by re-reading Grok source (which only
the describer team should do; see the cleanroom rules above).
