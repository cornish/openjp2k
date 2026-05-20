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

#ifdef __cplusplus
}
#endif

#endif /* OPJ_MQC_FAST_H */
