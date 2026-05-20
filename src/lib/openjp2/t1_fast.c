// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish

#include "opj_includes.h"
#include "t1_fast.h"

#ifndef OPJ_T1_LEGACY_ONLY

/* -1 = uninitialized. Concurrent first-callers race benignly: both compute the same value from the env var and write it. */
static int opj_t1_fast_flag = -1;

int opj_t1_fast_enabled(void)
{
    const char *env;
    int v = opj_t1_fast_flag;
    if (v >= 0) {
        return v;
    }
    env = getenv("OPJ_T1_FAST");
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
