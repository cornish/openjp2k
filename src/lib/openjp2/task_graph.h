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
