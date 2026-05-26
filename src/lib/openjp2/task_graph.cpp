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
#include <thread>
#include <vector>

#include "taskflow/taskflow.hpp"

extern "C" {
#include "openjpeg.h"
#include "task_graph.h"
}

struct opj_tg {
    /* Field order is load-bearing for safe teardown.  C++ destroys members
     * in reverse declaration order, so `executor` is destroyed before
     * `flow`: the executor joins its worker threads before the flowgraph
     * they were operating on is destructed.  If these fields are reordered
     * with `executor` first, teardown would destroy the flowgraph while
     * workers may still be inside its tasks. */
    tf::Taskflow flow;
    tf::Executor executor;
    std::vector<tf::Task> tasks;

    explicit opj_tg(int num_threads)
        : executor(num_threads > 0
                   ? static_cast<size_t>(num_threads)
                   : static_cast<size_t>(std::thread::hardware_concurrency())) {}
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
    tg->tasks[static_cast<size_t>(pred)].precede(
        tg->tasks[static_cast<size_t>(succ)]);
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

/* Cached env-var read.  -1 = uninitialized.  Benign concurrent
 * first-callers may race the store; all paths compute the same
 * value, mirroring opj_t1_fast_flag's invariant. */
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
