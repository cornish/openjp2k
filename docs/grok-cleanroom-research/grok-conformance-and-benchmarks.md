# Grok JPEG 2000 — Conformance, Test, and Benchmark Methodology

**A cleanroom companion to `grok-performance-innovations.md` for openjp2k**

---

## Cleanroom Statement

Same as the main spec doc: this document is produced by a describer who
has read Grok's AGPL source. Its contents describe Grok's verification
infrastructure in prose; no Grok code is transcribed. References to
external public standards (ISO/IEC 15444-4, the J2KP4files conformance
suite, OSS-Fuzz, Google Highway, Taskflow) are public knowledge.

The companion spec doc (`grok-performance-innovations.md`) describes
*what* Grok does. This document describes *how Grok knows it is right
and how Grok measures speed.* openjp2k will need both.

---

## 1. The Three Layers of Verification

Grok layers verification at three levels of rigor:

1. **Unit tests** — narrow correctness for in-process components
   (LRU cache, JP2 box metadata, MJ2 container, internal shared-memory
   messenger). CTest binaries; ~5 executables. Useful as templates,
   small in scope.

2. **Non-regression tests** — the load-bearing layer. ~2000 entries in
   `tests/nonregression/test_suite.ctest.in` that drive
   `grk_compress` and `grk_decompress` end-to-end on real imagery and
   compare MD5 hashes of decoded output against per-platform baselines.
   This is what catches most behavior regressions.

3. **Conformance tests** — ISO/IEC 15444-4 reference vectors with PSNR
   and MSE tolerance comparison. Structured under `tests/conformance/`
   for Class 0 / Class 1, Profile 0 / Profile 1, plus JP2 box-format
   vectors, Kakadu reference set, and Richter reference set.

Two important things to note up front:

* **Conformance is partial.** The CMakeLists in `tests/conformance/`
  explicitly enable only 2-of-16 Class 0 Profile 0 vectors and
  1-of-7 Class 0 Profile 1 vectors. Comments in the file admit the
  rest fail. The non-regression suite carries the practical
  correctness gate. For openjp2k this is both an opportunity (clean
  conformance pass would be a real differentiator) and a warning
  (vendors disagree on edge-case interpretation; passing all vectors
  is non-trivial).

* **MD5 is per-platform.** `tests/nonregression/checkmd5refs.cmake`
  reads from one of `md5refs.txt` (canonical Ubuntu dynamic),
  `md5refs-ubuntu-static.txt`, `md5refs-macos-dynamic.txt`,
  `md5refs-macos-static.txt`, `md5refs-windows-dynamic.txt`, or
  `md5refs-windows-static.txt`, with `md5blacklist-*.txt` files
  exempting tests that are non-deterministic on a given platform
  (typically 9/7 float-DWT outputs on ARM macOS). The per-platform
  variant files contain *only* differing entries — the canonical
  file is the fallback. This is the cleanest design I have seen for
  bit-exact regression testing across platforms without per-platform
  branches.

---

## 2. ISO/IEC 15444-4 Conformance — What openjp2k Should Know

### 2.1 The official test set

ISO/IEC 15444-4 ("JPEG 2000 image coding system — Part 4: Conformance
testing") defines a reference suite called **J2KP4files**, containing:

* **Class 0** — decoders that handle a defined subset of the standard.
  * Profile 0: 16 codestreams (`p0_01.j2k` through `p0_16.j2k`).
  * Profile 1: 7 codestreams (`p1_01.j2k` through `p1_07.j2k`).
* **Class 1** — decoders that handle the full Part-1 standard. A larger
  set of progressively harder codestreams.
* **JP2 files** — `file1.jp2` through `file9.jp2` exercising the JP2
  box file format (color spaces, ICC, channel definitions, palette).

The suite is distributed by the JPEG committee. Standard practice is
to mirror it in a test-data sidecar repository that the build pulls in
at configure time. Grok does exactly this with the `grok-test-data`
GitHub repository, cloned into `GRK_DATA_ROOT` at CMake configure.

### 2.2 How conformance is scored

For each test vector the standard gives per-component **PEAK** (peak
absolute error) and **MSE** (mean squared error) tolerance tables
(Tables C.1–C.7 in ISO 15444-4). The `compare_images` utility in
`tests/conformance/` extracts each decoded component as a PGX file
and compares it to the reference component PGX, asserting both
metrics are within tolerance.

Lossless test vectors require bit-exact decode (PEAK = 0, MSE = 0).
Lossy vectors allow finite tolerances that depend on profile and
component depth.

### 2.3 Recommendation for openjp2k

Mirror Grok's structure: a separate `openjp2k-test-data` repo holding
the J2KP4 vectors + per-platform MD5 baselines + Kakadu/Richter
reference sets, pulled in at configure time. Use ISO 15444-4's
exact PEAK/MSE matrices as the conformance scoring rule. Aim
to enable *every* vector — that is the real public-facing differentiator.

---

## 3. Non-Regression Test Suite

### 3.1 Structure

`tests/nonregression/test_suite.ctest.in` is a CMake-template-generated
list of test invocations. Two patterns dominate:

* **Encode regression** — encode a reference raster (PGX, PNM, TIFF)
  with a specified parameter set; MD5 the resulting `.j2k`/`.jp2`;
  compare against `md5refs.txt`.
* **Decode regression** — decode a reference codestream to PGX;
  MD5 the result; compare.

Each test entry takes the form:
```
NR-ENC-<source>-<param-tag>-encode-md5
NR-DEC-<source>-<param-tag>-decode-md5
```
giving stable, greppable test names.

### 3.2 Platform stratification

Because the 9/7 DWT path uses float multiplications whose rounding
varies by platform (x86 vs ARM, AVX2 vs scalar, compiler version), a
single MD5 baseline cannot work. Grok's solution:

* `md5refs.txt` — the canonical Ubuntu-dynamic baseline.
* `md5refs-<platform>-<linkage>.txt` — sparse override files containing
  *only* entries that differ.
* `md5blacklist-<platform>-<linkage>.txt` — entries to skip entirely
  on a given platform (used where non-determinism cannot be pinned).

This is straightforward to lift directly. For an Apache 2.0 project,
the override files are data, not code; openjp2k can author its own.

### 3.3 Recommendation

This pattern is the right one. The investment is in (a) authoring the
per-platform baselines initially (which means having every target
platform in CI to capture them), and (b) the discipline to update
them deliberately when a change legitimately alters output. Make
the update mechanism explicit (e.g. a `--update-md5` flag on the
test driver) so reviewers can see baseline diffs in PRs.

---

## 4. Fuzzing

### 4.1 Two fuzzer entry points

Grok ships two libFuzzer targets in `tests/fuzzers/`:

* **`grk_decompress_fuzzer.cpp`** — input is a raw byte stream
  interpreted as a JPEG 2000 codestream. The fuzz corpus contains
  malformed codestreams; the fuzzer drives the decode path and traps
  on assertion failures, ASan/UBSan/MSan reports, and crashes.
* **`grk_compress_fuzzer.cpp`** — input is consumed by a `FuzzStream`
  helper that maps the first N bytes to mutate compress parameters
  (image dimensions, bit depth, component count, codec format,
  progression order, DWT resolution count, codeblock dimensions and
  style flags, irreversible toggle, quality layer count, tile
  geometry, MCT, tile-part generation, HTJ2K mode), then uses the
  remainder as raw pixel data. This is the right split — the
  compress fuzzer attacks the parameter validation surface, not
  just the pixel pipeline.

### 4.2 OSS-Fuzz integration

Grok is enrolled in Google's OSS-Fuzz project. The `.github/workflows/fuzz.yml`
workflow builds with each sanitizer (`address`, `undefined`, `memory`)
and runs each fuzzer for 3600 seconds per push. OSS-Fuzz operates
its own corpus retention, regression suite, and bug filing pipeline
at `https://issues.oss-fuzz.com/issues?q=proj:grok`. Backups of the
corpus are available from
`gs://grok-backup.clusterfuzz-external.appspot.com/corpus/libFuzzer/`.

### 4.3 Recommendation

OSS-Fuzz enrollment for openjp2k is highly recommended. It is free,
extensively staffed, and gives continuous adversarial coverage that
no in-house fuzz job can match. The requirements are: open-source
project (yes), a `project.yaml` declaration, a `Dockerfile` and
`build.sh` that produce fuzzer binaries linkable against libFuzzer,
and a couple of fuzzer entry-point `.cpp` files. The two-fuzzer
split (decode vs compress-param) is the right shape; copy it.

---

## 5. CI Matrix

Grok's GitHub Actions matrix (`.github/workflows/build.yml`):

| dimension | values |
|---|---|
| OS | `ubuntu-latest`, `macos-latest`, `windows-latest` |
| linkage | `BUILD_SHARED_LIBS=ON`, `OFF` |
| build type | Release |
| test execution | `ctest --output-on-failure -C Release --output-junit ...` |

Six jobs per push. JUnit XML test results are uploaded as artifacts;
binary ZIPs uploaded for shared-libs builds. Test data repo is
cloned into `${DATA_ROOT}` at job start.

The GitLab mirror (`.gitlab-ci.yml`) duplicates the matrix on
`saas-linux-medium-amd64` runners.

**Gaps to note for openjp2k:**

* No sanitizer (ASan/UBSan/TSan) job in the main CI. Sanitizers run
  only inside OSS-Fuzz. A standalone ASan-build CI job catches
  use-after-free / double-free regressions before OSS-Fuzz does.
* No coverage tooling (gcov, llvm-cov, Codecov). Coverage isn't
  strictly necessary, but for openjp2k it would be cheap to add
  and useful for prioritizing test gaps.
* No nightly long-running job. Benchmark regression tracking would
  need one (see §6).
* No ARM CI on the main matrix — relevant for cloud (Graviton) and
  Apple Silicon. Grok presumably tests this manually.

---

## 6. Benchmark Methodology — and Grok's Weak Spot

### 6.1 What Grok provides

The `benchmark/` directory holds a handful of PowerShell scripts and
Python graph generators for timing `grk_decompress` against
Pleiades / SPOT 6 satellite imagery. The README cites the GDAL
JP2-driver workflow: `gdal_translate` to stitch and re-encode source
imagery to specific tile geometries, then `Measure-Command` on
`grk_decompress` and the comparator (Kakadu via `kdu_compress`, etc.).

This is the methodology behind the headline numbers in the project
README (Grok ~2× OpenJPEG, ~5–10× Kakadu on the same images).

### 6.2 What's missing

The benchmark setup is not reproducible in CI:

* Inputs are real satellite imagery referenced by filename
  (`IMG_PHR1B_P_...JP2`) — not redistributable.
* Commands are manual PowerShell, not parameterized fixtures.
* No automated tracking of results over time; no per-PR perf check.

For openjp2k this is a clear opportunity. A small set of
synthetically-generated test images (or freely-licensed reference
imagery from NASA, ESA Sentinel, or the JPEG Pleno corpus) at
representative sizes (4K, 16K, 40K) with a Python harness that runs
both encode and decode under both codecs and tracks wall-clock,
RSS, and codestream size would let openjp2k:

* prove parity (or improvement) over Grok publicly;
* catch performance regressions before merge.

### 6.3 Recommended benchmark axes

| axis | suggested values |
|---|---|
| image size | 1024², 4096², 16384², 40000² |
| precision | 8-bit, 12-bit, 16-bit |
| components | 1 (mono), 3 (RGB), 4 (RGBA), 8 (multispectral) |
| reversible | yes, no |
| codec | Part-1 EBCOT, Part-15 HTJ2K |
| workload | encode, full decode, single-tile decode, windowed decode |
| storage | local disk, S3 (MinIO loopback for CI determinism) |
| threads | 1, 4, 16, all |

This is ~3000 cells; a sensible random sample of ~50 per CI run is
plenty to spot regressions.

### 6.4 What to measure

* **Wall-clock time** — primary throughput measure.
* **Peak RSS** — the incremental-compositing claim only matters if
  measured.
* **Codestream size** — for encode, prove bit-budget compliance.
* **Decoded MD5** — sanity-check decode hasn't silently changed.
* **Allocations count** (via a malloc shim or `mtrace`) — proxy for
  whether the pool architecture is intact.

---

## 7. Test Harness Choice

Grok uses CTest exclusively, no GTest / Catch2 / doctest. Tests are
either C++ executables registered with `add_test()` or shell-level
invocations of `grk_compress` / `grk_decompress` from
`test_suite.ctest.in`. This is austere but works.

For openjp2k a slight enrichment is reasonable: GTest for the
in-process unit tests gives parameterized test cases and better
failure messages; CTest still wraps GTest as the top-level
runner. The end-to-end tests can stay as command-driven
ctest entries.

---

## 8. Python Bindings Tests

Grok's `tests/python/` directory contains 11 pytest files covering
round-trip encode/decode, codec parameters, async decompress,
multi-region decompress, S3 / MinIO integration, GPU plugin
behavior, error handling, metadata, and stdio. `conftest.py`
defines markers for optional features (`--run-s3`, `--run-gpu`)
that gate expensive tests behind explicit opt-in.

If openjp2k provides Python bindings (recommended — Grok's exist via
SWIG, but `pybind11` is simpler and Apache-friendly), mirror this
test structure. The marker-gated optional-feature pattern is a
clean way to keep CI runs fast while still allowing full coverage
on demand.

---

## 9. Specific Files Worth Studying

For openjp2k maintainers building equivalent infrastructure, these
Grok files are particularly useful as templates (read them to learn
the patterns, then write your own Apache-licensed equivalents):

| file | what to learn |
|---|---|
| `tests/nonregression/checkmd5refs.cmake` | per-platform MD5 stratification |
| `tests/nonregression/test_suite.ctest.in` | the ctest template idiom for ~2000 test entries |
| `tests/conformance/CMakeLists.txt` | how to bind ISO 15444-4 PEAK/MSE tolerances into ctest |
| `tests/python/conftest.py` | marker-gated optional-feature tests |
| `.github/workflows/build.yml` | minimal 6-cell CI matrix |
| `.github/workflows/fuzz.yml` | OSS-Fuzz wiring |
| `tests/fuzzers/grk_compress_fuzzer.cpp` | parameter-fuzzing pattern (read the *approach*, write your own) |

---

## 10. Recommendations Summary

1. **Mirror the test-data-sidecar repo pattern.** Don't bake test
   vectors into the main repo.
2. **Aim for clean ISO 15444-4 conformance pass** as an early public
   milestone. Grok's incomplete pass is a real opportunity to
   differentiate.
3. **Adopt per-platform stratified MD5 baselines.** Lift the pattern
   directly.
4. **Enroll in OSS-Fuzz on day 1.** Free, continuous adversarial
   testing. Two-fuzzer split (decode + compress-param).
5. **Add sanitizer (ASan/UBSan) CI jobs** beyond what OSS-Fuzz
   covers, for faster feedback in PRs.
6. **Build a reproducible benchmark harness** in CI with a
   synthetic-or-freely-licensed image corpus. This is Grok's weak
   spot; openjp2k can do better.
7. **Choose GTest for unit tests** if you want better failure
   messages; keep CTest as the top-level driver.
8. **Mirror the Python pytest structure** if you ship Python
   bindings; the marker-gated optional-feature idiom is clean.

---

## 11. References

* ISO/IEC 15444-4:2021 — *JPEG 2000 image coding system — Part 4:
  Conformance testing.*
* Grok test infrastructure (read by describer, not by implementers):
  `https://github.com/GrokImageCompression/grok/tree/master/tests`
* Grok test data sidecar:
  `https://github.com/GrokImageCompression/grok-test-data`
* OSS-Fuzz documentation:
  `https://google.github.io/oss-fuzz/`
* OSS-Fuzz Grok issues:
  `https://issues.oss-fuzz.com/issues?q=proj:grok`
* CTest manual: `https://cmake.org/cmake/help/latest/manual/ctest.1.html`
* GoogleTest: `https://github.com/google/googletest`
* pybind11: `https://github.com/pybind/pybind11`
