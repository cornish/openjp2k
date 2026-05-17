# Grok JPEG 2000 — Encoder Deep Dive

**A cleanroom companion to `grok-performance-innovations.md` for openjp2k**

---

## Cleanroom Statement

Same convention as the main spec: this document is produced by a
describer who has read Grok's AGPL source; it describes Grok's
encoder paths in prose. No Grok code is transcribed. Implementers
of openjp2k may read this document freely; they should not read
Grok source.

The main spec doc covers the load-bearing encoder-specific
innovations (incremental convex-hull early termination at T1,
progressive PCRD slope estimator at T2). This document fills in
the surrounding encoder architecture so an openjp2k implementer
can build the whole encode path, not just those two optimizations.

The corresponding decoder description is in
`grok-performance-innovations.md` §§3–8; section numbers below
cross-reference where useful.

---

## 1. Encoder Pipeline Overview

A single JPEG 2000 encode of one tile passes through these stages
in order. The reference order is from the standard; Grok's
implementation pipelines and parallelizes within this order using a
Taskflow DAG analogous to the decoder's (main spec §3).

```
  user image (per-component sample arrays)
    │
    ▼
  DC level shift                  (per component, integer subtract)
    │
    ▼
  MCT (forward)                   (RGB → YCC; optional; reversible or irreversible)
    │
    ▼
  Forward DWT                     (per component, L decomposition levels)
    │
    ▼
  Quantization                    (per subband, scalar uniform quantizer)
    │
    ▼
  Tier-1 block encoding (EBCOT)   (per codeblock, MQ arithmetic coder)
    │   ↕ ProgressiveSlopeEstimator (publishes early-stop threshold)
    ▼
  PCRD-opt rate allocation        (per tile, bisection over slope threshold)
    │
    ▼
  Tier-2 packet emission          (precincts × layers in progression order)
    │
    ▼
  Marker emission                 (SOT, SOD, optionally PLT; TLM updated incrementally)
    │
    ▼
  Tile-part bytes written to stream
```

Across tiles, Grok submits each tile's per-tile DAG to the global
Taskflow executor concurrently. Tile-parts are written to the
output stream in tile-index order, serially (the codestream
structure requires it), even though their internal computation
finishes out of order.

---

## 2. Top-Level Compress Orchestration

`TileProcessorCompress` is the per-tile orchestrator. Per tile:

1. **`preCompressTile`** — allocate tile-component window buffers.
   When the codestream is single-tile, the encoder can attach the
   user image's component data directly (zero-copy); otherwise it
   copies into per-tile working buffers. Decide whether the 16-bit
   forward DWT path applies (analogous to the inverse-side decision
   in main spec §5.1).

2. **`buildCompressDAG`** — construct the per-tile Taskflow graph
   with the stage chain: DC-shift / MCT → forward DWT (per level
   per component) → T1 (per codeblock) → rate allocation.

3. **`submitCompressDAG`** — submit the DAG to the global Taskflow
   executor. Multiple tiles' DAGs run concurrently.

4. **`writeTileParts`** (serialized across tiles after all DAGs
   complete) — emit SOT, packet headers and bodies via T2Compress,
   update the in-header TLM entry for this tile-part.

The CodeStream-level loop (`CodeStreamCompress.cpp`) submits all
tiles' DAGs first, collects their futures, then waits as a group.
This avoids any nested executor.run().wait() pattern, which would
deadlock with a finite worker pool.

---

## 3. DC Level Shift

For unsigned components, the standard requires subtracting
`2^(prec-1)` from every sample before transform (and adding it
back during decode). For signed components the shift is zero.

Grok fuses this subtraction into the first lifting step of the
forward DWT for non-MCT components, eliminating a separate pass.
For MCT components, the shift happens inside the MCT step itself
because the MCT operates on signed values.

This is the encoder counterpart of the decoder's DC-shift-fused-
into-final-DWT-store optimization (main spec §3.3).

---

## 4. MCT (Forward Multi-Component Transform)

JPEG 2000 defines two MCTs:

* **MCT = 1 (reversible RCT)**: `Y = (R + 2G + B) >> 2`,
  `Cb = B - G`, `Cr = R - G`. Lossless and integer-only. Used
  with the 5/3 reversible DWT.
* **MCT = 2 (irreversible ICT)**: standard YCbCr 601 matrix in
  floating-point. Used with the 9/7 irreversible DWT.
* **MCT = 0**: no MCT; components transform independently.

A custom MCT (user-supplied float matrix) is part of Part-2 but
appears not to be exercised in Grok's current code — the dispatch
exists but the path is disabled.

MCT runs as a single FlowComponent in the per-tile DAG, with
parallel tasks per row band. For an RGB image with MCT=1, the
encoder writes three transformed output components from three
input components in a single fused pass per pixel row.

---

## 5. Forward DWT

### 5.1 Kernel structure

Forward 5/3 and forward 9/7 are the inverse of the synthesis
kernels described in main spec §5. They run per component, per
decomposition level, in two passes: vertical first, then
horizontal. (Decoder is horizontal then vertical; encoder is the
mirror.) Each level produces four subbands (LL, LH, HL, HH) from
its input; the LL band is fed into the next level.

### 5.2 SIMD coverage

Highway-based SIMD lifting kernels exist for both 5/3 and 9/7
forward paths, dispatched dynamically via `HWY_DYNAMIC_DISPATCH`
the same way as the inverse path. Gather/scatter operations
deinterleave the lifting output back into spatially-arranged
subbands.

### 5.3 16-bit forward path

A 16-bit forward path mirrors the 16-bit inverse:

* Eligible when the same precision-and-headroom conditions hold
  (main spec §5.1).
* Reversible 5/3 case uses `int16_t` lifting end-to-end.
* Irreversible 9/7 case uses Q1.15 fixed-point lifting with the
  same coefficient decompositions as the inverse (main spec §5.5):
  α and β stay in the multiply-after / add-after-decomposition
  form; K and 2/K become `x + Q1.15(x, c-1)`.

The `intInput` flag is the encoder-side equivalent of the
`is16BitDwt` discriminator the decoder uses. For non-MCT
irreversible cases the first DWT level converts int32 inputs to
float (or to Q1.15 int16) before transform; for MCT cases the
MCT has already converted to the target type and the DWT
proceeds without conversion.

### 5.4 Scheduling shape

Forward DWT tasks for a level are scheduled in
`(vertical, horizontal)` FlowComponent pairs. Across levels the
dependencies are explicit: level N's horizontal completion
unblocks level N+1's vertical start. Within a level, the
vertical pass is split across worker threads by row band; the
horizontal pass is split by column band.

### 5.5 No forward-only innovation that isn't mirrored

The forward path does not introduce optimizations beyond what
the inverse path has — no cascade-synthesis equivalent, no
forward-only fixed-point tricks. This is the natural place to
keep the two sides symmetric.

---

## 6. Quantization

### 6.1 Scalar quantizer

Each subband has a step size `Δ_b` (depending on bit-depth,
subband orientation, and decomposition level). Encoder quantizes
coefficient `c` to `q = sign(c) * floor(|c| / Δ_b)`. Grok stores
both exponent and mantissa of `Δ_b` in the QCD/QCC marker:

* **Reversible (5/3)**: step size is `2^gain` where gain depends on
  orientation (0 for LL, 1 for LH/HL, 2 for HH). Only the exponent
  is stored; mantissa is implicit zero.
* **Irreversible (9/7)**: step size is `2^gain / norm[level, orient]`
  where `norm` comes from precomputed per-(level, orient) tables
  matching the 9/7 BIBO synthesis gain. Stored as `(exponent,
  mantissa)` pair in 5+11 bits.

The derivation is standard JPEG 2000; Grok does not appear to
deviate from Part-1 here.

### 6.2 HTJ2K quantization

For Part-15 HTJ2K, Grok delegates step-size derivation to OpenJPH's
quantizer rules via the `QuantizerOJPH` subclass. The semantics
differ in details (HTJ2K block geometry, sign-magnitude
representation), but the externally-stored QCD marker format is
the same.

### 6.3 No novel quantizer design

There is no perceptual quantizer, no rate-allocated per-band
quantizer, no contrast-sensitivity-function weighting. Quantization
is uniformly scalar per subband. Rate is shaped entirely by the
PCRD-opt selection of bitplane truncation points (§§7–8).

---

## 7. Tier-1 (Block) Encoder

### 7.1 Per-block encode loop

For each non-empty codeblock:

1. Determine the **zero-bitplane count** (ZBP) — the number of
   leading magnitude bitplanes that are entirely zero. The
   encoder skips these and records ZBP in the packet header
   (§9.2).
2. Iterate bitplanes from MSB-down. For each bitplane code three
   passes in order:
   * **Significance Propagation Pass (SPP)** — codes newly
     significant coefficients in neighborhoods of already-
     significant ones, plus their sign bits.
   * **Magnitude Refinement Pass (RP)** — codes the next-MSB of
     already-significant coefficients.
   * **Cleanup Pass (CP)** — codes any remaining coefficients
     becoming significant at this bitplane.
3. The first pass of the block is always a Cleanup Pass (no
   coefficient is significant yet).
4. After each pass, record:
   * **Pass length** (cumulative MQ output bytes after this pass).
   * **Distortion decrease** = MSE drop achieved by including this
     pass (subband-weighted, computed via per-(orient, bitplane)
     lookup tables in `BlockCoderEnc`).
   * **Slope** = `distortion_decrease / pass_length_delta`,
     stored as a 16-bit log-domain value (main spec §6.7,
     `slopeToLog`).

### 7.2 ProgressiveSlopeEstimator interaction

(Recap from main spec §7.4 — this is where it bites the encoder.)

As blocks complete, each contributes its (pass slopes, byte
counts) to a shared histogram. The estimator publishes a
`currentThreshold_` as an atomic. When the next block starts
T1, it reads the atomic and passes it to the block coder as
`earlyStopSlope`. The block coder then short-circuits its
bitplane loop (main spec §6.7) once the recent-window minimum
slope rises above the escalated threshold.

For the first ~16 blocks of a tile, no threshold is yet
published; those blocks encode all bitplanes. The threshold
stabilizes quickly thereafter.

### 7.3 Block-coder dispatch

Same as decoder (main spec §6.1): an `ICoder` abstract base with
`compress()` overload, dispatched per block by `CoderFactory`
based on whether HT mode is selected.

For Part-1 the compress entry pre-packs coefficients into
sign-magnitude `int32` (sign in MSB, magnitude in low 31 bits)
before invoking the MQ-driven pass loop.

For Part-15 / HTJ2K, the compress entry adapts the
`CompressBlockExec` to OpenJPH's `ojph_encode_codeblock32` or
`ojph_encode_codeblock64` interface. OpenJPH provides scalar,
AVX2, and AVX-512 encoder variants, dispatched at module init.

### 7.4 Distortion metric

Distortion is sum-of-squared-error of the coefficients
discarded by truncation, weighted by per-(orientation, level)
norm factors so that subband errors are commensurate. The
weight tables are precomputed during codec initialization and
shared across blocks. The exact form is standard JPEG 2000
PCRD-opt; nothing Grok-specific here.

### 7.5 Parallelism

The T1 stage is parallel across codeblocks. The encoder's
worker loop atomically pops blocks from a shared counter (one
per Taskflow worker), so the block-stealing pattern is the
same as decoder T1 (main spec §3.4). The coder pool (main spec
§3.4) supplies a per-(worker × codeblock-size) pre-allocated
coder instance.

---

## 8. PCRD-opt Rate Allocation

### 8.1 What it does

Each codeblock now has a sequence of (cumulative rate,
cumulative distortion) points, one per coded pass. PCRD-opt
picks, for each block and each quality layer, the truncation
point (number of passes to include) that — across the whole
tile — minimizes total distortion subject to a total rate
budget.

The classical algorithm (Taubman 2000) shows that the optimum
is achieved by choosing a single global slope threshold λ and,
per block, picking the latest pass whose slope ≥ λ. Different
target rates correspond to different λ values. The encoder
finds the λ that hits the target rate by bisection.

### 8.2 Two algorithms

Grok exposes two algorithm variants via the
`rate_control_algorithm` parameter:

* **`GRK_RATE_CONTROL_BISECT`** — `pcrdBisectSimple`. Considers
  every coded pass as a candidate truncation point. Simple.
* **`GRK_RATE_CONTROL_PCRD_OPT`** (default) —
  `pcrdBisectFeasible`. First computes the **convex hull** of
  each block's (rate, distortion) curve, discarding passes that
  cannot lie on the global optimum, then bisects over the hull.
  Smaller candidate set, fewer evaluations per bisection step.

### 8.3 Bisection loop

For each output layer (a layer is a rate-or-distortion target):

1. `lowerBound = min_slope_across_blocks`,
   `upperBound = max representable slope (65535)`.
2. Repeat up to 128 times:
   a. `mid = (lowerBound + upperBound) / 2`.
   b. For each block, walk its convex-hull passes and select the
      latest pass with slope > mid. Record selected passes per
      block.
   c. Run a fast packet simulation (`compressPacketsSimulate`)
      that builds packet headers and computes total bytes
      without writing to the output stream. This is the actual
      rate that this λ produces.
   d. If rate-targeted: compare total bytes to target byte
      budget. Adjust bounds. If distortion-targeted: compare
      cumulative distortion to target.
3. After 128 iterations or convergence, commit the chosen
   per-block truncations.

### 8.4 Parallel layer construction

For tiles with ≥256 codeblocks and >1 worker, the per-layer
"pick passes given threshold" pass is parallelized via
Taskflow: each worker handles a slice of blocks, accumulating
its own (bytes, distortion) partial sums, then a reduction.
For smaller tiles the serial path is faster (Taskflow overhead).

### 8.5 Multi-layer encoding

Quality layers are cumulative — layer L's bitstream is a strict
prefix of layer L+1's. PCRD chooses layer thresholds in
decreasing-slope order: tightest layer first (highest
threshold, fewest passes), then progressively more relaxed
layers add more passes.

The cumulative-distortion array tracks per-layer running totals
across blocks, used both for distortion-targeted layers and for
reporting.

### 8.6 Profile constraints

For Cinema and IMF profiles the standard mandates per-component
max-codestream-size constraints. Grok's PCRD-opt has a
`max_comp_size` parameter that constrains per-component byte
count in addition to the per-tile or per-layer budgets; the
bisection respects whichever constraint binds.

---

## 9. Tier-2 (Packet) Emission

### 9.1 Packet structure

A packet is the concatenation of compressed bitstream fragments
from all codeblocks in one (component, resolution, precinct,
layer) tuple. A packet has:

* Optional **SOP** (Start of Packet) marker: 6 bytes, signals
  packet boundary and carries packet-sequence number. Enabled
  by the `write_sop` parameter.
* **Packet header** (bit-packed): includes-this-packet flags,
  zero-bit-plane info, pass-count-this-layer info, and length
  info for each codeblock in each subband.
* Optional **EPH** (End of Packet Header) marker: 2 bytes,
  separates header from body. Enabled by `write_eph`.
* **Packet body**: concatenated MQ output bytes from each
  selected codeblock pass for this layer.

### 9.2 Tag trees

JPEG 2000 packet headers use **tag trees** — value-coded as
unary differences from a parent-node minimum — for two pieces of
information per codeblock per packet:

* **Inclusion tag tree** — encodes the layer at which a block
  is first included in any packet of its precinct.
* **Zero-bitplane tag tree** — encodes the number of leading
  zero bitplanes per block.

Tag trees are a per-precinct, per-resolution data structure;
each packet header walks them and emits the differential
codes. Grok's implementation is straightforward — this is one
of the few areas with little room for novel optimization
because the standard pins the bit-level format.

### 9.3 IncludeTracker — lazy chunked allocation

A non-trivial optimization: the `IncludeTracker` (which records
which blocks are included in which layers across the whole
tile) is potentially huge for tiles with many small precincts.
Grok allocates the tracker in chunks, lazily, only for
(resolution × component × precinct) triples that are actually
visited. For typical tiles this avoids materializing the
full-tile-of-blocks worst-case allocation.

### 9.4 Packet iteration

The encoder walks packets in the configured progression order
(LRCP / RLCP / RPCL / PCRL / CPRL). The `PacketManager`
maintains a `PacketIter` per active progression range (POC
markers can split a tile into segments with different
progression orders). Each step yields the next
(component, resolution, precinct, layer) tuple; the encoder
forms the corresponding packet, writes it to the tile-part
buffer, and advances.

### 9.5 PLT marker emission

If `write_plt` is enabled, the encoder records packet lengths
into a PLT marker emitted at the start of the tile-part. PLT
encoding is variable-byte-length (1 byte per packet typical;
up to 5 for very large packets). PLT is the data structure that
makes the *decoder*'s `SelectiveFetchRanges` (main spec §7.2)
possible, so emitting PLT during encode is the user-facing way
to make a JP2 file cloud-friendly.

### 9.6 Tile-part boundaries

A tile may be split into multiple tile-parts. The encoder splits
when either:

* The user sets `enableTilePartGeneration_` and POC marker
  transitions produce natural boundaries (one tile-part per POC
  segment).
* Maximum tile-part size constraints from the chosen profile
  (e.g. Cinema) demand it.

Each tile-part gets its own SOT marker, optional PLT, then SOD
(start-of-data) marker, then packet data.

---

## 10. Marker Emission

### 10.1 Main-header markers

Written once at the start of the codestream in this order:

* **SOC** — Start of Codestream (2 bytes).
* **CAP** — Capabilities marker (optional, indicates Part-2 or
  extension usage).
* **SIZ** — Image and tile size: reference grid, components,
  per-component precision and subsampling.
* **COD** — Default coding style: progression order, layer count,
  MCT flag, DWT levels, codeblock size, codeblock style bits.
* **QCD** — Default quantization: step-size table for each subband.
* **COC** — Per-component coding-style override (only if any
  component diverges from COD).
* **QCC** — Per-component quantization override.
* **POC** — Progression-order change (if multiple progression
  segments).
* **COM** — Comment (optional user data; supports text and binary).
* **TLM** — Tile-part length: emitted as a fixed-size placeholder
  to be backfilled incrementally as tile-parts complete.
* **CRG** — Component registration (subsampled component offsets,
  if any).
* **RGN** — Region-of-interest marker (if any component has
  `roi_shift` set).
* **PLM** — Packet length main header (alternative to per-tile-part
  PLT; rarely used).

### 10.2 Per-tile-part markers

Around each tile-part's packet data:

* **SOT** — Start of Tile-part: tile index, length (filled in
  after packets are encoded), tile-part index, tile-part count.
* **PLT** — Packet length tile-part (optional).
* **PPT** — Packed packet headers tile-part (a Part-2 feature;
  not common).
* **SOD** — Start of Data: 2-byte marker that signals end of
  tile-part headers and start of packet stream.
* (packet data follows SOD; no markers within packet data
  unless SOP/EPH are enabled.)

### 10.3 Final marker

* **EOC** — End of Codestream (2 bytes) terminates the file.

### 10.4 TLM backfill

TLM is interesting because the tile-part length is unknown
until the tile-part is fully written. The encoder reserves a
fixed-size TLM marker in the main header at the position
corresponding to the next-to-be-written tile-part, then
backfills the length field once the tile-part's bytes have been
counted. This requires a seekable output stream.

If the user-supplied stream is non-seekable (write-only pipe),
the encoder cannot emit TLM; in that case the decoder loses the
TLM-based random-access fast path, but the codestream is still
valid.

---

## 11. Multi-threading on the Encoder

Encode parallelizes at the same four granularities as decode
(main spec §3): tile, component, resolution, codeblock. Tile
parallelism through the global Taskflow executor; component and
resolution parallelism through per-component-per-level FlowComponents
in the per-tile DAG; codeblock parallelism through the work-stealing
loop in CompressScheduler.

Synchronization points:

* DAG edges replace explicit barriers within a tile.
* `tf::Future::wait()` at end of "all tiles' compress DAGs phase"
  before tile-part write phase begins.
* Tile-part write is serial across tiles (preserves codestream
  order; output stream is single-writer).

The same scheduling-shape choices as decode apply: avoid nested
`run().wait()`, prefer one DAG per tile submitted to a shared
executor, collect futures and await as a group.

---

## 12. HTJ2K (Part-15) Encode

HTJ2K encode is enabled by setting `cblk_sty` to include
`GRK_CBLKSTY_HT_ONLY` (or `_MIXED` for mixed Part-1/Part-15
tiles).

When HT mode is active:

* The RSIZ field in the SIZ marker gets the JPH profile bits.
* The `Quantizer` is replaced by `QuantizerOJPH`.
* The `CoderFactory` returns OpenJPH-wrapping coders.
* **Multi-layer rate control is disabled** — HTJ2K is
  fundamentally single-layer at the block-coder level, and Grok
  bypasses the layer-bisection loop for HT tiles.
* PLT emission still works; SOP/EPH still work.

OpenJPH's encoder has scalar, AVX2, and AVX-512 variants
dispatched at module init. For openjp2k, the cleanest path is
to embed OpenJPH directly (BSD-2-Clause, fully Apache-compatible)
rather than re-implementing the HT block coder.

---

## 13. Encoder-Specific Performance Innovations (Concentrated)

For convenience, a single list of encoder-side performance items
beyond OpenJPEG (the main spec covers each in context):

1. **Taskflow per-tile DAG** for compress (same shape as decode).
2. **Cross-tile encode parallelism** through one shared executor.
3. **DC-shift fusion** into first forward DWT lifting step.
4. **16-bit forward DWT path** for reversible and irreversible
   (mirrors decoder; main spec §5).
5. **Coder pool** keyed on (worker × codeblock size) for encode T1
   (main spec §3.4).
6. **Wavelet scratch pool** per worker per image (main spec §5.8).
7. **ProgressiveSlopeEstimator** publishing T1 early-stop threshold
   (main spec §7.4) — typically 20–40% T1 work reduction on
   rate-targeted compressions.
8. **Incremental convex-hull early termination** at T1 pass level
   (main spec §6.7).
9. **Convex-hull pre-filter** before PCRD bisection
   (`pcrdBisectFeasible`).
10. **Parallel per-layer construction** in PCRD when ≥256 blocks.
11. **IncludeTracker lazy chunked allocation** for tag-tree state.
12. **Incremental TLM backfill** so TLM is emittable on streams
    that support seeks.
13. **Optional PLT emission** to enable downstream cloud random-
    access via `SelectiveFetchRanges` (main spec §7.2).
14. **HTJ2K via OpenJPH** with its native SIMD encoder dispatch.

---

## 14. vs OpenJPEG Encoder

The decoder comparison (main spec §3.8, §4.6, §5.12, §6.11, §7.7,
§8.4) substantially carries over to the encoder side, since
OpenJPEG's encoder uses the same generic thread-pool, no DAG, no
cross-tile parallelism, and no memory pooling. The encoder-specific
deltas to call out:

* **No early termination.** `t1.c:2625–2695` encodes every pass of
  every bitplane of every block. PCRD-opt runs as a one-shot
  post-encode binary search (`opj_tcd_rateallocate`, `tcd.c:466`,
  up to 128 iterations). This is the work that Grok's combined
  §6.7 + §7.4 pair eliminates — the single largest encoder-side
  speedup.

* **No 16-bit forward DWT.** OpenJPEG forward DWT is int32 for 5/3
  and float-then-int32 for 9/7, just like its inverse path.

* **No parallel PCRD.** The bisection inner loop is serial in
  OpenJPEG; Grok's threshold-application step is parallelized
  per-block at ≥256-block tiles.

* **TLM/PLT writing.** OpenJPEG does write TLM and PLT (it's the
  *decode-time consumption* that's missing on the OpenJPEG side,
  main spec §7.7), so an openjp2k built atop OpenJPEG's encoder
  inherits the marker-emission code straightforwardly. The
  decode-time random-access machinery is greenfield work either
  way.

* **No HTJ2K encoder.** OpenJPEG only decodes HTJ2K (`ht_dec.c`);
  there is no `ht_enc.c`. Embed OpenJPH for both directions in
  openjp2k.

---

## 15. Reimplementation Notes

For openjp2k building this encoder side:

1. The decoder-side prerequisites (Taskflow integration, coder pool,
   wavelet pool, Memory Manager) are shared with encode — build
   them once.
2. Forward DWT is a near-mirror of the inverse; share kernel
   infrastructure (lifting templates, transposition layout) between
   directions.
3. The MQ encoder, like the MQ decoder, is sequential — don't try
   to vectorize it. SIMD effort belongs in DWT and packing.
4. Start with `GRK_RATE_CONTROL_BISECT` (simpler), add the
   convex-hull pre-filter after correctness is established.
5. Add `ProgressiveSlopeEstimator` + T1 early termination as a
   single pair after the baseline encoder works; both depend on
   the same slope plumbing.
6. PLT emission is independent of everything else — implement
   early so codestreams are immediately cloud-friendly.
7. TLM backfill needs a seekable output stream contract; expose
   that in the public stream API up front.
8. Embed OpenJPH for HTJ2K from day one; don't write an HT block
   coder unless there's a specific reason.

---

## 16. References

* Taubman, "High performance scalable image compression with EBCOT",
  IEEE TIP 9(7), 2000 — the PCRD-opt algorithm.
* Taubman & Marcellin, *JPEG 2000: Image Compression Fundamentals,
  Standards and Practice*, Springer 2002, Chapter 8 — encoder
  rate control.
* ITU-T T.800 / ISO/IEC 15444-1 — Part 1 codestream syntax (SIZ,
  COD, QCD, SOT, SOD, EOC marker definitions).
* ITU-T T.814 / ISO/IEC 15444-15 — Part 15 (HTJ2K).
* OpenJPH: `https://github.com/aous72/OpenJPH` — embeddable Apache-
  compatible HTJ2K library.
* Main spec doc: `grok-performance-innovations.md` for shared
  decoder-side context.
