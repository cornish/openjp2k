# Grok JPEG 2000 — Performance Innovations

**A cleanroom technical description for openjp2k**

---

## License & Cleanroom Statement

This document is produced by an author who has read Grok's AGPL v3 source
(`https://github.com/GrokImageCompression/grok`, master branch, May 2026).
Its purpose is to describe Grok's performance-relevant architecture and
algorithms in sufficient detail that an independent team can re-implement
equivalent behavior in **openjp2k** (Apache 2.0) **without ever reading
Grok's source**. This is the classic two-team cleanroom split: a
*describer* who may read the AGPL implementation freely, and *implementers*
who work only from the describer's output.

The contents of this document are descriptions, measurements, and
algorithmic statements — not Grok source. No file in this document is a
literal transcription of Grok code. Where pseudo-code appears, it
expresses the algorithm in a form chosen by the describer, not Grok's
implementation form.

The describer's recommendation to openjp2k maintainers is:

* This document is safe to read while writing Apache-licensed code in
  openjp2k.
* The Grok source tree itself is **not** safe to read for anyone who will
  contribute production code to openjp2k. Keep that boundary.
* Performance numbers, public docs, GitHub issue references, and citations
  to third-party libraries (OpenJPH, Highway, Taskflow) are public
  knowledge and may be freely consulted from any source.

The reference baseline against which "innovation" is judged is **OpenJPEG
2.5** — the codec from which Grok was forked many years ago. "Innovation"
here means "differs materially from OpenJPEG and contributes to Grok's
measured performance advantage."

---

## 1. Why Grok Is Fast — Headline Findings

Grok's published benchmarks (GDAL JP2 drivers, Fedora 42, 16 threads)
show roughly 2× over OpenJPEG and 5–10× over Kakadu on whole-image and
regional decode workloads. The performance comes from a small number of
concrete architectural decisions, layered together:

1. **A Taskflow-based per-tile DAG scheduler** that exposes parallelism
   at four granularities simultaneously: tile, component, resolution, and
   codeblock. (OpenJPEG uses a flat thread-pool queue.)

2. **Cross-tile pipelining with bounded in-flight memory** — a parser
   thread feeds tiles into the DAG ahead of the consumer by a fixed
   number of tile-rows (typically 2). The consumer composites rows
   incrementally and the parser back-pressures on `bandDrainCV_`.

3. **An incremental stripe-compositing output path** that keeps resident
   memory proportional to two tile-rows rather than the full image, plus
   an immediate `malloc_trim(0)` after each row release so RSS tracks the
   working set, not the high-water mark.

4. **A pooled-coder, pooled-scratch memory model** — codeblock coders
   are pre-allocated per worker thread per codeblock size; DWT scratch
   is pre-allocated per worker thread per image. The hot decode loop
   does no `malloc`.

5. **A 16-bit DWT path for 5/3 reversible and 16-bit fixed-point 9/7**
   that halves wavelet bandwidth and cache footprint for the common
   case (≤ 12-bit imagery). It requires overflow-safe SIMD averaging
   primitives that are derived from first principles in §5.

6. **Highway-based dynamic SIMD dispatch** for the DWT lifting kernels,
   compiled per target (SSE2, SSSE3, AVX2, AVX-512, NEON, WASM,
   RISC-V V) with a single runtime dispatcher.

7. **TLM/PLT-aware random access** with selective byte-range fetching,
   so a window decode over a cloud object only downloads the precincts
   intersecting the window. A two-tier compressed-data cache (in-memory
   LRU plus disk spillover) supports decoding files larger than RAM.

8. **A progressive PCRD slope estimator** that predicts the
   rate-distortion convex-hull threshold from partial T1 results and
   short-circuits 20–40% of the encoder's bitplane passes.

9. **HTJ2K (Part-15) via embedded OpenJPH**, with OpenJPH's own
   SSSE3/AVX2/AVX-512 dispatch. Grok itself adds a per-block-size coder
   pool around OpenJPH's stateless functions.

10. **Connection-pooled cloud I/O** (curl_multi, up to 100 concurrent
    sockets, HTTP/2 multiplexing) sitting under the compressed-chunk
    cache, so range requests are coalesced and connections reused
    across tiles.

Each of these is described in detail in the sections below, with
algorithmic specificity sufficient for independent implementation.

---

## 2. Subsystem Map

```
                ┌───────────────────────────────────────────┐
                │   Codestream / Marker Parser (TLM, PLT)   │
                │   (single thread, back-pressured)         │
                └─────────────────────┬─────────────────────┘
                                      │ tile parts, SOT metadata
                                      ▼
        ┌─────────────────────────────────────────────────────────┐
        │   Compressed-Chunk Cache  ◄──►  Disk Spill              │
        │   (per-file LRU, GRK_CACHEMAX budget)                   │
        │              ▲                                          │
        │              │ network fetch (S3 / GCS / Azure / HTTPS)│
        │              │ via curl_multi, HTTP/2, 100 conn pool   │
        └─────────────────────────────────────────────────────────┘
                                      │
                                      ▼
        ┌─────────────────────────────────────────────────────────┐
        │   Per-Tile Task Graph (Taskflow)                        │
        │                                                         │
        │     T2 (packet) ─► T1 (block) ─► DWT ─► post-callback   │
        │       per cblk      per cblk      H,V    completion     │
        │                                                         │
        │   • Coder Pool (per worker × per cblk size)             │
        │   • Wavelet Pool (per worker × per image)               │
        │   • Memory Manager (page release via malloc_trim)       │
        └─────────────────────┬───────────────────────────────────┘
                              │ tileCompletion_->complete()
                              ▼
        ┌─────────────────────────────────────────────────────────┐
        │   Tile Completion Tracker                                │
        │     • per-tile / per-row counters                       │
        │     • min-heap for contiguous completion                │
        │     • row callback fires off-lock                       │
        └─────────────────────┬───────────────────────────────────┘
                              │ row ready
                              ▼
        ┌─────────────────────────────────────────────────────────┐
        │   Stripe Compositor (scratchImage_ = one tile-row)       │
        │   → SIMD planar→packed interleave                       │
        │   → format writer (TIFF/PNG/JPEG strip)                 │
        │   → tile cache release + page return to OS              │
        └─────────────────────────────────────────────────────────┘
```

The dotted boundary across the diagram — between the parser and the
task graph — is the **back-pressure surface**. Producer-side
(`scheduleTileBatch`) checks how many tile rows are outstanding;
consumer-side (the row-completion callback) advances the drain pointer
and wakes the producer. This is the structural change that lets Grok
operate at near-constant memory for arbitrarily large images.

---

## 3. Scheduler & Task Graph

### 3.1 Why a DAG, not a queue

OpenJPEG dispatches T1 codeblock decoding through a generic thread pool
(`opj_thread_pool_*` in `thread.c`): the main thread parses, then
enqueues block-decode work items, then joins, then runs DWT, then joins,
then composites. The joins are serializing points. They also make it
natural to allocate per-tile working memory at the top of each tile and
free it at the bottom, which is why memory peaks track the full image.

Grok models every tile's decompression as a Taskflow DAG. A Taskflow
graph is a directed acyclic graph of `tf::Task` nodes with explicit
predecessor/successor edges; the executor runs the graph on a fixed
worker pool, releasing tasks as their predecessors complete. Two
properties matter here:

* **Dependencies, not joins.** A DWT task for resolution R becomes
  runnable as soon as all the T1 tasks for codeblocks in R-1's subbands
  finish — without any thread blocking on a barrier. Other tiles'
  unrelated work continues filling the cores.
* **Cross-tile interleaving.** All in-flight tiles' DAGs share one
  executor. Worker threads pull from a global ready queue, so a
  cheap T1 block in tile 5 runs while an expensive DWT in tile 1
  is still in progress.

### 3.2 Backend selection

Grok exposes a `SchedulerFactory` that selects one of two real backends
based on the `GRK_EXCALIBUR` environment variable: a *standard*
scheduler and an *Excalibur* scheduler. A historical *freebyrd* backend
exists as an unreachable stub. For an initial openjp2k implementation,
the standard backend is sufficient — Excalibur appears to be a
research path with no documented use case.

The single global Taskflow executor is constructed once at library
initialization with `std::thread::hardware_concurrency()` workers (or a
user override) and lives for the process lifetime. Per-tile graphs are
built fresh, submitted, and discarded.

### 3.3 Per-tile DAG shape

For each tile, the standard scheduler builds the following dependency
structure:

* **Per component, per resolution** — a "resolution flow"
  (`ImageComponentFlow`) bundling the codeblock T1 tasks for that
  resolution.
* **Codeblock tasks** within a resolution are siblings (independent of
  each other; the scheduler enqueues each as its own `tf::Task` via the
  `nextTask().work(...)` mechanism of an internal task batch).
* **Wavelet horizontal pass** for the resolution depends on all its
  T1 tasks. **Wavelet vertical pass** depends on horizontal.
* **Resolution-level dependencies** — DWT at resolution R consumes
  outputs at resolution R-1, so wavelet tasks chain across
  resolutions per component.
* **Components are independent** through the DWT; **MCT (inverse
  reversible/irreversible colour transform)** runs as a post-DWT task
  that takes all components as predecessors.
* **DC shift** is *fused* into the final wavelet store when
  whole-tile, non-MCT conditions hold; this saves a full pass over the
  output buffer.
* **Tile completion** is a sink task that decrements the per-row
  completion counter and may trigger the row callback.

The first two resolutions (0 and 1) are grouped into a single
`ResBlocks` to reduce per-resolution task-graph overhead, since they
contain few blocks. This is a small but meaningful constant-factor
optimization on highly-decomposed (≥ 5 levels) images.

### 3.4 Coder pool

The pool is keyed by `(codeblock_width_exp, codeblock_height_exp)`,
producing a vector of `num_workers` pre-allocated coder objects per
size. When a codeblock task starts, it calls into Taskflow's
`this_worker_id()` to obtain its stable worker index *i*, then
retrieves coder *i* from the pool entry for that block size. No
locking, no allocation, no per-block construction. The coder owns its
own scratch buffers (uncompressed coefficient array, flags array, MQ
state) and reuses them across blocks of the same size.

Stock OpenJPEG already does something similar but only at a single
size; Grok's per-size dimension allows correctness for codestreams
that mix block sizes across resolutions or tile-parts, and allows the
coder's internal buffers to be sized exactly once.

### 3.5 Memory manager

A thin singleton (`MemoryManager`) wraps `malloc`/`aligned_alloc` to:

* track aggregate live bytes;
* enforce 64-byte alignment for SIMD;
* expose `releaseFreedPages()` which calls `malloc_trim(0)` on Linux
  and the equivalent on Windows. This is the load-bearing call for
  RSS-tracks-working-set behavior: after `free()`, glibc keeps freed
  arenas in process memory until trimmed. Without an explicit trim,
  the incremental-output strategy still allocates and frees correctly
  but RSS stays at the high-water mark — which defeats the purpose.

`MemoryManager::releaseFreedPages()` is called at the end of every
row drain, immediately after the tile cache releases the row's tile
data.

### 3.6 Back-pressure protocol

The parser thread runs ahead of the executor. Before scheduling a tile
at row `tileY`, it acquires `bandOrderMutex_` and waits on
`bandDrainCV_` until either:

* `tileY < nextBandTileY_ + 2` — i.e. the tile is in the next two
  rows still to drain; or
* `tileY < neededTileY1_` — i.e. an explicit consumer wait extends
  the window to avoid deadlock; or
* `success_ == false` — fail-fast on error.

`nextBandTileY_` is the next row the row-callback will drain;
`neededTileY1_` is set by `TileCompletion::wait(swath)` when a
consumer thread explicitly requests a region. This gives the protocol
two natural modes:

* **Streaming mode** (CLI decode-to-file): the row callback writes
  strips as they complete; back-pressure keeps in-flight bounded.
* **Random-access mode** (GDAL window): a consumer explicitly asks for
  a swath; the producer races ahead but the window-of-need is bounded.

The TLM-aware batched-async path uses a slightly different mechanism
— a bounded `decompressQueue_` with `batchTileQueueCondition_` — but
the semantics are the same.

### 3.7 Tile completion structure

`TileCompletion` tracks two complementary completion concepts:

* **Per-row completion** for the streaming path. Each row has a
  counter of remaining tiles. When a worker thread finishes a tile,
  it decrements its row's counter; on reaching zero, it fires the row
  callback **outside** the TileCompletion lock so the callback's
  composite/write work doesn't serialize against other workers.

* **Contiguous-prefix completion** for the random-access path. A
  min-heap of completed-but-not-yet-acknowledged tile indices lets
  the consumer test whether all tiles `[0..k]` are done in O(1).
  Consumer threads block on `completionCV_` and wake when the heap
  advances past their `neededTileY1_`.

Implementation note for openjp2k: the heap is over tile indices, not
tile rows. The two views (per-row counters and contiguous-prefix
heap) are maintained together because the two consumer patterns
coexist.

### 3.8 vs OpenJPEG

OpenJPEG 2.5.3's scheduling, by direct inspection of `thread.c`,
`tcd.c`, `t1.c`, and `j2k.c`:

* **Thread pool.** `opj_thread_pool_create` / `opj_thread_pool_submit_job`
  / `opj_thread_pool_wait_completion` is a generic FIFO worker queue
  with condition-variable signaling. No DAG, no dependency edges, no
  work-stealing distinct from worker dequeue. With `num_threads ≤ 0`
  it degrades to synchronous execution.
* **Tile-level parallelism.** The decode loop in `opj_j2k_decode_tiles`
  (`j2k.c:11949`) processes tiles strictly sequentially: parse the
  next tile-part, decompress it fully, free, repeat. There is no
  cross-tile interleaving. (Grok overlaps all in-flight tiles in a
  single Taskflow executor.)
* **Within-tile parallelism.** Only T1 codeblock decode is
  parallelized. `opj_t1_decode_cblks` (`t1.c:1872`) submits one job
  per codeblock; `opj_tcd_t1_decode` (`tcd.c:2044`) calls
  `wait_completion(tp, 0)` before invoking DWT. DWT is serial per
  component within a tile; T2 (packet parsing) is single-threaded
  and runs before T1. (Grok parallelizes T1, DWT horizontal pass,
  DWT vertical pass, and post-MCT inside one DAG, all overlapping
  with other tiles.)
* **Coder reuse.** OpenJPEG uses thread-local storage:
  `opj_tls_get(tls, OPJ_TLS_KEY_T1)` (`t1.c:1621`) lazily creates one
  `opj_t1_t` instance per worker thread and reuses it across all
  codeblocks that thread handles. The buffer is grown to the largest
  block seen. This is functionally similar to Grok's coder pool but
  keyed only by worker, not by `(worker × codeblock-size)`, so a
  codestream that mixes block sizes across tile-parts pays
  reallocation cost.
* **Memory model.** Per-tile working buffers are allocated on demand
  by `opj_alloc_tile_component_data` (`tcd.c:760`) and freed in
  `opj_tcd_free_tile` (`tcd.c:1922`) at the end of each tile's
  decode. There is no `malloc_trim` / `madvise` call: freed arenas
  stay in process RSS. (Grok's `MemoryManager::releaseFreedPages`
  returns pages to the OS after each row drain.)
* **Back-pressure.** None. Each tile is parsed, decompressed, and
  written before the next is touched. There is no parser-thread /
  worker-pool separation.
* **Tile cache.** None. Each `opj_get_decoded_tile` / per-tile decode
  call is independent — no decompressed-image cache, no SOT-offset
  cache for skipping the codestream re-walk.

Net effect: at one tile in flight with no DAG and no streaming output,
OpenJPEG's parallelism saturates only when a single tile contains
enough codeblocks to feed the workers, and its peak RSS is bounded
below by the full tile working set. Both constraints fall away in
Grok's design.

---

## 4. Incremental Stripe Compositing

### 4.1 Enablement conditions

The CLI/format-writer path enables incremental writes when **all** of:

1. Output goes to disk (not an in-memory image).
2. The codestream has multiple tiles.
3. Post-processing is a no-op (no ICC conversion, no precision
   scaling, no colour-space change).
4. There is no windowed vertical crop.
5. The output format implements `supportsIncrementalBandWrite()`
   (TIFF, PNM/PAM, raw; PNG/JPEG can also if they support strip
   writing).

When enabled, a band callback is installed on the
`CodeStreamDecompress` instance.

### 4.2 scratchImage_ as a strip buffer

`scratchImage_` is a `GrkImage` whose component planes are sized to
hold one *tile-row* of pixels (i.e. tile-height × image-width per
component). Its `y0` and `h` fields are advanced as each row drains.
Compositing a tile into `scratchImage_` is per-component `memcpy` of
each row of the tile into the correct horizontal offset of the strip
buffer.

### 4.3 Row drain procedure

When `TileCompletion::complete()` detects a row is done, it fires the
row callback (a closure built during `decompressAllTiles()`). The
callback:

1. Inserts the new row into `pendingBands_` (an unordered map keyed
   by tile-row Y, since rows may complete out of order across
   different rows).
2. Drains in-order starting from `nextBandTileY_`:
   a. For each tile in the row, composite it into `scratchImage_`.
   b. Call `ioBandCallback_(yBegin, yEnd, scratchImage_)`. The format
      writer's `writeImageBand` then SIMD-interleaves planar int32 to
      packed uint8/uint16 (via Highway's `StoreInterleaved3` /
      `StoreInterleaved4`) and writes one or more TIFF strips (or
      PNG/PNM/JPEG equivalent) via `TIFFWriteEncodedStrip`.
   c. Release the row's tiles via `tileCache_->releaseForSwath()`
      and call `MemoryManager::releaseFreedPages()`.
   d. Advance `scratchImage_->components[*].{y0, h}` to the next row.
   e. Increment `nextBandTileY_` and `notify_all` on `bandDrainCV_`
      so the parser may schedule more tiles.

### 4.4 Memory bound

For a 40000×40000 8-bit RGB image with 256-pixel tile rows: full
composite is ~4.5 GB; incremental compositing keeps ~2 tile-rows
(~40 MB) resident. The `malloc_trim` call ensures RSS reflects that.

### 4.5 Format writer interleave

For the dominant non-subsampled case, the writer obtains a
`PlanarToInterleaved` interleaver from an `InterleaverFactory`. For
8- and 16-bit precisions it uses Highway's interleaving stores; for
subsampled YCbCr it falls back to a scalar luma-Cb-Cr packer matching
the TIFF YCbCr MCU layout. The interleaver uses an I/O buffer pool
to avoid per-strip allocation.

### 4.6 vs OpenJPEG

OpenJPEG has no equivalent. The CLI front-ends (`opj_decompress`,
the GDAL `JP2OpenJPEG` driver) materialize the full decompressed
image in memory before handing it to the format writer. For an
N-megapixel image with C components at P bits, peak RSS is at
least `N × C × ceil(P/8)` bytes regardless of how the writer would
have preferred to stream. There is no per-row callback hook, no
parser-to-writer back-pressure, and no API surface for
incremental band writes — adding one would require redesigning
the tile-decode boundary.

---

## 5. Wavelet (DWT) Subsystem

### 5.1 The 16-bit DWT path — why it matters

For typical satellite / medical / cultural-heritage imagery the per-
component precision is ≤ 12 bits. The wavelet coefficient buffer is
the largest hot-loop working set in decompression (often
hundreds of MB for a single tile-row of a large image). Performing
the inverse DWT in `int16_t` instead of `int32_t` halves the cache
footprint and the L2/L3 traffic. On modern x86 with AVX2 this
typically improves DWT throughput by 40–80% and improves end-to-end
decompression of 8/12-bit imagery measurably.

The 16-bit path is selected at runtime in `TileProcessor::decompressInit()`
when **all** the following hold:

* `qmfbid == 1` (reversible 5/3) — and a separate 16-bit irreversible
  9/7 path activates for `qmfbid == 0` with sufficiently low precision.
* Whole-tile decode (no ROI / windowed decode).
* Headroom fits: `prec + 4 ≤ 16` for non-MCT components,
  `prec + 5 ≤ 16` for MCT components (the extra bit covers the
  inverse RCT's worst-case 2× gain on chroma channels).

The conservative cap is therefore precision ≤ 12 bits non-MCT,
≤ 11 bits MCT. Implementations may use 5 bits of headroom uniformly
to simplify the dispatch.

### 5.2 Data flow into the 16-bit path

```
T1 decode (always emits int32)
  → NarrowShiftFilter   (int32 → int16; right-shift-by-1 + clamp,
                         applied band-by-band after T1)
  → 16-bit inverse DWT  (lifting kernels operating on int16)
  → DC shift            (fused into the final store)
  → inverse RCT (MCT)   (int16 RCT path; uses overflow-safe averages)
  → composite           (int16 → int32 widening into scratchImage_)
```

The band buffer abstraction carries a `data_type` discriminator
(int16/int32), and downstream stages dispatch on it. The
`NarrowShiftFilter` is the bridge that pays the precision tax
exactly once.

### 5.3 BIBO gain analysis for 5/3 (informal derivation)

The 5/3 inverse DWT is two lifting steps:

* Undo update: `s[n] -= floor((d[n-1] + d[n] + 2) / 4)`
* Undo predict: `d[n] += floor((s[n] + s[n+1]) / 2)`

With subband coefficients bounded by magnitude M:

* The update correction has magnitude ≤ M/2, so `s` after update
  is ≤ 3M/2.
* The predict correction uses already-updated `s` values
  (magnitude ≤ 3M/2), so its magnitude ≤ 3M/2; thus `d` after
  predict is ≤ 5M/2.

Per-level worst-case 1D gains: low 1.5, high 2.0. Multi-level
analysis (recursive on the LL subband) converges geometrically because
the update step has an attenuation factor of 1/4. The asymptotic 2D
overall gain converges to ~2^3.04. For practical purposes:

* ≤ 5 decomposition levels → 2D gain < 8 (3 bits).
* > 5 levels → slightly more than 3 bits.

Choosing 4 bits of headroom (non-MCT) gives a safety margin for
quantization-by-truncation distortion; 5 bits (MCT) covers the
additional inverse RCT gain.

### 5.4 Overflow-safe SIMD averaging — the crux

Scalar C++ promotes `int16_t` to `int` automatically; the lifting
sums `a + b` happen in 32-bit and never overflow. SIMD lanes do **not**
auto-promote — int16 lanes do int16 arithmetic. The 5/3 lifting sums
can overflow the int16 range mid-computation even when the final
result is in range. Two primitives solve this; both are SIMD
identities that compute the correct result without ever holding the
sum in a 16-bit lane.

#### update_avg_16_53 — compute floor((a + b + 2) / 4) without overflow

Algebraic identity:
```
floor((a + b + 2) / 4)  =  (floor((a + b) / 2) + 1) >> 1
```

Implementation using hardware unsigned-rounded-average (e.g.
`_mm256_avg_epu16`, `vrhaddq_u16`), which uses a 17-bit internal
accumulator and so cannot overflow:

```
a_u      := a XOR 0x8000                # signed → unsigned bias
b_biased := (b XOR 0x8000) + 0x7FFF     # subtract 1 in unsigned mod 2^16
avg      := UnsignedRoundedAverage(a_u, b_biased)
                                        # = floor((a_u + b_biased + 1)/2)
                                        # the +1 from the rounded avg
                                        # cancels the -1 from the bias
step     := (int16)(avg - 0x7FFF)       # un-bias to get
                                        # floor((a + b)/2) + 1
result   := step >> 1                   # arithmetic right shift
                                        # → floor((a + b + 2)/4)
```

This is correct for all signed-int16 inputs.

#### predict_avg_16_53 — compute floor((a + b) / 2) without overflow

Bit-decomposition identity:
```
(a + b) >> 1  =  (a >> 1) + (b >> 1) + ((a & b) & 1)
```

Each term is individually safe: `a >> 1` and `b >> 1` are halved
ranges; `(a & b) & 1` is 0 or 1.

Both identities work uniformly across SSE2, AVX2, NEON, and WASM
SIMD since they only require unsigned average and arithmetic shift
intrinsics, which Highway exposes portably.

### 5.5 16-bit fixed-point 9/7 path

The irreversible 9/7 has four lifting steps with non-integer
coefficients. Grok represents them in **Q1.15 signed fixed-point**:
the result of `a * b` (both Q1.15) is `(a*b + 2^14) >> 15`. This
holds the factors |c| < 1 directly. Factors |c| > 1 are decomposed
into an integer-plus-fraction form:

| step / scale | true value | decomposition |
|---|---|---|
| α (predict 1) | −1.586… | x −= (sum + sum·c'), c' = +0.586… in Q1.15 |
| β (update 1)  | −0.053… | multiply each input by 2^3 first, then sum, then >>3 (to recover precision) |
| γ (predict 2) | −0.883… | direct Q1.15 |
| δ (update 2)  | +0.444… | direct Q1.15 |
| K (even gain) | 1.230… | x += x·c', c' = 0.230… |
| 2/K (odd gain)| 1.626… | x += x·c', c' = 0.626… |

The interleave step at the bottom of synthesis carries an optional
right-shift if intermediate magnitudes are near the int16 limit,
balanced by a final left-shift after synthesis. The bookkeeping for
how many bits to downshift is precision-dependent (typically 0 or 1
for ≤ 12-bit inputs). The forward 9/7 mirrors this scheme to allow
encoder-side 16-bit operation.

### 5.6 SIMD dispatch via Highway

The wavelet kernels are written once and compiled multiple times
through Highway's `foreach_target.h` mechanism, which generates per-
target translation units (SSE2, SSSE3, AVX2, AVX-512, NEON, SVE,
WASM SIMD, RISC-V V). At runtime, `HWY_DYNAMIC_DISPATCH` resolves
to the fastest target supported by the CPU. Lane width varies by
target (8 lanes int32 on AVX2, 16 on AVX-512, etc.), so kernels are
expressed in terms of `Lanes(d)` and process column-batches of
`Lanes` or `2 × Lanes` at a time. The "PLL columns" constants
(`PLL_COLS_53` = `2 × Lanes(int32)`, etc.) define the scratch
buffer transposition stride.

### 5.7 Lifting kernel layout

For both 5/3 and 9/7:

* **Horizontal pass** writes into a per-thread scratch buffer in
  **column-major** layout (stride = `PLL_COLS × sizeof(T)`),
  arranging even and odd samples for easy vertical access.
* **Vertical pass** reads the scratch column-wise, applies the
  lifting steps in registers, and stores back. The 9/7 K / 2/K
  normalization is **fused into the horizontal pass's interleave**,
  saving a separate scaling pass.

### 5.8 Per-image wavelet scratch pool

`WaveletPoolData` allocates one scratch buffer pair (horizontal +
vertical) per worker thread, sized to the image's `max(width,
height)`. Allocation happens once at image setup; tiles and
resolutions reuse the same buffers. Thread safety is by partitioning:
worker *i* always uses pool[*i*]. No locks on the hot path.

### 5.9 Partial / ROI DWT

When the decode is windowed (region-of-interest), the
`WaveletReversePartial` path takes over. It uses an
`ISparseCanvas<int32_t>` abstraction to read only the rectangular
region of each subband that intersects the requested window, plus
the spatial halo required by the lifting filter support. The same
lifting kernels are reused; only the interleaver and the bounds
checks differ. Vertical pass width defaults to 4 columns per task
to amortize sparse-read cost.

### 5.10 9/7 cascade-synthesis (optional)

An opt-in (`GRK_CASCADE_DWT=1`) path for irreversible decode divides
each resolution into 64-row horizontal stripes with ±4-row halos
for the 9/7's wider filter support. Stripes process in parallel
across threads; each stripe stays in L3 cache. Halo overlap adds
~12% redundant H-DWT work but eliminates a DRAM round-trip on the
large SPLIT buffer at large image sizes. Output is bit-exact with
the non-cascade path.

### 5.11 Within-tile DWT parallelism

For the standard (non-cascade) 9/7 path the horizontal and vertical
passes are split across `num_workers` row-band or column-band tasks
respectively, with the vertical band depending on horizontal. For
5/3, the kernels are scalar enough that per-thread tile-level
parallelism is the only level.

### 5.12 vs OpenJPEG

OpenJPEG's DWT (in `dwt.c`) is competitive on scalar / SIMD lifting
but lacks the bit-width and pooling innovations:

* **Bit width.** Inverse DWT runs in `int32` exclusively for 5/3 and
  in float-then-store-int32 for 9/7. The scratch buffer
  (`opj_dwt_t::mem`) is always `OPJ_INT32 *`. There is no 16-bit
  reversible path and no Q1.15 fixed-point 9/7. For 8-/12-bit
  imagery this doubles the wavelet-stage cache footprint and DRAM
  traffic relative to Grok.
* **SIMD.** Heavily vectorized. The 5/3 inverse has SSE2, AVX2, and
  AVX-512F variants (`dwt.c:337–586`); the 9/7 inverse vertical pass
  has SSE2 and AVX2 float paths (`opj_idwt97_v` and friends near
  `dwt.c:1107`). Dispatch is compile-time (the build picks targets
  via CMake), not runtime. (Grok uses Highway with `HWY_DYNAMIC_DISPATCH`
  for runtime selection across SSE2 / SSSE3 / AVX2 / AVX-512 / NEON /
  SVE / WASM SIMD / RISC-V V from a single binary.)
* **Per-call allocation.** Each DWT job allocates its scratch via
  `opj_malloc` and frees on job exit (`dwt.c:2022` and the
  encode-side equivalents). There is no per-thread or per-image pool.
* **Windowed DWT.** OpenJPEG does have a partial path:
  `opj_dwt_decode_partial_tile` (`dwt.c:2831`) and `opj_dwt_decode_partial_1`
  (`dwt.c:2495`) backed by a `sparse_array` abstraction, dispatched
  on `p_tcd->whole_tile_decoding`. Functionally analogous to Grok's
  `WaveletReversePartial`; this is one subsystem where OpenJPEG's
  design is close to Grok's.
* **DWT parallelism shape.** Row/column jobs submitted to the generic
  thread pool, one per band per resolution. Comparable to Grok's
  intra-tile DWT task split, but synchronizes via
  `wait_completion` rather than DAG edges, so it cannot overlap with
  other tiles' T1.
* **No cascade synthesis.** OpenJPEG decomposes coarse-to-fine,
  resolution by resolution, with a full H+V pass per resolution and a
  DRAM round-trip on the split buffer between resolutions.

The DWT comparison is more nuanced than the scheduler comparison:
OpenJPEG's SIMD lifting is mature, so the speedup Grok gets here
comes principally from (a) the 16-bit path on the common case and
(b) avoiding per-job allocation, not from raw kernel quality.

---

## 6. Tier-1 Block Coder

### 6.1 Architectural shape

A small abstract interface (`ICoder` with `compress()` and
`decompress()`) is dispatched by a factory based on codestream
parameters:

* **Part-1 (EBCOT MQ)** — Grok's own implementation, descended from
  OpenJPEG but reorganized around the coder pool and around 32-byte-
  aligned, cache-friendly scratch buffers.
* **Part-15 (HTJ2K)** — wraps the embedded **OpenJPH** library
  (https://github.com/aous72/OpenJPH). Grok does not reimplement
  HTJ2K bitplane processing; OpenJPH provides scalar, SSSE3, AVX2,
  and AVX-512 block coder/decoder variants, selected by CPU detection
  at module init.

A block-execution object (`CompressBlockExec` /
`DecompressBlockExec`) carries the per-block metadata (position,
band orientation, quantization, ROI shift) and calls the chosen
coder polymorphically.

### 6.2 SIMD in the MQ coder — deliberate absence

The MQ arithmetic coder is fundamentally sequential: each symbol
updates a tiny state (interval register, code register, bit count),
and the next symbol depends on it. Grok does **not** vectorize MQ
itself. The performance work in Part-1 is elsewhere: the coder pool
removes allocation; the flags array layout (see below) reduces cache
trashing; the LUT layout reduces branchy context selection. This is
the right design call, not a missing feature.

### 6.3 Flags array layout

The bitplane coding passes need a "flags" word per coefficient
recording significance, refinement, and neighborhood propagation
states. Grok packs flags so that four vertically-adjacent
coefficients in the same column share a single flags word at
`flags[1 + col]`, the next four at `flags[1 + flagsStride + col]`,
and so on. This matches the EBCOT scan order (4-tall vertical
stripes within a column) and produces sequential cache-line accesses
during a pass. The stride is sized to avoid 4-KB aliasing on common
codeblock widths.

### 6.4 Context LUTs

The zero-coding and sign-coding context-number lookup tables are
allocated cache-line aligned (64 B) and pre-shifted by subband
orientation before each block (`lut + (orientation << 9)`), avoiding
a per-symbol orientation arithmetic during the hot loop.

### 6.5 Sign-magnitude packing

Coefficients are stored in the working buffer as `int32_t` packed
with the sign bit in the MSB and magnitude in the lower 31 bits:
`packed = (sign ? 0x80000000 : 0) | abs(coeff)`. This avoids branches
during MQ encoding (the sign is just an arithmetic-shift test) and
matches the context-selection logic's expectations.

### 6.6 Coder pooling

(See §3.4.) Worth re-emphasizing that the pool is keyed by codeblock
size in addition to worker, because codestreams may legitimately use
different codeblock sizes in different tile-parts (e.g. for region-
of-interest).

### 6.7 Incremental convex-hull early termination — the encoder PCRD shortcut

This is the most consequential T1 encoder innovation. PCRD-opt picks
truncation points for each codeblock that lie on the convex hull of
the rate-distortion curve at or above a target slope. Stock OpenJPEG
encodes *all* bitplane passes for each block, then runs PCRD-opt
globally to pick truncation points — most of the late passes are
discarded. Grok asks: can we stop encoding once we know further
passes won't be selected?

The algorithm:

1. As each pass is encoded, compute its slope = ΔD/ΔR (decrease in
   distortion divided by increase in rate), expressed in a 16-bit
   log-domain (`slopeToLog(s) ≈ log2(s/2^64) * 256 + 65536`).
2. Maintain an `earlyStopSlope` threshold (initially provided by the
   progressive estimator — see §7.4 — and updated as encoding
   progresses).
3. At each bitplane boundary, scan the recent passes (window of 7)
   and compute the minimum accumulated slope over the window.
4. Compare against an **escalating** threshold: at bitplane *z* the
   threshold is `base * 3^floor((z − z_0)/3)`. The factor of 3 per
   bitplane is empirical and accounts for the typical 4×-per-
   bitplane decay of slopes; the 3-bitplane grouping reduces
   false positives near boundaries where individual passes can
   transiently dip.
5. If the window minimum is above the escalated threshold, stop
   coding the block. The LL band is excluded from early termination
   because its statistics (non-zero-mean DC) make slope prediction
   unreliable.

Empirically this eliminates 5–15% of passes with no observable
quality impact when the threshold is set conservatively.

### 6.8 HTJ2K integration

OpenJPH's coder is stateless (takes input buffer, output buffer,
geometry; produces output). Grok adds:

* A small wrapper that adapts `DecompressBlockExec` / `CompressBlockExec`
  to OpenJPH's function signature.
* Pre-allocated `mem_fixed_allocator` and `mem_elastic_allocator`
  instances per worker (avoiding per-block allocator construction).
* CPU-dispatch (AVX-512 on x86-64, AVX2, SSSE3, scalar) is done
  inside OpenJPH; Grok queries it once at module load.

No Highway is involved in HTJ2K; OpenJPH brings its own intrinsics.

### 6.9 Decode segment buffer multiplexing

The MQ decoder accepts an array of input buffers with per-buffer
lengths rather than a single contiguous buffer. This avoids copying
segments into a monolithic buffer when a block's data spans multiple
tile-parts or precincts, which matters for progressive and
random-access decode.

### 6.10 Cached decode coder

For multi-pass decode (progressive refinement), `DecompressBlockExec`
holds a `cachedCoder_` pointer that survives across decode calls for
the same block, avoiding re-construction of MQ state between passes.

### 6.11 vs OpenJPEG

OpenJPEG's T1 is the ancestor of Grok's and shares the same low-level
data layouts; the differences are above the kernel level:

* **Coder dispatch.** Imperative if/else on `tccp->cblksty &
  J2K_CCP_CBLKSTY_HT` (`t1.c:1691`), calling either
  `opj_t1_ht_decode_cblk` or `opj_t1_decode_cblk` directly. No
  polymorphic `ICoder`. Equivalent in capability, less so in
  extensibility.
* **MQ coder.** Scalar only; `mqc_inl.h` macros (`mqc_inl.h:138–260`).
  Matches Grok — both treat MQ as fundamentally sequential.
* **Flags layout.** Identical 4-row vertical pack with sigma / chi /
  mu / pi bit fields and the `T1_FLAGS(x,y)` indexing
  (`t1.h:73–174`, `t1.c:63`). Grok inherited this directly.
* **Sign-magnitude packing.** Same MSB-sign / 31-bit-magnitude scheme
  (`opj_to_smr`, `opj_smr_abs`, `opj_smr_sign` at `t1.c:69–71`).
  Direct inheritance.
* **HTJ2K.** In-tree (`ht_dec.c`), a self-contained scalar
  implementation with NEON helpers for popcount but no SIMD on the
  MEL / VLC / MAGREF decode loops. (Grok delegates to OpenJPH, which
  has full SSSE3 / AVX2 / AVX-512 variants for both encode and
  decode and dispatches at module init.) For openjp2k this is a
  choice: embed OpenJPH for speed, or port OpenJPEG's `ht_dec.c` for
  simpler dependencies — both are license-compatible with Apache 2.0.
* **Encoder early termination.** None. The encoder loops over all
  bitplanes and all passes (`t1.c:2625–2695`), records
  `(rate, distortion)` per pass, then defers truncation to a
  one-shot post-encode PCRD-opt. This is precisely the work that
  Grok's incremental convex-hull early stop (§6.7) skips, and the
  difference is one of the larger encoder-side performance gaps.
* **Coder pool keying.** TLS (`OPJ_TLS_KEY_T1`) gives one coder per
  worker thread regardless of codeblock size. Grok's pool is keyed
  on `(worker, codeblock_size_exponents)`, allowing the scratch
  buffers to be sized exactly once per size. Significance is small
  unless the codestream mixes block sizes.

---

## 7. Tier-2 (Packet) Subsystem

### 7.1 Marker-driven random access

The Tier-2 layer is where Grok's random-access architecture lives.
Two markers do most of the work:

* **TLM** (tile-part length marker, in the main header) — tells you
  the byte length of every tile part.
* **PLT** (packet length tile-part marker, in tile-part headers) — tells
  you the byte length of every packet within its tile part. PLM
  (packet length main-header marker) is the equivalent at the
  codestream level.

A codestream produced by Grok's encoder (or by `grk_transcode -X -L`)
ships with TLM + PLT. Together they let a decoder compute, *without
parsing entropy*, the byte range of any (component, resolution,
precinct, layer) packet. This is what `SelectiveFetchRanges` does.

### 7.2 SelectiveFetchRanges — what to download

Given:

* The tile geometry (components, resolutions, precincts per
  resolution, layers).
* The progression order (LRCP / RLCP / RPCL / PCRL / CPRL).
* The PLT-decoded packet lengths.
* A target subset (e.g. "only resolutions 0–3" for a thumbnail; or
  "only precincts overlapping window W" for a regional decode).

The algorithm walks packets in progression order, marks the targeted
ones, and returns a coalesced `vector<FetchRange>` of (offset,
length) tuples. A configurable gap-merge threshold lets the planner
trade extra-downloaded bytes for fewer HTTP requests, which is the
right trade-off against AWS S3's per-request latency.

There is also an offline tile-part-header parser
(`extractTilePartHeaderInfo`) that takes raw bytes and extracts SOD
offsets + PLT lengths without instantiating a codec, so cloud
clients can probe headers in Phase 1 before deciding what to fetch
in Phase 2.

### 7.3 Packet iteration optimization

The standard PacketIter handles all five progression orders and
arbitrary precinct grids. For the common case (single progression,
no subsampling, constant resolutions, tile origin at (0,0)), Grok
constructs a `precinctInfoOPT_` cache and dispatches to specialized
`next_rpclOPT`, `next_cprlOPT`, etc. that skip the general-case
grid arithmetic. Overflow protection: precinct projection arithmetic
checks against `UINT32_MAX` before allowing iteration to proceed,
preventing silent truncation in `Rect32::scale` that could loop
infinitely on pathological codestreams.

For windowed decode in spatial progressions (RPCL especially),
per-precinct counts of packets *outside* the window are precomputed
so PLT-driven byte skips can leap over entire precinct columns
without parsing.

### 7.4 Progressive PCRD slope estimator (encoder)

`ProgressiveSlopeEstimator` is the encoder counterpart of T1 early
termination (§6.7). During compression it maintains a histogram of
slopes-seen-so-far across already-encoded blocks (binned by
log-slope, ~16 bins per slope decade) plus running totals of bytes
at each slope. Given the target byte budget, it solves for the
slope at which cumulative bytes meet the target.

Several heuristics keep the estimator conservative (publishes a
threshold below the true PCRD threshold, so no block is wrongly
truncated):

* Early-completing blocks are biased toward high-frequency / high-
  compressibility content — i.e. the early sample over-estimates
  rate at any given slope. Counteract by inflating the target by a
  padding factor `max(4096, totalSamples / 16)`.
* The published threshold is `α * raw`, with α = 0.75 by default,
  giving a 25% safety margin in log-slope space (~106 bins on the
  histogram).
* The threshold is *monotonically non-decreasing* across recomputes
  — once a block has been truncated based on a published threshold,
  later refinement can't invalidate that decision.
* Recompute frequency uses exponential backoff (2, 4, 8, 16 blocks
  between recomputes) to balance responsiveness against mutex
  contention on the histogram update.

The published threshold is read lock-free as an atomic by T1 worker
threads, who use it to drive the early-stop logic in §6.7. The
combined effect is 20–40% reduction in T1 encoder work on
rate-targeted compressions, which is a large fraction of total
encode time at typical bitrates.

### 7.5 PacketCache

Tier-2 decode uses `PacketCache` as a thin wrapper around a
`SparseBuffer` of packet bytes. Two access patterns: dense
(streaming sequential decode) and sparse (after selective-fetch,
some packets may be absent). `gen()` constructs a `PacketParser` for
a packet lazily, avoiding allocation for skipped packets.

`PacketProgressionState` tracks the maximum layer decoded per
resolution, supporting incremental progressive rendering ("we have
enough data to display the first three resolutions; the fourth
will arrive when more packets are read").

### 7.6 Rate control auxiliaries

`RateControl::convexHull()` extracts the convex hull of (rate,
distortion) points for a block. `RateInfo` maintains global min/max
slopes across blocks for PCRD orchestration. Together with
`ProgressiveSlopeEstimator` these are the encoder's
rate-distortion plumbing.

### 7.7 vs OpenJPEG

The T2 layer is where the architectural gap is widest:

* **Packet iteration.** OpenJPEG dispatches the five progression
  orders to dedicated functions (`opj_pi_next_lrcp`, `_rlcp`,
  `_rpcl`, `_pcrl`, `_cprl` in `pi.c:237–619`), but each is a
  general-case nested loop with `include[]`-array state tracking.
  There is no "OPT" fast-path for the common no-subsampling /
  single-progression case. Comparable correctness, fewer
  constant-factor optimizations.
* **TLM markers.** Parsed (`opj_j2k_read_tlm`, `j2k.c:3659`) and
  emitted on encode (`opj_j2k_write_tlm`, `_update_tlm`), but **not
  consulted during decode for random access.** The decoder walks
  tile-part headers sequentially even when TLM is present.
* **PLT markers.** Same picture. Read by `opj_j2k_read_plt`
  (`j2k.c:3857`), optionally written on encode
  (`opj_j2k_write_plt_in_memory`, `j2k.c:4751`), but no byte-range
  computation consumes them at decode time.
* **Random-access decode.** Tile-level only: `opj_get_decoded_tile`
  and `opj_set_decode_area` decode a single tile or window, but
  still require parsing the main header and walking forward through
  any preceding tile-parts. No precinct-level selective fetch.
* **Selective byte-range fetching.** Absent. `opj_packet_info_t`
  records packet positions only as a side effect of full decode or
  encode, never proactively. (Grok's `SelectiveFetchRanges` is the
  whole mechanism that turns TLM+PLT into a fetch plan.)
* **Encoder rate control.** `opj_tcd_rateallocate` (`tcd.c:466`) is
  a one-shot binary search over slope thresholds (up to 128
  iterations, `tcd.c:597`) executed after every codeblock has been
  fully encoded. No progressive estimator, no T1 early stop. This
  is the OpenJPEG-side cost that Grok's §6.7 + §7.4 pair avoids.

---

## 8. Tile and Compressed-Chunk Cache

### 8.1 Tile cache (decompressed-data side)

The TileCache holds, per tile:

* The TileProcessor with its `tilePartSeq_` — a list of (offset,
  length) for every tile-part of that tile, populated when the SOT
  marker is first parsed.
* Optionally the decompressed `GrkImage` payload and a `dirty_` flag.

Cache strategies (a bitmask):

| flag | meaning |
|---|---|
| `GRK_TILE_CACHE_NONE`  | no caching |
| `GRK_TILE_CACHE_IMAGE` | cache decompressed images for reuse |
| `GRK_TILE_CACHE_ALL`   | cache processors + images |
| `GRK_TILE_CACHE_LRU`   | enable LRU eviction + re-decompression |

Lifecycle has three nontrivial paths:

* **Cache hit** — clean entry, return the cached image directly.
* **SOT fast path (non-TLM)** — `decompressFromCachedTileParts()` seeks
  directly to each tile part by cached offset without re-walking the
  codestream. Activates whenever a previously-seen tile is requested
  but its image was never cached (or was evicted).
* **Re-decompress after eviction** — `reinitForReDecompress()` rebuilds
  the internal `Tile` structure and re-runs T2+T1. Compressed data
  is served from the compressed-chunk cache (next section), so no
  network re-fetch.

`releaseForSwath()` is the swath-consumer's explicit free: drop both
decompressed image and processor for the tile, since the consumer
guarantees no further need.

### 8.2 Compressed-chunk cache (compressed-data side)

Per-file LRU cache holding compressed tile-part data after it has
been fetched (from disk or network). Two purposes:

* Avoid network re-fetches across decompress / evict / re-decompress
  cycles.
* Cap total memory at a user-configurable budget. Default 256 MB,
  resolved from `GRK_CACHEMAX` (falls back to `GDAL_CACHEMAX`).
  Suffixes M / MB / G / GB are parsed.

When the budget is exceeded, the LRU tile's compressed buffers spill
to a temporary disk cache (`DiskCache`), serialized as
`[uint16 numParts][offsets][lengths][data]`. Reload reconstructs the
buffers and the `MemStream` wrappers around them. The disk cache
is process-local and removed on shutdown.

Reads from the chunk cache are lock-free (shared_ptr to the
`TPFetchSeq`); only spill/reload paths take the mutex.

### 8.3 Network I/O layer

The fetchers (`S3Fetcher`, `GSFetcher`, `AZFetcher`, `ADLSFetcher`,
`HTTPFetcher`) sit underneath the compressed-chunk cache and feed
it. Common machinery in `CurlFetcher`:

* `curl_multi` handle with `CURLMOPT_MAX_TOTAL_CONNECTIONS = 100`
  and HTTP/2 multiplexing (`CURLPIPE_MULTIPLEX`).
* A background worker thread processing the multi handle in a
  producer/consumer pattern.
* Retry policy: default 3 retries with 1 s back-off.

Range requests for selective fetch (§7.2) are pipelined through
the multi handle. Connection reuse across many tile requests is
where the cloud-decode speedups come from — a fresh TLS handshake
per range request would be catastrophic.

AWS S3 credentials resolve through a chain matching GDAL's order:
`AWS_NO_SIGN_REQUEST`, env vars, cached STS credentials, AWS
config files (web identity, assume-role, SSO, credential_process),
ECS container credentials, EC2 instance metadata. Signing is via
libcurl's `CURLOPT_AWS_SIGV4`.

### 8.4 vs OpenJPEG

This subsystem is essentially Grok-only:

* **Decompressed-tile cache.** None. Every
  `opj_decode_tile_data` / `opj_get_decoded_tile` call decompresses
  the tile from scratch; if the consumer asks for the same tile
  twice, it pays twice. No SOT-offset cache for fast-path seeking.
* **Compressed-data cache.** None. No memory budget, no LRU, no
  disk spill. Codestream bytes are pulled from the input stream on
  demand and dropped after parsing.
* **Network I/O.** None in-tree. The I/O abstraction
  (`opj_stream_*` in `cio.{c,h}`) is a callback interface over
  read / write / seek and ships with a `FILE *`-backed default.
  Remote access requires the application to supply its own HTTP
  client behind the stream callbacks — no S3 signing, no
  range-request planning, no connection pool, no HTTP/2.
* **Index boxes.** OpenJPEG has CIDX / PPIX / THIX / TPIX box
  managers (`cidx_manager.{c,h}` and friends) for the
  ISO/IEC 15444-1 codestream index format. These describe
  JP2-file-level packet positions but are not used as a
  decode-time seek index — they are emitted, not consumed.

For a cloud-aware reimplementation in openjp2k, the entire cache
+ network layer would be new construction; there is no
OpenJPEG starting point to fork here.

---

## 9. Codestream-Level Innovations

### 9.1 grk_transcode — packet-level rewriting

A separate CLI tool, `grk_transcode`, rewrites JP2 / J2K files at
the packet level without full decompression. Useful operations:

* Insert TLM and PLT markers into a file that lacks them (turning a
  sequential codestream into a random-access-friendly one).
* Inject SOP / EPH packet delimiters.
* Truncate quality layers (`--max-layers`).
* Strip resolution levels (`--max-res`).
* Reorder packet progression (any of LRCP / RLCP / RPCL / PCRL /
  CPRL).

For openjp2k this is an attractive standalone tool to build: it's
independent of the hot encode/decode paths and is operationally
useful for preparing JP2 imagery for cloud-friendly delivery.

### 9.2 Multi-level cache strategy as user-facing API

The user controls the tile-cache behavior via a bitmask in the
decompress parameters and a `max_active_tiles` integer. This is
worth lifting into openjp2k's public API because the right setting
depends on the consumer's access pattern (GDAL random-window decode
wants `IMAGE | LRU` with `max_active_tiles` ≈ working-set size; a
single-pass thumbnail wants `NONE`).

---

## 10. Reimplementation Guidance for openjp2k

### 10.1 Suggested order

Independent layers that openjp2k can pick up incrementally, ranked
by performance-impact-per-effort:

1. **MemoryManager + page release.** Trivial code, but without it
   every other memory optimization is invisible to RSS.
2. **Coder pool keyed by (worker, codeblock size).** Removes the
   biggest single source of allocation in the hot path.
3. **Per-worker wavelet scratch pool.** Same shape as the coder
   pool, applied to DWT scratch.
4. **Taskflow-based per-tile DAG scheduler** with cross-tile
   interleaving. The biggest architectural change; depends on
   adopting a DAG library (Taskflow is a single-header C++17
   library, MIT-licensed — directly usable from Apache 2.0).
5. **Incremental stripe compositing** with back-pressure. Requires
   (4); transformative for memory bounds on large images.
6. **TileCompletion** (per-row counters + contiguous-prefix heap).
   Glue between (4) and (5).
7. **TLM/PLT-driven SelectiveFetchRanges.** Independent of the
   threading work; transformative for cloud workloads.
8. **Compressed-chunk cache + disk spill.** Pairs with (7).
9. **16-bit 5/3 DWT path + overflow-safe SIMD averages.** Use
   Highway for SIMD dispatch (Apache 2.0). Substantial DWT speedup
   on common precisions.
10. **Highway-dispatched 9/7 lifting kernels.** Forward and inverse.
11. **16-bit 9/7 Q1.15 path** (more involved than the 5/3 case;
    consider deferring).
12. **Per-image network fetchers** built on libcurl multi + HTTP/2.
    Independent of everything else.
13. **HTJ2K (Part-15) via OpenJPH.** OpenJPH is BSD-2-Clause, fully
    compatible with Apache 2.0; embed it.
14. **Progressive PCRD slope estimator + T1 early stop.** Encoder-
    only; pays off on rate-targeted compression.
15. **9/7 cascade-synthesis path.** Marginal; opt-in only; defer.

### 10.2 Library dependencies, license-checked

| dependency | purpose | license | compatible with Apache 2.0 |
|---|---|---|---|
| Taskflow | task-graph executor | MIT | yes |
| Highway (Google) | portable SIMD | Apache 2.0 | yes |
| OpenJPH | HTJ2K block coder | BSD-2-Clause | yes |
| libcurl | network I/O | MIT-style | yes |

None of Grok's *own* code needs to be reused. The third-party
dependencies that Grok pulls in are independently obtainable and
compatibly licensed.

### 10.3 Things to verify in implementation

* Exact `slopeToLog` / `slopeFromLog` formulas (§6.7) — verify in
  reproduction by checking that an openjp2k encoder and a Grok
  decoder produce bit-identical decoded images for the same
  threshold.
* BIBO headroom (§5.3): test with synthetic content at maximum
  precision boundary (12-bit non-MCT, 11-bit MCT).
* Back-pressure window of 2 rows (§3.6): the constant 2 is the
  minimum that allows useful pipelining; openjp2k may want it
  exposed as a parameter for tuning.
* Tile-cache fast-path correctness (§8.1): regression-test SOT
  fast-path against full re-walk on a codestream without TLM.

### 10.4 Test corpus

Useful public corpora for benchmarking against Grok's published
numbers:

* **SPOT 6/7 sample imagery** (Airbus): the "Spot 6" rows in Grok's
  benchmark table.
* **Pleiades sample imagery** (Airbus): the "Pleiades" row.
* The **OpenJPEG test conformance set** for correctness.
* Synthetic 40000×40000 8-bit RGB for streaming-mode memory
  validation.

---

## 11. What This Document Does Not Cover

By scope:

* **HTJ2K (Part-15) internals.** Grok integrates OpenJPH; this
  document describes the integration shape, not HTJ2K block coding.
  openjp2k should embed OpenJPH directly rather than reimplement.
* **File-format (JP2/JPH box) parsing details.** Performance-neutral.
* **Plugin layer / GPU offload.** The `plugin/` subdirectory exists
  but appears not to be a published feature.

By license caution:

* **No verbatim code, constants beyond those that are mathematical
  facts (the 9/7 lifting coefficients α/β/γ/δ are defined by the
  standard), or copy-able structure definitions.**

By describer judgment:

* **The Excalibur scheduler backend.** Looks experimental; the
  standard backend is sufficient for openjp2k.
* **The Freebyrd scheduler stub.** Removed/unreachable.

---

## 12. References

* Grok source repository (read by describer, not by implementers):
  https://github.com/GrokImageCompression/grok
* OpenJPH (HTJ2K library, BSD-2-Clause):
  https://github.com/aous72/OpenJPH
* Taskflow (DAG executor, MIT):
  https://github.com/taskflow/taskflow
* Google Highway (portable SIMD, Apache 2.0):
  https://github.com/google/highway
* Taubman & Marcellin, *JPEG 2000: Image Compression Fundamentals,
  Standards and Practice* (Springer, 2002) — canonical reference
  for EBCOT, PCRD-opt, lifting wavelets.
* ITU-T T.800 / ISO/IEC 15444-1 (JPEG 2000 Part 1)
* ITU-T T.814 / ISO/IEC 15444-15 (HTJ2K, Part 15)
