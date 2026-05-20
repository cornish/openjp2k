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
 *       The comparison is case-sensitive; uppercase variants like "OFF" enable fast mode.
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef OPJ_T1_LEGACY_ONLY
static INLINE int opj_t1_fast_enabled(void) { return 0; }
#else
int opj_t1_fast_enabled(void);

/* opj_t1_t is typedef'd in t1.h, which is included transitively via
 * opj_includes.h by every TU that includes this header. */
void opj_t1_fast_dec_sigpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno, OPJ_INT32 cblksty);
void opj_t1_fast_dec_refpass_mqc(opj_t1_t *t1, OPJ_INT32 bpno);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPJ_T1_FAST_H */
