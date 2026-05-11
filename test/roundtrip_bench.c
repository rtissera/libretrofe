// SPDX-License-Identifier: MIT
/*
 * Round-trip benchmark: REG-Station-style SW upload path exercised end to
 * end against the real libra → core → libra glue, with a real GLES2 +
 * EGL pbuffer context for the upload stage.
 *
 * Per-frame stages timed independently:
 *
 *   t1-t0 : libra_run()           — core retro_run + libra glue
 *   t2-t1 : SW upload prep        — convert/copy + row flip (CPU)
 *   t3-t2 : glTexImage2D upload   — driver upload bandwidth (sync'd via glFinish)
 *
 * For each path (UP_NATIVE_RGB565 / UP_NATIVE_BGRA8 / UP_CONVERT_RGB565 /
 * UP_CONVERT_RGBA8) we run the same core for N frames and report the
 * mean/p50/p99 of each stage.  This mirrors the LibraVideoSW.cpp logic
 * but at the C level so we can swap paths under harness control.
 *
 * Mesa softpipe is sufficient — no GPU required.  Absolute numbers won't
 * match Cortex-A53; relative ratios between paths do.
 *
 * Usage:
 *   roundtrip_bench <core.so> <rom> [frames] [path-name]
 *   path-name: rgb565-native | bgra-native | convert-rgba8 | convert-rgb565 | auto
 */
#define _POSIX_C_SOURCE 200809L

#include "libra.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* GLES2-ext constants we may need but headers may omit. */
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif

/* ---------------------------------------------------------------------- */

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

/* ---------------------------------------------------------------------- */
/* Captured frame (libra video callback) */

static const void *g_frame_data  = NULL;
static unsigned    g_frame_w     = 0;
static unsigned    g_frame_h     = 0;
static size_t      g_frame_pitch = 0;
static int         g_frame_fmt   = -1;
static int         g_have_frame  = 0;

/* Shadow buffer: callback fires inside retro_run() and the source pointer
 * is only valid for the duration of that call.  Copy it out so the
 * harness can use it after libra_run() returns. */
static uint8_t *g_frame_copy     = NULL;
static size_t   g_frame_copy_cap = 0;

static void video_cb(void *ud, const void *data, unsigned w, unsigned h,
                     size_t pitch, int pixel_format)
{
    (void)ud;
    if (!data || w == 0 || h == 0) { g_have_frame = 0; return; }
    size_t bytes = pitch * h;
    if (bytes > g_frame_copy_cap) {
        g_frame_copy = realloc(g_frame_copy, bytes);
        g_frame_copy_cap = bytes;
    }
    memcpy(g_frame_copy, data, bytes);
    g_frame_data  = g_frame_copy;
    g_frame_w     = w;
    g_frame_h     = h;
    g_frame_pitch = pitch;
    g_frame_fmt   = pixel_format;
    g_have_frame  = 1;
}

/* Drop audio + input — we only care about video here. */
static void audio_cb(void *ud, const int16_t *frames, unsigned n)
{ (void)ud; (void)frames; (void)n; }
static void input_poll_cb(void *ud) { (void)ud; }
static int16_t input_state_cb(void *ud, unsigned p, unsigned d, unsigned i, unsigned id)
{ (void)ud; (void)p; (void)d; (void)i; (void)id; return 0; }

/* ---------------------------------------------------------------------- */
/* SW upload prep — mirror of LibraVideoSW.cpp logic */

enum UploadPath {
    UP_NATIVE_RGB565,
    UP_NATIVE_BGRA8,
    UP_CONVERT_RGB565,
    UP_CONVERT_RGBA8,
};

static const char *path_name(enum UploadPath p)
{
    switch (p) {
    case UP_NATIVE_RGB565:  return "rgb565-native";
    case UP_NATIVE_BGRA8:   return "bgra-native";
    case UP_CONVERT_RGB565: return "convert-rgb565";
    case UP_CONVERT_RGBA8:  return "convert-rgba8";
    }
    return "?";
}

/* Pick the realistic path for a given source format (mirror of pickPath
 * in REG-Station's LibraVideoSW.cpp). */
static enum UploadPath pick_auto(int fmt, bool bgra_ext)
{
    if (fmt == 2)              return UP_NATIVE_RGB565;
    if (fmt == 1 && bgra_ext)  return UP_NATIVE_BGRA8;
    if (fmt == 1)              return UP_CONVERT_RGBA8;
    return UP_CONVERT_RGB565;
}

/* Copy `src` rows into `dst` in reverse order (HW-convention bottom-left
 * origin).  Single CPU pass — matches REG-Station's copyRowsFlipped. */
static void copy_rows_flipped(const uint8_t *src, size_t src_pitch,
                              uint8_t *dst, unsigned w, unsigned h,
                              unsigned bpp)
{
    size_t rb = (size_t)w * bpp;
    for (unsigned y = 0; y < h; y++)
        memcpy(dst + (h - 1 - y) * rb, src + (size_t)y * src_pitch, rb);
}

static void flip_in_place(uint8_t *buf, unsigned w, unsigned h, unsigned bpp)
{
    size_t rb = (size_t)w * bpp;
    uint8_t *tmp = malloc(rb);
    for (unsigned y = 0; y < h / 2; y++) {
        uint8_t *a = buf + y * rb;
        uint8_t *b = buf + (h - 1 - y) * rb;
        memcpy(tmp, a, rb);
        memcpy(a, b, rb);
        memcpy(b, tmp, rb);
    }
    free(tmp);
}

/* ---------------------------------------------------------------------- */
/* EGL pbuffer + GLES2 bootstrap */

static EGLDisplay g_egl_dpy = EGL_NO_DISPLAY;
static EGLContext g_egl_ctx = EGL_NO_CONTEXT;
static EGLSurface g_egl_sfc = EGL_NO_SURFACE;
static bool       g_bgra_ext = false;

static bool gl_init(void)
{
    g_egl_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return false; }
    if (!eglInitialize(g_egl_dpy, NULL, NULL)) {
        fprintf(stderr, "eglInitialize failed\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n;
    if (!eglChooseConfig(g_egl_dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return false;
    }
    EGLint pb_attrs[] = { EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_NONE };
    g_egl_sfc = eglCreatePbufferSurface(g_egl_dpy, cfg, pb_attrs);
    if (g_egl_sfc == EGL_NO_SURFACE) { fprintf(stderr, "eglCreatePbufferSurface failed\n"); return false; }

    EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_egl_ctx = eglCreateContext(g_egl_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g_egl_ctx == EGL_NO_CONTEXT) { fprintf(stderr, "eglCreateContext failed\n"); return false; }

    if (!eglMakeCurrent(g_egl_dpy, g_egl_sfc, g_egl_sfc, g_egl_ctx)) {
        fprintf(stderr, "eglMakeCurrent failed\n"); return false;
    }

    const char *exts = (const char *)glGetString(GL_EXTENSIONS);
    g_bgra_ext = exts && (strstr(exts, "GL_EXT_texture_format_BGRA8888") != NULL
                       || strstr(exts, "GL_APPLE_texture_format_BGRA8888") != NULL);

    fprintf(stderr, "GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "GL_VERSION:  %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "BGRA ext:    %s\n", g_bgra_ext ? "yes" : "no");
    return true;
}

static void gl_shutdown(void)
{
    if (g_egl_dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g_egl_ctx) eglDestroyContext(g_egl_dpy, g_egl_ctx);
        if (g_egl_sfc) eglDestroySurface(g_egl_dpy, g_egl_sfc);
        eglTerminate(g_egl_dpy);
    }
}

/* ---------------------------------------------------------------------- */
/* Bench loop */

struct stats {
    double *samples;
    unsigned n;
    unsigned cap;
};

static void stats_push(struct stats *s, double v)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->samples = realloc(s->samples, s->cap * sizeof(double));
    }
    s->samples[s->n++] = v;
}

static void stats_report(const char *name, struct stats *s)
{
    if (s->n == 0) { printf("  %-22s no samples\n", name); return; }
    qsort(s->samples, s->n, sizeof(double), cmp_double);
    double mean = 0; for (unsigned i = 0; i < s->n; i++) mean += s->samples[i]; mean /= s->n;
    double p50 = s->samples[s->n / 2];
    double p99 = s->samples[(s->n * 99) / 100];
    printf("  %-22s  mean=%8.1f µs  p50=%8.1f µs  p99=%8.1f µs\n",
           name, mean / 1000.0, p50 / 1000.0, p99 / 1000.0);
}

static int run_bench(const char *core_path, const char *rom_path,
                     unsigned frames, enum UploadPath forced_path,
                     bool use_auto)
{
    /* libra setup */
    libra_config_t cfg = {
        .video        = video_cb,
        .audio        = audio_cb,
        .input_poll   = input_poll_cb,
        .input_state  = input_state_cb,
        .userdata     = NULL,
        .audio_output_rate = 48000,
    };
    libra_ctx_t *ctx = libra_create(&cfg);
    if (!ctx) { fprintf(stderr, "libra_create failed\n"); return 1; }
    if (!libra_load_core(ctx, core_path)) {
        fprintf(stderr, "libra_load_core(%s) failed\n", core_path);
        libra_destroy(ctx); return 1;
    }
    if (!libra_load_game(ctx, rom_path)) {
        fprintf(stderr, "libra_load_game(%s) failed\n", rom_path);
        libra_destroy(ctx); return 1;
    }

    /* Prime: run a few frames so the core's first-frame allocations settle. */
    for (unsigned i = 0; i < 10; i++) libra_run(ctx);
    if (!g_have_frame) {
        fprintf(stderr, "No frame produced after priming.\n");
        libra_destroy(ctx); return 1;
    }

    /* Decide path per first-frame format if auto. */
    enum UploadPath path = use_auto ? pick_auto(g_frame_fmt, g_bgra_ext)
                                    : forced_path;
    fprintf(stderr, "Core fmt = %d (%s), bench path = %s\n",
            g_frame_fmt,
            g_frame_fmt == 0 ? "XRGB1555" : g_frame_fmt == 1 ? "XRGB8888" : "RGB565",
            path_name(path));

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    uint8_t *upload_buf = NULL;
    size_t   upload_cap = 0;

    struct stats s_run = {0}, s_prep = {0}, s_upload = {0}, s_total = {0};

    GLint  tex_iformat_seen = 0;
    bool   first_tex = true;

    for (unsigned f = 0; f < frames; f++) {
        double t0 = now_ns();
        libra_run(ctx);
        double t1 = now_ns();

        if (!g_have_frame) continue;
        unsigned w = g_frame_w, h = g_frame_h;
        size_t   pitch = g_frame_pitch;
        int      fmt   = g_frame_fmt;

        /* Pick upload params from the bench path. */
        const void *upload_ptr = NULL;
        unsigned    bpp        = 0;
        GLint       gl_iformat = 0;
        GLenum      gl_format  = 0;
        GLenum      gl_type    = 0;
        size_t      bytes      = 0;

        switch (path) {
        case UP_NATIVE_RGB565:
            bpp        = 2;
            gl_iformat = GL_RGB; gl_format = GL_RGB; gl_type = GL_UNSIGNED_SHORT_5_6_5;
            bytes      = (size_t)w * h * 2;
            if (upload_cap < bytes) { upload_buf = realloc(upload_buf, bytes); upload_cap = bytes; }
            if (fmt == 2) {
                copy_rows_flipped(g_frame_data, pitch, upload_buf, w, h, 2);
            } else {
                /* Force-path test on a non-RGB565 core: convert first. */
                libra_convert_to_rgb565(g_frame_data, w, h, pitch, fmt, upload_buf);
                flip_in_place(upload_buf, w, h, 2);
            }
            upload_ptr = upload_buf;
            break;
        case UP_NATIVE_BGRA8:
            bpp        = 4;
            gl_iformat = GL_BGRA_EXT; gl_format = GL_BGRA_EXT; gl_type = GL_UNSIGNED_BYTE;
            bytes      = (size_t)w * h * 4;
            if (upload_cap < bytes) { upload_buf = realloc(upload_buf, bytes); upload_cap = bytes; }
            if (fmt == 1) {
                copy_rows_flipped(g_frame_data, pitch, upload_buf, w, h, 4);
            } else {
                /* Force-path test on a non-XRGB8888 core: convert then upload as BGRA-shaped bytes. */
                libra_convert_to_rgba(g_frame_data, w, h, pitch, fmt, upload_buf);
                flip_in_place(upload_buf, w, h, 4);
            }
            upload_ptr = upload_buf;
            break;
        case UP_CONVERT_RGB565:
            bpp        = 2;
            gl_iformat = GL_RGB; gl_format = GL_RGB; gl_type = GL_UNSIGNED_SHORT_5_6_5;
            bytes      = (size_t)w * h * 2;
            if (upload_cap < bytes) { upload_buf = realloc(upload_buf, bytes); upload_cap = bytes; }
            libra_convert_to_rgb565(g_frame_data, w, h, pitch, fmt, upload_buf);
            flip_in_place(upload_buf, w, h, 2);
            upload_ptr = upload_buf;
            break;
        case UP_CONVERT_RGBA8:
            bpp        = 4;
            gl_iformat = GL_RGBA; gl_format = GL_RGBA; gl_type = GL_UNSIGNED_BYTE;
            bytes      = (size_t)w * h * 4;
            if (upload_cap < bytes) { upload_buf = realloc(upload_buf, bytes); upload_cap = bytes; }
            libra_convert_to_rgba(g_frame_data, w, h, pitch, fmt, upload_buf);
            flip_in_place(upload_buf, w, h, 4);
            upload_ptr = upload_buf;
            break;
        }

        glPixelStorei(GL_UNPACK_ALIGNMENT, (bpp == 2) ? 2 : 4);
        double t2 = now_ns();

        /* Upload — retex on first frame and on format change. */
        if (first_tex || tex_iformat_seen != gl_iformat) {
            glTexImage2D(GL_TEXTURE_2D, 0, gl_iformat, w, h, 0,
                         gl_format, gl_type, upload_ptr);
            tex_iformat_seen = gl_iformat;
            first_tex = false;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            gl_format, gl_type, upload_ptr);
        }
        glFinish();
        double t3 = now_ns();

        stats_push(&s_run,    t1 - t0);
        stats_push(&s_prep,   t2 - t1);
        stats_push(&s_upload, t3 - t2);
        stats_push(&s_total,  t3 - t0);
    }

    printf("\n[%s] core_fmt=%s frame=%ux%u  N=%u\n",
           path_name(path),
           g_frame_fmt == 0 ? "XRGB1555" : g_frame_fmt == 1 ? "XRGB8888" : "RGB565",
           g_frame_w, g_frame_h, s_total.n);
    stats_report("libra_run", &s_run);
    stats_report("upload-prep", &s_prep);
    stats_report("glTexImage2D+Finish", &s_upload);
    stats_report("TOTAL",     &s_total);

    free(s_run.samples); free(s_prep.samples);
    free(s_upload.samples); free(s_total.samples);
    free(upload_buf);

    glDeleteTextures(1, &tex);
    libra_destroy(ctx);
    return 0;
}

/* ---------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "Usage: %s <core.so> <rom> [frames=240] [path=auto|rgb565-native|bgra-native|convert-rgba8|convert-rgb565|all]\n",
            argv[0]);
        return 1;
    }
    const char *core = argv[1];
    const char *rom  = argv[2];
    unsigned    frames = (argc > 3) ? (unsigned)strtoul(argv[3], NULL, 0) : 240;
    const char *path_s = (argc > 4) ? argv[4] : "all";

    if (!gl_init()) return 1;

    int rc = 0;
    if (strcmp(path_s, "auto") == 0) {
        rc |= run_bench(core, rom, frames, UP_NATIVE_RGB565 /*ignored*/, true);
    } else if (strcmp(path_s, "all") == 0) {
        rc |= run_bench(core, rom, frames, UP_CONVERT_RGBA8,  false);
        rc |= run_bench(core, rom, frames, UP_CONVERT_RGB565, false);
        rc |= run_bench(core, rom, frames, UP_NATIVE_RGB565,  false);
        rc |= run_bench(core, rom, frames, UP_NATIVE_BGRA8,   false);
    } else if (strcmp(path_s, "rgb565-native") == 0) {
        rc |= run_bench(core, rom, frames, UP_NATIVE_RGB565, false);
    } else if (strcmp(path_s, "bgra-native") == 0) {
        rc |= run_bench(core, rom, frames, UP_NATIVE_BGRA8, false);
    } else if (strcmp(path_s, "convert-rgba8") == 0) {
        rc |= run_bench(core, rom, frames, UP_CONVERT_RGBA8, false);
    } else if (strcmp(path_s, "convert-rgb565") == 0) {
        rc |= run_bench(core, rom, frames, UP_CONVERT_RGB565, false);
    } else {
        fprintf(stderr, "unknown path: %s\n", path_s);
        rc = 1;
    }

    gl_shutdown();
    free(g_frame_copy);
    return rc;
}
