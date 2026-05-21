# D2 Reframed — Post-D1 Decode Profile

## Goal

Identify the next real bottleneck in the post-D1 fast MQ decoder path and output a concrete recommendation for the next perf deliverable.

Background: D1 (MQ renormalize + packed-state LUT) shipped as parity-with-legacy infrastructure rather than a perf win. The clz renormalize materialized in the binary but did not move wall-clock time; the assumed bottleneck wasn't the actual bottleneck. Before launching D2 as written in the original spec ("function-pointer dispatch parameterized by codeblock style"), we want measurement to inform the choice — that spec text doesn't map cleanly to T1's actual hot path (the inner loop is already specialized on the bits that matter).

This deliverable is investigative, not implementation. The output is a report + a recommendation, not code.

## Methodology

### 1. Enable kernel perf access

```sh
sudo sysctl kernel.perf_event_paranoid=1
# restore at end:
sudo sysctl kernel.perf_event_paranoid=4
```

Confirm via `perf stat -- /bin/true` before the runs.

### 2. Pick a 5-file representative subset

Each chosen to cover one axis the corpus varies on:

| file | covers |
|---|---|
| `corpus/synthetic/rgb8_1024/pLRCP_d5_b64_t1024x1024_lossless_l1_mon_enone.jp2` | Simple-cblksty inner loop, 5 decomp levels, lossless 9-7 + MQ |
| `corpus/public/wsi-tiles/240x240/TC_031_0010__L1_c032r016.j2k` | Small-codeblock, WSI-domain access pattern |
| `corpus/public/archival/loc-maps/01964_1912-0001.jp2` | Multi-segment cblks, large file, real-world |
| One DICOM mono16 lossless | 16-bit precision path (specific file to be picked at run time from `corpus/public/medical/` that decodes successfully) |
| `corpus/public/conformance/openjpeg-data/input/conformance/p1_07.j2k` | Tiny file (dispatch + setup overhead visible) |

If a file isn't available, substitute the closest sibling and note it in the report.

### 3. Run perf record on each file

```sh
perf record -e cycles --call-graph dwarf -g -F 9999 -o /tmp/perf-<label>.data \
  -- jp2k-bench --iters 200 --warmup 20 --threads 1 --decoder openjp2k <file>
perf report --no-children --stdio --input /tmp/perf-<label>.data \
  > /tmp/perf-<label>.report.txt
```

`--iters 200 --warmup 20` gives enough samples for short files. The `--threads 1` keeps profiling clean. `--call-graph dwarf` lets `perf report` resolve inlined macro frames.

### 4. Cross-check stage attribution

For each file, also run:

```sh
jp2k-bench --iters 200 --warmup 20 --threads 1 --decoder openjp2k --profile-stages --jsonl <file>
```

Confirms the perf-observed time is in the `decode` stage, not `setup`/`unpack`/`teardown`.

### 5. Source-annotate the top hotspots

For each file's top 2-3 self-time functions:

```sh
perf annotate --stdio --input /tmp/perf-<label>.data --symbol <func>
```

Extract the 5-10 hottest source/assembly lines per function.

## Output Artifact

A markdown report at `docs/superpowers/profile/2026-05-21-post-d1-decode-profile.md` with these sections:

1. **Setup confirmation** — perf event paranoid value, jp2k-bench commit/SHA used, build flags.
2. **Per-file top-N table** — for each of the 5 files: top 15 functions by self-time %, total cycles, samples count.
3. **Cross-file synthesis** — which functions appear in everyone's top-5? Which file has unique hot spots? Workload-specific vs universal bottlenecks.
4. **Annotated hot lines** — the source/assembly excerpts that dominate cycles in the top 2-3 functions.
5. **Recommendation** — one named deliverable for the next sprint, with:
   - Which function/loop it targets
   - Expected magnitude of win (rough, based on what % of cycles the hotspot represents)
   - Whether it aligns with an existing spec item (D3 sign-table, D7 multi-buffer MQ, D8 pre-shifted LUT, Sub-project 3 IDWT) or is novel

## Scope Boundaries

- Single-threaded only. Multi-thread profiling is Sub-project 2 territory.
- No code changes in this deliverable. If the profile reveals an obvious cheap fix (like the D1.3 GOT-indirection finding), record it in the report but defer the fix to the next deliverable's plan.
- 5 files is the limit. Expanding to the full iter corpus drowns out per-file detail without buying more cross-workload signal.

## Decision Gate

After the report lands, present the recommendation to the user. The next deliverable's spec + plan only get written after the user approves the recommendation.
