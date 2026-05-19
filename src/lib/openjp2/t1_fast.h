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
