/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
 * Copyright (c) 2002-2014, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux
 * Copyright (c) 2003-2014, Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2007, Callum Lerwick <seg@haxxed.com>
 * Copyright (c) 2012, Carl Hetherington
 * Copyright (c) 2017, IntoPIX SA <support@intopix.com>
 * Copyright (c) 2026, Toby Cornish
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * `AS IS' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * t1_inl.h — shared static-inline T1 helpers.
 *
 * Extracted from t1.c so that t1_fast.c can share them without
 * duplicating source.  Both t1.c and t1_fast.c include this header
 * after their respective includes of opj_includes.h (which pulls in
 * t1.h for opj_flag_t / opj_mqc_t etc.) and t1_luts.h.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef OPJ_T1_INL_H
#define OPJ_T1_INL_H

/* Pull in the LUT tables (static const arrays). */
#include "t1_luts.h"

/* -----------------------------------------------------------------------
 * Context-number helpers
 * ----------------------------------------------------------------------- */

static INLINE OPJ_BYTE opj_t1_getctxno_zc(opj_mqc_t *mqc, OPJ_UINT32 f)
{
    return mqc->lut_ctxno_zc_orient[(f & T1_SIGMA_NEIGHBOURS)];
}

static INLINE OPJ_UINT32 opj_t1_getctxtno_sc_or_spb_index(OPJ_UINT32 fX,
        OPJ_UINT32 pfX,
        OPJ_UINT32 nfX,
        OPJ_UINT32 ci)
{
    /*
      0 pfX T1_CHI_THIS           T1_LUT_SGN_W
      1 tfX T1_SIGMA_1            T1_LUT_SIG_N
      2 nfX T1_CHI_THIS           T1_LUT_SGN_E
      3 tfX T1_SIGMA_3            T1_LUT_SIG_W
      4  fX T1_CHI_(THIS - 1)     T1_LUT_SGN_N
      5 tfX T1_SIGMA_5            T1_LUT_SIG_E
      6  fX T1_CHI_(THIS + 1)     T1_LUT_SGN_S
      7 tfX T1_SIGMA_7            T1_LUT_SIG_S
    */

    OPJ_UINT32 lu = (fX >> (ci * 3U)) & (T1_SIGMA_1 | T1_SIGMA_3 | T1_SIGMA_5 |
                                         T1_SIGMA_7);

    lu |= (pfX >> (T1_CHI_THIS_I      + (ci * 3U))) & (1U << 0);
    lu |= (nfX >> (T1_CHI_THIS_I - 2U + (ci * 3U))) & (1U << 2);
    if (ci == 0U) {
        lu |= (fX >> (T1_CHI_0_I - 4U)) & (1U << 4);
    } else {
        lu |= (fX >> (T1_CHI_1_I - 4U + ((ci - 1U) * 3U))) & (1U << 4);
    }
    lu |= (fX >> (T1_CHI_2_I - 6U + (ci * 3U))) & (1U << 6);
    return lu;
}

static INLINE OPJ_BYTE opj_t1_getctxno_sc(OPJ_UINT32 lu)
{
    return lut_ctxno_sc[lu];
}

static INLINE OPJ_UINT32 opj_t1_getctxno_mag(OPJ_UINT32 f)
{
    OPJ_UINT32 tmp = (f & T1_SIGMA_NEIGHBOURS) ? T1_CTXNO_MAG + 1 : T1_CTXNO_MAG;
    OPJ_UINT32 tmp2 = (f & T1_MU_0) ? T1_CTXNO_MAG + 2 : tmp;
    return tmp2;
}

static INLINE OPJ_BYTE opj_t1_getspb(OPJ_UINT32 lu)
{
    return lut_spb[lu];
}

/* -----------------------------------------------------------------------
 * Flag-update macro and helper
 * ----------------------------------------------------------------------- */

#define opj_t1_update_flags_macro(flags, flagsp, ci, s, stride, vsc) \
{ \
    /* east */ \
    flagsp[-1] |= T1_SIGMA_5 << (3U * ci); \
 \
    /* mark target as significant */ \
    flags |= ((s << T1_CHI_1_I) | T1_SIGMA_4) << (3U * ci); \
 \
    /* west */ \
    flagsp[1] |= T1_SIGMA_3 << (3U * ci); \
 \
    /* north-west, north, north-east */ \
    if (ci == 0U && !(vsc)) { \
        opj_flag_t* north = flagsp - (stride); \
        *north |= (s << T1_CHI_5_I) | T1_SIGMA_16; \
        north[-1] |= T1_SIGMA_17; \
        north[1] |= T1_SIGMA_15; \
    } \
 \
    /* south-west, south, south-east */ \
    if (ci == 3U) { \
        opj_flag_t* south = flagsp + (stride); \
        *south |= (s << T1_CHI_0_I) | T1_SIGMA_1; \
        south[-1] |= T1_SIGMA_2; \
        south[1] |= T1_SIGMA_0; \
    } \
}

static INLINE void opj_t1_update_flags(opj_flag_t *flagsp, OPJ_UINT32 ci,
                                       OPJ_UINT32 s, OPJ_UINT32 stride,
                                       OPJ_UINT32 vsc)
{
    opj_t1_update_flags_macro(*flagsp, flagsp, ci, s, stride, vsc);
}

#endif /* OPJ_T1_INL_H */
