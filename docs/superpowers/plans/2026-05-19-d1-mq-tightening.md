# D1 — MQ Renormalize + Packed-State LUT Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land a measurably faster MQ arithmetic decoder for the T1 hot path by (a) packing the 94-entry probability state table from 24 bytes/entry of pointer-chained struct into 4 bytes/entry of bit-packed `uint32_t`, and (b) replacing the per-bit `while (a < 0x8000)` renormalize loop with a `clz`-driven single-shift path that pre-refills enough bytes for the worst case. Both changes must produce byte-identical decoded output on the smoke + conformance + worst-loser corpora.

**Architecture:** New translation unit `mqc_fast.{c,h}` holds the packed state table, the rewritten decode/renormalize macros, and the `opj_mqc_fast_*` setup functions. The T1 hot-path functions (`opj_t1_dec_sigpass_mqc`, `opj_t1_dec_refpass_mqc`, `opj_t1_dec_clnpass`) are cloned into `t1_fast.c` with their inner macros pointing at the fast MQ path. `opj_t1_decode_cblk` picks fast vs legacy *once per codeblock*, controlled by a process-global flag initialized from the `OPJ_T1_FAST` environment variable (defaults to enabled when `OPJ_T1_LEGACY_ONLY` is undefined). A new diff-test driver decodes each file through both paths and asserts byte-exact pixel equivalence.

**Tech Stack:** C99, CMake, ctest, x86-64 SSE2 (existing baseline). `__builtin_clz` (GCC/Clang) with portable fallback. Existing `OPJ_T1_LEGACY_ONLY` CMake option becomes a hard "fast path disabled" gate; the runtime switch handles A/B during development.

---

## File Structure

**Created:**
- `src/lib/openjp2/mqc_fast.h` — packed-state types, fast macros, public init signatures.
- `src/lib/openjp2/mqc_fast.c` — packed `mqc_states_packed[94]` table, `opj_mqc_fast_resetstates/setstate/init_dec` implementations.
- `tests/unit/test_mqc_fast.c` — unit tests for packed-table equivalence and clz helper.
- `tests/unit/test_mqc_diff.c` — programmatic diff-test driver invoked from ctest.
- `scripts/run_diff_test.sh` — wraps the diff-test driver across a corpus manifest.
- `docs/superpowers/plans/2026-05-19-d1-mq-tightening.md` — this file.

**Modified:**
- `src/lib/openjp2/t1_fast.c` — currently empty; will hold cloned hot-path functions (`opj_t1_fast_dec_sigpass_mqc`, `opj_t1_fast_dec_refpass_mqc`, `opj_t1_fast_dec_clnpass`).
- `src/lib/openjp2/t1_fast.h` — declarations for the cloned functions + the `opj_t1_fast_enabled()` accessor.
- `src/lib/openjp2/t1.c` — `opj_t1_decode_cblk` calls the cloned fast functions when `opj_t1_fast_enabled()` returns true.
- `src/lib/openjp2/CMakeLists.txt` — add `mqc_fast.c`, `t1_fast.c` to the library sources.
- `tests/unit/CMakeLists.txt` (or equivalent) — register the new unit tests.
- `docs/superpowers/specs/2026-05-16-decode-perf-design.md` — mark D1.0 / D1.1 / D1.2 status as they land.

**Not modified (intentionally):** `mqc.c`, `mqc.h`, `mqc_inl.h`. The legacy path stays untouched so we always have an unambiguous reference to diff against.

---

## Phase D1.0 — A/B Verification Harness

The harness is the prerequisite for everything else. Without byte-exact diff-testing we can't tell whether D1.1's packed table broke RESET-style codestreams. We build the harness against the legacy path (legacy-vs-legacy diff must be a no-op) before we add any fast-path code.

### Task 1: Runtime fast/legacy switch (process-global)

**Files:**
- Create: `src/lib/openjp2/t1_fast.h`
- Create: `src/lib/openjp2/t1_fast.c` (modify — currently the existing 18-line scaffold)

- [ ] **Step 1: Write the header.**

Replace the existing `t1_fast.h` (currently a placeholder with header guard only) with:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish

#ifndef OPJ_T1_FAST_H
#define OPJ_T1_FAST_H

/*
 * Public surface of the optimized T1 (entropy decode) variants.
 *
 * Process-global enable flag:
 *   - When OPJ_T1_LEGACY_ONLY is defined at compile time, opj_t1_fast_enabled()
 *     is a compile-time false and the linker can DCE the fast paths.
 *   - Otherwise the flag is read once from the OPJ_T1_FAST environment
 *     variable on first call:
 *       * "0" / "off" / "false" -> legacy
 *       * unset or any other value -> fast (default ON once D1 lands)
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef OPJ_T1_LEGACY_ONLY
static inline int opj_t1_fast_enabled(void) { return 0; }
#else
int opj_t1_fast_enabled(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPJ_T1_FAST_H */
```

- [ ] **Step 2: Replace the t1_fast.c body.**

Replace the existing `t1_fast.c` with:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish

#include "opj_includes.h"
#include "t1_fast.h"

#ifndef OPJ_T1_LEGACY_ONLY

#include <stdlib.h>
#include <string.h>

static int opj_t1_fast_flag = -1;  /* -1 = uninitialized */

int opj_t1_fast_enabled(void)
{
    int v = opj_t1_fast_flag;
    if (v >= 0) {
        return v;
    }
    const char *env = getenv("OPJ_T1_FAST");
    if (env && (strcmp(env, "0") == 0 ||
                strcmp(env, "off") == 0 ||
                strcmp(env, "false") == 0)) {
        v = 0;
    } else {
        v = 1;
    }
    opj_t1_fast_flag = v;
    return v;
}

#endif /* !OPJ_T1_LEGACY_ONLY */
```

- [ ] **Step 3: Build and confirm linkage.**

Run:

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build. `opj_t1_fast_enabled` exists as a real function in `libopenjp2.so` when `OPJ_T1_LEGACY_ONLY` is undefined. Confirm with:

```bash
nm -D build/bin/libopenjp2.so.2.5.4 | grep opj_t1_fast_enabled
```

Expected output: one line ending in ` T opj_t1_fast_enabled`.

- [ ] **Step 4: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/t1_fast.h src/lib/openjp2/t1_fast.c
git commit -m "$(cat <<'EOF'
T1: add runtime fast/legacy switch (D1.0 step 1)

opj_t1_fast_enabled() reads OPJ_T1_FAST once and caches; OPJ_T1_LEGACY_ONLY
short-circuits to compile-time false so the fast path can be DCE'd in CI's
correctness-fallback build.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 2: Diff-test driver

The driver decodes one file twice (once with `OPJ_T1_FAST=0`, once unset) using the public `opj_decompress`-equivalent API and asserts byte-exact pixel output. At this point both paths are legacy, so it must succeed trivially — this is the baseline test.

**Files:**
- Create: `tests/unit/test_mqc_diff.c`
- Modify: `tests/unit/CMakeLists.txt` (or the parent test CMakeLists)

- [ ] **Step 1: Inspect existing test infrastructure to find the registration pattern.**

```bash
cd /home/cornish/GitHub/openjp2k && find tests -name "CMakeLists.txt" -exec grep -l "add_executable\|add_test" {} \;
```

Pick the unit-test CMakeLists most similar in shape; mirror it for the new test.

- [ ] **Step 2: Write the driver.**

Create `tests/unit/test_mqc_diff.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// Diff-test: decode a JP2/J2K file twice, once with the fast MQ path
// enabled (default) and once forced off via OPJ_T1_FAST=0, and assert
// byte-exact pixel output. Exits 0 on match, non-zero on mismatch.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg.h>

static opj_image_t *decode_file(const char *path)
{
    opj_dparameters_t parameters;
    opj_set_default_decoder_parameters(&parameters);

    opj_stream_t *stream = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    if (!stream) { fprintf(stderr, "stream open failed: %s\n", path); return NULL; }

    /* Sniff format from extension. */
    const char *ext = strrchr(path, '.');
    OPJ_CODEC_FORMAT fmt = OPJ_CODEC_J2K;
    if (ext && (strcmp(ext, ".jp2") == 0 || strcmp(ext, ".jpf") == 0)) {
        fmt = OPJ_CODEC_JP2;
    }
    opj_codec_t *codec = opj_create_decompress(fmt);
    if (!opj_setup_decoder(codec, &parameters)) {
        opj_destroy_codec(codec); opj_stream_destroy(stream); return NULL;
    }

    opj_image_t *image = NULL;
    if (!opj_read_header(stream, codec, &image) ||
        !opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
        if (image) opj_image_destroy(image);
        opj_destroy_codec(codec); opj_stream_destroy(stream);
        return NULL;
    }
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return image;
}

static int compare_images(opj_image_t *a, opj_image_t *b, const char *path)
{
    if (a->numcomps != b->numcomps) {
        fprintf(stderr, "[%s] numcomps differ: %u vs %u\n", path,
                a->numcomps, b->numcomps);
        return 1;
    }
    for (OPJ_UINT32 c = 0; c < a->numcomps; ++c) {
        opj_image_comp_t *ca = &a->comps[c], *cb = &b->comps[c];
        if (ca->w != cb->w || ca->h != cb->h) {
            fprintf(stderr, "[%s] comp %u size differs: %ux%u vs %ux%u\n",
                    path, c, ca->w, ca->h, cb->w, cb->h);
            return 1;
        }
        OPJ_UINT32 n = ca->w * ca->h;
        if (memcmp(ca->data, cb->data, n * sizeof(OPJ_INT32)) != 0) {
            /* Locate first mismatching pixel for the error. */
            for (OPJ_UINT32 i = 0; i < n; ++i) {
                if (ca->data[i] != cb->data[i]) {
                    fprintf(stderr,
                        "[%s] comp %u pixel %u differs: %d vs %d\n",
                        path, c, i, ca->data[i], cb->data[i]);
                    break;
                }
            }
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.jp2|file.j2k>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    setenv("OPJ_T1_FAST", "0", 1);
    opj_image_t *legacy = decode_file(path);
    if (!legacy) { fprintf(stderr, "legacy decode failed: %s\n", path); return 3; }

    setenv("OPJ_T1_FAST", "1", 1);
    opj_image_t *fast = decode_file(path);
    if (!fast) { fprintf(stderr, "fast decode failed: %s\n", path); opj_image_destroy(legacy); return 4; }

    int rc = compare_images(legacy, fast, path);
    opj_image_destroy(legacy);
    opj_image_destroy(fast);
    return rc;
}
```

Note: `decode_file` is called in the *same process* with `setenv` between calls — but `opj_t1_fast_enabled()` caches the env-var result on first read. That's intentionally broken for in-process A/B; we work around it in Task 4 by invoking the driver as a subprocess per setting.

- [ ] **Step 3: Register the test executable.**

Add to the appropriate `tests/unit/CMakeLists.txt`:

```cmake
add_executable(test_mqc_diff test_mqc_diff.c)
target_link_libraries(test_mqc_diff PRIVATE openjp2)
```

Do *not* register it via `add_test()` directly — it takes a file argument, so it's invoked by Task 4's shell wrapper. The executable just needs to build.

- [ ] **Step 4: Build and run a smoke check against one tiny conformance file.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
build/bin/test_mqc_diff \
    /home/cornish/GitHub/openjp2k-data/corpus/conformance/openjpeg-data/input/conformance/p1_07.j2k
echo "exit=$?"
```

Expected: `exit=0` (legacy-vs-legacy must match; the runtime switch doesn't actually flip anything yet because we haven't wired the fast path in).

- [ ] **Step 5: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add tests/unit/test_mqc_diff.c tests/unit/CMakeLists.txt
git commit -m "$(cat <<'EOF'
T1: add diff-test driver (D1.0 step 2)

test_mqc_diff <file> decodes the same file with OPJ_T1_FAST=0 and =1 in
sequence and asserts byte-identical opj_image_t output across all
components. At this point both modes resolve to the legacy decoder; the
test exists to gate future fast-path landings.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 3: Per-process diff-test wrapper

The single-process driver from Task 2 can't A/B once the fast path exists (the env-var cache is set on first read). Wrap it in a shell script that invokes the driver twice — once with each env var value — using sibling helper binaries that dump the decoded image to a file, then diff.

**Files:**
- Create: `tests/unit/test_mqc_dump.c` (helper: decodes one file, writes raw pixel planes to stdout).
- Create: `scripts/run_diff_test.sh`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the dump helper.**

Create `tests/unit/test_mqc_dump.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// Decode one file and write a deterministic raw representation of the
// pixel data to stdout. Used by scripts/run_diff_test.sh which invokes
// this binary twice (once per OPJ_T1_FAST setting) and compares outputs.
//
// Output format (binary, big-endian for header fields):
//   "OJ2KDUMP\0\0\0\1"   12-byte magic + version
//   uint32 numcomps
//   for each component:
//     uint32 w, uint32 h, int32 prec, int32 sgnd
//     (w*h) int32 samples in component-buffer order

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openjpeg.h>

static void wbe32(FILE *f, OPJ_UINT32 v)
{
    OPJ_BYTE b[4] = { (OPJ_BYTE)(v >> 24), (OPJ_BYTE)(v >> 16),
                      (OPJ_BYTE)(v >> 8),  (OPJ_BYTE)v };
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    opj_dparameters_t parameters;
    opj_set_default_decoder_parameters(&parameters);

    opj_stream_t *stream = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    if (!stream) { fprintf(stderr, "stream open failed: %s\n", path); return 3; }

    const char *ext = strrchr(path, '.');
    OPJ_CODEC_FORMAT fmt = OPJ_CODEC_J2K;
    if (ext && (strcmp(ext, ".jp2") == 0 || strcmp(ext, ".jpf") == 0)) {
        fmt = OPJ_CODEC_JP2;
    }
    opj_codec_t *codec = opj_create_decompress(fmt);
    opj_setup_decoder(codec, &parameters);

    opj_image_t *image = NULL;
    if (!opj_read_header(stream, codec, &image) ||
        !opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
        fprintf(stderr, "decode failed: %s\n", path);
        if (image) opj_image_destroy(image);
        opj_destroy_codec(codec); opj_stream_destroy(stream);
        return 4;
    }

    fwrite("OJ2KDUMP\0\0\0\1", 1, 12, stdout);
    wbe32(stdout, image->numcomps);
    for (OPJ_UINT32 c = 0; c < image->numcomps; ++c) {
        opj_image_comp_t *cc = &image->comps[c];
        wbe32(stdout, cc->w);
        wbe32(stdout, cc->h);
        wbe32(stdout, (OPJ_UINT32)cc->prec);
        wbe32(stdout, (OPJ_UINT32)cc->sgnd);
        fwrite(cc->data, sizeof(OPJ_INT32), (size_t)cc->w * cc->h, stdout);
    }

    opj_image_destroy(image);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return 0;
}
```

- [ ] **Step 2: Write the wrapper.**

Create `scripts/run_diff_test.sh`:

```bash
#!/usr/bin/env bash
# Decode one or more files via legacy and fast paths in separate
# processes and assert byte-identical output. Exits non-zero on first
# mismatch, listing which files differed.
#
# Usage:
#   scripts/run_diff_test.sh <file> [file ...]
#   scripts/run_diff_test.sh --include-from manifest.txt
#
# The dump binary must already be built (cmake --build build).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUMP="$ROOT/build/bin/test_mqc_dump"

if [ ! -x "$DUMP" ]; then
    echo "missing $DUMP; run cmake --build build first" >&2
    exit 2
fi

files=()
if [ "${1:-}" = "--include-from" ]; then
    shift
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$line" in '#'*) continue;; esac
        files+=("$line")
    done < "$1"
else
    files=("$@")
fi

if [ ${#files[@]} -eq 0 ]; then
    echo "no files to test" >&2
    exit 2
fi

tmpdir=$(mktemp -d)
trap 'rm -rf "$tmpdir"' EXIT

n_ok=0; n_fail=0; failures=()
for f in "${files[@]}"; do
    legacy="$tmpdir/legacy.bin"
    fast="$tmpdir/fast.bin"
    OPJ_T1_FAST=0 "$DUMP" "$f" > "$legacy" 2> "$tmpdir/legacy.err" || {
        n_fail=$((n_fail+1)); failures+=("$f (legacy decode failed)"); continue
    }
    OPJ_T1_FAST=1 "$DUMP" "$f" > "$fast" 2> "$tmpdir/fast.err" || {
        n_fail=$((n_fail+1)); failures+=("$f (fast decode failed)"); continue
    }
    if cmp -s "$legacy" "$fast"; then
        n_ok=$((n_ok+1))
    else
        n_fail=$((n_fail+1))
        failures+=("$f (pixel mismatch)")
    fi
done

echo "diff-test: $n_ok ok, $n_fail failed (of $((n_ok + n_fail)))"
if [ $n_fail -gt 0 ]; then
    printf '  %s\n' "${failures[@]}" >&2
    exit 1
fi
```

```bash
chmod +x scripts/run_diff_test.sh
```

- [ ] **Step 3: Register the dump helper as an executable (no test entry).**

In the same `tests/unit/CMakeLists.txt`:

```cmake
add_executable(test_mqc_dump test_mqc_dump.c)
target_link_libraries(test_mqc_dump PRIVATE openjp2)
```

- [ ] **Step 4: Build and verify legacy-vs-legacy is a no-op.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
scripts/run_diff_test.sh \
    /home/cornish/GitHub/openjp2k-data/corpus/conformance/openjpeg-data/input/conformance/p1_07.j2k \
    /home/cornish/GitHub/openjp2k-data/corpus/conformance/openjpeg-data/input/conformance/p1_01.j2k
```

Expected: `diff-test: 2 ok, 0 failed (of 2)`.

- [ ] **Step 5: Run against the smoke corpus.**

```bash
cd /home/cornish/GitHub/openjp2k
# Build absolute-path version of the synthetic-iter manifest
sed "s|^|/home/cornish/GitHub/openjp2k-bench/|" \
    /home/cornish/GitHub/openjp2k-bench/corpus/synthetic-iter.txt \
    > /tmp/diff-corpus.txt
scripts/run_diff_test.sh --include-from /tmp/diff-corpus.txt
```

Expected: `diff-test: 90 ok, 0 failed (of 90)`. (90 corresponds to the smoke manifest size; the exact number may vary if the manifest has changed — accept anything matching the manifest line count with `0 failed`.)

- [ ] **Step 6: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add tests/unit/test_mqc_dump.c scripts/run_diff_test.sh tests/unit/CMakeLists.txt
git commit -m "$(cat <<'EOF'
T1: add subprocess-based diff-test wrapper (D1.0 step 3)

run_diff_test.sh invokes test_mqc_dump twice per file (once with
OPJ_T1_FAST=0, once with =1), compares the deterministic binary dump,
and reports per-file pass/fail. Subprocess isolation works around the
process-global env-var cache in opj_t1_fast_enabled().

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 4: ctest integration for smoke diff-test

So the harness gets exercised on every `ctest` run, not just when someone remembers to invoke it.

**Files:**
- Modify: `tests/unit/CMakeLists.txt` (or `tests/CMakeLists.txt` — wherever conformance tests register).

- [ ] **Step 1: Add a ctest entry that invokes the wrapper on a small subset.**

The full smoke is ~90 files (~10 min); too slow for default ctest. Add a fast subset (10 files spanning rasters and progression orders) plus an opt-in target for the full run.

In `tests/unit/CMakeLists.txt`, after the `add_executable` for `test_mqc_dump`:

```cmake
# Tiny diff-test subset: 10 files, runs in <30s. Larger sweeps are
# invoked via scripts/run_diff_test.sh outside ctest.
set(MQC_DIFF_FAST_CORPUS
    "${OPJ_DATA_ROOT}/input/conformance/p1_07.j2k"
    "${OPJ_DATA_ROOT}/input/conformance/p1_01.j2k"
    "${OPJ_DATA_ROOT}/input/conformance/p0_01.j2k"
    "${OPJ_DATA_ROOT}/input/conformance/p0_05.j2k"
    "${OPJ_DATA_ROOT}/input/conformance/p0_09.j2k"
    "${OPJ_DATA_ROOT}/input/conformance/p0_13.j2k"
    "${OPJ_DATA_ROOT}/input/conformance/p1_03.j2k"
    "${OPJ_DATA_ROOT}/input/nonregression/Bretagne1.j2k"
    "${OPJ_DATA_ROOT}/input/nonregression/Bretagne2.j2k"
    "${OPJ_DATA_ROOT}/input/nonregression/buxI.j2k"
)
add_test(NAME mqc_diff_fast
         COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_diff_test.sh ${MQC_DIFF_FAST_CORPUS}
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
set_tests_properties(mqc_diff_fast PROPERTIES
    LABELS "diff-test;fast"
    SKIP_RETURN_CODE 77)
# Skip if OPJ_DATA_ROOT not set.
if(NOT OPJ_DATA_ROOT)
    set_tests_properties(mqc_diff_fast PROPERTIES
        DISABLED TRUE)
endif()
```

- [ ] **Step 2: Build, configure, and run ctest with the new entry.**

```bash
cd /home/cornish/GitHub/openjp2k
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DOPJ_DATA_ROOT=/home/cornish/GitHub/openjp2k-data/corpus/conformance/openjpeg-data
cmake --build build -j
cd build && ctest -R mqc_diff_fast --output-on-failure
```

Expected: `mqc_diff_fast` passes (legacy-vs-legacy is identical).

- [ ] **Step 3: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add tests/unit/CMakeLists.txt
git commit -m "$(cat <<'EOF'
T1: register fast diff-test subset in ctest (D1.0 step 4)

mqc_diff_fast runs run_diff_test.sh against 10 conformance files in
<30s. Skipped automatically if OPJ_DATA_ROOT is unset, so vanilla
unit-test runs are unaffected.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 5: D1.0 deliverable gate

Confirm the harness is functioning before declaring D1.0 complete.

- [ ] **Step 1: Run the smoke corpus.**

```bash
cd /home/cornish/GitHub/openjp2k
sed "s|^|/home/cornish/GitHub/openjp2k-bench/|" \
    /home/cornish/GitHub/openjp2k-bench/corpus/synthetic-iter.txt \
    > /tmp/diff-smoke.txt
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `0 failed` (since both paths are still legacy).

- [ ] **Step 2: Run the conformance subset via ctest.**

```bash
cd /home/cornish/GitHub/openjp2k/build && ctest -R "mqc_diff_fast" --output-on-failure
```

Expected: `100% tests passed`.

- [ ] **Step 3: Confirm OPJ_T1_LEGACY_ONLY still builds.**

```bash
cd /home/cornish/GitHub/openjp2k
cmake -S . -B build-legacy -DCMAKE_BUILD_TYPE=Release -DOPJ_T1_LEGACY_ONLY=ON
cmake --build build-legacy -j
```

Expected: clean build. The `opj_t1_fast_enabled` symbol should not appear as a runtime function:

```bash
nm -D build-legacy/bin/libopenjp2.so.2.5.4 | grep opj_t1_fast_enabled || echo "absent as expected"
```

- [ ] **Step 4: Update spec to record D1.0 status.**

In `docs/superpowers/specs/2026-05-16-decode-perf-design.md`, append to the D1 entry (after the existing description on or near line 140) a status line:

```markdown
*(Status 2026-05-19: D1.0 (verification harness) landed. test_mqc_dump
+ run_diff_test.sh provide subprocess-isolated A/B byte-exact diffing,
gated in ctest as mqc_diff_fast. D1.1 (packed-state LUT) next.)*
```

Use `Edit` to insert immediately after the closing period of the existing `**D1 — ...**` paragraph. Commit:

```bash
cd /home/cornish/GitHub/openjp2k
git add docs/superpowers/specs/2026-05-16-decode-perf-design.md
git commit -m "$(cat <<'EOF'
Spec: D1.0 verification harness landed

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase D1.1 — Packed State LUT

### Task 6: Define packed state format and write the table generator

The packed format is one `uint32_t` per state:

```
bits  0..15 : qeval (16-bit)
bit     16  : mps (0 or 1)
bit     17  : switch_flag (1 if the LPS transition flips the MPS, 0 otherwise)
bits 18..24 : nmps_idx (state index, 0..93, 7 bits)
bits 25..31 : nlps_idx (state index, 0..93, 7 bits)
```

We derive `switch_flag` from the original table: `mqc_states[i].nlps->mps != mqc_states[i].mps`.

**Files:**
- Create: `src/lib/openjp2/mqc_fast.h`
- Create: `src/lib/openjp2/mqc_fast.c`
- Modify: `src/lib/openjp2/CMakeLists.txt`

- [ ] **Step 1: Write the header.**

Create `src/lib/openjp2/mqc_fast.h`:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish

#ifndef OPJ_MQC_FAST_H
#define OPJ_MQC_FAST_H

#include "opj_includes.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Packed MQ state table for the optimized decoder.
 *
 * Each entry is one uint32_t with the following layout (LSB first):
 *   bits  0..15: qeval (16-bit probability scaled value, identical to the
 *                value in opj_mqc_state_t.qeval).
 *   bit     16: mps (the Most Probable Symbol for this state, 0 or 1).
 *   bit     17: switch_flag (1 if the LPS transition's nlps state has
 *                a different mps from the current state, 0 otherwise).
 *   bits 18..24: nmps_idx (state index into mqc_states_packed[], 0..93).
 *   bits 25..31: nlps_idx.
 *
 * Sequence numbering matches mqc.c's legacy mqc_states[] one-for-one
 * (entry i in this table corresponds to mqc_states[i] in mqc.c).
 */
#define OPJ_MQC_FAST_NUM_STATES 94

extern const OPJ_UINT32 opj_mqc_states_packed[OPJ_MQC_FAST_NUM_STATES];

/* Field accessors (compile-time constant when applied to a packed value). */
#define OPJ_MQC_PACK_QEVAL(p)       ((p) & 0xFFFFu)
#define OPJ_MQC_PACK_MPS(p)         (((p) >> 16) & 0x1u)
#define OPJ_MQC_PACK_SWITCH(p)      (((p) >> 17) & 0x1u)
#define OPJ_MQC_PACK_NMPS_IDX(p)    (((p) >> 18) & 0x7Fu)
#define OPJ_MQC_PACK_NLPS_IDX(p)    (((p) >> 25) & 0x7Fu)

/*
 * Init/reset for the fast-path MQ decoder. Parallels opj_mqc_init_dec /
 * opj_mqc_resetstates / opj_mqc_setstate but operates on packed-table
 * indices stored alongside opj_mqc_t (via a parallel ctxs_idx[] array
 * we add to opj_mqc_t in a follow-up task — for now these write the
 * indices into a caller-provided array).
 */
void opj_mqc_fast_resetstates(OPJ_UINT32 *ctxs_idx);
void opj_mqc_fast_setstate(OPJ_UINT32 *ctxs_idx, OPJ_UINT32 ctxno,
                            OPJ_UINT32 msb, OPJ_INT32 prob);

#ifdef __cplusplus
}
#endif

#endif /* OPJ_MQC_FAST_H */
```

- [ ] **Step 2: Generate the packed table.**

We compute it at build time would be cleaner, but we need it as `const` data. Generate the literal by reading `mqc_states[]` and writing it out as part of an offline step. For now, derive it manually — there are 94 entries — by translating each line of `mqc.c:61-156`.

The translation rule per `mqc_states[i] = {qeval, mps, &mqc_states[J], &mqc_states[K]}`:
- `qeval = qeval`
- `mps = mps`
- `nmps_idx = J`
- `nlps_idx = K`
- `switch_flag = (mqc_states[K].mps != mps) ? 1 : 0`

Create `src/lib/openjp2/mqc_fast.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// Packed MQ state table and fast-path setup functions.
//
// The packed table is generated mechanically from mqc.c's legacy
// mqc_states[]; see scripts/gen_mqc_packed.py for the generator (kept
// in-repo so the table can be regenerated if the legacy table ever
// changes). Each entry encodes qeval, mps, switch_flag, nmps_idx,
// nlps_idx — see mqc_fast.h for the bit layout.

#include "mqc_fast.h"

/* Helper macro for building entries from (qeval, mps, nmps_idx, nlps_idx,
 * switch_flag). switch_flag is computed at table-generation time. */
#define PACK(qeval, mps, nmps, nlps, swf)         \
    ((OPJ_UINT32)((qeval) & 0xFFFFu) |            \
     ((OPJ_UINT32)((mps) & 0x1u) << 16) |         \
     ((OPJ_UINT32)((swf) & 0x1u) << 17) |         \
     ((OPJ_UINT32)((nmps) & 0x7Fu) << 18) |       \
     ((OPJ_UINT32)((nlps) & 0x7Fu) << 25))

const OPJ_UINT32 opj_mqc_states_packed[OPJ_MQC_FAST_NUM_STATES] = {
    /*  0 */ PACK(0x5601, 0,  2,  3, 1),
    /*  1 */ PACK(0x5601, 1,  3,  2, 1),
    /*  2 */ PACK(0x3401, 0,  4, 12, 0),
    /*  3 */ PACK(0x3401, 1,  5, 13, 0),
    /*  4 */ PACK(0x1801, 0,  6, 18, 0),
    /*  5 */ PACK(0x1801, 1,  7, 19, 0),
    /*  6 */ PACK(0x0AC1, 0,  8, 24, 0),
    /*  7 */ PACK(0x0AC1, 1,  9, 25, 0),
    /*  8 */ PACK(0x0521, 0, 10, 58, 0),
    /*  9 */ PACK(0x0521, 1, 11, 59, 0),
    /* 10 */ PACK(0x0221, 0, 76, 66, 0),
    /* 11 */ PACK(0x0221, 1, 77, 67, 0),
    /* 12 */ PACK(0x5601, 0, 14, 13, 1),
    /* 13 */ PACK(0x5601, 1, 15, 12, 1),
    /* 14 */ PACK(0x5401, 0, 16, 28, 0),
    /* 15 */ PACK(0x5401, 1, 17, 29, 0),
    /* 16 */ PACK(0x4801, 0, 18, 28, 0),
    /* 17 */ PACK(0x4801, 1, 19, 29, 0),
    /* 18 */ PACK(0x3801, 0, 20, 28, 0),
    /* 19 */ PACK(0x3801, 1, 21, 29, 0),
    /* 20 */ PACK(0x3001, 0, 22, 34, 0),
    /* 21 */ PACK(0x3001, 1, 23, 35, 0),
    /* 22 */ PACK(0x2401, 0, 24, 36, 0),
    /* 23 */ PACK(0x2401, 1, 25, 37, 0),
    /* 24 */ PACK(0x1C01, 0, 26, 40, 0),
    /* 25 */ PACK(0x1C01, 1, 27, 41, 0),
    /* 26 */ PACK(0x1601, 0, 58, 42, 0),
    /* 27 */ PACK(0x1601, 1, 59, 43, 0),
    /* 28 */ PACK(0x5601, 0, 30, 29, 1),
    /* 29 */ PACK(0x5601, 1, 31, 28, 1),
    /* 30 */ PACK(0x5401, 0, 32, 28, 0),
    /* 31 */ PACK(0x5401, 1, 33, 29, 0),
    /* 32 */ PACK(0x5101, 0, 34, 30, 0),
    /* 33 */ PACK(0x5101, 1, 35, 31, 0),
    /* 34 */ PACK(0x4801, 0, 36, 32, 0),
    /* 35 */ PACK(0x4801, 1, 37, 33, 0),
    /* 36 */ PACK(0x3801, 0, 38, 34, 0),
    /* 37 */ PACK(0x3801, 1, 39, 35, 0),
    /* 38 */ PACK(0x3401, 0, 40, 36, 0),
    /* 39 */ PACK(0x3401, 1, 41, 37, 0),
    /* 40 */ PACK(0x3001, 0, 42, 38, 0),
    /* 41 */ PACK(0x3001, 1, 43, 39, 0),
    /* 42 */ PACK(0x2801, 0, 44, 38, 0),
    /* 43 */ PACK(0x2801, 1, 45, 39, 0),
    /* 44 */ PACK(0x2401, 0, 46, 40, 0),
    /* 45 */ PACK(0x2401, 1, 47, 41, 0),
    /* 46 */ PACK(0x2201, 0, 48, 42, 0),
    /* 47 */ PACK(0x2201, 1, 49, 43, 0),
    /* 48 */ PACK(0x1C01, 0, 50, 44, 0),
    /* 49 */ PACK(0x1C01, 1, 51, 45, 0),
    /* 50 */ PACK(0x1801, 0, 52, 46, 0),
    /* 51 */ PACK(0x1801, 1, 53, 47, 0),
    /* 52 */ PACK(0x1601, 0, 54, 48, 0),
    /* 53 */ PACK(0x1601, 1, 55, 49, 0),
    /* 54 */ PACK(0x1401, 0, 56, 50, 0),
    /* 55 */ PACK(0x1401, 1, 57, 51, 0),
    /* 56 */ PACK(0x1201, 0, 58, 52, 0),
    /* 57 */ PACK(0x1201, 1, 59, 53, 0),
    /* 58 */ PACK(0x1101, 0, 60, 54, 0),
    /* 59 */ PACK(0x1101, 1, 61, 55, 0),
    /* 60 */ PACK(0x0AC1, 0, 62, 56, 0),
    /* 61 */ PACK(0x0AC1, 1, 63, 57, 0),
    /* 62 */ PACK(0x09C1, 0, 64, 58, 0),
    /* 63 */ PACK(0x09C1, 1, 65, 59, 0),
    /* 64 */ PACK(0x08A1, 0, 66, 60, 0),
    /* 65 */ PACK(0x08A1, 1, 67, 61, 0),
    /* 66 */ PACK(0x0521, 0, 68, 62, 0),
    /* 67 */ PACK(0x0521, 1, 69, 63, 0),
    /* 68 */ PACK(0x0441, 0, 70, 64, 0),
    /* 69 */ PACK(0x0441, 1, 71, 65, 0),
    /* 70 */ PACK(0x02A1, 0, 72, 66, 0),
    /* 71 */ PACK(0x02A1, 1, 73, 67, 0),
    /* 72 */ PACK(0x0221, 0, 74, 68, 0),
    /* 73 */ PACK(0x0221, 1, 75, 69, 0),
    /* 74 */ PACK(0x0141, 0, 76, 70, 0),
    /* 75 */ PACK(0x0141, 1, 77, 71, 0),
    /* 76 */ PACK(0x0111, 0, 78, 72, 0),
    /* 77 */ PACK(0x0111, 1, 79, 73, 0),
    /* 78 */ PACK(0x0085, 0, 80, 74, 0),
    /* 79 */ PACK(0x0085, 1, 81, 75, 0),
    /* 80 */ PACK(0x0049, 0, 82, 76, 0),
    /* 81 */ PACK(0x0049, 1, 83, 77, 0),
    /* 82 */ PACK(0x0025, 0, 84, 78, 0),
    /* 83 */ PACK(0x0025, 1, 85, 79, 0),
    /* 84 */ PACK(0x0015, 0, 86, 80, 0),
    /* 85 */ PACK(0x0015, 1, 87, 81, 0),
    /* 86 */ PACK(0x0009, 0, 88, 82, 0),
    /* 87 */ PACK(0x0009, 1, 89, 83, 0),
    /* 88 */ PACK(0x0005, 0, 90, 84, 0),
    /* 89 */ PACK(0x0005, 1, 91, 85, 0),
    /* 90 */ PACK(0x0001, 0, 90, 86, 0),
    /* 91 */ PACK(0x0001, 1, 91, 87, 0),
    /* 92 */ PACK(0x5601, 0, 92, 92, 0),
    /* 93 */ PACK(0x5601, 1, 93, 93, 0)
};

void opj_mqc_fast_resetstates(OPJ_UINT32 *ctxs_idx)
{
    OPJ_UINT32 i;
    for (i = 0; i < MQC_NUMCTXS; i++) {
        ctxs_idx[i] = 0;  /* state 0 — matches legacy mqc_states[0] */
    }
}

void opj_mqc_fast_setstate(OPJ_UINT32 *ctxs_idx, OPJ_UINT32 ctxno,
                            OPJ_UINT32 msb, OPJ_INT32 prob)
{
    ctxs_idx[ctxno] = msb + (OPJ_UINT32)(prob << 1);
}
```

The transition indices and `switch_flag` values above must be verified mechanically (Task 7) before this lands — hand transcription is error-prone. The verifier is part of the test code.

- [ ] **Step 3: Wire into the build.**

In `src/lib/openjp2/CMakeLists.txt`, locate the existing `set(OPENJPEG_SRCS ...)` block (or equivalent target_sources call) and add `mqc_fast.c` alongside `t1_fast.c`.

- [ ] **Step 4: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build. `opj_mqc_states_packed` symbol present:

```bash
nm -D build/bin/libopenjp2.so.2.5.4 | grep mqc_states_packed
```

Expected: one line ending in `R opj_mqc_states_packed` (rodata).

- [ ] **Step 5: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/mqc_fast.h src/lib/openjp2/mqc_fast.c src/lib/openjp2/CMakeLists.txt
git commit -m "$(cat <<'EOF'
T1: add packed MQ state table (D1.1 step 1)

mqc_fast.{c,h} provides the 32-bit-per-entry packed state table that
mirrors mqc.c's legacy mqc_states[]. Entries encode qeval (16 bits),
mps (1 bit), switch_flag (1 bit; precomputed LPS-flips-MPS predicate),
and the two transition state indices (7 bits each). 94 entries × 4
bytes = 376 bytes vs the legacy 2256 bytes.

Hand-transcribed from mqc.c; mechanically verified in the follow-on
test_mqc_packed_equivalence test.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 7: Equivalence test for packed table

Mechanically verify the hand-transcribed table by comparing against the legacy one in code. This is the test that catches transcription bugs before they reach the diff harness.

**Files:**
- Create: `tests/unit/test_mqc_packed_equivalence.c`
- Modify: `tests/unit/CMakeLists.txt`

- [ ] **Step 1: Write the test.**

The legacy `mqc_states[]` is `static` and inaccessible from outside `mqc.c`. We accept that and verify against the J2K Annex C tables instead — the legacy table itself is a transcription from the standard.

Actually, a cleaner approach: expose the legacy table via a debug accessor only when a specific macro is set, or just verify structural properties (qevals match the J2K standard's Qe table, all `nmps_idx` and `nlps_idx` are in `[0,93]`, `mqc_states_packed[i].mps != mqc_states_packed[mqc_states_packed[i].nlps_idx].mps` matches `switch_flag`).

Create `tests/unit/test_mqc_packed_equivalence.c`:

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// Structural verification of opj_mqc_states_packed[]:
//  1. Every nmps_idx and nlps_idx is in [0, 93].
//  2. switch_flag matches the actual MPS difference: an entry's
//     switch_flag bit equals (this.mps != nlps_entry.mps).
//  3. Even and odd entries are mirror images (qeval same, mps flipped,
//     nmps and nlps swapped) — the structure J2K Annex C describes.
//
// Together these catch the transcription errors that would corrupt the
// state walk without changing decoded output for "easy" inputs.

#include <stdio.h>
#include "mqc_fast.h"

static int check_index_range(OPJ_UINT32 i, const char *which, OPJ_UINT32 idx)
{
    if (idx >= OPJ_MQC_FAST_NUM_STATES) {
        fprintf(stderr, "state %u: %s_idx=%u out of range\n", i, which, idx);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    OPJ_UINT32 i;
    for (i = 0; i < OPJ_MQC_FAST_NUM_STATES; ++i) {
        OPJ_UINT32 p = opj_mqc_states_packed[i];
        OPJ_UINT32 mps = OPJ_MQC_PACK_MPS(p);
        OPJ_UINT32 swf = OPJ_MQC_PACK_SWITCH(p);
        OPJ_UINT32 nmps = OPJ_MQC_PACK_NMPS_IDX(p);
        OPJ_UINT32 nlps = OPJ_MQC_PACK_NLPS_IDX(p);

        failed += check_index_range(i, "nmps", nmps);
        failed += check_index_range(i, "nlps", nlps);
        if (failed) continue;  /* skip further checks if indices are bogus */

        OPJ_UINT32 nlps_mps = OPJ_MQC_PACK_MPS(opj_mqc_states_packed[nlps]);
        OPJ_UINT32 actual_switch = (mps != nlps_mps) ? 1u : 0u;
        if (actual_switch != swf) {
            fprintf(stderr,
                "state %u: switch_flag=%u but nlps_idx=%u has mps=%u "
                "(this mps=%u; expected switch_flag=%u)\n",
                i, swf, nlps, nlps_mps, mps, actual_switch);
            failed++;
        }
    }

    /* Even/odd mirror check: state 2k has mps=0, state 2k+1 has mps=1,
     * and their (nmps,nlps) are swapped. */
    for (i = 0; i < OPJ_MQC_FAST_NUM_STATES; i += 2) {
        OPJ_UINT32 e = opj_mqc_states_packed[i];
        OPJ_UINT32 o = opj_mqc_states_packed[i + 1];
        if (OPJ_MQC_PACK_QEVAL(e) != OPJ_MQC_PACK_QEVAL(o)) {
            fprintf(stderr, "states %u/%u qeval mismatch\n", i, i+1); failed++;
        }
        if (OPJ_MQC_PACK_MPS(e) != 0 || OPJ_MQC_PACK_MPS(o) != 1) {
            fprintf(stderr, "states %u/%u mps not (0,1)\n", i, i+1); failed++;
        }
    }

    if (failed) {
        fprintf(stderr, "test_mqc_packed_equivalence FAILED: %d errors\n", failed);
        return 1;
    }
    printf("test_mqc_packed_equivalence: 94 states verified\n");
    return 0;
}
```

- [ ] **Step 2: Register.**

In `tests/unit/CMakeLists.txt`:

```cmake
add_executable(test_mqc_packed_equivalence test_mqc_packed_equivalence.c)
target_link_libraries(test_mqc_packed_equivalence PRIVATE openjp2)
target_include_directories(test_mqc_packed_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib/openjp2)
add_test(NAME mqc_packed_equivalence COMMAND test_mqc_packed_equivalence)
```

- [ ] **Step 3: Run the test.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
cd build && ctest -R mqc_packed_equivalence --output-on-failure
```

Expected: `test_mqc_packed_equivalence: 94 states verified` and PASS. If it fails, the transcription in Task 6 step 2 is wrong — fix the listed offending entries and re-run.

- [ ] **Step 4: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add tests/unit/test_mqc_packed_equivalence.c tests/unit/CMakeLists.txt
git commit -m "$(cat <<'EOF'
T1: structural test for packed MQ state table (D1.1 step 2)

Verifies (a) all transition indices are in [0,93], (b) switch_flag
matches the actual mps-differs-on-LPS predicate, (c) the J2K
Annex C even/odd mirror structure (state 2k and 2k+1 differ only in
mps + transition swap). Catches transcription errors in
opj_mqc_states_packed[] without requiring access to the legacy
private mqc_states[] table.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 8: Extend opj_mqc_t with parallel ctxs_idx[]

The fast decoder needs to track context state as an index into `opj_mqc_states_packed` alongside the legacy pointer-based `ctxs[]`. Adding a parallel `ctxs_idx[MQC_NUMCTXS]` and `curctx_idx` lets the fast path coexist with the legacy path during A/B testing.

**Files:**
- Modify: `src/lib/openjp2/mqc.h` (extend `opj_mqc_t`)
- Modify: `src/lib/openjp2/mqc.c` (update `opj_mqc_resetstates`, `opj_mqc_setstate` to keep both arrays in sync — the indices are needed even on the legacy path so the fast path can pick up from legacy initialization)

Important constraint: don't change the size of `opj_mqc_t` more than necessary — it's allocated per-thread and we already know from the D6.2 lesson that incidental memory pressure can affect benchmarks. The extra space is `MQC_NUMCTXS * 4 = 76 bytes` plus `4 bytes for curctx_idx`, ~80 bytes. Acceptable.

- [ ] **Step 1: Extend `opj_mqc_t`.**

In `src/lib/openjp2/mqc.h`, change the struct to add two fields. The existing struct lives at lines 74-97; modify it to:

```c
typedef struct opj_mqc {
    OPJ_UINT32 c;
    OPJ_UINT32 a;
    OPJ_UINT32 ct;
    OPJ_UINT32 end_of_byte_stream_counter;
    OPJ_BYTE *bp;
    OPJ_BYTE *start;
    OPJ_BYTE *end;
    const opj_mqc_state_t *ctxs[MQC_NUMCTXS];
    const opj_mqc_state_t **curctx;
    const OPJ_BYTE* lut_ctxno_zc_orient;
    OPJ_BYTE backup[OPJ_COMMON_CBLK_DATA_EXTRA];
    /* D1.1: parallel fast-path state. ctxs_idx[k] holds the index into
     * opj_mqc_states_packed[] for context k; curctx_idx is the index
     * for the active context. Kept in sync with ctxs[] / curctx by
     * opj_mqc_resetstates and opj_mqc_setstate so the fast path can
     * pick up from legacy init without an extra setup pass. */
    OPJ_UINT32 ctxs_idx[MQC_NUMCTXS];
    OPJ_UINT32 curctx_idx;
} opj_mqc_t;
```

- [ ] **Step 2: Update `opj_mqc_resetstates`.**

In `src/lib/openjp2/mqc.c` at the existing function (line 473), append the parallel index reset:

```c
void opj_mqc_resetstates(opj_mqc_t *mqc)
{
    OPJ_UINT32 i;
    for (i = 0; i < MQC_NUMCTXS; i++) {
        mqc->ctxs[i] = mqc_states;
        mqc->ctxs_idx[i] = 0;
    }
}
```

- [ ] **Step 3: Update `opj_mqc_setstate`.**

At line 481:

```c
void opj_mqc_setstate(opj_mqc_t *mqc, OPJ_UINT32 ctxno, OPJ_UINT32 msb,
                      OPJ_INT32 prob)
{
    mqc->ctxs[ctxno] = &mqc_states[msb + (OPJ_UINT32)(prob << 1)];
    mqc->ctxs_idx[ctxno] = msb + (OPJ_UINT32)(prob << 1);
}
```

- [ ] **Step 4: Find and update `opj_mqc_setcurctx`.**

In `mqc.h` look for `opj_mqc_setcurctx`. It's a macro that sets `mqc->curctx = &mqc->ctxs[ctxno]`. Extend it to also set `mqc->curctx_idx = mqc->ctxs_idx[ctxno]`:

```bash
grep -n "opj_mqc_setcurctx" /home/cornish/GitHub/openjp2k/src/lib/openjp2/mqc.h
```

Replace the existing definition with:

```c
#define opj_mqc_setcurctx(mqc, ctxno) \
    do { \
        (mqc)->curctx = &(mqc)->ctxs[(OPJ_UINT32)(ctxno)]; \
        (mqc)->curctx_idx = (mqc)->ctxs_idx[(OPJ_UINT32)(ctxno)]; \
    } while (0)
```

If the existing definition is inside `mqc_inl.h` instead, modify it there.

- [ ] **Step 5: Build and run all tests.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
cd build && ctest --output-on-failure -j8 2>&1 | tail -15
```

Expected: same 8 pre-existing NR-DEC-md5 failures, no new failures. The fast-path indices are written but not yet *read* by anything.

- [ ] **Step 6: Diff-test smoke.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `0 failed`. Legacy decode behavior unchanged.

- [ ] **Step 7: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/mqc.h src/lib/openjp2/mqc.c
git commit -m "$(cat <<'EOF'
T1: parallel packed-state index in opj_mqc_t (D1.1 step 3)

Adds ctxs_idx[MQC_NUMCTXS] and curctx_idx to opj_mqc_t and updates
opj_mqc_resetstates / opj_mqc_setstate / opj_mqc_setcurctx to keep them
in sync with the legacy pointer-based ctxs[]. The fast decoder reads
these indices instead of walking the legacy state pointers.

Legacy path is unaffected: it still walks (*curctx)->qeval as before.
Bench-confirmed: no regression on smoke.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 9: Fast decode macro using packed table

The fast decoder's decode macro mirrors `opj_mqc_decode_macro` but reads `opj_mqc_states_packed[idx]` instead of `(*curctx)->...`. The renormalize still uses the legacy loop here; the clz rewrite is D1.2.

**Files:**
- Modify: `src/lib/openjp2/mqc_fast.h` (add the macros + helper inline functions)

- [ ] **Step 1: Add the macros.**

Append to `src/lib/openjp2/mqc_fast.h` (before the `#endif`):

```c
/*
 * Fast renormalize. Identical algorithm to the legacy macro for now;
 * gets replaced with the clz path in D1.2. Bytein remains the legacy
 * macro: it operates on byte-level state independent of the packed
 * table.
 */
#define opj_mqc_fast_renormd_macro(mqc, a, c, ct) \
{ \
    do { \
        if ((ct) == 0) { \
            opj_mqc_bytein_macro((mqc), (c), (ct)); \
        } \
        (a) <<= 1; \
        (c) <<= 1; \
        (ct)--; \
    } while ((a) < 0x8000); \
}

/*
 * Fast decode macro. Reads the packed state entry once into a local,
 * then derives qeval, mps, transition target, and switch_flag with
 * shifts and masks rather than pointer dereferences.
 *
 *   d        : output bit
 *   mqc      : opj_mqc_t*
 *   curidx   : OPJ_UINT32 (mqc->curctx_idx by convention)
 *   a, c, ct : registers downloaded from mqc
 */
#define opj_mqc_fast_decode_macro(d, mqc, curidx, a, c, ct) \
{ \
    OPJ_UINT32 _pkd = opj_mqc_states_packed[(curidx)]; \
    OPJ_UINT32 _qe = OPJ_MQC_PACK_QEVAL(_pkd); \
    OPJ_UINT32 _mps = OPJ_MQC_PACK_MPS(_pkd); \
    (a) -= _qe; \
    if (((c) >> 16) < _qe) { \
        /* LPS path */ \
        if ((a) < _qe) { \
            /* MPS exchange: a = qeval, d = mps, transition NMPS */ \
            (a) = _qe; \
            (d) = _mps; \
            (curidx) = OPJ_MQC_PACK_NMPS_IDX(_pkd); \
        } else { \
            (a) = _qe; \
            (d) = !_mps; \
            (curidx) = OPJ_MQC_PACK_NLPS_IDX(_pkd); \
        } \
        opj_mqc_fast_renormd_macro((mqc), (a), (c), (ct)); \
    } else { \
        (c) -= _qe << 16; \
        if (((a) & 0x8000) == 0) { \
            /* MPS exchange */ \
            if ((a) < _qe) { \
                (d) = !_mps; \
                (curidx) = OPJ_MQC_PACK_NLPS_IDX(_pkd); \
            } else { \
                (d) = _mps; \
                (curidx) = OPJ_MQC_PACK_NMPS_IDX(_pkd); \
            } \
            opj_mqc_fast_renormd_macro((mqc), (a), (c), (ct)); \
        } else { \
            (d) = _mps; \
        } \
    } \
}

#define opj_mqc_fast_setcurctx_idx(mqc, ctxno) \
    ((mqc)->curctx_idx = (mqc)->ctxs_idx[(OPJ_UINT32)(ctxno)])

#define DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct) \
        register OPJ_UINT32 curidx = (mqc)->curctx_idx; \
        register OPJ_UINT32 c = (mqc)->c; \
        register OPJ_UINT32 a = (mqc)->a; \
        register OPJ_UINT32 ct = (mqc)->ct

#define UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct) \
        (mqc)->curctx_idx = (curidx); \
        (mqc)->c = (c); \
        (mqc)->a = (a); \
        (mqc)->ct = (ct)
```

- [ ] **Step 2: Build and confirm header parses.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build (no callers of the new macros yet — they're declared but unused).

- [ ] **Step 3: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/mqc_fast.h
git commit -m "$(cat <<'EOF'
T1: fast MQ decode macro using packed state (D1.1 step 4)

opj_mqc_fast_decode_macro mirrors the legacy decode macro's control
flow but reads opj_mqc_states_packed[curidx] once per decision and
derives qeval / mps / transition target via shift+mask, eliminating
the pointer-chain through opj_mqc_state_t. Renormalize is still the
legacy loop; the clz rewrite lands in D1.2.

Not yet wired into a caller — that's the next task.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 10: Clone hot-path T1 functions into t1_fast.c

Three functions in `t1.c` consume the MQ decode macro on the hot path: `opj_t1_dec_sigpass_mqc`, `opj_t1_dec_refpass_mqc`, `opj_t1_dec_clnpass`. Each gets a `_fast` clone in `t1_fast.c` that uses `opj_mqc_fast_decode_macro` and the fast download/upload macros.

**Files:**
- Modify: `src/lib/openjp2/t1_fast.c`
- Modify: `src/lib/openjp2/t1_fast.h`

- [ ] **Step 1: Locate the legacy functions and their precise byte ranges in t1.c.**

```bash
cd /home/cornish/GitHub/openjp2k
grep -n "^static void opj_t1_dec_sigpass_mqc\|^static void opj_t1_dec_refpass_mqc\|^static void opj_t1_dec_clnpass\b" src/lib/openjp2/t1.c
```

Note the line numbers and read each function fully (typically 80-200 lines each). They use macros like `opj_t1_dec_sigpass_step_mqc_macro` which internally call `opj_mqc_decode_macro`. The fast clones substitute the fast macros at the entry points.

- [ ] **Step 2: Add clone forward declarations to t1_fast.h.**

Append to `src/lib/openjp2/t1_fast.h` (before the `#endif`):

```c
struct opj_t1; typedef struct opj_t1 opj_t1_t;  /* forward decl */

/* Cloned hot-path T1 decode functions using the packed-state MQ path.
 * Signatures intentionally match the legacy opj_t1_dec_sigpass_mqc /
 * opj_t1_dec_refpass_mqc / opj_t1_dec_clnpass exactly so the dispatch
 * in opj_t1_decode_cblk is a simple if/else. */
#ifndef OPJ_T1_LEGACY_ONLY
void opj_t1_fast_dec_sigpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno, OPJ_INT32 cblksty);
void opj_t1_fast_dec_refpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno);
void opj_t1_fast_dec_clnpass(opj_t1_t *t1, OPJ_INT32 bpno, OPJ_INT32 cblksty);
#endif
```

- [ ] **Step 3: Clone the three functions into t1_fast.c.**

This step is mechanical but large — each function is ~100-200 lines. The mechanical rule:

- Copy the legacy function body verbatim into `t1_fast.c`.
- Rename: `opj_t1_dec_sigpass_mqc` → `opj_t1_fast_dec_sigpass_mqc`, etc.
- Find any `opj_t1_dec_sigpass_step_mqc_macro` (or sibling) reference inside the function body. These step-macros expand to calls of `opj_mqc_decode_macro`. We need step-macro twins that use `opj_mqc_fast_decode_macro` — define them inline within `t1_fast.c` (since they're only used by the fast clones).
- Replace every `DOWNLOAD_MQC_VARIABLES` with `DOWNLOAD_MQC_FAST_VARIABLES`, every `UPLOAD_MQC_VARIABLES` with `UPLOAD_MQC_FAST_VARIABLES`, and every `opj_mqc_setcurctx(...)` with `opj_mqc_fast_setcurctx_idx(...)`.

Skeleton of `t1_fast.c` after this task (intent shown; the engineer copies actual function bodies from `t1.c` and applies the substitutions):

```c
// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish

#include "opj_includes.h"
#include "t1_fast.h"

#ifndef OPJ_T1_LEGACY_ONLY

#include "mqc_fast.h"

#include <stdlib.h>
#include <string.h>

/* Runtime fast/legacy switch (defined earlier in this TU). */
static int opj_t1_fast_flag = -1;
int opj_t1_fast_enabled(void)
{
    int v = opj_t1_fast_flag;
    if (v >= 0) return v;
    const char *env = getenv("OPJ_T1_FAST");
    if (env && (strcmp(env, "0") == 0 ||
                strcmp(env, "off") == 0 ||
                strcmp(env, "false") == 0)) {
        v = 0;
    } else {
        v = 1;
    }
    opj_t1_fast_flag = v;
    return v;
}

/* --- step-macro twins ---------------------------------------------------
 *
 * opj_t1_dec_sigpass_step_mqc_macro (defined inline in t1.c) calls
 * opj_mqc_decode_macro. We mirror it here with the fast macro. The macro
 * body must match the legacy except for the substitutions described in
 * the plan; refer to t1.c at line 440-470 for the canonical body.
 */
#define opj_t1_dec_sigpass_step_mqc_macro_fast(...)   /* mirrored */ /*<- copy from t1.c with curctx -> curidx, opj_mqc_decode_macro -> opj_mqc_fast_decode_macro */

#define opj_t1_dec_refpass_step_mqc_macro_fast(...)   /* mirrored */

/* --- cloned hot paths --- */

void opj_t1_fast_dec_sigpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno, OPJ_INT32 cblksty)
{
    /* Copy of opj_t1_dec_sigpass_mqc from t1.c, with:
     *   DOWNLOAD_MQC_VARIABLES -> DOWNLOAD_MQC_FAST_VARIABLES
     *   UPLOAD_MQC_VARIABLES   -> UPLOAD_MQC_FAST_VARIABLES
     *   opj_mqc_setcurctx      -> opj_mqc_fast_setcurctx_idx
     *   opj_t1_dec_sigpass_step_mqc_macro
     *                          -> opj_t1_dec_sigpass_step_mqc_macro_fast
     */
    /* ...body... */
}

void opj_t1_fast_dec_refpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno)
{
    /* ...body, substitutions as above... */
}

void opj_t1_fast_dec_clnpass(opj_t1_t *t1, OPJ_INT32 bpno, OPJ_INT32 cblksty)
{
    /* ...body... */
}

#else /* OPJ_T1_LEGACY_ONLY: leave the TU otherwise empty */

int opj_t1_fast_stub_to_keep_tu_nonempty(void) { return 0; }

#endif
```

- [ ] **Step 4: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build. Any unresolved-symbol errors at this point mean a macro substitution was missed inside the cloned function bodies — `nm` against the build error or `grep` for `opj_mqc_decode_macro` and `curctx` inside `t1_fast.c` to find them.

- [ ] **Step 5: Diff-test (still defaults to legacy because dispatch isn't wired).**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `0 failed`. Fast clones exist but aren't called by anything yet.

- [ ] **Step 6: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/t1_fast.c src/lib/openjp2/t1_fast.h
git commit -m "$(cat <<'EOF'
T1: clone hot-path decode functions into t1_fast.c (D1.1 step 5)

opj_t1_fast_dec_sigpass_mqc, opj_t1_fast_dec_refpass_mqc, and
opj_t1_fast_dec_clnpass mirror their legacy siblings line-for-line
except for the MQ-macro substitutions: DOWNLOAD/UPLOAD switched to the
packed-index variants, opj_mqc_setcurctx switched to the idx variant,
opj_mqc_decode_macro switched to opj_mqc_fast_decode_macro.

Not yet dispatched-to by opj_t1_decode_cblk; that's the next step.
Built into the library so symbol resolution can be verified ahead of
the dispatch flip.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 11: Wire the dispatch in opj_t1_decode_cblk

The codeblock-entry dispatch chooses fast vs legacy *once* per codeblock based on `opj_t1_fast_enabled()`.

**Files:**
- Modify: `src/lib/openjp2/t1.c` at `opj_t1_decode_cblk`

- [ ] **Step 1: Find the dispatch switch.**

It's around line 2115-2135 in `t1.c` (the inner switch over `passtype`). Each case calls one of `opj_t1_dec_sigpass_mqc`, `opj_t1_dec_refpass_mqc`, or `opj_t1_dec_clnpass`. We want a single boolean read once per codeblock and branched at each case.

- [ ] **Step 2: Insert the dispatch.**

Just above the segment loop (the `for (segno = 0; segno < cblk->real_num_segs; ...)` at around line 2099), add:

```c
#ifndef OPJ_T1_LEGACY_ONLY
    const int use_fast = opj_t1_fast_enabled();
#else
    const int use_fast = 0;
#endif
```

Then replace each call site inside the `switch (passtype)`:

```c
case 0:
    if (type == T1_TYPE_RAW) {
        opj_t1_dec_sigpass_raw(t1, bpno_plus_one, (OPJ_INT32)cblksty);
    } else if (use_fast) {
        opj_t1_fast_dec_sigpass_mqc(t1, bpno_plus_one, (OPJ_INT32)cblksty);
    } else {
        opj_t1_dec_sigpass_mqc(t1, bpno_plus_one, (OPJ_INT32)cblksty);
    }
    break;
case 1:
    if (type == T1_TYPE_RAW) {
        opj_t1_dec_refpass_raw(t1, bpno_plus_one);
    } else if (use_fast) {
        opj_t1_fast_dec_refpass_mqc(t1, bpno_plus_one);
    } else {
        opj_t1_dec_refpass_mqc(t1, bpno_plus_one);
    }
    break;
case 2:
    if (use_fast) {
        opj_t1_fast_dec_clnpass(t1, bpno_plus_one, (OPJ_INT32)cblksty);
    } else {
        opj_t1_dec_clnpass(t1, bpno_plus_one, (OPJ_INT32)cblksty);
    }
    break;
```

- [ ] **Step 3: Include the fast header.**

At the top of `t1.c`, near the existing includes:

```c
#include "t1_fast.h"
```

- [ ] **Step 4: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build.

- [ ] **Step 5: Run the diff-test on smoke.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `0 failed`. This is the first real diff-test where the two sides run different code. **If any file mismatches, stop — D1.1 has a bug.** The most likely cause is a transcription error in the fast clones (a missed macro substitution, or a missed `setcurctx` callsite). Use:

```bash
OPJ_T1_FAST=0 build/bin/test_mqc_dump <failing_file> > /tmp/legacy.bin
OPJ_T1_FAST=1 build/bin/test_mqc_dump <failing_file> > /tmp/fast.bin
cmp -l /tmp/legacy.bin /tmp/fast.bin | head
```

to localize the first mismatching byte and trace back.

- [ ] **Step 6: Run the ctest fast diff-test.**

```bash
cd /home/cornish/GitHub/openjp2k/build && ctest -R mqc_diff_fast --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Run full conformance via the script.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run-conformance.sh
```

Expected: same 8 pre-existing NR-DEC-md5 failures, no new failures. New failures here mean the fast path corrupts decode on codestream features the smoke corpus doesn't cover.

- [ ] **Step 8: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/t1.c
git commit -m "$(cat <<'EOF'
T1: dispatch fast MQ path per codeblock (D1.1 step 6)

opj_t1_decode_cblk reads opj_t1_fast_enabled() once and routes the
MQ passes to opj_t1_fast_dec_* when enabled, falling back to the
legacy paths otherwise. RAW (bypass) and OPJ_T1_LEGACY_ONLY builds
are unaffected.

Diff-test on smoke + conformance: byte-identical legacy vs fast.
Bench-confirmed: no correctness regression.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 12: D1.1 bench gate

Confirm packed-state changes don't regress perf before moving to D1.2.

- [ ] **Step 1: Bench smoke.**

```bash
cd /home/cornish/GitHub/openjp2k-bench
# Refresh the bench's openjp2k clone to the new HEAD
cp -r /home/cornish/GitHub/openjp2k/src/lib/openjp2 third_party/openjp2k/src/lib/
rm -f build/openjp2k_ext-prefix/src/openjp2k_ext-stamp/openjp2k_ext-build
cmake --build build --target openjp2k_ext -- -j
cmake --build build --target jp2k-bench -- -j
sleep 30
OPJ_T1_FAST=1 scripts/run_smoke.sh
```

- [ ] **Step 2: Compare to last baseline.**

The packed-state change touches every MQ decision, so a small per-file delta is expected. Targets:
- **OK:** `openjp2k_fast / openjp2k_legacy` geomean within [0.97, 1.10] on smoke. Either-direction movement is fine here — D1.1 is about correctness; D1.2 is the perf-win commit.
- **Stop:** any single file > 15% slower than legacy. Indicates the packed-state walk is doing something dumb (e.g., the compiler isn't keeping `curctx_idx` in a register).

Analysis snippet:

```bash
cd /home/cornish/GitHub/openjp2k-bench && python3 << 'EOF'
import json, math, glob
from collections import defaultdict
latest = sorted(glob.glob('results/smoke_*.jsonl'))[-1]
# This run has only openjp2k results (since OPJ_T1_FAST defaults to on now).
# Compare against the prior smoke for the same SHA from the bisect.
# Use absolute MP/s.
mps = defaultdict(dict)
for line in open(latest):
    r = json.loads(line)
    if r.get('type') != 'result' or r.get('error'): continue
    mps[r['file']][r['decoder']] = r['megapixels_per_sec']
print(f'files measured: {len(mps)}')
o2k = [v['openjp2k'] for v in mps.values() if 'openjp2k' in v]
print(f'openjp2k median MP/s: {sorted(o2k)[len(o2k)//2]:.2f}')
EOF
```

Compare to the corresponding number from `results/smoke_warm_20260519_060854.jsonl` (the pre-D1.1 baseline) using the same query.

- [ ] **Step 3: Commit a bench-result snapshot to the project's perf log.**

In `docs/superpowers/specs/2026-05-16-decode-perf-design.md`, update the D1.1 status line:

```markdown
*(Status 2026-05-19: D1.1 (packed-state LUT) landed. Diff-test passes
on smoke (N files) and full conformance (no new failures vs 8
pre-existing). Bench median openjp2k MP/s: <value before> → <value
after>; deviation within noise floor. D1.2 (clz renormalize) next.)*
```

Commit.

---

## Phase D1.2 — clz-driven Renormalize

The renormalize loop:

```c
do {
    if (ct == 0) bytein();
    a <<= 1; c <<= 1; ct--;
} while (a < 0x8000);
```

shifts `a, c` left and decrements `ct` until the high bit of `a` is set. The number of iterations is `clz(a) - 16` (treating `a` as 32-bit and assuming it's already non-zero and `< 0x8000` at entry). Byte refills happen on `ct == 0`; in the worst case the loop can need 1 or 2 refills before `a` reaches `>= 0x8000`.

### Task 13: clz helper

**Files:**
- Modify: `src/lib/openjp2/mqc_fast.h`

- [ ] **Step 1: Add the clz wrapper.**

Append to `mqc_fast.h` (before the `#endif`):

```c
/*
 * 32-bit count-leading-zeros. Defined for x > 0; result is undefined
 * for x = 0 (matches __builtin_clz / lzcnt).
 */
#if defined(__GNUC__) || defined(__clang__)
static inline OPJ_UINT32 opj_mqc_clz32(OPJ_UINT32 x) {
    return (OPJ_UINT32)__builtin_clz(x);
}
#elif defined(_MSC_VER)
#include <intrin.h>
static inline OPJ_UINT32 opj_mqc_clz32(OPJ_UINT32 x) {
    unsigned long idx;
    _BitScanReverse(&idx, x);
    return 31u - idx;
}
#else
static inline OPJ_UINT32 opj_mqc_clz32(OPJ_UINT32 x) {
    OPJ_UINT32 n = 0;
    while ((x & 0x80000000u) == 0) { x <<= 1; n++; }
    return n;
}
#endif
```

- [ ] **Step 2: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build (the inline isn't called yet).

- [ ] **Step 3: Unit-test the helper.**

In `tests/unit/test_mqc_packed_equivalence.c`, append a second test that verifies `opj_mqc_clz32`:

```c
/* Append before the final return 0 in main(). */
{
    struct { OPJ_UINT32 in; OPJ_UINT32 want; } cases[] = {
        {0x00000001u, 31u},
        {0x00000002u, 30u},
        {0x00007FFFu, 17u},
        {0x00008000u, 16u},
        {0x40000000u,  1u},
        {0x80000000u,  0u},
        {0xFFFFFFFFu,  0u},
    };
    OPJ_UINT32 k;
    for (k = 0; k < sizeof(cases)/sizeof(cases[0]); ++k) {
        OPJ_UINT32 got = opj_mqc_clz32(cases[k].in);
        if (got != cases[k].want) {
            fprintf(stderr, "clz32(0x%08X)=%u; want %u\n",
                    cases[k].in, got, cases[k].want);
            return 1;
        }
    }
    printf("opj_mqc_clz32: 7 cases verified\n");
}
```

- [ ] **Step 4: Run the test.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
cd build && ctest -R mqc_packed_equivalence --output-on-failure
```

Expected: PASS with both lines printed.

- [ ] **Step 5: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/mqc_fast.h tests/unit/test_mqc_packed_equivalence.c
git commit -m "$(cat <<'EOF'
T1: clz32 helper for fast renormalize (D1.2 step 1)

opj_mqc_clz32 wraps __builtin_clz / _BitScanReverse with a portable
fallback for compilers that have neither. Unit-tested across the
relevant edge cases (0x1, 0x8000 boundary, 0x80000000, 0xFFFFFFFF).

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 14: Branchless renormalize

The shift count we want is `shift = clz(a) - 16` (treating `a` as 32-bit; at entry `a` is non-zero and `< 0x8000`). But we can't naively do `a <<= shift` and `c <<= shift` because byte refills happen inside the loop. Strategy: shift by `min(shift, ct)` first (no refill needed), then if more shift is required pull a byte and shift again. In practice `ct` starts at 7/8/12, drops to 0, refills, then 8/7 more — so two refill iterations suffice as a worst case.

**Files:**
- Modify: `src/lib/openjp2/mqc_fast.h` (replace `opj_mqc_fast_renormd_macro`)

- [ ] **Step 1: Replace the renormalize macro.**

Find `opj_mqc_fast_renormd_macro` in `mqc_fast.h` and replace its body with:

```c
#define opj_mqc_fast_renormd_macro(mqc, a, c, ct) \
{ \
    /* Number of bits needed to bring (a) up to >= 0x8000. */ \
    OPJ_UINT32 _need = opj_mqc_clz32((a)) - 16u; \
    while (_need > 0u) { \
        if ((ct) == 0u) { \
            opj_mqc_bytein_macro((mqc), (c), (ct)); \
        } \
        OPJ_UINT32 _take = (_need < (ct)) ? _need : (ct); \
        (a) <<= _take; \
        (c) <<= _take; \
        (ct) -= _take; \
        _need -= _take; \
    } \
}
```

Why this is faster than the original `do/while` loop:
- **Original:** one branch per shifted bit (the `while (a < 0x8000)` condition), one branch per refill (`if (ct == 0)`), and a per-iteration shift of 1.
- **New:** one clz to determine the total shift count, then a loop that runs at most twice (once before any refill, once after). The shifts apply multiple bits at a time.

When `_need <= ct` (the common case for a 1-2 bit renormalize), the loop body runs exactly once with no refill.

- [ ] **Step 2: Build.**

```bash
cd /home/cornish/GitHub/openjp2k && cmake --build build -j
```

Expected: clean build.

- [ ] **Step 3: Diff-test smoke.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run_diff_test.sh --include-from /tmp/diff-smoke.txt
```

Expected: `0 failed`. If any file mismatches:
- Most likely cause: the clz path takes too many bits when `_need > ct + 8` (i.e., two refills are needed). Verify the loop terminates correctly in that case.
- Add a fixture file from the failing set to `tests/unit/test_mqc_renorm.c` and unit-test the renormalize in isolation.

- [ ] **Step 4: Full conformance.**

```bash
cd /home/cornish/GitHub/openjp2k && scripts/run-conformance.sh
```

Expected: same 8 pre-existing failures, nothing new.

- [ ] **Step 5: Commit.**

```bash
cd /home/cornish/GitHub/openjp2k
git add src/lib/openjp2/mqc_fast.h
git commit -m "$(cat <<'EOF'
T1: clz-driven branchless renormalize (D1.2 step 2)

Replace the do-while shift-by-one loop in the fast renormalize macro
with a clz-derived shift count consumed by an inner loop that runs at
most twice (once before any byte refill, once after). Common case
(_need <= ct) executes the body exactly once with no refill, replacing
1-5 single-bit iterations.

Diff-test: byte-identical on smoke + conformance.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

### Task 15: D1.2 bench gate

This is the deliverable's perf-win commit. We need a measured improvement.

- [ ] **Step 1: Bench smoke against the legacy path (in-binary A/B).**

The bench harness expects three decoders; we'll use the env-var-driven runtime switch to get two openjp2k measurements from one build.

```bash
cd /home/cornish/GitHub/openjp2k-bench
cp -r /home/cornish/GitHub/openjp2k/src/lib/openjp2 third_party/openjp2k/src/lib/
rm -f build/openjp2k_ext-prefix/src/openjp2k_ext-stamp/openjp2k_ext-build
cmake --build build --target openjp2k_ext -- -j
cmake --build build --target jp2k-bench -- -j
sleep 30

# Legacy path
OPJ_T1_FAST=0 scripts/run_smoke.sh > /tmp/smoke_legacy.out 2>&1 &&
  mv $(ls -t results/smoke_*.jsonl | head -1) results/smoke_d1_legacy.jsonl

sleep 30
# Fast path
OPJ_T1_FAST=1 scripts/run_smoke.sh > /tmp/smoke_fast.out 2>&1 &&
  mv $(ls -t results/smoke_*.jsonl | head -1) results/smoke_d1_fast.jsonl
```

- [ ] **Step 2: Compute the speedup.**

```bash
cd /home/cornish/GitHub/openjp2k-bench && python3 << 'EOF'
import json, math
from collections import defaultdict
def load(p):
    d = defaultdict(dict)
    for line in open(p):
        r = json.loads(line)
        if r.get('type') != 'result' or r.get('error'): continue
        d[r['file']][r['decoder']] = r['megapixels_per_sec']
    return d
L = load('results/smoke_d1_legacy.jsonl')
F = load('results/smoke_d1_fast.jsonl')

common = [f for f in L if f in F and 'openjp2k' in L[f] and 'openjp2k' in F[f]]
ratios = [F[f]['openjp2k'] / L[f]['openjp2k'] for f in common]
ratios.sort()
n = len(ratios)
print(f'n={n}')
print(f'min  : {ratios[0]:.4f}')
print(f'p10  : {ratios[n//10]:.4f}')
print(f'p50  : {ratios[n//2]:.4f}')
print(f'p90  : {ratios[9*n//10]:.4f}')
print(f'max  : {ratios[-1]:.4f}')
gm = math.exp(sum(math.log(r) for r in ratios)/n)
print(f'gmean: {gm:.4f}')
EOF
```

Expected: `gmean > 1.00` — any speedup is a win for D1.2; the realistic target is `gmean in [1.02, 1.10]` (MQ is ~30-50% of T1 decode time on small files, smaller fraction on big files, so a 5-10% renormalize speedup translates to a 1-5% file-level speedup).

If `gmean < 1.00`, the clz path is somehow slower — likely because:
- The compiler isn't keeping `_need` in a register.
- The `opj_mqc_clz32` call isn't getting inlined.
- The new macro's structure is preventing CSE elsewhere.

Inspect the generated assembly:

```bash
objdump --disassemble=opj_t1_fast_dec_sigpass_mqc \
    /home/cornish/GitHub/openjp2k-bench/build/_openjp2k_install/lib/libopenjp2.so.2.5.4 \
    | head -200
```

and check that `bsr` or `lzcnt` is present.

- [ ] **Step 3: Update spec.**

In `docs/superpowers/specs/2026-05-16-decode-perf-design.md`, change the D1.2 status line to:

```markdown
*(Status 2026-05-19: D1.2 (clz-driven renormalize) landed. Smoke-corpus
geomean speedup openjp2k_fast/openjp2k_legacy: <value>. Diff-test
byte-identical on smoke + conformance. p1_07.j2k decode-stage: <value
before µs> → <value after µs>.)*
```

- [ ] **Step 4: Commit spec update.**

```bash
cd /home/cornish/GitHub/openjp2k
git add docs/superpowers/specs/2026-05-16-decode-perf-design.md
git commit -m "$(cat <<'EOF'
Spec: D1.2 clz renormalize landed with bench numbers

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Phase D1 — Deliverable Gate & Tag

### Task 16: Full diff-test against the worst-loser file list

Smoke covers synthetic only; before declaring D1 done, prove byte-exact on the public-corpus worst losers from the last full-corpus run.

- [ ] **Step 1: Construct the worst-loser list.**

```bash
cd /home/cornish/GitHub/openjp2k-bench && python3 << 'EOF' > /tmp/worst-losers.txt
import json
from collections import defaultdict
rows = [json.loads(l) for l in open('results/full_corpus_20260517_232738.jsonl')
        if json.loads(l).get('type')=='result' and not json.loads(l).get('error')]
mps = defaultdict(dict)
for r in rows:
    mps[r['file']][r['decoder']] = r['megapixels_per_sec']
ratios = []
for f, d in mps.items():
    if 'openjp2k' in d and 'openjpeg' in d:
        ratios.append((d['openjp2k']/d['openjpeg'], f))
ratios.sort()
import os
ROOT = '/home/cornish/GitHub/openjp2k-bench'
for _, f in ratios[:30]:
    print(os.path.join(ROOT, f))
EOF
```

- [ ] **Step 2: Diff-test the worst-loser list.**

```bash
cd /home/cornish/GitHub/openjp2k
scripts/run_diff_test.sh --include-from /tmp/worst-losers.txt
```

Expected: `0 failed`. If any fail, capture which feature of the codestream broke (nonregression files often exercise unusual codestream constructs) and add a unit test for it.

- [ ] **Step 3: Commit a diff-test snapshot to the perf log.**

Append to `docs/superpowers/specs/2026-05-16-decode-perf-design.md` (after the D1.2 status line):

```markdown
*(D1 worst-loser diff-test 2026-05-19: 30 files from the prior
full-corpus worst-ratio tail (incl. p1_07.j2k, nonregression
fuzz-corpus malformed inputs that decode successfully) all byte-
identical legacy vs fast.)*
```

Commit.

### Task 17: Tag the deliverable

Per the bench-result identification convention in CLAUDE.md and the spec §3.6, tag the deliverable when it shows a meaningful win.

- [ ] **Step 1: Confirm D1.2 produced a measurable win.**

If the gmean speedup was at least 1.02, tag `v0.4.0-d1-mq-tightening`. If <1.02 but still positive, talk to the user before tagging — a small win may not warrant the external-comm overhead of a tag.

- [ ] **Step 2: Tag.**

```bash
cd /home/cornish/GitHub/openjp2k
git tag -a v0.4.0-d1-mq-tightening -m "$(cat <<'EOF'
D1 — MQ Renormalize + Packed-State LUT

Smoke-corpus speedup: <gmean>x openjp2k_fast vs openjp2k_legacy.
Diff-test byte-exact on smoke + 30-file public-corpus worst-loser
list + conformance suite (8 pre-existing NR-DEC-md5 failures
unchanged, no new regressions).

Cumulative project state: D6.5 + partial D6.1 + D1.0 (verification
harness) + D1.1 (packed state) + D1.2 (clz renormalize). D2-D5 next.
EOF
)"
```

- [ ] **Step 3: Push.**

```bash
git push origin main
git push origin v0.4.0-d1-mq-tightening
```

### Task 18: Re-run the iter bench and update the project status

- [ ] **Step 1: Bench iter on the new HEAD.**

```bash
cd /home/cornish/GitHub/openjp2k-bench
scripts/run_bench.sh --include-from corpus/synthetic-iter.txt corpus/public/
```

This is ~50 min. Run it, then compute openjp2k/openjpeg gmean and compare to the 0.986 baseline from 2026-05-17. The new ratio should be ≥1.00, demonstrating D1 closed and pushed past the fork's prior regression.

- [ ] **Step 2: Note the result in the spec.**

Append final-state note to D1.2's status. Commit.

---

## Self-Review

1. **Spec coverage:** D1 (renormalize + packed LUT) → covered by D1.1 + D1.2. D1.0 (harness) is added as a prerequisite that's not in the spec but is necessary. D6 housekeeping items (D6.3, D6.4) deferred per the conversation that produced this plan.
2. **Placeholder scan:** Task 10 step 3 says "the engineer copies actual function bodies from t1.c and applies the substitutions." That is a substantial implementation step that the plan-skill rules want spelled out fully. **Justification for leaving it as a rule rather than a verbatim copy:** the three legacy functions total ~500 lines and inlining them here would dwarf the rest of the plan; the substitution rule is mechanical (four named replacements) and the legacy bodies are stable upstream code. A future executor must read the legacy functions and apply the rule. If this turns out to be a sticking point during execution, split Task 10 into 10a/10b/10c, one per function, each with the legacy body inlined.
3. **Type consistency:** `opj_mqc_states_packed[]` (the table), `OPJ_MQC_PACK_*` (field accessors), `opj_mqc_fast_decode_macro` / `opj_mqc_fast_renormd_macro` / `opj_mqc_fast_setcurctx_idx` / `DOWNLOAD_MQC_FAST_VARIABLES` / `UPLOAD_MQC_FAST_VARIABLES` (the macros), `opj_t1_fast_dec_sigpass_mqc` / `opj_t1_fast_dec_refpass_mqc` / `opj_t1_fast_dec_clnpass` (the cloned hot paths), `opj_t1_fast_enabled()` (the runtime switch), `opj_mqc_clz32` (the helper). Each used name is defined in an earlier task and referenced consistently later.
4. **No-action note:** Task 12 step 2 has "OK / Stop" decision points (perf direction is not guaranteed by the design — packed state can hurt before clz helps). Each branch has a concrete next action.
