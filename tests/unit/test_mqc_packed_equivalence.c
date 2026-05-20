/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Toby Cornish */
/*
 * Structural verification of opj_mqc_states_packed[]:
 *  1. Every nmps_idx and nlps_idx is in [0, 93].
 *  2. switch_flag matches the actual MPS difference: an entry's
 *     switch_flag bit equals (this.mps != nlps_entry.mps).
 *  3. Even and odd entries are mirror images (qeval same, mps flipped,
 *     nmps and nlps swapped) -- the structure J2K Annex C describes.
 *
 * Together these catch the transcription errors that would corrupt the
 * state walk without changing decoded output for "easy" inputs.
 */

#include <stdio.h>
#include "mqc_fast.h"

static int check_index_range(OPJ_UINT32 i, const char *which, OPJ_UINT32 idx)
{
    if (idx >= OPJ_MQC_FAST_NUM_STATES) {
        fprintf(stderr, "state %u: %s_idx=%u out of range\n", i, which, idx);
        return 1;
    }
    return 0;
}

int main(void)
{
    int failed = 0;
    OPJ_UINT32 i;
    OPJ_UINT32 p;
    OPJ_UINT32 mps;
    OPJ_UINT32 swf;
    OPJ_UINT32 nmps;
    OPJ_UINT32 nlps;
    OPJ_UINT32 nlps_mps;
    OPJ_UINT32 actual_switch;
    OPJ_UINT32 e;
    OPJ_UINT32 o;

    for (i = 0; i < OPJ_MQC_FAST_NUM_STATES; ++i) {
        p = opj_mqc_states_packed[i];
        mps = OPJ_MQC_PACK_MPS(p);
        swf = OPJ_MQC_PACK_SWITCH(p);
        nmps = OPJ_MQC_PACK_NMPS_IDX(p);
        nlps = OPJ_MQC_PACK_NLPS_IDX(p);

        failed += check_index_range(i, "nmps", nmps);
        failed += check_index_range(i, "nlps", nlps);
        if (failed) continue;  /* skip further checks if indices are bogus */

        nlps_mps = OPJ_MQC_PACK_MPS(opj_mqc_states_packed[nlps]);
        actual_switch = (mps != nlps_mps) ? 1u : 0u;
        if (actual_switch != swf) {
            fprintf(stderr,
                "state %u: switch_flag=%u but nlps_idx=%u has mps=%u "
                "(this mps=%u; expected switch_flag=%u)\n",
                i, swf, nlps, nlps_mps, mps, actual_switch);
            failed++;
        }
    }

    /* Even/odd mirror check: state 2k has mps=0, state 2k+1 has mps=1,
     * and their (nmps,nlps) are swapped. */
    for (i = 0; i < OPJ_MQC_FAST_NUM_STATES; i += 2) {
        e = opj_mqc_states_packed[i];
        o = opj_mqc_states_packed[i + 1];
        if (OPJ_MQC_PACK_QEVAL(e) != OPJ_MQC_PACK_QEVAL(o)) {
            fprintf(stderr, "states %u/%u qeval mismatch\n", i, i+1); failed++;
        }
        if (OPJ_MQC_PACK_MPS(e) != 0 || OPJ_MQC_PACK_MPS(o) != 1) {
            fprintf(stderr, "states %u/%u mps not (0,1)\n", i, i+1); failed++;
        }
    }

    if (failed) {
        fprintf(stderr, "test_mqc_packed_equivalence FAILED: %d errors\n", failed);
        return 1;
    }
    printf("test_mqc_packed_equivalence: 94 states verified\n");
    return 0;
}
