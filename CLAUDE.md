# openjp2k — Claude context

## What this is

Performance-oriented fork of [uclouvain/openjpeg](https://github.com/uclouvain/openjpeg).
Goal: faster JPEG 2000 decode on whole-slide-imaging workloads — typically
large `.jp2` tiles (often 256×256 to 4096×4096), decoded one tile at a
time, frequently as a partial region rather than the whole image. Encode
performance is not a priority.

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

Upstream's CTest suite is sizable; run a subset during iteration with
`ctest -R <regex>`.

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

WSI workloads are unusual in that they often decode one component
(or a partial region) at a time; opportunities exist to short-circuit
work that vanilla openjpeg doesn't bother optimizing for that pattern.

## Upstream tracking

```sh
git fetch upstream                  # uclouvain/openjpeg
git merge upstream/master           # they're on 'master', we're on 'main'
```

Merge upstream periodically (monthly-ish) so divergence stays manageable.
Expect conflicts on files we've modified for performance.

## Sibling repos

- **[cornish/openjp2k-bench](https://github.com/cornish/openjp2k-bench)** —
  benchmark harness for evaluating changes. Build with
  `--openjpeg-source <path-to-this-fork>` to test our changes against
  vanilla openjpeg and Grok.
- **[cornish/openscope](https://github.com/cornish/openscope)** — the
  WSI viewer that ultimately consumes this fork. Drives the workload
  shape (single-tile decode, often partial region) that we're
  optimizing for.
