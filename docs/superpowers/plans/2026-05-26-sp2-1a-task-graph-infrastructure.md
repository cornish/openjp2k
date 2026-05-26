# SP-2.1a — Task-Graph Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the SP-2.1 infrastructure: vendor Taskflow, add the `task_graph` C-ABI module that wraps it, add the `OPJ_ENABLE_TASK_GRAPH` cmake option + `OPJ_DAG` runtime env var, and add the `opj_dispatch_job` abstraction in `tcd.c` that routes per-cblk and per-row work either through the existing `opj_thread_pool` (when `OPJ_DAG=0` or `OPJ_ENABLE_TASK_GRAPH=OFF`) or through Taskflow's executor (when both are enabled). **No DAG topology is built yet** — the Taskflow executor runs the same jobs the existing pool would, just through a different worker pool. This sets the stage for SP-2.1b (T2→T1 DAG edges) without changing decode behavior.

**Architecture:** `task_graph.h` declares an opaque `opj_tg_t` plus `_create/_destroy/_add_task/_add_dep/_run/_enabled` functions. `task_graph.cpp` implements them with Taskflow. `tcd.c` gains an `opj_dispatch_job` helper. t1.c and dwt.c switch their job dispatch sites to the helper.

**Tech Stack:** C99 (caller side) + C++17 (Taskflow wrapper), Taskflow v3.7.x single-header, CMake, libstdc++.

---

## File Structure

**Added:**
- `third_party/taskflow/taskflow.hpp` (~3000 LOC) — vendored single-header Taskflow at a pinned version. License: MIT. Add a `LICENSE.taskflow` file alongside it for distribution clarity.
- `src/lib/openjp2/task_graph.h` (~60 LOC) — C ABI: `opj_tg_t`, `opj_tg_create`, `opj_tg_destroy`, `opj_tg_add_task`, `opj_tg_add_dep`, `opj_tg_run`, `opj_tg_enabled`.
- `src/lib/openjp2/task_graph.cpp` (~250 LOC) — Taskflow wrapper behind the C ABI. Compiled as C++17 only when `OPJ_ENABLE_TASK_GRAPH=ON`.

**Modified:**
- `src/lib/openjp2/CMakeLists.txt` — add `OPJ_ENABLE_TASK_GRAPH` option (default ON), conditionally include `task_graph.cpp` in sources, enable C++17 for that file, link libstdc++. ~20 LOC delta.
- `src/lib/openjp2/tcd.c` — add `opj_dispatch_job` helper that picks between Taskflow and the legacy pool. ~40 LOC delta.
- `src/lib/openjp2/tcd.h` — declare `opj_dispatch_job`. ~5 LOC delta.
- `src/lib/openjp2/t1.c` — replace the existing `opj_thread_pool_submit_job` call in `opj_t1_decode_cblks` with `opj_dispatch_job`. ~5 LOC delta.
- `src/lib/openjp2/dwt.c` — same swap at the per-row/per-stripe dispatch sites. ~10 LOC delta.

**Not modified:** t2.c, mct.c, mqc.c, openjpeg.h, any other file.

---

## Task 1: Vendor Taskflow + add task_graph module (atomic commit)

Build the new C++ module and confirm it compiles and runs in isolation before any caller uses it. The legacy path is unchanged; the new module is added but unreferenced from production code.

**Files:**
- Create: `third_party/taskflow/taskflow.hpp`
- Create: `third_party/taskflow/LICENSE.taskflow`
- Create: `src/lib/openjp2/task_graph.h`
- Create: `src/lib/openjp2/task_graph.cpp`
- Modify: `src/lib/openjp2/CMakeLists.txt`

### Step 1: Vendor Taskflow at v3.7.0

```bash
cd /home/cornish/GitHub/openjp2k
mkdir -p third_party/taskflow
curl -fsSL https://raw.githubusercontent.com/taskflow/taskflow/v3.7.0/taskflow/taskflow.hpp -o third_party/taskflow/taskflow.hpp
curl -fsSL https://raw.githubusercontent.com/taskflow/taskflow/v3.7.0/LICENSE -o third_party/taskflow/LICENSE.taskflow
ls -la third_party/taskflow/
```

Expected: `taskflow.hpp` is one big single-header file (~3000-5000 LOC) and `LICENSE.taskflow` contains the MIT license.

**Verification:** the file `#include`s nothing unexpected (only STL headers). Quick sanity check:

```bash
head -5 third_party/taskflow/taskflow.hpp
grep -c '^#include' third_party/taskflow/taskflow.hpp
```

### Step 2: Create `src/lib/openjp2/task_graph.h`

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Toby Cornish
 *
 * Task-graph C ABI for openjp2k.  Wraps Taskflow (C++17, MIT) so the
 * C decoder orchestration in tcd.c can build per-tile DAGs without
 * adopting C++ throughout.  Implementation is in task_graph.cpp.
 *
 * When OPJ_ENABLE_TASK_GRAPH is OFF at build time, this header is
 * not included and the legacy barrier-synchronized path is the only
 * available decoder orchestration.
 */

#ifndef OPJ_TASK_GRAPH_H
#define OPJ_TASK_GRAPH_H

#ifdef OPJ_ENABLE_TASK_GRAPH

#ifdef __cplusplus
extern "C" {
#endif

typedef struct opj_tg opj_tg_t;
typedef int opj_tg_task_id;
typedef void (*opj_tg_task_fn)(void* arg);

/* Create a task graph with num_threads workers.  0 = hardware
 * concurrency.  Returns NULL on allocation failure. */
opj_tg_t* opj_tg_create(int num_threads);
void      opj_tg_destroy(opj_tg_t* tg);

/* Add a task; returns an opaque handle stable for the lifetime of
 * the graph. */
opj_tg_task_id opj_tg_add_task(opj_tg_t* tg, opj_tg_task_fn fn, void* arg);

/* Declare succ runs only after pred completes. */
void opj_tg_add_dep(opj_tg_t* tg, opj_tg_task_id pred, opj_tg_task_id succ);

/* Execute the graph; blocks until all tasks complete.  Returns
 * OPJ_TRUE on success. */
OPJ_BOOL opj_tg_run(opj_tg_t* tg);

/* Reset for a new graph in the same executor (avoids per-tile
 * allocation churn).  Drops all tasks and edges. */
void opj_tg_reset(opj_tg_t* tg);

/* Runtime gate: returns 1 unless OPJ_DAG=0/off/false in the
 * environment.  Cached after first call.  Mirrors
 * opj_t1_fast_enabled() in t1_fast.c. */
int opj_tg_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* OPJ_ENABLE_TASK_GRAPH */

#endif /* OPJ_TASK_GRAPH_H */
```

### Step 3: Create `src/lib/openjp2/task_graph.cpp`

```cpp
/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Toby Cornish
 *
 * Taskflow-backed implementation of the task-graph C ABI declared
 * in task_graph.h.  Compiled only when OPJ_ENABLE_TASK_GRAPH=ON.
 *
 * Single-source-of-truth for thread-pool ownership inside the
 * decoder: when the runtime OPJ_DAG gate is on, this executor
 * drives all worker jobs (T1 cblks, DWT rows/cols); the legacy
 * opj_thread_pool stays dormant for that decode call.
 */

#ifdef OPJ_ENABLE_TASK_GRAPH

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "taskflow/taskflow.hpp"

extern "C" {
#include "opj_includes.h"
#include "task_graph.h"
}

struct opj_tg {
    tf::Taskflow flow;
    tf::Executor executor;
    std::vector<tf::Task> tasks;

    explicit opj_tg(int num_threads)
        : executor(num_threads > 0
                   ? static_cast<unsigned>(num_threads)
                   : std::thread::hardware_concurrency()) {}
};

extern "C" {

opj_tg_t* opj_tg_create(int num_threads) {
    try {
        return new opj_tg(num_threads);
    } catch (...) {
        return nullptr;
    }
}

void opj_tg_destroy(opj_tg_t* tg) {
    delete tg;
}

opj_tg_task_id opj_tg_add_task(opj_tg_t* tg, opj_tg_task_fn fn, void* arg) {
    tg->tasks.push_back(tg->flow.emplace([fn, arg]{ fn(arg); }));
    return static_cast<opj_tg_task_id>(tg->tasks.size() - 1);
}

void opj_tg_add_dep(opj_tg_t* tg, opj_tg_task_id pred, opj_tg_task_id succ) {
    tg->tasks[static_cast<size_t>(pred)].precede(tg->tasks[static_cast<size_t>(succ)]);
}

OPJ_BOOL opj_tg_run(opj_tg_t* tg) {
    try {
        tg->executor.run(tg->flow).wait();
        return OPJ_TRUE;
    } catch (...) {
        return OPJ_FALSE;
    }
}

void opj_tg_reset(opj_tg_t* tg) {
    tg->flow.clear();
    tg->tasks.clear();
}

/* Cached env-var read.  -1 = uninitialized. */
static std::atomic<int> g_tg_flag{-1};

int opj_tg_enabled(void) {
    int v = g_tg_flag.load(std::memory_order_relaxed);
    if (v >= 0) return v;
    const char* env = std::getenv("OPJ_DAG");
    if (env && (std::strcmp(env, "0") == 0 ||
                std::strcmp(env, "off") == 0 ||
                std::strcmp(env, "false") == 0)) {
        v = 0;
    } else {
        v = 1;
    }
    g_tg_flag.store(v, std::memory_order_relaxed);
    return v;
}

}  /* extern "C" */

#endif  /* OPJ_ENABLE_TASK_GRAPH */
```

### Step 4: Update `src/lib/openjp2/CMakeLists.txt`

```bash
grep -n 'add_library.*openjp2\|target_sources\|set.*OPENJPEG_SRCS\|option.*OPJ_' src/lib/openjp2/CMakeLists.txt | head -10
```

Find the source-list block (likely a `set(OPENJPEG_SRCS ...)` or direct `add_library` argument list). Add:

```cmake
option(OPJ_ENABLE_TASK_GRAPH
       "Build openjp2 with the Taskflow-based DAG decoder orchestration (requires C++17 + libstdc++)"
       ON)

if(OPJ_ENABLE_TASK_GRAPH)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    list(APPEND OPENJPEG_SRCS task_graph.cpp)
    set_source_files_properties(task_graph.cpp PROPERTIES
        COMPILE_FLAGS "-DOPJ_ENABLE_TASK_GRAPH"
        LANGUAGE CXX)
    target_compile_definitions(openjp2 PRIVATE OPJ_ENABLE_TASK_GRAPH)
    target_include_directories(openjp2 PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
endif()
```

The exact integration point depends on the file's existing structure — look for a similar `option()` (e.g., the `OPJ_ENABLE_AVX2` from SP3.1) and mirror its shape. If the file uses `add_library(openjp2 ${SRCS})` directly, the conditional append to SRCS pattern works.

### Step 5: Build AVX2-ON + TASK_GRAPH-ON, confirm clean

```bash
cd /home/cornish/GitHub/openjp2k
cmake -S . -B build-sp21a -DCMAKE_BUILD_TYPE=Release \
      -DOPJ_ENABLE_AVX2=ON -DOPJ_ENABLE_TASK_GRAPH=ON -DBUILD_TESTING=ON 2>&1 | tail -5
cmake --build build-sp21a -j 2>&1 | tail -5
```

Expected: clean build. C++ files compiled with `-std=c++17`. `task_graph.cpp` shows up in build output. No warnings.

If `-Wno-pedantic` or similar isn't propagated to C++ files, you may see Taskflow-internal warnings. Suppress at the per-file level rather than globally — Taskflow's own code is third-party.

### Step 6: Build TASK_GRAPH-OFF, confirm pure-C path still works

```bash
cmake -S . -B build-sp21a-pure-c -DCMAKE_BUILD_TYPE=Release \
      -DOPJ_ENABLE_AVX2=ON -DOPJ_ENABLE_TASK_GRAPH=OFF -DBUILD_TESTING=ON 2>&1 | tail -5
cmake --build build-sp21a-pure-c -j 2>&1 | tail -5
```

Expected: clean build, `task_graph.cpp` not compiled (no .o file appears in build output), no libstdc++ dependency. Verify:

```bash
ldd build-sp21a-pure-c/bin/libopenjp2.so* | grep -i stdc++ && echo "FAIL: libstdc++ leaked into pure-C build" || echo "OK: no libstdc++"
```

(The shared library path may vary by platform — adjust if needed.)

### Step 7: Conformance on TASK_GRAPH-ON build

```bash
scripts/run-conformance.sh -B build-sp21a -- -R NR-DEC -j4 2>&1 | tail -12
```

Expected: 545/553, exactly the same 8 pre-existing NR-DEC-md5 failures. No new mismatches. SP-2.1a doesn't touch any decode path yet — the new module is unreferenced from production code, so conformance MUST be unchanged.

### Step 8: Quick standalone task_graph sanity test

Verify the module actually works in isolation. Create a tiny test inline (don't commit it — just for confidence):

```bash
cat > /tmp/_tg_test.cpp <<'EOF'
extern "C" {
#include <stdio.h>
typedef int OPJ_BOOL;
#include "src/lib/openjp2/task_graph.h"
}
static int counter = 0;
static void incr(void* arg) { (void)arg; __atomic_add_fetch(&counter, 1, __ATOMIC_RELAXED); }
int main() {
    opj_tg_t* tg = opj_tg_create(4);
    auto a = opj_tg_add_task(tg, incr, nullptr);
    auto b = opj_tg_add_task(tg, incr, nullptr);
    auto c = opj_tg_add_task(tg, incr, nullptr);
    opj_tg_add_dep(tg, a, b);
    opj_tg_add_dep(tg, b, c);
    opj_tg_run(tg);
    opj_tg_destroy(tg);
    printf("counter=%d enabled=%d\n", counter, opj_tg_enabled());
    return counter == 3 ? 0 : 1;
}
EOF
g++ -std=c++17 -DOPJ_ENABLE_TASK_GRAPH -I third_party -I src/lib/openjp2 /tmp/_tg_test.cpp \
    build-sp21a/src/lib/openjp2/CMakeFiles/openjp2.dir/task_graph.cpp.o \
    -lpthread -o /tmp/_tg_test && /tmp/_tg_test && echo OK
```

Expected: `counter=3 enabled=1`, exit 0.

### Step 9: Stage and commit

```bash
git add third_party/taskflow/ src/lib/openjp2/task_graph.h src/lib/openjp2/task_graph.cpp src/lib/openjp2/CMakeLists.txt
git status --short
git diff --cached --stat
git commit -m "$(cat <<'EOF'
SP-2.1a: vendor Taskflow + task_graph C ABI module

Lands the SP-2.1 infrastructure: Taskflow v3.7.0 vendored at
third_party/taskflow/, plus a task_graph.{h,cpp} module that wraps
it behind a C ABI usable from tcd.c.  Gated by the
OPJ_ENABLE_TASK_GRAPH cmake option (default ON, mirrors SP3.1's
OPJ_ENABLE_AVX2 pattern).  Runtime gate via OPJ_DAG env var
(default 1, mirrors D7's OPJ_T1_FAST).

The module is unreferenced from production code in this commit;
conformance is unchanged.  SP-2.1b will add the first caller
(T2->T1 DAG edges).

Taskflow is MIT-licensed (LICENSE.taskflow checked in alongside);
license-compatible with our Apache-2.0 + BSD-2 mix.

OPJ_ENABLE_TASK_GRAPH=OFF builds remain pure C; no libstdc++
dependency in that configuration.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add `opj_dispatch_job` abstraction; switch t1.c + dwt.c callers (atomic commit)

Behavior change: when `OPJ_DAG=1` (default) and `OPJ_ENABLE_TASK_GRAPH=ON`, per-cblk and per-row jobs are dispatched through Taskflow's executor instead of the legacy `opj_thread_pool`. The legacy pool stays dormant for the decode. Decode output and conformance are unchanged because jobs are still independent (no DAG topology built yet — Taskflow runs them as a flat soup of parallel tasks).

Why this is an interesting commit by itself: it's the first place Taskflow actually runs decode work. Bench gate at this step should show **parity** — Taskflow's executor and the existing thread pool should perform within ±1% for "flat parallel jobs". If we see a regression here, fix it before stacking DAG edges on top.

**Files:**
- Modify: `src/lib/openjp2/tcd.h`
- Modify: `src/lib/openjp2/tcd.c`
- Modify: `src/lib/openjp2/t1.c`
- Modify: `src/lib/openjp2/dwt.c`

### Step 1: Read the existing dispatch sites

```bash
grep -n 'opj_thread_pool_submit_job\|opj_thread_pool_wait_completion' src/lib/openjp2/t1.c src/lib/openjp2/dwt.c src/lib/openjp2/tcd.c | head -30
```

The interesting submit sites are:
- `t1.c:~1897` — `opj_thread_pool_submit_job(tp, opj_t1_clbl_decode_processor, job);` inside `opj_t1_decode_cblks`.
- `dwt.c` — `opj_thread_pool_submit_job(...)` calls for per-row DWT-H and per-stripe DWT-V jobs.

The wait sites (where we block until all submitted jobs complete) are paired with these. Each `wait_completion` becomes the implicit barrier.

### Step 2: Add `opj_dispatch_job` to `tcd.h`

Open `src/lib/openjp2/tcd.h` and add (near the existing thread-pool declarations):

```c
/* Submit a job through whichever worker pool the tcd's current
 * decode call is using.  When OPJ_DAG=1 (the default in
 * OPJ_ENABLE_TASK_GRAPH=ON builds) jobs run on the Taskflow
 * executor accessible via p_tcd->task_graph.  Otherwise (or in
 * OPJ_ENABLE_TASK_GRAPH=OFF builds) jobs run on the legacy
 * opj_thread_pool at p_tcd->thread_pool.
 *
 * The job function and arg signatures match opj_thread_pool's
 * submit_job; callers do not see the underlying pool. */
void opj_tcd_dispatch_job(opj_tcd_t* p_tcd,
                          void (*job_fn)(void* user_data, opj_tls_t* tls),
                          void* user_data);

/* Wait for all jobs dispatched on p_tcd's current pool to
 * complete.  Mirrors opj_thread_pool_wait_completion semantics
 * but selects the pool the same way as opj_tcd_dispatch_job. */
void opj_tcd_wait_jobs(opj_tcd_t* p_tcd);
```

Naming convention: `opj_tcd_*` mirrors existing helpers in tcd.h. Mind the existing `opj_tls_t* tls` second parameter — it threads TLS context to worker callbacks (needed by t1.c's TLS-cached t1 struct).

### Step 3: Add a `task_graph` field to `opj_tcd_t`

In `src/lib/openjp2/tcd.h`, find the `opj_tcd` struct definition and add:

```c
#ifdef OPJ_ENABLE_TASK_GRAPH
    opj_tg_t* task_graph;  /* Active when OPJ_DAG=1; NULL otherwise. */
#endif
```

near the existing `opj_thread_pool_t* thread_pool;` field.

### Step 4: Implement the dispatch + wait helpers in `tcd.c`

In `src/lib/openjp2/tcd.c`, add:

```c
#ifdef OPJ_ENABLE_TASK_GRAPH
#include "task_graph.h"
#endif

void opj_tcd_dispatch_job(opj_tcd_t* p_tcd,
                          void (*job_fn)(void* user_data, opj_tls_t* tls),
                          void* user_data)
{
#ifdef OPJ_ENABLE_TASK_GRAPH
    if (p_tcd->task_graph) {
        /* Wrap the (void*, opj_tls_t*) signature into Taskflow's
         * (void*) signature.  TLS is fetched inside the wrapper
         * from a per-executor TLS pool. */
        /* SP-2.1a: jobs run as a flat parallel soup (no DAG edges
         * yet).  Each call to opj_tg_add_task returns immediately;
         * opj_tg_run is invoked by opj_tcd_wait_jobs below. */
        /* Allocate a small thunk to bind the TLS-aware signature
         * to opj_tg_task_fn's signature. */
        opj_tcd_tg_thunk_t* thunk = ... ;
        opj_tg_add_task(p_tcd->task_graph,
                        &opj_tcd_tg_run_thunk, thunk);
        return;
    }
#endif
    opj_thread_pool_submit_job(p_tcd->thread_pool, job_fn, user_data);
}

void opj_tcd_wait_jobs(opj_tcd_t* p_tcd)
{
#ifdef OPJ_ENABLE_TASK_GRAPH
    if (p_tcd->task_graph) {
        opj_tg_run(p_tcd->task_graph);
        opj_tg_reset(p_tcd->task_graph);  /* Reuse for next stage. */
        return;
    }
#endif
    opj_thread_pool_wait_completion(p_tcd->thread_pool, 0);
}
```

The TLS thunk is the trickiest piece. The existing thread pool exposes `opj_tls_t*` through `opj_thread_pool_submit_job`'s job-function signature; Taskflow's tasks are plain `void(*)(void*)`. To bridge: allocate a per-tcd `opj_tls_t*` once at create time, pass a pointer to it through a thunk struct alongside `(job_fn, user_data)`.

```c
typedef struct {
    void (*job_fn)(void* user_data, opj_tls_t* tls);
    void* user_data;
    opj_tls_t* tls;  /* per-tcd, set at create */
} opj_tcd_tg_thunk_t;

static void opj_tcd_tg_run_thunk(void* arg) {
    opj_tcd_tg_thunk_t* t = (opj_tcd_tg_thunk_t*)arg;
    t->job_fn(t->user_data, t->tls);
    opj_free(t);
}
```

The thunk struct is `opj_calloc`'d at dispatch time and `opj_free`'d after the job runs. Tiny allocation; can be replaced with a pool in SP-2.1d if it shows up in perf.

### Step 5: Initialize/teardown the task_graph in `opj_tcd_create` and `opj_tcd_destroy`

```c
opj_tcd_t* opj_tcd_create(...) {
    opj_tcd_t* tcd = ... ;
    /* existing init... */
#ifdef OPJ_ENABLE_TASK_GRAPH
    if (opj_tg_enabled()) {
        tcd->task_graph = opj_tg_create(opj_thread_pool_get_thread_count(tcd->thread_pool));
        /* Optionally destroy thread_pool here OR leave it dormant. */
    } else {
        tcd->task_graph = NULL;
    }
#endif
    return tcd;
}

void opj_tcd_destroy(opj_tcd_t* tcd) {
#ifdef OPJ_ENABLE_TASK_GRAPH
    if (tcd->task_graph) {
        opj_tg_destroy(tcd->task_graph);
        tcd->task_graph = NULL;
    }
#endif
    /* existing teardown... */
}
```

Decision point: do we keep the legacy `thread_pool` allocated when `task_graph` is active? The simplest correct option is keep it allocated (so any code path we haven't migrated yet still works), but ensure no jobs are submitted to it when `task_graph` is non-NULL. The pool is mostly idle memory — ~1 KB per worker — and removing the allocation can come in a later cleanup.

### Step 6: Switch the t1.c dispatch site

In `src/lib/openjp2/t1.c` find line ~1897:

```c
opj_thread_pool_submit_job(tp, opj_t1_clbl_decode_processor, job);
```

Change to:

```c
opj_tcd_dispatch_job(tcd, opj_t1_clbl_decode_processor, job);
```

(`tcd` is the first parameter to `opj_t1_decode_cblks`, already in scope.)

There may also be an `opj_thread_pool_wait_completion(tp, 0);` later in the same function — switch it to `opj_tcd_wait_jobs(tcd);`.

### Step 7: Switch the dwt.c dispatch sites

In `src/lib/openjp2/dwt.c` find all `opj_thread_pool_submit_job` calls (and their paired `opj_thread_pool_wait_completion` calls). The dispatch sites are inside `opj_dwt_decode_tile` (the full-tile orchestrator) and possibly elsewhere.

For each pair:
- Replace `opj_thread_pool_submit_job(p_tcd->thread_pool, fn, arg)` → `opj_tcd_dispatch_job(p_tcd, fn, arg)`.
- Replace `opj_thread_pool_wait_completion(p_tcd->thread_pool, 0)` → `opj_tcd_wait_jobs(p_tcd)`.

Note: the partial-tile path is currently single-threaded (no thread pool used) — D7's int16 partial path is single-threaded by design — so no swap is needed there.

### Step 8: Build (both task_graph configurations)

```bash
cmake --build build-sp21a -j 2>&1 | tail -5
cmake --build build-sp21a-pure-c -j 2>&1 | tail -5
```

Both expected clean.

### Step 9: Conformance with default `OPJ_DAG=1`

```bash
scripts/run-conformance.sh -B build-sp21a -- -R NR-DEC -j4 2>&1 | tail -12
```

Expected: 545/553, same 8 pre-existing failures. Decode jobs are now running on Taskflow's executor instead of the thread pool, but they're still independent tasks — output bytes must be identical.

### Step 10: Conformance with `OPJ_DAG=0`

```bash
OPJ_DAG=0 scripts/run-conformance.sh -B build-sp21a -- -R NR-DEC -j4 2>&1 | tail -12
```

Expected: same 8 failures. Confirms the legacy thread-pool path remains intact and reachable.

### Step 11: Manual byte-cmp matrix

```bash
F8=$HOME/GitHub/openjp2k-bench/corpus/synthetic/rgb8_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_mon_enone.jp2
F12=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono12_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k
F16=$HOME/GitHub/openjp2k-bench/corpus/synthetic/mono16_1024/pCPRL_d5_b32_t1024x1024_lossless_l1_moff_esop_eph.j2k

for F in "$F8" "$F12" "$F16"; do
  for MODE in "dag-on" "dag-off"; do
    case "$MODE" in
      dag-on)  PRE="" ;;
      dag-off) PRE="OPJ_DAG=0" ;;
    esac
    env $PRE build-sp21a/bin/opj_decompress -i "$F" -o /tmp/$MODE.raw > /dev/null 2>&1
  done
  cmp -s /tmp/dag-on.raw /tmp/dag-off.raw \
    && echo "OK   $(basename "$F")" \
    || echo "FAIL $(basename "$F")"
done
```

Expected: 3/3 OK. Any FAIL → STOP. The thunk's TLS plumbing or job ordering is wrong.

### Step 12: TSan smoke (catches missing edges before they cause flakes)

```bash
cmake -S . -B build-sp21a-tsan -DCMAKE_BUILD_TYPE=Debug \
      -DOPJ_ENABLE_AVX2=OFF -DOPJ_ENABLE_TASK_GRAPH=ON \
      -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O1" \
      -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" 2>&1 | tail -3
cmake --build build-sp21a-tsan -j 2>&1 | tail -3

for F in "$F8" "$F12" "$F16"; do
  build-sp21a-tsan/bin/opj_decompress -i "$F" -o /tmp/_tsan.raw 2>&1 | grep -E 'WARNING|ThreadSanitizer' | head -10
done
```

Expected: no `WARNING: ThreadSanitizer` lines. SP-2.1a doesn't introduce DAG edges yet, but Taskflow's executor pool reuse + the thunk allocator could still have subtle races; TSan catches them.

### Step 13: Stage and commit

```bash
git add src/lib/openjp2/tcd.h src/lib/openjp2/tcd.c src/lib/openjp2/t1.c src/lib/openjp2/dwt.c
git status --short
git diff --cached --stat
```

Expected: 4 files modified, ~70-100 LOC total delta.

```bash
git commit -m "$(cat <<'EOF'
SP-2.1a: route per-cblk/per-row jobs through opj_tcd_dispatch_job

Adds an opj_tcd_dispatch_job/opj_tcd_wait_jobs helper pair in tcd.c
that routes worker jobs through the Taskflow executor (when OPJ_DAG=1
and OPJ_ENABLE_TASK_GRAPH=ON) or the legacy opj_thread_pool (when
either is off).  Existing dispatch sites in t1.c
(opj_t1_decode_cblks) and dwt.c (full-tile per-row/per-stripe) are
switched to use the helper.

No DAG topology is built in this commit — Taskflow's executor runs
the same jobs the existing pool would, just on a different worker
pool.  Conformance unchanged.  Bench expected parity (within ±1%);
any regression here indicates pool-implementation overhead that
must be fixed before SP-2.1b stacks DAG edges on top.

The legacy opj_thread_pool remains allocated alongside the task
graph; submit calls are routed at job-dispatch time.  Future SP-2.1
cleanup can drop the legacy pool when OPJ_DAG=1.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Bench gate + outcome + merge + tag

Same-day baseline, paired ratios. Expected: parity at all thread counts (this is infrastructure, no perf change).

**Files:**
- Modify: `docs/superpowers/specs/2026-05-26-sp2-1a-task-graph-infrastructure.md`
  *(Actually — SP-2.1a is a sub-deliverable; we may want to fold its outcome into the SP-2.1 spec rather than a separate file. Decide at outcome time: either append to the main SP-2.1 spec's outcome section, OR commit a small standalone outcome doc for SP-2.1a. Either works.)*

### Step 1: Set up the D7.1 baseline worktree

```bash
cd /home/cornish/GitHub/openjp2k
git worktree add /home/cornish/GitHub/openjp2k-d7-1-baseline v0.10.0-d7-1-t1-precision-gate
```

### Step 2: Rebuild bench against the baseline, run iter at t=1,2,4,8

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source /home/cornish/GitHub/openjp2k-d7-1-baseline 2>&1 | tail -3
nohup ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt --threads 1,2,4,8 corpus/public/ > results/sp2_1a_baseline_$(date +%Y%m%d_%H%M%S).jsonl 2> results/sp2_1a_baseline_$(date +%Y%m%d_%H%M%S).log
echo D7_1_BASELINE_DONE
```

(Note: `run_bench.sh` now spawns one bench process per thread value automatically — grok numbers are correct at every t. The `c07c9c3` fix from earlier landed.)

Wall time: ~3-4 hours (iter at 4 thread counts, each ~50 min).

### Step 3: Rebuild bench against SP-2.1a head, run the same

```bash
cd ~/GitHub/openjp2k-bench
rm -rf build
./scripts/build.sh --openjp2k-source /home/cornish/GitHub/openjp2k 2>&1 | tail -3
nohup ./scripts/run_bench.sh --include-from corpus/synthetic-iter.txt --threads 1,2,4,8 corpus/public/ > results/sp2_1a_head_$(date +%Y%m%d_%H%M%S).jsonl 2> results/sp2_1a_head_$(date +%Y%m%d_%H%M%S).log
echo SP2_1A_HEAD_DONE
```

### Step 4: Paired-ratio analysis

```python
import json, math, glob
from collections import defaultdict

def load(p):
    fd = defaultdict(lambda: defaultdict(dict))
    meta = {}
    for l in open(p):
        if '"type"' not in l: continue
        r = json.loads(l)
        if r.get('type') != 'result': continue
        if r.get('error') or r['timing_s']['min'] <= 0: continue
        fd[r['file']][r['threads']][r['decoder']] = r['timing_s']['min'] * 1000.0
        meta[r['file']] = (r['bit_depth'], r['channels'])
    return fd, meta

def gm(xs):
    xs = [x for x in xs if x and x>0]
    return math.exp(sum(map(math.log,xs))/len(xs)) if xs else None

base, m = load(sorted(glob.glob('results/sp2_1a_baseline_*.jsonl'))[-1])
head, _ = load(sorted(glob.glob('results/sp2_1a_head_*.jsonl'))[-1])

for t in (1, 2, 4, 8):
    rs = []
    for f in set(base) & set(head):
        if t not in base[f] or t not in head[f]: continue
        a, b = base[f][t], head[f][t]
        if not all(d in a and d in b for d in ('openjp2k', 'openjpeg')): continue
        r_b = a['openjpeg'] / a['openjp2k']
        r_h = b['openjpeg'] / b['openjp2k']
        rs.append(r_h / r_b)
    g = gm(rs)
    print(f"t={t}  SP-2.1a delta vs D7.1: {(g-1)*100:+.2f}% (gm {g:.4f}, n={len(rs)})")
```

### Step 5: Verify the gate

| Gate row | Expected | Pass |
|---|---:|---|
| Iter t=1 | within ±0.5% | ±0.5% |
| Iter t=4 | within ±1% | ±1% |
| Iter t=8 | within ±1% | ±1% |
| Conformance | 8 pre-existing | exact |

SP-2.1a is infrastructure — the bench gate is **parity, not improvement**. If any thread count regresses > 1%, the Taskflow executor has overhead vs the legacy pool that must be fixed before SP-2.1b. The likely culprits if regression appears: per-job thunk allocation overhead, or pool-startup cost not amortized.

If iter t=1 regresses > 0.5%, the thunk allocation is bleeding into the single-threaded path even when there's no parallelism — short-circuit at job-dispatch when only one worker exists.

### Step 6: Append outcome to the SP-2.1 spec (or create SP-2.1a-outcome spec)

Recommendation: append a "SP-2.1a outcome" subsection to the main SP-2.1 spec (`2026-05-26-sp2-1-dag-scheduling-design.md`). Each sub-deliverable's outcome accretes on the same spec, keeping the architectural narrative in one place.

Outcome content: confirm conformance + byte-cmp + TSan all passed, bench parity confirmed, list any t-specific surprises, brief carry-over to SP-2.1b.

### Step 7: Commit outcome + tag

```bash
git add docs/superpowers/specs/2026-05-26-sp2-1-dag-scheduling-design.md
git commit -m "Spec: SP-2.1a outcome — infrastructure landed at <bench %>"
git checkout main
git merge --ff-only feat/sp2-1a-task-graph-infrastructure
git tag -a v0.11.0-sp2-1a-task-graph-infra -m "SP-2.1a — Task-graph infrastructure landing (Taskflow + dispatch helper)"
git push origin main
git push origin v0.11.0-sp2-1a-task-graph-infra
```

### Step 8: Clean up

```bash
git worktree remove /home/cornish/GitHub/openjp2k-d7-1-baseline
git branch -d feat/sp2-1a-task-graph-infrastructure
```

---

## Summary checklist

- [ ] Task 1: Taskflow vendored, task_graph module compiles AVX2-ON + AVX2-OFF + TASK_GRAPH-ON + TASK_GRAPH-OFF (4 configurations), standalone task_graph sanity test passes, conformance unchanged (8 pre-existing failures), task_graph.cpp unreferenced from production code at this commit.
- [ ] Task 2: opj_tcd_dispatch_job + opj_tcd_wait_jobs implemented, t1.c and dwt.c switched to use them. Default OPJ_DAG=1 routes jobs through Taskflow; OPJ_DAG=0 routes through the legacy pool. Conformance + byte-cmp + TSan all pass.
- [ ] Task 3: Iter at t=1,2,4,8 vs same-day D7.1 baseline shows parity (within ±1% at every thread count). SP-2.1a outcome appended to spec; merged; tagged `v0.11.0-sp2-1a-task-graph-infra`.
