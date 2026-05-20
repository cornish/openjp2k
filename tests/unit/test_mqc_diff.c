/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Toby Cornish */
/*
 * Diff-test: decode a JP2/J2K file twice, once with the fast MQ path
 * enabled (default) and once forced off via OPJ_T1_FAST=0, and assert
 * byte-exact pixel output. Exits 0 on match, non-zero on mismatch.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openjpeg.h"

static opj_image_t *decode_file(const char *path)
{
    opj_dparameters_t parameters;
    opj_stream_t *stream;
    const char *ext;
    OPJ_CODEC_FORMAT fmt;
    opj_codec_t *codec;
    opj_image_t *image;

    opj_set_default_decoder_parameters(&parameters);

    stream = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    if (!stream) {
        fprintf(stderr, "stream open failed: %s\n", path);
        return NULL;
    }

    /* Sniff format from extension. */
    ext = strrchr(path, '.');
    fmt = OPJ_CODEC_J2K;
    if (ext && (strcmp(ext, ".jp2") == 0 || strcmp(ext, ".jpf") == 0)) {
        fmt = OPJ_CODEC_JP2;
    }
    codec = opj_create_decompress(fmt);
    if (!opj_setup_decoder(codec, &parameters)) {
        opj_destroy_codec(codec);
        opj_stream_destroy(stream);
        return NULL;
    }

    image = NULL;
    if (!opj_read_header(stream, codec, &image) ||
        !opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
        if (image) opj_image_destroy(image);
        opj_destroy_codec(codec);
        opj_stream_destroy(stream);
        return NULL;
    }
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return image;
}

static int compare_images(opj_image_t *a, opj_image_t *b, const char *path)
{
    OPJ_UINT32 c;

    if (a->numcomps != b->numcomps) {
        fprintf(stderr, "[%s] numcomps differ: %u vs %u\n", path,
                a->numcomps, b->numcomps);
        return 1;
    }
    for (c = 0; c < a->numcomps; ++c) {
        opj_image_comp_t *ca = &a->comps[c];
        opj_image_comp_t *cb = &b->comps[c];
        OPJ_UINT32 n;
        OPJ_UINT32 i;

        if (ca->w != cb->w || ca->h != cb->h) {
            fprintf(stderr, "[%s] comp %u size differs: %ux%u vs %ux%u\n",
                    path, c, ca->w, ca->h, cb->w, cb->h);
            return 1;
        }
        n = ca->w * ca->h;
        if (memcmp(ca->data, cb->data, n * sizeof(OPJ_INT32)) != 0) {
            /* Locate first mismatching pixel for the error. */
            for (i = 0; i < n; ++i) {
                if (ca->data[i] != cb->data[i]) {
                    fprintf(stderr,
                        "[%s] comp %u pixel %u differs: %d vs %d\n",
                        path, c, i, ca->data[i], cb->data[i]);
                    break;
                }
            }
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path;
    opj_image_t *legacy;
    opj_image_t *fast;
    int rc;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.jp2|file.j2k>\n", argv[0]);
        return 2;
    }
    path = argv[1];

    setenv("OPJ_T1_FAST", "0", 1);
    legacy = decode_file(path);
    if (!legacy) {
        fprintf(stderr, "legacy decode failed: %s\n", path);
        return 3;
    }

    setenv("OPJ_T1_FAST", "1", 1);
    fast = decode_file(path);
    if (!fast) {
        fprintf(stderr, "fast decode failed: %s\n", path);
        opj_image_destroy(legacy);
        return 4;
    }

    rc = compare_images(legacy, fast, path);
    opj_image_destroy(legacy);
    opj_image_destroy(fast);
    return rc;
}
