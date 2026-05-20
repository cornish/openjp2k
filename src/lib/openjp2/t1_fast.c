// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish

#include "opj_includes.h"
#include "t1_inl.h"
#include "mqc_fast.h"
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

/* -----------------------------------------------------------------------
 * sigpass fast clone
 *
 * Mechanically mirrors the legacy opj_t1_dec_sigpass_mqc dispatch +
 * 4 specializations + internal macro + step macro + step function,
 * with the following substitutions:
 *
 *   opj_mqc_decode_macro(v, mqc, curctx, a, c, ct)
 *       -> opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct)
 *   opj_t1_setcurctx(curctx, X)
 *       -> opj_t1_setcurctx_fast(curidx, X)
 *   DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct)
 *       -> DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct)
 *   UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct)
 *       -> UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct)
 *
 * Everything else (flag helpers, update macros, data constants) is
 * identical to the legacy.
 * ----------------------------------------------------------------------- */

/* Fast equivalent of the t1.c-local opj_t1_setcurctx macro. Requires
 * 'mqc' to be in scope (same macro-hygiene assumption as the legacy). */
#define opj_t1_setcurctx_fast(curidx, ctxno) \
    (curidx) = &(mqc)->ctxs_idx[(OPJ_UINT32)(ctxno)]

/* Clone of opj_t1_dec_sigpass_step_mqc_macro (t1.c:436-460). */
#define opj_t1_dec_sigpass_step_mqc_fast_macro(flags, flagsp, flags_stride, data, \
                                               data_stride, ci, mqc, curidx, \
                                               v, a, c, ct, oneplushalf, vsc) \
{ \
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == 0U && \
        (flags & (T1_SIGMA_NEIGHBOURS << (ci * 3U))) != 0U) { \
        OPJ_UINT32 ctxt1 = opj_t1_getctxno_zc(mqc, flags >> (ci * 3U)); \
        opj_t1_setcurctx_fast(curidx, ctxt1); \
        opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
        if (v) { \
            OPJ_UINT32 lu = opj_t1_getctxtno_sc_or_spb_index( \
                                flags, \
                                flagsp[-1], flagsp[1], \
                                ci); \
            OPJ_UINT32 ctxt2 = opj_t1_getctxno_sc(lu); \
            OPJ_UINT32 spb = opj_t1_getspb(lu); \
            opj_t1_setcurctx_fast(curidx, ctxt2); \
            opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
            v = v ^ spb; \
            data[ci*data_stride] = v ? -oneplushalf : oneplushalf; \
            opj_t1_update_flags_macro(flags, flagsp, ci, v, flags_stride, vsc); \
        } \
        flags |= T1_PI_THIS << (ci * 3U); \
    } \
}

/* Clone of opj_t1_dec_sigpass_step_mqc (t1.c:462-477). */
static INLINE void opj_t1_dec_sigpass_step_mqc_fast(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 oneplushalf,
    OPJ_UINT32 ci,
    OPJ_UINT32 flags_stride,
    OPJ_UINT32 vsc)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);       /* MQC component */
    opj_t1_dec_sigpass_step_mqc_fast_macro(*flagsp, flagsp, flags_stride, datap,
                                           0, ci, mqc, mqc->curctx_idx,
                                           v, mqc->a, mqc->c, mqc->ct, oneplushalf, vsc);
}

/* Clone of opj_t1_dec_sigpass_mqc_internal (t1.c:646-688). */
#define opj_t1_dec_sigpass_mqc_fast_internal(t1, bpno, vsc, w, h, flags_stride) \
{ \
        OPJ_INT32 one, half, oneplushalf; \
        OPJ_UINT32 i, j, k; \
        register OPJ_INT32 *data = t1->data; \
        register opj_flag_t *flagsp = &t1->flags[(flags_stride) + 1]; \
        const OPJ_UINT32 l_w = w; \
        opj_mqc_t* mqc = &(t1->mqc); \
        DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct); \
        register OPJ_UINT32 v; \
        one = 1 << bpno; \
        half = one >> 1; \
        oneplushalf = one | half; \
        for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) { \
                for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                        opj_flag_t flags = *flagsp; \
                        if( flags != 0 ) { \
                            opj_t1_dec_sigpass_step_mqc_fast_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 0, mqc, curidx, v, a, c, ct, oneplushalf, vsc); \
                            opj_t1_dec_sigpass_step_mqc_fast_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 1, mqc, curidx, v, a, c, ct, oneplushalf, OPJ_FALSE); \
                            opj_t1_dec_sigpass_step_mqc_fast_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 2, mqc, curidx, v, a, c, ct, oneplushalf, OPJ_FALSE); \
                            opj_t1_dec_sigpass_step_mqc_fast_macro( \
                                flags, flagsp, flags_stride, data, \
                                l_w, 3, mqc, curidx, v, a, c, ct, oneplushalf, OPJ_FALSE); \
                            *flagsp = flags; \
                        } \
                } \
        } \
        UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct); \
        if( k < h ) { \
            for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                for (j = 0; j < h - k; ++j) { \
                        opj_t1_dec_sigpass_step_mqc_fast(t1, flagsp, \
                            data + j * l_w, oneplushalf, j, flags_stride, vsc); \
                } \
            } \
        } \
}

/* Clone of opj_t1_dec_sigpass_mqc_64x64_novsc (t1.c:690-695). */
static void opj_t1_dec_sigpass_mqc_fast_64x64_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_fast_internal(t1, bpno, OPJ_FALSE, 64, 64, 66);
}

/* Clone of opj_t1_dec_sigpass_mqc_64x64_vsc (t1.c:697-702). */
static void opj_t1_dec_sigpass_mqc_fast_64x64_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_fast_internal(t1, bpno, OPJ_TRUE, 64, 64, 66);
}

/* Clone of opj_t1_dec_sigpass_mqc_generic_novsc (t1.c:704-710). */
static void opj_t1_dec_sigpass_mqc_fast_generic_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_fast_internal(t1, bpno, OPJ_FALSE, t1->w, t1->h,
                                         t1->w + 2U);
}

/* Clone of opj_t1_dec_sigpass_mqc_generic_vsc (t1.c:712-718). */
static void opj_t1_dec_sigpass_mqc_fast_generic_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_sigpass_mqc_fast_internal(t1, bpno, OPJ_TRUE, t1->w, t1->h,
                                         t1->w + 2U);
}

/* Clone of opj_t1_dec_sigpass_mqc dispatch (t1.c:720-738).
 * Renamed to opj_t1_fast_dec_sigpass_mqc and made non-static
 * (externally linkable public entry point). */
void opj_t1_fast_dec_sigpass_mqc(
    opj_t1_t *t1,
    OPJ_INT32 bpno,
    OPJ_INT32 cblksty)
{
    if (t1->w == 64 && t1->h == 64) {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_sigpass_mqc_fast_64x64_vsc(t1, bpno);
        } else {
            opj_t1_dec_sigpass_mqc_fast_64x64_novsc(t1, bpno);
        }
    } else {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_sigpass_mqc_fast_generic_vsc(t1, bpno);
        } else {
            opj_t1_dec_sigpass_mqc_fast_generic_novsc(t1, bpno);
        }
    }
}

/* -----------------------------------------------------------------------
 * refpass fast clone
 *
 * Mechanically mirrors the legacy opj_t1_dec_refpass_mqc dispatch +
 * 2 specializations + internal macro + step macro + step function,
 * with the same substitutions as sigpass:
 *
 *   opj_mqc_decode_macro(v, mqc, curctx, a, c, ct)
 *       -> opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct)
 *   opj_t1_setcurctx(curctx, X)
 *       -> opj_t1_setcurctx_fast(curidx, X)
 *   DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct)
 *       -> DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct)
 *   UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct)
 *       -> UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct)
 *
 * Note: refpass has no vsc variants (only 64x64 vs generic).
 * ----------------------------------------------------------------------- */

/* Clone of opj_t1_dec_refpass_step_mqc_macro (t1.c:702-713). */
#define opj_t1_dec_refpass_step_mqc_fast_macro(flags, data, data_stride, ci, \
                                               mqc, curidx, v, a, c, ct, poshalf) \
{ \
    if ((flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U))) == \
            (T1_SIGMA_THIS << (ci * 3U))) { \
        OPJ_UINT32 ctxt = opj_t1_getctxno_mag(flags >> (ci * 3U)); \
        opj_t1_setcurctx_fast(curidx, ctxt); \
        opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
        data[ci*data_stride] += (v ^ (data[ci*data_stride] < 0)) ? poshalf : -poshalf; \
        flags |= T1_MU_THIS << (ci * 3U); \
    } \
}

/* Clone of opj_t1_dec_refpass_step_mqc (t1.c:715-728). */
static INLINE void opj_t1_dec_refpass_step_mqc_fast(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 poshalf,
    OPJ_UINT32 ci)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);       /* MQC component */
    opj_t1_dec_refpass_step_mqc_fast_macro(*flagsp, datap, 0, ci,
                                           mqc, mqc->curctx_idx, v, mqc->a, mqc->c,
                                           mqc->ct, poshalf);
}

/* Clone of opj_t1_dec_refpass_mqc_internal (t1.c:897-937). */
#define opj_t1_dec_refpass_mqc_fast_internal(t1, bpno, w, h, flags_stride) \
{ \
        OPJ_INT32 one, poshalf; \
        OPJ_UINT32 i, j, k; \
        register OPJ_INT32 *data = t1->data; \
        register opj_flag_t *flagsp = &t1->flags[flags_stride + 1]; \
        const OPJ_UINT32 l_w = w; \
        opj_mqc_t* mqc = &(t1->mqc); \
        DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct); \
        register OPJ_UINT32 v; \
        one = 1 << bpno; \
        poshalf = one >> 1; \
        for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) { \
                for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                        opj_flag_t flags = *flagsp; \
                        if( flags != 0 ) { \
                            opj_t1_dec_refpass_step_mqc_fast_macro( \
                                flags, data, l_w, 0, \
                                mqc, curidx, v, a, c, ct, poshalf); \
                            opj_t1_dec_refpass_step_mqc_fast_macro( \
                                flags, data, l_w, 1, \
                                mqc, curidx, v, a, c, ct, poshalf); \
                            opj_t1_dec_refpass_step_mqc_fast_macro( \
                                flags, data, l_w, 2, \
                                mqc, curidx, v, a, c, ct, poshalf); \
                            opj_t1_dec_refpass_step_mqc_fast_macro( \
                                flags, data, l_w, 3, \
                                mqc, curidx, v, a, c, ct, poshalf); \
                            *flagsp = flags; \
                        } \
                } \
        } \
        UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct); \
        if( k < h ) { \
            for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
                for (j = 0; j < h - k; ++j) { \
                        opj_t1_dec_refpass_step_mqc_fast(t1, flagsp, data + j * l_w, poshalf, j); \
                } \
            } \
        } \
}

/* Clone of opj_t1_dec_refpass_mqc_64x64 (t1.c:939-944). */
static void opj_t1_dec_refpass_mqc_fast_64x64(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_refpass_mqc_fast_internal(t1, bpno, 64, 64, 66);
}

/* Clone of opj_t1_dec_refpass_mqc_generic (t1.c:946-951). */
static void opj_t1_dec_refpass_mqc_fast_generic(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_refpass_mqc_fast_internal(t1, bpno, t1->w, t1->h, t1->w + 2U);
}

/* Clone of opj_t1_dec_refpass_mqc dispatch (t1.c:953-962).
 * Renamed to opj_t1_fast_dec_refpass_mqc and made non-static
 * (externally linkable public entry point). */
void opj_t1_fast_dec_refpass_mqc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    if (t1->w == 64 && t1->h == 64) {
        opj_t1_dec_refpass_mqc_fast_64x64(t1, bpno);
    } else {
        opj_t1_dec_refpass_mqc_fast_generic(t1, bpno);
    }
}

/* -----------------------------------------------------------------------
 * clnpass fast clone
 *
 * Mechanically mirrors the legacy opj_t1_dec_clnpass dispatch +
 * 4 specializations + internal macro + step macro + step function,
 * with the same substitutions as sigpass/refpass:
 *
 *   opj_mqc_decode_macro(v, mqc, curctx, a, c, ct)
 *       -> opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct)
 *   opj_t1_setcurctx(curctx, X)
 *       -> opj_t1_setcurctx_fast(curidx, X)
 *   DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct)
 *       -> DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct)
 *   UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct)
 *       -> UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct)
 *
 * Note: the step-macro contains MULTIPLE opj_mqc_decode_macro call sites
 * (one per-bit ZC probe and one SC sign decode). The internal macro
 * additionally has three aggregate-mode decode calls (AGG flag, two UNI
 * runlen bits). All are substituted.
 *
 * opj_t1_dec_clnpass_check_segsym is NOT cloned — it operates on
 * already-uploaded MQC state via opj_mqc_decode (not the macro form)
 * and is called after UPLOAD; it is safe to call directly from the
 * fast dispatch.
 * ----------------------------------------------------------------------- */

/* Clone of opj_t1_dec_clnpass_step_macro (t1.c:1034-1060). */
#define opj_t1_dec_clnpass_step_fast_macro(check_flags, partial, \
                                           flags, flagsp, flags_stride, data, \
                                           data_stride, ci, mqc, curidx, \
                                           v, a, c, ct, oneplushalf, vsc) \
{ \
    if ( !check_flags || !(flags & ((T1_SIGMA_THIS | T1_PI_THIS) << (ci * 3U)))) {\
        do { \
            if( !partial ) { \
                OPJ_UINT32 ctxt1 = opj_t1_getctxno_zc(mqc, flags >> (ci * 3U)); \
                opj_t1_setcurctx_fast(curidx, ctxt1); \
                opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
                if( !v ) \
                    break; \
            } \
            { \
                OPJ_UINT32 lu = opj_t1_getctxtno_sc_or_spb_index( \
                                    flags, flagsp[-1], flagsp[1], \
                                    ci); \
                opj_t1_setcurctx_fast(curidx, opj_t1_getctxno_sc(lu)); \
                opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
                v = v ^ opj_t1_getspb(lu); \
                data[ci*data_stride] = v ? -oneplushalf : oneplushalf; \
                opj_t1_update_flags_macro(flags, flagsp, ci, v, flags_stride, vsc); \
            } \
        } while(0); \
    } \
}

/* Clone of opj_t1_dec_clnpass_step (t1.c:1062-1077). */
static void opj_t1_dec_clnpass_step_fast(
    opj_t1_t *t1,
    opj_flag_t *flagsp,
    OPJ_INT32 *datap,
    OPJ_INT32 oneplushalf,
    OPJ_UINT32 ci,
    OPJ_UINT32 vsc)
{
    OPJ_UINT32 v;

    opj_mqc_t *mqc = &(t1->mqc);   /* MQC component */
    opj_t1_dec_clnpass_step_fast_macro(OPJ_TRUE, OPJ_FALSE,
                                       *flagsp, flagsp, t1->w + 2U, datap,
                                       0, ci, mqc, mqc->curctx_idx,
                                       v, mqc->a, mqc->c, mqc->ct, oneplushalf, vsc);
}

/* Clone of opj_t1_dec_clnpass_internal (t1.c:1170-1257). */
#define opj_t1_dec_clnpass_fast_internal(t1, bpno, vsc, w, h, flags_stride) \
{ \
    OPJ_INT32 one, half, oneplushalf; \
    OPJ_UINT32 runlen; \
    OPJ_UINT32 i, j, k; \
    const OPJ_UINT32 l_w = w; \
    opj_mqc_t* mqc = &(t1->mqc); \
    register OPJ_INT32 *data = t1->data; \
    register opj_flag_t *flagsp = &t1->flags[flags_stride + 1]; \
    DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct); \
    register OPJ_UINT32 v; \
    one = 1 << bpno; \
    half = one >> 1; \
    oneplushalf = one | half; \
    for (k = 0; k < (h & ~3u); k += 4, data += 3*l_w, flagsp += 2) { \
        for (i = 0; i < l_w; ++i, ++data, ++flagsp) { \
            opj_flag_t flags = *flagsp; \
            if (flags == 0) { \
                OPJ_UINT32 partial = OPJ_TRUE; \
                opj_t1_setcurctx_fast(curidx, T1_CTXNO_AGG); \
                opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
                if (!v) { \
                    continue; \
                } \
                opj_t1_setcurctx_fast(curidx, T1_CTXNO_UNI); \
                opj_mqc_fast_decode_macro(runlen, mqc, curidx, a, c, ct); \
                opj_mqc_fast_decode_macro(v, mqc, curidx, a, c, ct); \
                runlen = (runlen << 1) | v; \
                switch(runlen) { \
                    case 0: \
                        opj_t1_dec_clnpass_step_fast_macro(OPJ_FALSE, OPJ_TRUE,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 0, mqc, curidx, \
                                            v, a, c, ct, oneplushalf, vsc); \
                        partial = OPJ_FALSE; \
                        /* FALLTHRU */ \
                    case 1: \
                        opj_t1_dec_clnpass_step_fast_macro(OPJ_FALSE, partial,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 1, mqc, curidx, \
                                            v, a, c, ct, oneplushalf, OPJ_FALSE); \
                        partial = OPJ_FALSE; \
                        /* FALLTHRU */ \
                    case 2: \
                        opj_t1_dec_clnpass_step_fast_macro(OPJ_FALSE, partial,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 2, mqc, curidx, \
                                            v, a, c, ct, oneplushalf, OPJ_FALSE); \
                        partial = OPJ_FALSE; \
                        /* FALLTHRU */ \
                    case 3: \
                        opj_t1_dec_clnpass_step_fast_macro(OPJ_FALSE, partial,\
                                            flags, flagsp, flags_stride, data, \
                                            l_w, 3, mqc, curidx, \
                                            v, a, c, ct, oneplushalf, OPJ_FALSE); \
                        break; \
                } \
            } else { \
                opj_t1_dec_clnpass_step_fast_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 0, mqc, curidx, \
                                    v, a, c, ct, oneplushalf, vsc); \
                opj_t1_dec_clnpass_step_fast_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 1, mqc, curidx, \
                                    v, a, c, ct, oneplushalf, OPJ_FALSE); \
                opj_t1_dec_clnpass_step_fast_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 2, mqc, curidx, \
                                    v, a, c, ct, oneplushalf, OPJ_FALSE); \
                opj_t1_dec_clnpass_step_fast_macro(OPJ_TRUE, OPJ_FALSE, \
                                    flags, flagsp, flags_stride, data, \
                                    l_w, 3, mqc, curidx, \
                                    v, a, c, ct, oneplushalf, OPJ_FALSE); \
            } \
            *flagsp = flags & ~(T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3); \
        } \
    } \
    UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct); \
    if( k < h ) { \
        for (i = 0; i < l_w; ++i, ++flagsp, ++data) { \
            for (j = 0; j < h - k; ++j) { \
                opj_t1_dec_clnpass_step_fast(t1, flagsp, data + j * l_w, oneplushalf, j, vsc); \
            } \
            *flagsp &= ~(T1_PI_0 | T1_PI_1 | T1_PI_2 | T1_PI_3); \
        } \
    } \
}

/* Clone of opj_t1_dec_clnpass_64x64_novsc (t1.c:1280-1285). */
static void opj_t1_dec_clnpass_fast_64x64_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_fast_internal(t1, bpno, OPJ_FALSE, 64, 64, 66);
}

/* Clone of opj_t1_dec_clnpass_64x64_vsc (t1.c:1287-1292). */
static void opj_t1_dec_clnpass_fast_64x64_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_fast_internal(t1, bpno, OPJ_TRUE, 64, 64, 66);
}

/* Clone of opj_t1_dec_clnpass_generic_novsc (t1.c:1294-1300). */
static void opj_t1_dec_clnpass_fast_generic_novsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_fast_internal(t1, bpno, OPJ_FALSE, t1->w, t1->h,
                                     t1->w + 2U);
}

/* Clone of opj_t1_dec_clnpass_generic_vsc (t1.c:1302-1308). */
static void opj_t1_dec_clnpass_fast_generic_vsc(
    opj_t1_t *t1,
    OPJ_INT32 bpno)
{
    opj_t1_dec_clnpass_fast_internal(t1, bpno, OPJ_TRUE, t1->w, t1->h,
                                     t1->w + 2U);
}

/* Clone of opj_t1_dec_clnpass dispatch (t1.c:1310-1329).
 * Renamed to opj_t1_fast_dec_clnpass and made non-static
 * (externally linkable public entry point).
 * The segsym check (t1.c:1259-1278) is inlined here because the original
 * opj_t1_dec_clnpass_check_segsym is file-static in t1.c and not accessible
 * from t1_fast.c.  It uses opj_mqc_decode (not the macro form) on
 * already-uploaded MQC state, so it is safe to call identically. */
void opj_t1_fast_dec_clnpass(
    opj_t1_t *t1,
    OPJ_INT32 bpno,
    OPJ_INT32 cblksty)
{
    if (t1->w == 64 && t1->h == 64) {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_clnpass_fast_64x64_vsc(t1, bpno);
        } else {
            opj_t1_dec_clnpass_fast_64x64_novsc(t1, bpno);
        }
    } else {
        if (cblksty & J2K_CCP_CBLKSTY_VSC) {
            opj_t1_dec_clnpass_fast_generic_vsc(t1, bpno);
        } else {
            opj_t1_dec_clnpass_fast_generic_novsc(t1, bpno);
        }
    }
    /* Inline of opj_t1_dec_clnpass_check_segsym (t1.c:1259-1278). */
    if (cblksty & J2K_CCP_CBLKSTY_SEGSYM) {
        opj_mqc_t* mqc = &(t1->mqc);
        OPJ_UINT32 v, v2;
        opj_mqc_setcurctx(mqc, T1_CTXNO_UNI);
        opj_mqc_decode(v, mqc);
        opj_mqc_decode(v2, mqc);
        v = (v << 1) | v2;
        opj_mqc_decode(v2, mqc);
        v = (v << 1) | v2;
        opj_mqc_decode(v2, mqc);
        v = (v << 1) | v2;
        (void)v; /* result unused — same as legacy */
    }
}

#endif /* !OPJ_T1_LEGACY_ONLY */
