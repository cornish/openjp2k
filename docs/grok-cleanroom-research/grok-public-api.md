# Grok JPEG 2000 — Public C API Surface

**A cleanroom companion to `grok-performance-innovations.md` for openjp2k**

---

## Cleanroom Statement

Same convention as the main spec: this document is produced by a
describer who has read Grok's AGPL source (`src/lib/core/grok.h`,
2482 lines as of the surveyed master branch). It describes the
shape and intent of Grok's public C API in prose so openjp2k can
decide whether to mirror it for drop-in compatibility, modify it,
or design fresh. No declarations or struct bodies are transcribed.

The purpose is not to specify openjp2k's API. It is to give the
openjp2k team a clear picture of:

* what Grok exposes today and why;
* which design choices reflect a specific consumer (notably GDAL);
* what's worth keeping vs reconsidering for an Apache-licensed
  reimplementation.

---

## 1. High-Level Shape

The Grok public API is a single C header (`grok.h`) consumed by both
C and C++ callers, with a small set of conventions:

* **Opaque object handles**: `grk_object*`. Reference counted via
  `grk_object_ref` / `grk_object_unref`. A single opaque-handle
  type is reused for both compressor and decompressor contexts —
  the type is discriminated only by *which constructor* produced it.
* **Transparent parameter and metadata structs**: `grk_image`,
  `grk_image_comp`, `grk_decompress_core_params`,
  `grk_decompress_parameters`, `grk_cparameters`, `grk_image_meta`,
  `grk_stream_params`, `grk_header_info`, and friends. Caller-allocated
  on the stack or heap, populated, passed in.
* **Function-pointer callbacks** for I/O, message logging, band
  callbacks, decompress completion callbacks.
* **Boolean / byte-count return conventions** — no exceptions, no
  thread-local errno. Diagnostic detail comes through the message
  handler callbacks.

The boundary is plain C with `extern "C"` linkage; no C++ types
leak through.

---

## 2. Library Lifecycle

### 2.1 Initialization

* `grk_initialize(plugin_path, num_threads, plugin_loaded_out)` —
  called once at process start. Spins up the global Taskflow
  executor. `num_threads = 0` auto-detects. Optional plugin path
  for GPU offload.
* `grk_deinitialize()` — process teardown.

The threading model is **opaque to the user**. There is no API
to inject a user-supplied thread pool, executor, or scheduler. The
Taskflow executor is global, shared across all codecs.

* `grk_thread_pool()` returns the Taskflow executor as an opaque
  handle for C++ consumers that want to submit their own tasks to
  the same pool (e.g. GDAL adding its own work alongside Grok's,
  to avoid thread thrashing).
* `grk_num_workers()` and `grk_worker_id()` expose worker count and
  current worker index.

### 2.2 Version & format probing

* `grk_version()` — returns `"MAJOR.MINOR.PATCH"` string.
* `grk_detect_format(stream)` — sniffs magic bytes; returns one of
  `GRK_CODEC_J2K`, `GRK_CODEC_JP2`, `GRK_CODEC_MJ2`.

### 2.3 Logging

A `grk_msg_handlers` struct holds five callback pointers (info,
debug, trace, warning, error), each `void(const char* msg, void* client_data)`.
Set via `grk_set_msg_handlers()` at any time; NULL handlers fall
back to stderr/stdout defaults.

### 2.4 openjp2k considerations

* **The global-executor model is convenient but rigid.** Users who
  need fine-grained scheduling (e.g. a server that wants per-request
  worker affinity, or an embedding in a host that already has a
  thread pool) cannot get it. For openjp2k a slightly richer
  contract — let the user optionally pass an executor handle, fall
  back to a library default — would be a quality-of-life
  improvement. The cost is a thin abstraction layer over Taskflow
  inside the library.
* **The message handler design is fine** and worth keeping
  essentially as-is.

---

## 3. Stream Abstraction

A single `grk_stream_params` struct supports three I/O backends:

* **File-based** — pass a path plus a flag choosing stdio vs
  memory-mapped I/O.
* **Buffer-based** — pass a pre-allocated memory buffer plus
  length.
* **Callback-based** — pass a `read_fn`, `write_fn`, `seek_fn`,
  optional `free_user_data_fn`, and a `void* user_data`.
  Signatures roughly mirror POSIX read/write/lseek but typed for
  the library.

The struct also carries extensive **cloud-storage configuration**
inline: S3 endpoint, credentials chain selectors, virtual-hosted
vs path-style toggle, requester-pays flag, HTTP/2 multiplexing,
TLS verification, proxy, retry policy, fetch concurrency, custom
HTTP headers, cookies, .netrc support. This is roughly a
miniature `aws-sdk-cpp` config plus a curl config, in one struct.

### 3.1 The one-of-three-backends pattern

The struct contains fields for all three backends simultaneously;
the user is expected to populate exactly one set (per
documentation). There is no explicit tag indicating which backend
is selected — the library inspects which fields are non-default.
This is fragile.

For openjp2k a tagged union (or three separate constructor
functions) would be cleaner.

### 3.2 Cloud config in the stream struct

Embedding S3 config in the stream parameters has pros and cons:

* **Pro**: one struct to populate; no separate "cloud client"
  abstraction; matches how GDAL configures its cloud drivers.
* **Con**: the codec library accumulates a lot of network-policy
  surface area (TLS, proxies, retries, signing) that strictly is
  not codec concerns. Bugs in this surface become CVE-level for
  the codec.

A more defensive design for openjp2k: keep the codec stream
abstraction agnostic of network details; provide a separate
cloud-stream factory in a sibling module that produces a
callback-style `grk_stream_params` (or its openjp2k equivalent)
from a cloud config. This isolates network-attack surface from
codec-attack surface and lets non-cloud users link a smaller
library.

---

## 4. The Image Type

`grk_image` is the user-facing image representation:

* Bounding box on a reference grid (`x0, y0, x1, y1`).
* Number of components and a pointer to an array of
  `grk_image_comp`.
* Color space enum (sRGB, grayscale, YCC variants, CMYK, Lab,
  or "ICC profile attached").
* Capture and display resolution (DPI-like).
* Flags for whether palette / channel-definition / forced-RGB /
  upsampling post-processing have been applied or are pending.
* Output format hints for the decoder
  (`decompress_width/height/prec/colour_space`) that the format
  writer may consult.
* A `grk_image_meta*` pointer (§5).

Each `grk_image_comp` holds:

* Per-component geometry (`x0, y0, w, h, stride`).
* Subsampling factors (`dx, dy`).
* Precision and signedness.
* A `void* data` pointer with an `owns_data` flag indicating
  whether `grk_image_meta_unref` (transitively) frees the buffer.
* A data-type discriminator (`GRK_INT_32`, `GRK_INT_16`, etc.).
* Channel metadata (color/opacity/premultiplied type, sRGB
  channel association).
* Sub-pixel component registration (`crg_x, crg_y`) for
  precisely-aligned multispectral imagery.

### 4.1 Memory ownership

`grk_image_new(numcomps, comp_params, colour_space, alloc_data)`:
if `alloc_data == true`, the library allocates per-component
buffers (always int32, padded to per-component stride). If
false, the caller supplies buffers and is responsible for
their lifetime.

This works but creates a footgun: it's easy to set
`owns_data = true` on caller-supplied memory. openjp2k might want
either separate constructors (`_with_alloc`, `_attaching_buffer`)
or a clear contract that `owns_data` is library-private.

### 4.2 Data type union

`grk_image_comp.data` is `void*` plus a `data_type` enum allowing
int32, int16, float, or double. In practice the codec works in
int32 (or int16 on the 16-bit-DWT fast path); float/double are
options for non-codec consumers. For openjp2k, narrowing this to
just int32 and int16 (with int16 used internally on the fast
path but typically materialized as int32 at the API boundary) is
simpler and matches what implementations actually need.

---

## 5. Metadata

`grk_image_meta` holds optional metadata:

* ICC profile buffer + name.
* Channel definitions (which logical channel each codestream
  component corresponds to, plus opacity flags).
* Palette (LUT plus component mapping).
* GeoTIFF / IPR (intellectual property rights) / XMP / IPTC /
  EXIF binary blobs.
* One designated "primary" XML buffer plus an array of additional
  XML boxes (up to `GRK_NUM_XML_BOXES_SUPPORTED`).
* Association boxes (GMLJP2-style nested labeled XML) flattened
  to an array with a level-index tree shape.

Population is via `grk_image_meta_set_field(meta, "name", buf, len)`
where "name" is a string literal ("xmp", "iptc", "exif",
"geotiff", "ipr", "xml"). The string-keyed API is slightly
awkward for a C interface (typos at runtime become silent
no-ops); typed setters per field would be cleaner.

For openjp2k, the metadata model itself is reasonable — JPEG 2000
has many optional metadata boxes and a flat array of them is the
right shape — but per-field accessor functions instead of a
string-keyed dispatch would be safer.

---

## 6. Decompress Parameters

`grk_decompress_core_params` (the smaller, shared subset) exposes:

* **Resolution reduction** (`reduce`): discard top N levels;
  output is 1/2^N resolution.
* **Layer count** (`layers_to_decompress`): max quality layers
  to decode.
* **Cache strategy** (`tile_cache_strategy`): bitmask of
  `GRK_TILE_CACHE_NONE` / `_IMAGE` / `_ALL` / `_LRU`. See main
  spec §8.1.
* **Active tile limit** (`max_active_tiles`): cap on
  decompressed-image residency.
* **Marker disable** (`disable_random_access_flags`): override
  which of TLM / PLT / PLM the decoder consults (for measurement
  or to force the slow path).
* **Component subset** (`comps_to_decode[]`,
  `num_comps_to_decode`): decode only listed components. When
  MCT is active, the codec auto-includes components 0–2 if any
  is requested.
* **Skip composite allocation** (`skip_allocate_composite`): if
  you'll consume per-tile images directly via the tile API.
* **Band callback** (`io_band_callback`, `io_band_user_data`):
  per-completed-band callback (main spec §4).
* **Output format hints**: precision and post-processing
  toggles for non-codec consumers.

`grk_decompress_parameters` wraps `grk_decompress_core_params`
with additional knobs:

* **Async mode** (`asynchronous`): if true, `grk_decompress`
  returns immediately; consumer uses `grk_decompress_wait()`
  / region APIs.
* **Decompress callback** (`decompress_callback`): per-tile
  completion callback for async mode.
* **Decompression window** (`dw_x0..dw_y1, dw_reduced`): region
  of interest.
* **Thread count, device ID**: for plugin / GPU work.

### 6.1 Two-layer design

The split between "core" and "extended" parameters is real but
the line is not always obvious. For openjp2k, a flatter single
struct, or a structurally clearer split (e.g. "what to decode"
vs "how to deliver" vs "where to compute"), would be easier to
reason about.

### 6.2 The async API

Grok's async path consists of:

1. `grk_decompress_init` with `asynchronous = true`.
2. `grk_decompress` schedules tasks and returns.
3. `grk_decompress_wait(codec, swath)` blocks until the requested
   region is ready.
4. `grk_decompress_schedule_swath_copy(codec, swath, user_buffer)`
   schedules an asynchronous copy of the ready region into a
   user-shaped buffer (configurable pixel/line/band spacing —
   GDAL-style).
5. `grk_decompress_wait_swath_copy(codec)` blocks until the copy
   completes.

This is a sophisticated model designed to interleave codec
work with consumer work in a single thread pool. It is also
clearly designed *for GDAL specifically*: the swath buffer
configuration matches GDAL's `RasterIO` interface verbatim.

For openjp2k:

* Keep the **per-tile completion callback** — it's how a consumer
  knows tile N is ready without polling.
* Reconsider whether the **swath-copy machinery** belongs in the
  codec library. GDAL-style swath copy is essentially a strided
  memcpy; pushing it into the codec ties the codec's API to one
  consumer's buffer conventions. A cleaner factoring: codec
  delivers tile images via callback; an optional sibling helper
  module (or just GDAL's own code) does the swath copy.

---

## 7. Compress Parameters

`grk_cparameters` exposes a large surface (the main spec §6 + §7
describe the underlying machinery):

**Tiling and image geometry**

* `tile_size_on`, `tx0`, `ty0`, `t_width`, `t_height`.
* `subsampling_dx`, `subsampling_dy`.

**Rate / quality targets**

* `numlayers` (≤ 256).
* Either `allocation_by_rate_distortion + layer_rate[]`, or
  `allocation_by_quality + layer_distortion[]`.
* `max_cs_size` (whole-codestream byte cap).
* `max_comp_size` (per-component byte cap, used for profile
  constraints).
* `rate_control_algorithm` (`BISECT` vs `PCRD_OPT`).
* `progressive_rate_control` (enable the slope estimator + T1
  early stop; non-bit-exact but ~20–40% faster).

**Transform**

* `irreversible` (5/3 vs 9/7).
* `numresolution` (DWT decomposition levels + 1).
* `mct` (0 / 1 reversible RCT / 2 irreversible ICT).
* `mct_data`, `apply_xyz_transform`.

**Codeblock and precinct**

* `cblockw_init`, `cblockh_init` (defaults 64×64).
* `cblk_sty` bitmask (`LAZY`, `RESET`, `TERMALL`, `VSC`, `PTERM`,
  `SEGSYM`, `HT_ONLY`, `HT_MIXED`).
* `prcw_init[]`, `prch_init[]` (per-resolution precinct sizes).

**Progression and markers**

* `prog_order` (LRCP/RLCP/RPCL/PCRL/CPRL).
* `progression[]` and `numpocs` (POC marker entries).
* `csty` (general coding-style flags).
* `write_plt`, `write_tlm`, `write_sop`, `write_eph`.

**ROI and comments**

* `roi_compno`, `roi_shift`.
* `comment[]`, `comment_len[]`, `is_binary_comment[]`,
  `num_comments`.

**Profile and standards conformance**

* `rsiz` (profile selector packing).
* `write_rreq`, `rreq_standard_features[]`.
* `jpx_branding`, `geoboxes_after_jp2c`.

**Transcode mode**

* `transcode` flag and `transcode_src` (a source
  `grk_stream_params`) plus packet-level rewrite options:
  `max_layers_transcode`, `max_res_transcode`,
  `transcode_prog_order`.

**Acceleration**

* `num_threads`, `device_id`, `kernel_build_options`.

**Misc metadata**

* `capture_resolution`, `display_resolution`, `apply_icc`.

### 7.1 Observations

* The parameter struct is large but JPEG 2000 genuinely has a
  large parameter surface. There is no easy way to shrink it.
* The transcode subfields are interleaved with normal compress
  parameters. Splitting transcode into its own function
  (`grk_transcode_*`) and parameter struct would clarify intent
  and avoid the "are we encoding or rewriting?" confusion.
* The plugin / device fields are present even though the GPU
  plugin is not documented as a shipped feature. For openjp2k,
  unless GPU support is in scope, omit them.

---

## 8. Decompress Flow

Canonical call sequence for a synchronous full-image decompress:

1. `grk_decompress_init(stream_params, decompress_params)` →
   creates codec, opens stream, peeks at format.
2. `grk_decompress_read_header(codec, header_info)` → parses main
   header; populates dimensions, tile grid, ICC profile, metadata.
3. (optional) `grk_decompress_update(new_params, codec)` to
   refine parameters now that header is known (e.g. set window).
4. (optional) `grk_decompress_set_band_callback(codec, cb, ud)` to
   wire incremental output (main spec §4).
5. `grk_decompress(codec, NULL)` → run pipeline.
6. `grk_decompress_get_image(codec)` → retrieve.
7. `grk_object_unref((grk_object*)codec)` → release.

For random-access single-tile decode the middle steps become
`grk_decompress_tile(codec, tile_index)` followed by
`grk_decompress_get_tile_image(codec, tile_index, ...)`.

For async (server / interactive) use, the swath wait/copy pair
described in §6.2 applies.

### 8.1 Progression-state inspection

A nice feature: `grk_decompress_get_progression_state()` and
`grk_decompress_set_progression_state()` let a caller inspect or
override per-tile layer budgets between decompress calls. This
enables "decode 1 layer, evaluate quality, request another layer"
without re-parsing the codestream. Worth keeping.

---

## 9. Compress Flow

Canonical call sequence:

1. `grk_compress_set_default_params(&cparams)` to populate
   defaults.
2. Fill in `cparams` overrides.
3. `grk_image_new(numcomps, comp_params, colorspace, alloc=true)`.
4. Fill `image->comps[i].data` with samples.
5. `grk_compress_init(stream_params, cparams, image)`.
6. `bytes_written = grk_compress(codec, NULL)`.
7. `grk_object_unref(codec)`.

For Motion JPEG 2000 (MJ2) multi-frame:
`grk_compress_init` with first frame, then `grk_compress_frame`
per additional frame, then `grk_compress_finish`.

The compress return-value convention (uint64 bytes; 0 means
failure) is workable but boolean + outparam would be clearer.

---

## 10. Threading Contract

The header does not state a formal threading contract. By
inspection of the design:

* **Multiple independent codecs in parallel** — fine. The
  global Taskflow executor schedules across them.
* **One codec, concurrent decompress + wait from two threads** —
  unsafe. The async API is designed for single-thread caller
  driving the codec while internal workers run in parallel.
* **Library init/deinit** — single-threaded, at process start/end.
* **Message handler set/clear** — looks safe to change, but no
  explicit guarantee; assume single-threaded.

For openjp2k, **spell out the threading contract in the header
doxygen comments**. The fact that Grok's contract has to be
inferred is itself a usability problem.

---

## 11. Versioning & ABI

* `grk_version()` returns a `"MAJOR.MINOR.PATCH"` string. No
  programmatic version-comparison helpers.
* The header mixes opaque handles (codec, plugin) with
  transparent structs (parameters, image, metadata). Transparent
  structs make ABI fragile — any field reorder breaks linked
  consumers.

For openjp2k, two options:

* **Pure transparent C-struct ABI** — accept that ABI breaks at
  every minor version, recompile downstream. Simple.
* **Mostly-opaque ABI** — make all parameter structs opaque,
  expose set/get accessors. More stable but more API surface.

Given JPEG 2000's slow churn rate, transparent-struct + clear
"this is API, not ABI" documentation is probably the right
trade-off.

---

## 12. Integration Hints That Reveal the Consumer

Several Grok design choices are clearly shaped by specific consumers:

### 12.1 GDAL

* The swath-copy machinery (§6.2) mirrors GDAL `RasterIO` buffer
  conventions: configurable pixel/line/band spacing, 1-based band
  indexing, alpha promotion.
* The cloud-storage parameters (§3.2) match GDAL's
  `CPLSetConfigOption` keys (`AWS_S3_ENDPOINT`, `AWS_REGION`,
  etc.).
* `GDAL_CACHEMAX` is recognized as a fallback for `GRK_CACHEMAX`.

For openjp2k, GDAL integration is presumably also a target.
Whether to bake the GDAL conventions into the public API or
keep the API consumer-agnostic and provide a separate
GDAL-adapter library is a design call.

### 12.2 Plugin / GPU offload

* `num_threads`, `device_id`, `kernel_build_options`, and a
  plugin-load step at `grk_initialize` time.
* Reference to a GPU plugin path in some places.

The plugin path is gated and not documented as a shipped
feature. For openjp2k, leaving these fields out is reasonable.

### 12.3 MJ2 (Motion JPEG 2000)

A full container format implementation is exposed via
`grk_compress_frame`, `grk_compress_finish`,
`grk_decompress_num_samples`, `grk_decompress_sample`, etc.
This is a meaningful chunk of code for a niche format. If
openjp2k targets stills-only initially, MJ2 can be deferred.

---

## 13. Recommendations for openjp2k

If openjp2k wants **drop-in source compatibility** with Grok:

* Keep the enum names and values for codec format, color space,
  progression order, profile (RSIZ), and codeblock-style bits.
  These leak into user configuration files and command-line tools.
* Keep the broad parameter struct shapes; users have populated
  `grk_cparameters` from many places.
* Keep `grk_initialize` / `grk_deinitialize`, the message-handler
  contract, and the basic decompress flow (`init` →
  `read_header` → `decompress` → `get_image` → `unref`).
* Provide compatible function names (perhaps via a thin
  `grk_compat.h` header that maps `grk_*` to `ojp2k_*`).

If openjp2k wants a **cleaner, more maintainable API** at the
cost of source compatibility:

* Use tagged unions or separate constructors for the three
  stream backends, not a one-of-three struct.
* Separate cloud-storage configuration from codec stream
  configuration.
* Drop the string-keyed metadata setters in favor of typed
  per-field functions.
* Split transcode out of the main compress flow into its own
  functions and parameter struct.
* Make threading contract explicit in the header.
* Split swath-copy out of the codec API into a separate
  GDAL-adapter module.
* Decide on either pure transparent struct ABI or pure opaque
  with accessors — don't mix.
* Drop or feature-gate the plugin / GPU surface unless it's a
  shipped feature.

A reasonable middle path: ship `openjp2k.h` as the fresh API,
plus a `grok_compat.h` shim that translates the Grok names. The
shim lets existing Grok-using applications switch implementations
with a recompile, while new code uses the cleaner API.

---

## 14. Summary Table — Public API Subsystems

| subsystem | shape | recommendation |
|---|---|---|
| Lifecycle (init/deinit, version, format detect) | global functions | keep, possibly accept user-supplied executor |
| Logging | 5-callback struct | keep |
| Stream I/O | one-of-three struct + cloud config | redesign as tagged union; split cloud |
| Image type | transparent struct with component array | keep shape; clarify ownership |
| Metadata | string-keyed field setters | move to typed setters |
| Decompress params | two-layer (core + extended) struct | flatten or restructure |
| Decompress flow | init → read_header → decompress → get_image | keep |
| Async / swath | wait + scheduled copy | keep wait; split swath copy |
| Compress params | one large struct with transcode mixed in | split transcode out |
| Compress flow | set_defaults → fill → init → compress → unref | keep |
| Threading contract | inferred | document explicitly |
| ABI | mixed opaque + transparent | pick one |
| GDAL hooks | scattered | isolate to adapter module |
| Plugin / GPU | gated, undocumented | omit unless shipped |
| MJ2 | full container | defer |

---

## 15. References

* Grok header surveyed: `src/lib/core/grok.h` (master branch).
* Main spec doc: `grok-performance-innovations.md` for the
  internals these API surfaces expose.
* Encoder deep dive: `grok-encoder-deep-dive.md`.
* Test/benchmark methodology: `grok-conformance-and-benchmarks.md`.
* GDAL RasterIO documentation (for understanding the swath-copy
  shape): `https://gdal.org/api/raster_c_api.html`.
* ISO/IEC 15444-1 — codestream parameters whose values populate
  the parameter structs.
