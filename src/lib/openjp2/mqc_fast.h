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
 *   curidx   : OPJ_UINT32 * (pointer to active slot in mqc->ctxs_idx[])
 *   a, c, ct : registers downloaded from mqc
 */
#define opj_mqc_fast_decode_macro(d, mqc, curidx, a, c, ct) \
{ \
    OPJ_UINT32 _pkd = opj_mqc_states_packed[*(curidx)]; \
    OPJ_UINT32 _qe = OPJ_MQC_PACK_QEVAL(_pkd); \
    OPJ_UINT32 _mps = OPJ_MQC_PACK_MPS(_pkd); \
    (a) -= _qe; \
    if (((c) >> 16) < _qe) { \
        /* LPS path */ \
        if ((a) < _qe) { \
            /* MPS exchange: a = qeval, d = mps, transition NMPS */ \
            (a) = _qe; \
            (d) = _mps; \
            *(curidx) = OPJ_MQC_PACK_NMPS_IDX(_pkd); \
        } else { \
            (a) = _qe; \
            (d) = !_mps; \
            *(curidx) = OPJ_MQC_PACK_NLPS_IDX(_pkd); \
        } \
        opj_mqc_fast_renormd_macro((mqc), (a), (c), (ct)); \
    } else { \
        (c) -= _qe << 16; \
        if (((a) & 0x8000) == 0) { \
            /* MPS exchange */ \
            if ((a) < _qe) { \
                (d) = !_mps; \
                *(curidx) = OPJ_MQC_PACK_NLPS_IDX(_pkd); \
            } else { \
                (d) = _mps; \
                *(curidx) = OPJ_MQC_PACK_NMPS_IDX(_pkd); \
            } \
            opj_mqc_fast_renormd_macro((mqc), (a), (c), (ct)); \
        } else { \
            (d) = _mps; \
        } \
    } \
}

#define opj_mqc_fast_setcurctx_idx(mqc, ctxno) \
    ((mqc)->curctx_idx = &(mqc)->ctxs_idx[(OPJ_UINT32)(ctxno)])

#define DOWNLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct) \
        register OPJ_UINT32 *curidx = (mqc)->curctx_idx; \
        register OPJ_UINT32 c = (mqc)->c; \
        register OPJ_UINT32 a = (mqc)->a; \
        register OPJ_UINT32 ct = (mqc)->ct

#define UPLOAD_MQC_FAST_VARIABLES(mqc, curidx, a, c, ct) \
        (mqc)->curctx_idx = (curidx); \
        (mqc)->c = (c); \
        (mqc)->a = (a); \
        (mqc)->ct = (ct);

/*
 * 32-bit count-leading-zeros. Defined for x > 0; result is undefined
 * for x = 0 (matches __builtin_clz / lzcnt).
 */
#if defined(__GNUC__) || defined(__clang__)
static INLINE OPJ_UINT32 opj_mqc_clz32(OPJ_UINT32 x) {
    return (OPJ_UINT32)__builtin_clz(x);
}
#elif defined(_MSC_VER)
#include <intrin.h>
static INLINE OPJ_UINT32 opj_mqc_clz32(OPJ_UINT32 x) {
    unsigned long idx;
    _BitScanReverse(&idx, x);
    return 31u - idx;
}
#else
static INLINE OPJ_UINT32 opj_mqc_clz32(OPJ_UINT32 x) {
    OPJ_UINT32 n = 0;
    while ((x & 0x80000000u) == 0) { x <<= 1; n++; }
    return n;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* OPJ_MQC_FAST_H */
