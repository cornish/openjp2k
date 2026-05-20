// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// Packed MQ state table and fast-path setup functions.
//
// The packed table is generated mechanically from mqc.c's legacy
// mqc_states[]; see scripts/gen_mqc_packed.py for the generator (kept
// in-repo so the table can be regenerated if the legacy table ever
// changes). Each entry encodes qeval, mps, switch_flag, nmps_idx,
// nlps_idx — see mqc_fast.h for the bit layout.

#include "mqc_fast.h"

/* Helper macro for building entries from (qeval, mps, nmps_idx, nlps_idx,
 * switch_flag). switch_flag is computed at table-generation time. */
#define PACK(qeval, mps, nmps, nlps, swf)         \
    ((OPJ_UINT32)((qeval) & 0xFFFFu) |            \
     ((OPJ_UINT32)((mps) & 0x1u) << 16) |         \
     ((OPJ_UINT32)((swf) & 0x1u) << 17) |         \
     ((OPJ_UINT32)((nmps) & 0x7Fu) << 18) |       \
     ((OPJ_UINT32)((nlps) & 0x7Fu) << 25))

const OPJ_UINT32 opj_mqc_states_packed[OPJ_MQC_FAST_NUM_STATES] = {
    /*  0 */ PACK(0x5601, 0,  2,  3, 1),
    /*  1 */ PACK(0x5601, 1,  3,  2, 1),
    /*  2 */ PACK(0x3401, 0,  4, 12, 0),
    /*  3 */ PACK(0x3401, 1,  5, 13, 0),
    /*  4 */ PACK(0x1801, 0,  6, 18, 0),
    /*  5 */ PACK(0x1801, 1,  7, 19, 0),
    /*  6 */ PACK(0x0AC1, 0,  8, 24, 0),
    /*  7 */ PACK(0x0AC1, 1,  9, 25, 0),
    /*  8 */ PACK(0x0521, 0, 10, 58, 0),
    /*  9 */ PACK(0x0521, 1, 11, 59, 0),
    /* 10 */ PACK(0x0221, 0, 76, 66, 0),
    /* 11 */ PACK(0x0221, 1, 77, 67, 0),
    /* 12 */ PACK(0x5601, 0, 14, 13, 1),
    /* 13 */ PACK(0x5601, 1, 15, 12, 1),
    /* 14 */ PACK(0x5401, 0, 16, 28, 0),
    /* 15 */ PACK(0x5401, 1, 17, 29, 0),
    /* 16 */ PACK(0x4801, 0, 18, 28, 0),
    /* 17 */ PACK(0x4801, 1, 19, 29, 0),
    /* 18 */ PACK(0x3801, 0, 20, 28, 0),
    /* 19 */ PACK(0x3801, 1, 21, 29, 0),
    /* 20 */ PACK(0x3001, 0, 22, 34, 0),
    /* 21 */ PACK(0x3001, 1, 23, 35, 0),
    /* 22 */ PACK(0x2401, 0, 24, 36, 0),
    /* 23 */ PACK(0x2401, 1, 25, 37, 0),
    /* 24 */ PACK(0x1C01, 0, 26, 40, 0),
    /* 25 */ PACK(0x1C01, 1, 27, 41, 0),
    /* 26 */ PACK(0x1601, 0, 58, 42, 0),
    /* 27 */ PACK(0x1601, 1, 59, 43, 0),
    /* 28 */ PACK(0x5601, 0, 30, 29, 1),
    /* 29 */ PACK(0x5601, 1, 31, 28, 1),
    /* 30 */ PACK(0x5401, 0, 32, 28, 0),
    /* 31 */ PACK(0x5401, 1, 33, 29, 0),
    /* 32 */ PACK(0x5101, 0, 34, 30, 0),
    /* 33 */ PACK(0x5101, 1, 35, 31, 0),
    /* 34 */ PACK(0x4801, 0, 36, 32, 0),
    /* 35 */ PACK(0x4801, 1, 37, 33, 0),
    /* 36 */ PACK(0x3801, 0, 38, 34, 0),
    /* 37 */ PACK(0x3801, 1, 39, 35, 0),
    /* 38 */ PACK(0x3401, 0, 40, 36, 0),
    /* 39 */ PACK(0x3401, 1, 41, 37, 0),
    /* 40 */ PACK(0x3001, 0, 42, 38, 0),
    /* 41 */ PACK(0x3001, 1, 43, 39, 0),
    /* 42 */ PACK(0x2801, 0, 44, 38, 0),
    /* 43 */ PACK(0x2801, 1, 45, 39, 0),
    /* 44 */ PACK(0x2401, 0, 46, 40, 0),
    /* 45 */ PACK(0x2401, 1, 47, 41, 0),
    /* 46 */ PACK(0x2201, 0, 48, 42, 0),
    /* 47 */ PACK(0x2201, 1, 49, 43, 0),
    /* 48 */ PACK(0x1C01, 0, 50, 44, 0),
    /* 49 */ PACK(0x1C01, 1, 51, 45, 0),
    /* 50 */ PACK(0x1801, 0, 52, 46, 0),
    /* 51 */ PACK(0x1801, 1, 53, 47, 0),
    /* 52 */ PACK(0x1601, 0, 54, 48, 0),
    /* 53 */ PACK(0x1601, 1, 55, 49, 0),
    /* 54 */ PACK(0x1401, 0, 56, 50, 0),
    /* 55 */ PACK(0x1401, 1, 57, 51, 0),
    /* 56 */ PACK(0x1201, 0, 58, 52, 0),
    /* 57 */ PACK(0x1201, 1, 59, 53, 0),
    /* 58 */ PACK(0x1101, 0, 60, 54, 0),
    /* 59 */ PACK(0x1101, 1, 61, 55, 0),
    /* 60 */ PACK(0x0AC1, 0, 62, 56, 0),
    /* 61 */ PACK(0x0AC1, 1, 63, 57, 0),
    /* 62 */ PACK(0x09C1, 0, 64, 58, 0),
    /* 63 */ PACK(0x09C1, 1, 65, 59, 0),
    /* 64 */ PACK(0x08A1, 0, 66, 60, 0),
    /* 65 */ PACK(0x08A1, 1, 67, 61, 0),
    /* 66 */ PACK(0x0521, 0, 68, 62, 0),
    /* 67 */ PACK(0x0521, 1, 69, 63, 0),
    /* 68 */ PACK(0x0441, 0, 70, 64, 0),
    /* 69 */ PACK(0x0441, 1, 71, 65, 0),
    /* 70 */ PACK(0x02A1, 0, 72, 66, 0),
    /* 71 */ PACK(0x02A1, 1, 73, 67, 0),
    /* 72 */ PACK(0x0221, 0, 74, 68, 0),
    /* 73 */ PACK(0x0221, 1, 75, 69, 0),
    /* 74 */ PACK(0x0141, 0, 76, 70, 0),
    /* 75 */ PACK(0x0141, 1, 77, 71, 0),
    /* 76 */ PACK(0x0111, 0, 78, 72, 0),
    /* 77 */ PACK(0x0111, 1, 79, 73, 0),
    /* 78 */ PACK(0x0085, 0, 80, 74, 0),
    /* 79 */ PACK(0x0085, 1, 81, 75, 0),
    /* 80 */ PACK(0x0049, 0, 82, 76, 0),
    /* 81 */ PACK(0x0049, 1, 83, 77, 0),
    /* 82 */ PACK(0x0025, 0, 84, 78, 0),
    /* 83 */ PACK(0x0025, 1, 85, 79, 0),
    /* 84 */ PACK(0x0015, 0, 86, 80, 0),
    /* 85 */ PACK(0x0015, 1, 87, 81, 0),
    /* 86 */ PACK(0x0009, 0, 88, 82, 0),
    /* 87 */ PACK(0x0009, 1, 89, 83, 0),
    /* 88 */ PACK(0x0005, 0, 90, 84, 0),
    /* 89 */ PACK(0x0005, 1, 91, 85, 0),
    /* 90 */ PACK(0x0001, 0, 90, 86, 0),
    /* 91 */ PACK(0x0001, 1, 91, 87, 0),
    /* 92 */ PACK(0x5601, 0, 92, 92, 0),
    /* 93 */ PACK(0x5601, 1, 93, 93, 0)
};

void opj_mqc_fast_resetstates(OPJ_UINT32 *ctxs_idx)
{
    OPJ_UINT32 i;
    for (i = 0; i < MQC_NUMCTXS; i++) {
        ctxs_idx[i] = 0;  /* state 0 — matches legacy mqc_states[0] */
    }
}

void opj_mqc_fast_setstate(OPJ_UINT32 *ctxs_idx, OPJ_UINT32 ctxno,
                            OPJ_UINT32 msb, OPJ_INT32 prob)
{
    ctxs_idx[ctxno] = msb + (OPJ_UINT32)(prob << 1);
}
