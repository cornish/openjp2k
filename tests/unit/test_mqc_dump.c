// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Toby Cornish
//
// Decode one file and write a deterministic raw representation of the
// pixel data to stdout. Used by scripts/run_diff_test.sh which invokes
// this binary twice (once per OPJ_T1_FAST setting) and compares outputs.
//
// Output format (binary, big-endian for header fields):
//   "OJ2KDUMP\0\0\0\1"   12-byte magic + version
//   uint32 numcomps
//   for each component:
//     uint32 w, uint32 h, int32 prec, int32 sgnd
//     (w*h) int32 samples in component-buffer order
//     (samples are written in host byte order; this dump format is
//     intended for cmp-style same-machine comparison only)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openjpeg.h"

static void wbe32(FILE *f, OPJ_UINT32 v)
{
    OPJ_BYTE b[4];
    b[0] = (OPJ_BYTE)(v >> 24);
    b[1] = (OPJ_BYTE)(v >> 16);
    b[2] = (OPJ_BYTE)(v >> 8);
    b[3] = (OPJ_BYTE)v;
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
    const char *path;
    opj_dparameters_t parameters;
    opj_stream_t *stream;
    const char *ext;
    OPJ_CODEC_FORMAT fmt;
    opj_codec_t *codec;
    opj_image_t *image;
    OPJ_UINT32 c;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file>\n", argv[0]);
        return 2;
    }
    path = argv[1];

    opj_set_default_decoder_parameters(&parameters);

    stream = opj_stream_create_default_file_stream(path, OPJ_TRUE);
    if (!stream) {
        fprintf(stderr, "stream open failed: %s\n", path);
        return 3;
    }

    ext = strrchr(path, '.');
    fmt = OPJ_CODEC_J2K;
    if (ext && (strcmp(ext, ".jp2") == 0 || strcmp(ext, ".jpf") == 0)) {
        fmt = OPJ_CODEC_JP2;
    }

    codec = opj_create_decompress(fmt);
    if (!codec) {
        fprintf(stderr, "opj_create_decompress failed: %s\n", path);
        opj_stream_destroy(stream);
        return 3;
    }

    opj_setup_decoder(codec, &parameters);

    image = NULL;
    if (!opj_read_header(stream, codec, &image) ||
        !opj_decode(codec, stream, image) ||
        !opj_end_decompress(codec, stream)) {
        fprintf(stderr, "decode failed: %s\n", path);
        if (image) {
            opj_image_destroy(image);
        }
        opj_destroy_codec(codec);
        opj_stream_destroy(stream);
        return 4;
    }

    fwrite("OJ2KDUMP\0\0\0\1", 1, 12, stdout);
    wbe32(stdout, image->numcomps);
    for (c = 0; c < image->numcomps; ++c) {
        opj_image_comp_t *cc = &image->comps[c];
        wbe32(stdout, cc->w);
        wbe32(stdout, cc->h);
        wbe32(stdout, (OPJ_UINT32)cc->prec);
        wbe32(stdout, (OPJ_UINT32)cc->sgnd);
        fwrite(cc->data, sizeof(OPJ_INT32), (size_t)cc->w * cc->h, stdout);
    }

    opj_image_destroy(image);
    opj_destroy_codec(codec);
    opj_stream_destroy(stream);
    return 0;
}
