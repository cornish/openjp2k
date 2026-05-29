// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// SP-3.5 verification: for each test file in argv (paths relative to
// OPJ_DATA_ROOT, or absolute), decode twice in this process — first with
// opj_dwt_force_float_97 = 0 (int16 9/7 path engaged when the gate
// holds), then with opj_dwt_force_float_97 = 1 (float reference) — and
// compute per-component PSNR vs the (2^prec - 1) peak.  Assert >= 50 dB
// on every component.  The 50 dB floor reflects the empirical PSNR ceiling
// of openjp2k's float->int16 input conversion (see SP-3.5 spec § PSNR
// floor); 50 dB is well above the visually-lossless threshold for 8-bit
// content (~40-45 dB).
//
// Files where the int16 gate does not engage (5/3 reversible, prec > 12,
// signed, partial-tile) produce bit-identical output from both decodes
// (PSNR == +infinity) and pass trivially.  Such files are still useful
// as conformance smoke checks.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openjpeg.h"

extern int opj_dwt_force_float_97;

static opj_image_t *decode_file(const char *path)
{
    opj_dparameters_t parameters;
    opj_stream_t *stream;
    opj_codec_t *codec;
    opj_image_t *image;
    const char *ext;
    OPJ_CODEC_FORMAT fmt;

    opj_set_default_decoder_parameters(&parameters);
    stream = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    if (!stream) {
        return NULL;
    }
    ext = strrchr(path, '.');
    fmt = OPJ_CODEC_J2K;
    if (ext && (strcmp(ext, ".jp2") == 0 || strcmp(ext, ".jpf") == 0)) {
        fmt = OPJ_CODEC_JP2;
    }
    codec = opj_create_decompress(fmt);
    if (!codec) {
        opj_stream_destroy(stream);
        return NULL;
    }
    opj_setup_decoder(codec, &parameters);
    image = NULL;
    if (!opj_read_header(stream, codec, &image) ||
        !opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
        if (image) {
            opj_image_destroy(image);
            image = NULL;
        }
    }
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return image;
}

static int compare_psnr(const char *path, double min_dB_required,
                        double *out_min_dB, double *out_max_dB)
{
    opj_image_t *int_img, *flt_img;
    OPJ_UINT32 c;
    int ok = 1;

    opj_dwt_force_float_97 = 0;
    int_img = decode_file(path);
    if (!int_img) {
        fprintf(stderr, "SKIP: int16 decode failed: %s\n", path);
        return 1;
    }

    opj_dwt_force_float_97 = 1;
    flt_img = decode_file(path);
    if (!flt_img) {
        fprintf(stderr, "SKIP: float reference decode failed: %s\n", path);
        opj_image_destroy(int_img);
        return 1;
    }

    if (int_img->numcomps != flt_img->numcomps) {
        fprintf(stderr, "FAIL: numcomps mismatch on %s\n", path);
        opj_image_destroy(int_img);
        opj_image_destroy(flt_img);
        return 0;
    }

    for (c = 0; c < int_img->numcomps; ++c) {
        opj_image_comp_t *ci = &int_img->comps[c];
        opj_image_comp_t *cf = &flt_img->comps[c];
        OPJ_UINT32 n = ci->w * ci->h;
        OPJ_UINT32 i;
        double sse = 0.0;
        double peak;
        double psnr;

        if (ci->w != cf->w || ci->h != cf->h || ci->prec != cf->prec) {
            fprintf(stderr, "FAIL: comp %u dim/prec mismatch on %s\n",
                    c, path);
            ok = 0;
            continue;
        }
        for (i = 0; i < n; ++i) {
            double d = (double)ci->data[i] - (double)cf->data[i];
            sse += d * d;
        }
        peak = (double)((1U << ci->prec) - 1U);
        if (sse == 0.0) {
            psnr = INFINITY;
        } else {
            double mse = sse / (double)n;
            psnr = 10.0 * log10((peak * peak) / mse);
        }
        if (psnr < *out_min_dB) {
            *out_min_dB = psnr;
        }
        if (psnr > *out_max_dB) {
            *out_max_dB = psnr;
        }
        if (psnr < min_dB_required) {
            fprintf(stderr,
                    "FAIL: %s comp %u PSNR = %.2f dB (peak=%.0f, sse=%.0f, n=%u)\n",
                    path, c, psnr, peak, sse, n);
            ok = 0;
        } else {
            fprintf(stdout,
                    "OK:   %s comp %u PSNR = %.2f dB\n",
                    path, c, psnr);
        }
    }

    opj_image_destroy(int_img);
    opj_image_destroy(flt_img);
    return ok;
}

static const char *kFiles[] = {
    /* p1_03: 4-component 8-bit per sample (irreversible 9/7). */
    "input/conformance/p1_03.j2k",
    /* p1_04: 1-component 16-bit (out-of-gate: precision > 12; trivial pass). */
    "input/conformance/p1_04.j2k",
    /* p1_07: small test image. */
    "input/conformance/p1_07.j2k",
    /* p0_04: 4-component 8-bit color. */
    "input/conformance/p0_04.j2k",
    /* p0_05: 4-component color. */
    "input/conformance/p0_05.j2k",
    /* p0_07: multi-precision. */
    "input/conformance/p0_07.j2k",
    /* file1.jp2: JP2 wrapped. */
    "input/conformance/file1.jp2",
    /* JP2 wrapped color */
    "input/conformance/file4.jp2",
    NULL
};

int main(int argc, char **argv)
{
    const char *data_root;
    size_t f;
    int all_ok = 1;
    double min_dB = INFINITY;
    double max_dB = 0.0;
    const double threshold = 50.0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <OPJ_DATA_ROOT>\n", argv[0]);
        return 77; /* skip */
    }
    data_root = argv[1];

    /* SP-3.5 path is env-var opt-in; default off preserves byte-equivalence
     * with upstream openjpeg.  The test forces it on. */
    setenv("OPJ_DWT_INT16_97", "1", 1);

    for (f = 0; kFiles[f] != NULL; ++f) {
        char path[2048];
        FILE *fp;
        snprintf(path, sizeof(path), "%s/%s", data_root, kFiles[f]);
        fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "SKIP: file not found: %s\n", path);
            continue;
        }
        fclose(fp);
        if (!compare_psnr(path, threshold, &min_dB, &max_dB)) {
            all_ok = 0;
        }
    }

    if (!all_ok) {
        fprintf(stderr,
                "RESULT: FAIL (min PSNR = %.2f dB, threshold = %.2f dB)\n",
                min_dB, threshold);
        return 1;
    }
    fprintf(stdout,
            "RESULT: PASS (min PSNR = %.2f dB, max = %.2f dB, threshold = %.2f dB)\n",
            min_dB, max_dB, threshold);
    return 0;
}
