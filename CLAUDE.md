# openjp2k — Claude context

## What this is

Performance-oriented fork of [uclouvain/openjpeg](https://github.com/uclouvain/openjpeg).
Goal: **permissively-licensed, Kakadu-class JPEG 2000 decode** for the
production domains that depend on JP2K — radiology / DICOM (8/12/16-bit
monochrome and color), geospatial (GeoJP2 / NITF), general photographic
imaging, and whole-slide pathology. Optimizations are measured against a
cross-domain corpus; no single domain's quirks drive decisions in isolation.
Encode performance is not a priority.

Whole-slide imaging prompted the fork (see openscope, below) and remains a
first-class workload, but the value proposition — Apache-licensed decode that
closes the gap with Kakadu and beats Grok where the AGPL forbids use — is
domain-agnostic.

## Licensing — read this before editing files

This fork is dual-status:
- **New code:** Apache-2.0.
- **Inherited from upstream:** BSD-2-Clause, headers preserved.

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for the SPDX header rules. The
critical thing to NOT do: do not strip or rewrite the BSD-2 headers on
inherited files, even when modifying them. Append your copyright line
under the existing notice; the SPDX identifier on those files stays
`BSD-2-Clause`.

## Build & test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Without `OPJ_DATA_ROOT` set, ~1200 tests fail because they can't find their
input files — only the ~117 unit tests pass. The full ~1587-test
conformance suite needs the cross-domain corpus in the sister repo
`openjp2k-data` (which nests upstream's `openjpeg-data` at
`corpus/conformance/openjpeg-data`). Easiest way:

```sh
scripts/run-conformance.sh                # auto-finds the sibling corpus
scripts/run-conformance.sh -- -R NR-DEC   # subset selection
```

The script also accepts `OPJ_DATA_ROOT=/path` to override the sibling-path
auto-discovery. Run a smaller subset during iteration with `ctest -R <regex>`.

## Where performance work lives

The decode hot paths in order of typical wall-time contribution:

- **T1 / entropy decoding** (`src/lib/openjp2/t1.c`, `t1.h`). The MQ
  arithmetic decoder and bitplane coder. Single largest CPU contributor
  for most images. SIMD opportunities and branch-prediction tuning.
- **Inverse DWT** (`src/lib/openjp2/dwt.c`). Already has some SIMD
  paths; check `WITH_SSE`, `WITH_AVX2` defines for what's enabled.
- **Inverse color transform** (`src/lib/openjp2/mct.c`). Mostly memory-
  bound; gains come from SIMD + cache-friendly access patterns.
- **Tile-component buffer management** (`src/lib/openjp2/tcd.c`). Less
  CPU-bound but allocation patterns affect throughput.

Partial-region / ROI decode is a cross-cutting opportunity: geospatial
viewers panning over GeoJP2, DICOM viewers zooming into a radiograph, and
WSI viewers tiling a slide all decode subregions. Vanilla openjpeg does not
short-circuit work outside the requested region; we should. Likewise,
single-component decode (common in DICOM monochrome and in WSI tile
pipelines) has optimization headroom vanilla doesn't bother with.

## Upstream tracking

```sh
git fetch upstream                  # uclouvain/openjpeg
git merge upstream/master           # they're on 'master', we're on 'main'
```

Merge upstream periodically (monthly-ish) so divergence stays manageable.
Expect conflicts on files we've modified for performance.

## Sibling repos

- **[cornish/openjp2k-bench](https://github.com/cornish/openjp2k-bench)** —
  benchmark harness for evaluating changes against vanilla openjpeg, Grok,
  and published Kakadu numbers. Cross-domain corpus (radiology, geospatial,
  photographic, WSI). Build with `--openjpeg-source <path-to-this-fork>` to
  measure our changes.
- **[cornish/openscope](https://github.com/cornish/openscope)** — a WSI
  viewer that consumes this fork. One of several real-world consumers; it
  surfaces the WSI workload shape (single-tile decode, often partial region)
  but does not get to dictate decode-side tradeoffs against other domains.
