// SPDX-License-Identifier: MIT
/*
 * Unit tests for the shader preset parser additions:
 *   - LIBRA_SCALE_ORIGINAL (4th scale type)
 *   - libra_shader_preset_load_ctx (wildcard $KEY$ substitution)
 *   - #pragma parameter step default of 0.02 (vs old 1.0)
 *   - LIBRA_SHADER_MAX_PASSES = 32 (raised from 16)
 *
 * Standalone — links shader.c directly, no other libra deps needed.
 */
#include "libra_shader.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;

#define ASSERT(c) do {                                                   \
    if (!(c)) {                                                          \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
        g_fail = 1;                                                      \
        return;                                                          \
    }                                                                    \
} while (0)

/* Tempdir setup: per-test scratch dir under /tmp.  Caller deletes. */
static void make_tempdir(char *out, size_t n, const char *name)
{
    snprintf(out, n, "/tmp/libra-shader-test-%s", name);
    mkdir(out, 0755);
}

static void write_file(const char *dir, const char *name, const char *content)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { perror(p); return; }
    fputs(content, f);
    fclose(f);
}

/* ---------- LIBRA_SCALE_ORIGINAL ---------- */

static void test_scale_original_parses(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "scale-orig");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"s.slang\"\n"
               "scale_type0 = original\n"
               "scale0 = 2.0\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load(&p, path));
    ASSERT(p.pass_count == 1);
    ASSERT(p.passes[0].scale_type_x == LIBRA_SCALE_ORIGINAL);
    ASSERT(p.passes[0].scale_type_y == LIBRA_SCALE_ORIGINAL);
    ASSERT(p.passes[0].scale_x == 2.0f);
}

static void test_scale_original_axis_specific(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "scale-orig-axis");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"s.slang\"\n"
               "scale_type_x0 = original\n"
               "scale_type_y0 = source\n"
               "scale_x0 = 1.5\n"
               "scale_y0 = 1.0\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load(&p, path));
    ASSERT(p.passes[0].scale_type_x == LIBRA_SCALE_ORIGINAL);
    ASSERT(p.passes[0].scale_type_y == LIBRA_SCALE_SOURCE);
}

static void test_scale_unknown_falls_back_to_source(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "scale-unk");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"s.slang\"\n"
               "scale_type0 = bogus_name\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load(&p, path));
    ASSERT(p.passes[0].scale_type_x == LIBRA_SCALE_SOURCE);
}

/* ---------- Wildcard substitution ---------- */

static void test_wildcard_basic(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wc-basic");

    write_file(dir, "myshader.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"$PRESET_DIR$/myshader.slang\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_context_item_t ctx[] = { { "PRESET_DIR", dir } };
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load_ctx(&p, path, ctx, 1));
    ASSERT(p.pass_count == 1);
    /* Wildcard must be expanded — no '$' should remain in resolved path. */
    ASSERT(strchr(p.passes[0].path, '$') == NULL);
    ASSERT(strstr(p.passes[0].path, "myshader.slang") != NULL);
}

static void test_wildcard_unknown_key_preserved(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wc-unk");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"$UNKNOWN_KEY$/s.slang\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_context_item_t ctx[] = { { "PRESET_DIR", dir } };
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load_ctx(&p, path, ctx, 1));
    /* Unknown key remains literal in the resolved path. */
    ASSERT(strstr(p.passes[0].path, "$UNKNOWN_KEY$") != NULL);
}

static void test_wildcard_no_context_no_expansion(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wc-none");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"$PRESET_DIR$/s.slang\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    /* Old API: no context, no expansion — literal wildcard preserved. */
    ASSERT(libra_shader_preset_load(&p, path));
    ASSERT(strstr(p.passes[0].path, "$PRESET_DIR$") != NULL);
}

static void test_wildcard_absolute_value_no_double_prefix(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wc-abs");

    char subdir[256];
    snprintf(subdir, sizeof(subdir), "%s/shaders", dir);
    mkdir(subdir, 0755);
    write_file(subdir, "s.slang", "// stub\n");

    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"$SHADERS_ROOT$/s.slang\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_context_item_t ctx[] = { { "SHADERS_ROOT", subdir } };
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load_ctx(&p, path, ctx, 1));
    /* Expansion to absolute path must NOT be prefixed by preset's base_dir. */
    ASSERT(strstr(p.passes[0].path, "//") == NULL);
    ASSERT(strstr(p.passes[0].path, "shaders/s.slang") != NULL);
}

static void test_wildcard_in_lut_path(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wc-lut");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "palette.png", "fake-png");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"s.slang\"\n"
               "textures = \"PaletteLUT\"\n"
               "PaletteLUT = \"$PRESET_DIR$/palette.png\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_context_item_t ctx[] = { { "PRESET_DIR", dir } };
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load_ctx(&p, path, ctx, 1));
    ASSERT(p.lut_count == 1);
    ASSERT(strchr(p.luts[0].path, '$') == NULL);
    ASSERT(strstr(p.luts[0].path, "palette.png") != NULL);
}

/* ---------- #pragma parameter step default ---------- */

static void test_param_step_default_is_002(void)
{
    /* When step is omitted from #pragma parameter, default must be 0.02
     * (matches slang-shaders spec / RetroArch / librashader), not 1.0. */
    const char *src =
        "#pragma parameter MyParam \"My Param\" 0.5 0.0 1.0\n"
        "void main() {}\n";

    libra_shader_param_t params[8];
    memset(params, 0, sizeof(params));
    unsigned n = libra_shader_extract_params(src, params, 8);
    ASSERT(n == 1);
    ASSERT(strcmp(params[0].id, "MyParam") == 0);
    ASSERT(params[0].def == 0.5f);
    ASSERT(params[0].minimum == 0.0f);
    ASSERT(params[0].maximum == 1.0f);
    ASSERT(params[0].step == 0.02f);
}

static void test_param_explicit_step_kept(void)
{
    const char *src =
        "#pragma parameter X \"X\" 1.0 0.0 10.0 0.5\n";
    libra_shader_param_t params[8];
    memset(params, 0, sizeof(params));
    unsigned n = libra_shader_extract_params(src, params, 8);
    ASSERT(n == 1);
    ASSERT(params[0].step == 0.5f);
}

/* ---------- WrapMode parsing ---------- */

static void test_wrap_mode_clamp_to_border(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wrap-border");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"s.slang\"\n"
               "wrap_mode0 = \"clamp_to_border\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load(&p, path));
    ASSERT(p.passes[0].wrap_mode == LIBRA_WRAP_BORDER);
}

static void test_wrap_mode_clamp_to_edge_still_works(void)
{
    char dir[256];
    make_tempdir(dir, sizeof(dir), "wrap-edge");

    write_file(dir, "s.slang", "// stub\n");
    write_file(dir, "p.slangp",
               "shaders = 1\n"
               "shader0 = \"s.slang\"\n"
               "wrap_mode0 = \"clamp_to_edge\"\n");

    char path[512]; snprintf(path, sizeof(path), "%s/p.slangp", dir);
    libra_shader_preset_t p; memset(&p, 0, sizeof(p));
    ASSERT(libra_shader_preset_load(&p, path));
    ASSERT(p.passes[0].wrap_mode == LIBRA_WRAP_CLAMP);
}

/* ---------- Push-constant reflection ---------- */

static void test_pc_reflection_round_trip(void)
{
    /* Tiny slang with a push_constant block holding MVP + OutputSize + a param. */
    const char *src =
        "#version 450\n"
        "#pragma stage vertex\n"
        "layout(push_constant) uniform Push {\n"
        "    mat4 MVP;\n"
        "    vec4 OutputSize;\n"
        "    float TestParam;\n"
        "} regs;\n"
        "layout(location = 0) in vec4 Position;\n"
        "layout(location = 0) out vec2 vTexCoord;\n"
        "void main() { gl_Position = regs.MVP * Position; vTexCoord = Position.xy; }\n"
        "#pragma stage fragment\n"
        "layout(push_constant) uniform Push {\n"
        "    mat4 MVP;\n"
        "    vec4 OutputSize;\n"
        "    float TestParam;\n"
        "} regs;\n"
        "layout(location = 0) in vec2 vTexCoord;\n"
        "layout(location = 0) out vec4 FragColor;\n"
        "void main() { FragColor = vec4(regs.TestParam); }\n";

    uint32_t *vs = NULL, *fs = NULL;
    size_t vs_w = 0, fs_w = 0;
    char fmt[64] = {};
    ASSERT(libra_slang_compile_spirv(src, strlen(src), NULL,
                                      &vs, &vs_w, &fs, &fs_w,
                                      fmt, sizeof(fmt)));
    ASSERT(fs && fs_w > 0);

    libra_ubo_member_t members[16];
    unsigned member_count = 16;
    uint32_t pc_size = 0;
    ASSERT(libra_spirv_reflect_push_constants(fs, fs_w, &pc_size,
                                              members, &member_count));
    ASSERT(pc_size >= 64 + 16 + 4);  /* mat4 + vec4 + float, plus alignment */
    ASSERT(member_count == 3);

    bool sawMVP = false, sawOut = false, sawParam = false;
    for (unsigned i = 0; i < member_count; i++) {
        if (strcmp(members[i].name, "MVP") == 0) {
            ASSERT(members[i].size == 64);
            sawMVP = true;
        } else if (strcmp(members[i].name, "OutputSize") == 0) {
            ASSERT(members[i].size == 16);
            sawOut = true;
        } else if (strcmp(members[i].name, "TestParam") == 0) {
            ASSERT(members[i].size == 4);
            sawParam = true;
        }
    }
    ASSERT(sawMVP); ASSERT(sawOut); ASSERT(sawParam);

    libra_spirv_free(vs);
    libra_spirv_free(fs);
}

static void test_pc_reflection_absent(void)
{
    /* Shader with only UBO, no push_constant block: API returns true with size=0. */
    const char *src =
        "#version 450\n"
        "#pragma stage vertex\n"
        "layout(set = 0, binding = 0, std140) uniform UBO { mat4 MVP; } global;\n"
        "layout(location = 0) in vec4 Position;\n"
        "void main() { gl_Position = global.MVP * Position; }\n"
        "#pragma stage fragment\n"
        "layout(set = 0, binding = 0, std140) uniform UBO { mat4 MVP; } global;\n"
        "layout(location = 0) out vec4 FragColor;\n"
        "void main() { FragColor = global.MVP[0]; }\n";

    uint32_t *vs = NULL, *fs = NULL;
    size_t vs_w = 0, fs_w = 0;
    char fmt[64] = {};
    ASSERT(libra_slang_compile_spirv(src, strlen(src), NULL,
                                      &vs, &vs_w, &fs, &fs_w,
                                      fmt, sizeof(fmt)));

    libra_ubo_member_t members[16];
    unsigned member_count = 16;
    uint32_t pc_size = 0;
    ASSERT(libra_spirv_reflect_push_constants(fs, fs_w, &pc_size,
                                              members, &member_count));
    ASSERT(pc_size == 0);
    ASSERT(member_count == 0);

    libra_spirv_free(vs);
    libra_spirv_free(fs);
}

/* ---------- MAX_PASSES headroom ---------- */

static void test_max_passes_raised(void)
{
    /* Compile-time check — 32 lets full Mega Bezel fit. */
    ASSERT(LIBRA_SHADER_MAX_PASSES >= 32);
    ASSERT(LIBRA_SHADER_MAX_LUTS   >= 16);
}

int main(void)
{
    test_scale_original_parses();
    test_scale_original_axis_specific();
    test_scale_unknown_falls_back_to_source();
    test_wildcard_basic();
    test_wildcard_unknown_key_preserved();
    test_wildcard_no_context_no_expansion();
    test_wildcard_absolute_value_no_double_prefix();
    test_wildcard_in_lut_path();
    test_param_step_default_is_002();
    test_param_explicit_step_kept();
    test_wrap_mode_clamp_to_border();
    test_wrap_mode_clamp_to_edge_still_works();
    test_pc_reflection_round_trip();
    test_pc_reflection_absent();
    test_max_passes_raised();

    if (g_fail) {
        fprintf(stderr, "shader_unit: FAILED\n");
        return 1;
    }
    fprintf(stderr, "shader_unit: 15 tests OK\n");
    return 0;
}
