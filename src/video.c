/* SPDX-License-Identifier: MIT */
#include "libra.h"
#include <stdint.h>
#include <string.h>

/* ========================================================================= */
/* Pixel format conversion → RGBA8                                            */
/*                                                                            */
/* This helper exists for frontends that cannot upload the core's native      */
/* pixel format directly (e.g. software framebuffers, screenshot dumps).      */
/* GPU-accelerated frontends SHOULD NOT call this in the frame loop — they    */
/* should upload the native format and let the GPU swizzle.  See              */
/* libra_video_format_info() for the per-format upload hints.                 */
/*                                                                            */
/* Implementation notes:                                                      */
/*   - Bit-replication ((c << 3) | (c >> 2)) replaces (c * 255 / 31)           */
/*     divides.  Max error vs the divide is ±1 LSB; visually identical, but   */
/*     existing byte-exact golden images will need regen.                     */
/*   - Per-pixel uint32 store via __builtin_memcpy handles unaligned dst on   */
/*     every target (incl. mips32r2) without the 4 byte-stores penalty.       */
/*   - Inner loops are branchless and straight-line so the compiler           */
/*     auto-vectorises to NEON / SSE2 / RVV without intrinsics.               */
/* ========================================================================= */

static inline uint32_t expand5(uint32_t c) { return (c << 3) | (c >> 2); }
static inline uint32_t expand6(uint32_t c) { return (c << 2) | (c >> 4); }

static void convert_row_rgb565(const uint16_t *src, uint8_t *dst, unsigned w)
{
    for (unsigned x = 0; x < w; x++) {
        uint32_t c  = src[x];
        uint32_t r8 = expand5((c >> 11) & 0x1F);
        uint32_t g8 = expand6((c >>  5) & 0x3F);
        uint32_t b8 = expand5( c        & 0x1F);
        uint32_t p  = 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
        __builtin_memcpy(dst + x * 4, &p, 4);
    }
}

static void convert_row_xrgb1555(const uint16_t *src, uint8_t *dst, unsigned w)
{
    for (unsigned x = 0; x < w; x++) {
        uint32_t c  = src[x];
        uint32_t r8 = expand5((c >> 10) & 0x1F);
        uint32_t g8 = expand5((c >>  5) & 0x1F);
        uint32_t b8 = expand5( c        & 0x1F);
        uint32_t p  = 0xFF000000u | (b8 << 16) | (g8 << 8) | r8;
        __builtin_memcpy(dst + x * 4, &p, 4);
    }
}

static void convert_row_xrgb8888(const uint32_t *src, uint8_t *dst, unsigned w)
{
    /* Source uint32 (little-endian load): 0xXXRRGGBB → bytes X,R,G,B from
     * low to high in memory ordering of the original libretro layout.
     * Destination RGBA8: dst[0]=R, dst[1]=G, dst[2]=B, dst[3]=A.
     * As a little-endian uint32 that's 0xAABBGGRR. */
    for (unsigned x = 0; x < w; x++) {
        uint32_t c = src[x];
        uint32_t p = 0xFF000000u                   /* A = 0xFF              */
                   | ((c & 0x00FF0000u) >> 16)     /* R: src[23:16] → [7:0] */
                   |  (c & 0x0000FF00u)            /* G: src[15:8]  → [15:8]*/
                   | ((c & 0x000000FFu) << 16);    /* B: src[7:0]   → [23:16]*/
        __builtin_memcpy(dst + x * 4, &p, 4);
    }
}

void libra_convert_to_rgba(const void *src, unsigned w, unsigned h,
                           size_t pitch, int pixel_format,
                           unsigned char *dst)
{
    if (!src || !dst || w == 0 || h == 0) return;

    const uint8_t *src_bytes = (const uint8_t *)src;

    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = src_bytes + y * pitch;
        uint8_t       *out = dst + (size_t)y * w * 4;

        switch (pixel_format) {
        case 1: convert_row_xrgb8888((const uint32_t *)row, out, w); break;
        case 2: convert_row_rgb565  ((const uint16_t *)row, out, w); break;
        default: convert_row_xrgb1555((const uint16_t *)row, out, w); break;
        }
    }
}

/* ========================================================================= */
/* Pixel format introspection                                                 */
/*                                                                            */
/* Lets a frontend pick the cheapest GPU upload path (or a software path)     */
/* without hard-coding format tables.                                         */
/* ========================================================================= */

bool libra_video_format_info(int pixel_format,
                             struct libra_video_format_info *out)
{
    if (!out) return false;

    static const struct libra_video_format_info table[] = {
        /* 0: XRGB1555 (RETRO_PIXEL_FORMAT_0RGB1555) */
        {
            .name             = "XRGB1555",
            .bytes_per_pixel  = 2,
            .gl_internalformat = 0x8057,                /* GL_RGB5_A1            */
            .gl_format         = 0x80E1,                /* GL_BGRA               */
            .gl_type           = 0x8366,                /* UNSIGNED_SHORT_1_5_5_5_REV */
            .vk_format         = 5,                     /* VK_FORMAT_A1R5G5B5_UNORM_PACK16 */
            .sdl_pixelformat   = 0x16365002,            /* SDL_PIXELFORMAT_ARGB1555 */
            .channel_order_matches_gpu = true,
        },
        /* 1: XRGB8888 (RETRO_PIXEL_FORMAT_XRGB8888) */
        {
            .name             = "XRGB8888",
            .bytes_per_pixel  = 4,
            .gl_internalformat = 0x8058,                /* GL_RGBA8              */
            .gl_format         = 0x80E1,                /* GL_BGRA               */
            .gl_type           = 0x1401,                /* GL_UNSIGNED_BYTE      */
            .vk_format         = 50,                    /* VK_FORMAT_B8G8R8A8_UNORM */
            .sdl_pixelformat   = 0x16362004,            /* SDL_PIXELFORMAT_ARGB8888 */
            .channel_order_matches_gpu = true,
        },
        /* 2: RGB565 (RETRO_PIXEL_FORMAT_RGB565) */
        {
            .name             = "RGB565",
            .bytes_per_pixel  = 2,
            .gl_internalformat = 0x8D62,                /* GL_RGB565             */
            .gl_format         = 0x1907,                /* GL_RGB                */
            .gl_type           = 0x8363,                /* UNSIGNED_SHORT_5_6_5  */
            .vk_format         = 4,                     /* VK_FORMAT_R5G6B5_UNORM_PACK16 */
            .sdl_pixelformat   = 0x15151002,            /* SDL_PIXELFORMAT_RGB565 */
            .channel_order_matches_gpu = true,
        },
    };

    if (pixel_format < 0 || pixel_format > 2) {
        *out = (struct libra_video_format_info){0};
        return false;
    }
    *out = table[pixel_format];
    return true;
}

/* ========================================================================= */
/* Cross-format conversion: RGB565 ← XRGB1555 / XRGB8888                      */
/*                                                                            */
/* For software frontends that want to stay at 16bpp.  Same vectorisable      */
/* shape as the RGBA8 paths.                                                  */
/* ========================================================================= */

static inline uint16_t pack_565(uint32_t r8, uint32_t g8, uint32_t b8)
{
    return (uint16_t)(((r8 >> 3) << 11) | ((g8 >> 2) << 5) | (b8 >> 3));
}

void libra_convert_to_rgb565(const void *src, unsigned w, unsigned h,
                             size_t pitch, int pixel_format,
                             unsigned char *dst)
{
    if (!src || !dst || w == 0 || h == 0) return;

    const uint8_t *src_bytes = (const uint8_t *)src;

    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row  = src_bytes + y * pitch;
        uint16_t      *out  = (uint16_t *)(dst + (size_t)y * w * 2);

        if (pixel_format == 2) {
            /* RGB565 → RGB565: just a row memcpy. */
            memcpy(out, row, (size_t)w * 2);
        } else if (pixel_format == 1) {
            /* XRGB8888 → RGB565: down-sample channels. */
            const uint32_t *px = (const uint32_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint32_t c = px[x];
                out[x] = pack_565((c >> 16) & 0xFF,
                                  (c >>  8) & 0xFF,
                                   c        & 0xFF);
            }
        } else {
            /* XRGB1555 → RGB565: r,b stay 5-bit; g grows 5→6 via bit-replicate. */
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint32_t c  = px[x];
                uint32_t r5 = (c >> 10) & 0x1F;
                uint32_t g5 = (c >>  5) & 0x1F;
                uint32_t b5 =  c        & 0x1F;
                uint32_t g6 = (g5 << 1) | (g5 >> 4);   /* 5-bit → 6-bit */
                out[x] = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
            }
        }
    }
}

/* ========================================================================= */
/* Viewport calculation                                                       */
/* ========================================================================= */

void libra_compute_viewport(unsigned win_w, unsigned win_h,
                            unsigned rotation, float core_aspect,
                            unsigned src_w, unsigned src_h,
                            int display_mode,
                            float *dst_x, float *dst_y,
                            float *dst_w, float *dst_h)
{
    if (!dst_x || !dst_y || !dst_w || !dst_h) return;

    /* Apply rotation: if 90° or 270°, swap source dimensions and invert aspect */
    unsigned eff_w = (rotation & 1) ? src_h : src_w;
    unsigned eff_h = (rotation & 1) ? src_w : src_h;
    float eff_aspect = (rotation & 1)
        ? (core_aspect > 0.0f ? 1.0f / core_aspect : 0.0f)
        : core_aspect;

    float dw = (float)win_w;
    float dh = (float)win_h;
    float dx = 0.0f;
    float dy = 0.0f;

    if (display_mode == LIBRA_DISPLAY_INTEGER && eff_w > 0 && eff_h > 0) {
        /* Integer scaling */
        unsigned sx = win_w / eff_w;
        unsigned sy = win_h / eff_h;
        unsigned scale = sx < sy ? sx : sy;
        if (scale < 1) scale = 1;
        dw = (float)(eff_w * scale);
        dh = (float)(eff_h * scale);
        dx = ((float)win_w - dw) * 0.5f;
        dy = ((float)win_h - dh) * 0.5f;
    } else if (display_mode == LIBRA_DISPLAY_ASPECT && eff_aspect > 0.0f) {
        /* Letterbox/pillarbox */
        float screen_aspect = (float)win_w / (float)win_h;
        if (eff_aspect > screen_aspect) {
            dw = (float)win_w;
            dh = (float)win_w / eff_aspect;
            dx = 0.0f;
            dy = ((float)win_h - dh) * 0.5f;
        } else {
            dh = (float)win_h;
            dw = (float)win_h * eff_aspect;
            dx = ((float)win_w - dw) * 0.5f;
            dy = 0.0f;
        }
    }
    /* LIBRA_DISPLAY_STRETCH: dx=0, dy=0, dw=win_w, dh=win_h (already set) */

    *dst_x = dx;
    *dst_y = dy;
    *dst_w = dw;
    *dst_h = dh;
}
