// SPDX-License-Identifier: MIT
/*
 * Unit tests for the video helpers added in commit 00bdba0:
 *   - libra_convert_to_rgba   (XRGB1555 / XRGB8888 / RGB565 → RGBA8)
 *   - libra_convert_to_rgb565 (XRGB1555 / XRGB8888 / RGB565 → RGB565)
 *   - libra_video_format_info (upload-metadata lookup)
 *
 * Pure-C, no libra context needed.  video.c is compiled directly into the
 * test binary so the test has zero link-time dependencies.
 */
#include "libra.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;

#define ASSERT(c) do {                                                   \
    if (!(c)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        g_fail = 1;                                                      \
        return;                                                          \
    }                                                                    \
} while (0)

/* Bit-replication reference — matches the implementation in video.c.
 * (5-bit → 8-bit: (c<<3)|(c>>2);  6-bit → 8-bit: (c<<2)|(c>>4)) */
static uint8_t exp5(uint8_t c) { return (uint8_t)((c << 3) | (c >> 2)); }
static uint8_t exp6(uint8_t c) { return (uint8_t)((c << 2) | (c >> 4)); }

/* -------------------------------------------------------------------------
 * Boundary / edge-case behaviour
 * ---------------------------------------------------------------------- */

static void test_null_and_zero(void)
{
    /* All of these are no-ops; the function must not segfault. */
    libra_convert_to_rgba(NULL, 4, 4, 8, 2, (uint8_t *)1);
    libra_convert_to_rgba((const void *)1, 4, 4, 8, 2, NULL);
    libra_convert_to_rgba((const void *)1, 0, 4, 8, 2, (uint8_t *)1);
    libra_convert_to_rgba((const void *)1, 4, 0, 8, 2, (uint8_t *)1);

    libra_convert_to_rgb565(NULL, 4, 4, 8, 2, (uint8_t *)1);
    libra_convert_to_rgb565((const void *)1, 4, 4, 8, 2, NULL);
    libra_convert_to_rgb565((const void *)1, 0, 4, 8, 2, (uint8_t *)1);
    libra_convert_to_rgb565((const void *)1, 4, 0, 8, 2, (uint8_t *)1);

    printf("PASS test_null_and_zero\n");
}

/* -------------------------------------------------------------------------
 * libra_convert_to_rgba — bit-replication correctness
 * ---------------------------------------------------------------------- */

static void test_rgba_from_rgb565(void)
{
    /* Build a row with every 5-bit R, every 5-bit B, and a swept 6-bit G.
     * Picking specific endpoint values forces the channel extraction to be
     * exact at both 0 and the max value. */
    enum { W = 4 };
    uint16_t src[W];
    /* (R5, G6, B5) tuples */
    const uint8_t in[W][3] = {
        { 0,    0,  0 },
        { 31,  63, 31 },
        { 16,  32, 16 },
        {  1,   1,  1 },
    };
    for (unsigned x = 0; x < W; x++) {
        src[x] = (uint16_t)((in[x][0] << 11) | (in[x][1] << 5) | in[x][2]);
    }

    uint8_t dst[W * 4] = {0};
    libra_convert_to_rgba(src, W, 1, sizeof(src), 2 /* RGB565 */, dst);

    for (unsigned x = 0; x < W; x++) {
        ASSERT(dst[x * 4 + 0] == exp5(in[x][0]));   /* R */
        ASSERT(dst[x * 4 + 1] == exp6(in[x][1]));   /* G */
        ASSERT(dst[x * 4 + 2] == exp5(in[x][2]));   /* B */
        ASSERT(dst[x * 4 + 3] == 0xFF);             /* A */
    }
    printf("PASS test_rgba_from_rgb565\n");
}

static void test_rgba_from_xrgb1555(void)
{
    enum { W = 4 };
    uint16_t src[W];
    const uint8_t in[W][3] = {
        { 0,   0,  0 },
        { 31, 31, 31 },
        { 16, 16, 16 },
        {  3, 12, 20 },
    };
    for (unsigned x = 0; x < W; x++) {
        /* X bit (15) set to 1 — must be ignored by the converter. */
        src[x] = (uint16_t)(0x8000u | (in[x][0] << 10) | (in[x][1] << 5) | in[x][2]);
    }

    uint8_t dst[W * 4] = {0};
    libra_convert_to_rgba(src, W, 1, sizeof(src), 0 /* XRGB1555 */, dst);

    for (unsigned x = 0; x < W; x++) {
        ASSERT(dst[x * 4 + 0] == exp5(in[x][0]));
        ASSERT(dst[x * 4 + 1] == exp5(in[x][1]));
        ASSERT(dst[x * 4 + 2] == exp5(in[x][2]));
        ASSERT(dst[x * 4 + 3] == 0xFF);
    }
    printf("PASS test_rgba_from_xrgb1555\n");
}

static void test_rgba_from_xrgb8888(void)
{
    enum { W = 4 };
    /* Source uint32 layout (libretro XRGB8888, little-endian load):
     *   bits 31-24 = X, 23-16 = R, 15-8 = G, 7-0 = B. */
    uint32_t src[W] = {
        0xDE000000u,                     /* X arbitrary; R=G=B=0  */
        0x00FFFFFFu,                     /* X=0;        R=G=B=255 */
        0x12345678u,                     /* R=0x34 G=0x56 B=0x78  */
        0xFF112233u,                     /* X=FF; R=0x11 G=0x22 B=0x33 */
    };
    const uint8_t expected[W][3] = {
        { 0x00, 0x00, 0x00 },
        { 0xFF, 0xFF, 0xFF },
        { 0x34, 0x56, 0x78 },
        { 0x11, 0x22, 0x33 },
    };

    uint8_t dst[W * 4] = {0};
    libra_convert_to_rgba(src, W, 1, sizeof(src), 1 /* XRGB8888 */, dst);

    for (unsigned x = 0; x < W; x++) {
        ASSERT(dst[x * 4 + 0] == expected[x][0]);   /* R */
        ASSERT(dst[x * 4 + 1] == expected[x][1]);   /* G */
        ASSERT(dst[x * 4 + 2] == expected[x][2]);   /* B */
        ASSERT(dst[x * 4 + 3] == 0xFF);
    }
    printf("PASS test_rgba_from_xrgb8888\n");
}

/* Source pitch wider than w*bpp — the converter must respect pitch when
 * stepping rows and never read into the padding. */
static void test_rgba_non_tight_pitch(void)
{
    enum { W = 3, H = 2 };
    const size_t pitch = 16;  /* > W*2 */
    uint8_t src_buf[H * pitch];
    memset(src_buf, 0xCC, sizeof(src_buf));      /* poison the padding */

    /* Row 0: solid red.  Row 1: solid blue. */
    uint16_t row0_px = (uint16_t)((31 << 11) | (0 << 5) | 0);
    uint16_t row1_px = (uint16_t)((0  << 11) | (0 << 5) | 31);
    for (unsigned x = 0; x < W; x++) {
        memcpy(src_buf + 0 * pitch + x * 2, &row0_px, 2);
        memcpy(src_buf + 1 * pitch + x * 2, &row1_px, 2);
    }

    uint8_t dst[W * H * 4] = {0};
    libra_convert_to_rgba(src_buf, W, H, pitch, 2 /* RGB565 */, dst);

    for (unsigned x = 0; x < W; x++) {
        /* Row 0 → red */
        ASSERT(dst[(0 * W + x) * 4 + 0] == 0xFF);
        ASSERT(dst[(0 * W + x) * 4 + 1] == 0x00);
        ASSERT(dst[(0 * W + x) * 4 + 2] == 0x00);
        ASSERT(dst[(0 * W + x) * 4 + 3] == 0xFF);
        /* Row 1 → blue */
        ASSERT(dst[(1 * W + x) * 4 + 0] == 0x00);
        ASSERT(dst[(1 * W + x) * 4 + 1] == 0x00);
        ASSERT(dst[(1 * W + x) * 4 + 2] == 0xFF);
        ASSERT(dst[(1 * W + x) * 4 + 3] == 0xFF);
    }
    printf("PASS test_rgba_non_tight_pitch\n");
}

/* -------------------------------------------------------------------------
 * libra_convert_to_rgb565
 * ---------------------------------------------------------------------- */

static void test_rgb565_passthrough(void)
{
    enum { W = 5 };
    uint16_t src[W] = { 0x0000, 0xF800, 0x07E0, 0x001F, 0xABCD };
    uint16_t dst[W] = {0};
    libra_convert_to_rgb565(src, W, 1, sizeof(src), 2 /* RGB565 */, (uint8_t *)dst);
    for (unsigned x = 0; x < W; x++) ASSERT(dst[x] == src[x]);
    printf("PASS test_rgb565_passthrough\n");
}

static void test_rgb565_from_xrgb1555(void)
{
    /* Green expansion: 5-bit g5 → 6-bit g6 via (g5 << 1) | (g5 >> 4). */
    enum { W = 4 };
    uint16_t src[W];
    const uint8_t in[W][3] = {
        { 0,   0,  0 },
        { 31, 31, 31 },
        { 16, 16, 16 },
        {  3, 12, 20 },
    };
    for (unsigned x = 0; x < W; x++)
        src[x] = (uint16_t)(0x8000u | (in[x][0] << 10) | (in[x][1] << 5) | in[x][2]);

    uint16_t dst[W] = {0};
    libra_convert_to_rgb565(src, W, 1, sizeof(src), 0 /* XRGB1555 */, (uint8_t *)dst);

    for (unsigned x = 0; x < W; x++) {
        uint16_t g6 = (uint16_t)((in[x][1] << 1) | (in[x][1] >> 4));
        uint16_t expected = (uint16_t)((in[x][0] << 11) | (g6 << 5) | in[x][2]);
        ASSERT(dst[x] == expected);
    }
    printf("PASS test_rgb565_from_xrgb1555\n");
}

static void test_rgb565_from_xrgb8888(void)
{
    /* Down-sample: keep top 5 bits of R/B, top 6 bits of G. */
    enum { W = 4 };
    uint32_t src[W] = {
        0x00000000u,
        0x00FFFFFFu,
        0x00112233u,
        0xFF818182u,   /* X set; payload R=0x81 G=0x81 B=0x82 */
    };

    uint16_t dst[W] = {0};
    libra_convert_to_rgb565(src, W, 1, sizeof(src), 1 /* XRGB8888 */, (uint8_t *)dst);

    for (unsigned x = 0; x < W; x++) {
        uint32_t c = src[x];
        uint16_t r5 = (uint16_t)(((c >> 16) & 0xFF) >> 3);
        uint16_t g6 = (uint16_t)(((c >>  8) & 0xFF) >> 2);
        uint16_t b5 = (uint16_t)(( c        & 0xFF) >> 3);
        uint16_t expected = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        ASSERT(dst[x] == expected);
    }
    printf("PASS test_rgb565_from_xrgb8888\n");
}

/* -------------------------------------------------------------------------
 * libra_video_format_info
 * ---------------------------------------------------------------------- */

static void test_format_info(void)
{
    struct libra_video_format_info info;

    ASSERT(libra_video_format_info(0, &info));
    ASSERT(info.bytes_per_pixel == 2);
    ASSERT(info.name != NULL && strcmp(info.name, "XRGB1555") == 0);

    ASSERT(libra_video_format_info(1, &info));
    ASSERT(info.bytes_per_pixel == 4);
    ASSERT(strcmp(info.name, "XRGB8888") == 0);

    ASSERT(libra_video_format_info(2, &info));
    ASSERT(info.bytes_per_pixel == 2);
    ASSERT(strcmp(info.name, "RGB565") == 0);
    /* RGB565 → GL type = UNSIGNED_SHORT_5_6_5 (0x8363) */
    ASSERT(info.gl_type == 0x8363);

    /* Invalid format index — must return false and zero the output. */
    memset(&info, 0xAA, sizeof(info));
    ASSERT(!libra_video_format_info(-1, &info));
    ASSERT(info.bytes_per_pixel == 0);
    ASSERT(info.name == NULL);

    memset(&info, 0xAA, sizeof(info));
    ASSERT(!libra_video_format_info(99, &info));
    ASSERT(info.bytes_per_pixel == 0);

    /* NULL output pointer — must return false. */
    ASSERT(!libra_video_format_info(2, NULL));

    printf("PASS test_format_info\n");
}

/* -------------------------------------------------------------------------
 * Endpoint sweep — exhaustive over the 16-bit input space for the two
 * 16bpp source formats; sanity-checks the bit-replication arithmetic on
 * every possible 5/6/5 channel combination.
 * ---------------------------------------------------------------------- */

static void test_full_sweep_rgb565(void)
{
    /* Synthesize one row of every 16-bit value, convert, and re-derive
     * the expected RGBA from the formula.  Cheap (~64k pixels). */
    enum { W = 65536 };
    uint16_t *src = malloc((size_t)W * 2);
    uint8_t  *dst = malloc((size_t)W * 4);
    ASSERT(src && dst);

    for (unsigned x = 0; x < W; x++) src[x] = (uint16_t)x;
    libra_convert_to_rgba(src, W, 1, (size_t)W * 2, 2, dst);

    for (unsigned x = 0; x < W; x++) {
        uint16_t c = src[x];
        uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
        uint8_t g6 = (uint8_t)((c >>  5) & 0x3F);
        uint8_t b5 = (uint8_t)( c        & 0x1F);
        ASSERT(dst[x * 4 + 0] == exp5(r5));
        ASSERT(dst[x * 4 + 1] == exp6(g6));
        ASSERT(dst[x * 4 + 2] == exp5(b5));
        ASSERT(dst[x * 4 + 3] == 0xFF);
    }
    free(src);
    free(dst);
    printf("PASS test_full_sweep_rgb565\n");
}

static void test_full_sweep_xrgb1555(void)
{
    enum { W = 65536 };
    uint16_t *src = malloc((size_t)W * 2);
    uint8_t  *dst = malloc((size_t)W * 4);
    ASSERT(src && dst);

    for (unsigned x = 0; x < W; x++) src[x] = (uint16_t)x;
    libra_convert_to_rgba(src, W, 1, (size_t)W * 2, 0, dst);

    for (unsigned x = 0; x < W; x++) {
        uint16_t c = src[x];
        uint8_t r5 = (uint8_t)((c >> 10) & 0x1F);
        uint8_t g5 = (uint8_t)((c >>  5) & 0x1F);
        uint8_t b5 = (uint8_t)( c        & 0x1F);
        ASSERT(dst[x * 4 + 0] == exp5(r5));
        ASSERT(dst[x * 4 + 1] == exp5(g5));
        ASSERT(dst[x * 4 + 2] == exp5(b5));
        ASSERT(dst[x * 4 + 3] == 0xFF);
    }
    free(src);
    free(dst);
    printf("PASS test_full_sweep_xrgb1555\n");
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    test_null_and_zero();
    test_rgba_from_rgb565();
    test_rgba_from_xrgb1555();
    test_rgba_from_xrgb8888();
    test_rgba_non_tight_pitch();
    test_rgb565_passthrough();
    test_rgb565_from_xrgb1555();
    test_rgb565_from_xrgb8888();
    test_format_info();
    test_full_sweep_rgb565();
    test_full_sweep_xrgb1555();

    if (g_fail) {
        fprintf(stderr, "FAILED\n");
        return 1;
    }
    printf("ALL PASS\n");
    return 0;
}
