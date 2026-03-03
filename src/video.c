/* SPDX-License-Identifier: MIT */
#include "libra.h"
#include <string.h>

/* ========================================================================= */
/* Pixel format conversion → RGBA8                                            */
/* ========================================================================= */

void libra_convert_to_rgba(const void *src, unsigned w, unsigned h,
                           size_t pitch, int pixel_format,
                           unsigned char *dst)
{
    if (!src || !dst || w == 0 || h == 0) return;

    const unsigned char *src_bytes = (const unsigned char *)src;

    for (unsigned y = 0; y < h; y++) {
        const unsigned char *row = src_bytes + y * pitch;
        unsigned char *out = dst + y * w * 4;

        if (pixel_format == 1) {
            /* XRGB8888 */
            const uint32_t *px = (const uint32_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint32_t c = px[x];
                out[x * 4 + 0] = (unsigned char)((c >> 16) & 0xFF);
                out[x * 4 + 1] = (unsigned char)((c >>  8) & 0xFF);
                out[x * 4 + 2] = (unsigned char)( c        & 0xFF);
                out[x * 4 + 3] = 255;
            }
        } else if (pixel_format == 2) {
            /* RGB565 */
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint16_t c = px[x];
                unsigned r = (c >> 11) & 0x1F;
                unsigned g = (c >>  5) & 0x3F;
                unsigned b =  c        & 0x1F;
                out[x * 4 + 0] = (unsigned char)(r * 255 / 31);
                out[x * 4 + 1] = (unsigned char)(g * 255 / 63);
                out[x * 4 + 2] = (unsigned char)(b * 255 / 31);
                out[x * 4 + 3] = 255;
            }
        } else {
            /* XRGB1555 (format 0, default) */
            const uint16_t *px = (const uint16_t *)row;
            for (unsigned x = 0; x < w; x++) {
                uint16_t c = px[x];
                unsigned r = (c >> 10) & 0x1F;
                unsigned g = (c >>  5) & 0x1F;
                unsigned b =  c        & 0x1F;
                out[x * 4 + 0] = (unsigned char)(r * 255 / 31);
                out[x * 4 + 1] = (unsigned char)(g * 255 / 31);
                out[x * 4 + 2] = (unsigned char)(b * 255 / 31);
                out[x * 4 + 3] = 255;
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
