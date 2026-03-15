/* SPDX-License-Identifier: MIT */
#ifndef LIBRA_SHADER_H
#define LIBRA_SHADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIBRA_SHADER_MAX_PASSES  16
#define LIBRA_SHADER_MAX_LUTS     8
#define LIBRA_SHADER_MAX_PARAMS  256

enum libra_scale_type {
    LIBRA_SCALE_SOURCE   = 0,
    LIBRA_SCALE_VIEWPORT = 1,
    LIBRA_SCALE_ABSOLUTE = 2
};

enum libra_wrap_mode {
    LIBRA_WRAP_CLAMP    = 0,
    LIBRA_WRAP_REPEAT   = 1,
    LIBRA_WRAP_MIRRORED = 2
};

typedef struct {
    char  path[512];           /* resolved absolute path to .glsl/.slang */
    int   filter_linear;       /* 1=linear, 0=nearest, -1=unset (default linear) */
    int   scale_type_x;
    int   scale_type_y;
    float scale_x, scale_y;   /* default 1.0 */
    int   wrap_mode;
    int   mipmap_input;
    int   float_framebuffer;
    int   srgb_framebuffer;
    int   frame_count_mod;     /* 0 = no wrapping */
    char  alias[64];
} libra_shader_pass_t;

typedef struct {
    char name[64];             /* sampler name in shader source */
    char path[512];
    int  filter_linear;
    int  mipmap;
    int  wrap_mode;
} libra_shader_lut_t;

typedef struct {
    char  id[64];
    char  label[128];
    float value, def, minimum, maximum, step;
} libra_shader_param_t;

typedef struct {
    int      is_slang;         /* 0=glsl, 1=slang (detected from shader0 extension) */
    unsigned pass_count;
    libra_shader_pass_t  passes[LIBRA_SHADER_MAX_PASSES];
    unsigned lut_count;
    libra_shader_lut_t   luts[LIBRA_SHADER_MAX_LUTS];
    unsigned param_count;
    libra_shader_param_t params[LIBRA_SHADER_MAX_PARAMS];
    char     base_dir[512];
} libra_shader_preset_t;

/* Parse a .glslp or .slangp preset file.  Resolves relative paths. */
bool libra_shader_preset_load(libra_shader_preset_t *out, const char *path);

/* Extract #pragma parameter lines from shader source into params[].
 * Returns number of parameters found. */
unsigned libra_shader_extract_params(const char *source,
    libra_shader_param_t *params, unsigned max);

/* Split a .glsl source into VS and FS by prepending #define VERTEX/FRAGMENT.
 * version_line is used as fallback if no #version found in source.
 * gles_version: 0 = desktop GL, 100 = GLES 2.0 (native syntax),
 *               300 = GLES 3.0+ (keyword remapping). */
bool libra_glsl_split(const char *source, size_t len,
    const char *version_line,
    int gles_version,
    char *vs_out, size_t vs_size, char *fs_out, size_t fs_size);

/* Compile .slang via SPIR-V pipeline -> target GLSL.
 * gl_version: 130 (desktop GL), 300 (GLES 3.0), etc.
 * base_dir: directory for resolving #include "file" (NULL to skip).
 * fmt_out/fmt_size: if non-NULL, receives the #pragma format string (e.g.
 *   "R16G16B16A16_SFLOAT"). Empty string if no #pragma format found.
 * Output includes #version directive — no wrapping needed. */
bool libra_slang_compile(const char *source, size_t len,
    const char *base_dir,
    int gl_version, bool is_es,
    char *vs_out, size_t vs_size, char *fs_out, size_t fs_size,
    char *fmt_out, size_t fmt_size);

/* SPIR-V output from .slang compilation — for Vulkan shader chain.
 * vs_out / fs_out are heap-allocated uint32 word arrays.
 * Free each with libra_spirv_free().  fmt_out receives the #pragma format
 * string (e.g. "R16G16B16A16_SFLOAT"), empty if absent. */
bool libra_slang_compile_spirv(const char *source, size_t len,
    const char *base_dir,
    uint32_t **vs_out, size_t *vs_words,
    uint32_t **fs_out, size_t *fs_words,
    char *fmt_out, size_t fmt_size);

void libra_spirv_free(uint32_t *p);

/* Per-member info returned by UBO reflection. */
typedef struct {
    char     name[64];
    uint32_t offset;  /* byte offset within the UBO */
    uint32_t size;    /* byte size of this member */
} libra_ubo_member_t;

/* Per-sampler binding info returned by sampler reflection. */
typedef struct {
    char     name[64];
    uint32_t binding; /* descriptor binding number */
} libra_sampler_binding_t;

/* Reflect UBO members from SPIR-V (pass the fragment shader words).
 * ubo_size_out receives total declared struct size in bytes.
 * On input *member_count is max capacity; on output actual count written. */
bool libra_spirv_reflect_ubo(const uint32_t *spirv, size_t words,
    uint32_t *ubo_size_out,
    libra_ubo_member_t *members, unsigned *member_count);

/* Reflect combined-image-sampler bindings from SPIR-V.
 * On input *count is max capacity; on output actual count written. */
bool libra_spirv_reflect_samplers(const uint32_t *spirv, size_t words,
    libra_sampler_binding_t *samplers, unsigned *count);

#ifdef __cplusplus
}
#endif

#endif /* LIBRA_SHADER_H */
