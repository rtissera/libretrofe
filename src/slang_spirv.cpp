/* SPDX-License-Identifier: MIT
 *
 * SPIR-V pipeline for .slang shader compilation:
 *   .slang source → preprocess → glslang → SPIR-V → SPIRV-Cross → target GLSL
 */

#include "libra_shader.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <spirv_cross_c.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "[slang_spirv] "

/* ── glslang process init (ref-counted) ──────────────────────────── */

static int g_glslang_refs = 0;

static void ensure_glslang_init(void)
{
    if (g_glslang_refs++ == 0)
        glslang_initialize_process();
}

static void release_glslang(void)
{
    if (--g_glslang_refs == 0)
        glslang_finalize_process();
}

/* ── Preprocess .slang source ────────────────────────────────────── */

static bool preprocess_slang(const char *source, size_t len,
                             std::string &vs_out, std::string &fs_out)
{
    const char *end = source + len;

    /* Collect line spans */
    struct Span { const char *start; size_t len; };
    std::vector<Span> common_lines, vs_lines, fs_lines;
    std::vector<Span> *current = &common_lines;
    bool found_vs = false, found_fs = false;

    const char *p = source;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        size_t line_len = (size_t)(nl - p);

        /* Skip leading whitespace for pragma detection */
        const char *tp = p;
        while (tp < nl && (*tp == ' ' || *tp == '\t')) tp++;

        bool skip = false;

        /* #pragma stage vertex/fragment → switch target */
        if (line_len >= 13 && strncmp(tp, "#pragma stage", 13) == 0) {
            const char *stage = tp + 13;
            while (*stage == ' ') stage++;
            if (strncmp(stage, "vertex", 6) == 0) {
                current = &vs_lines;
                found_vs = true;
            } else if (strncmp(stage, "fragment", 8) == 0) {
                current = &fs_lines;
                found_fs = true;
            }
            skip = true;
        }

        /* Strip pragmas and #version lines */
        if (!skip && strncmp(tp, "#version", 8) == 0)           skip = true;
        if (!skip && strncmp(tp, "#pragma parameter", 17) == 0) skip = true;
        if (!skip && strncmp(tp, "#pragma name", 12) == 0)      skip = true;
        if (!skip && strncmp(tp, "#pragma format", 14) == 0)    skip = true;

        if (!skip) {
            size_t copy_len = (nl < end) ? line_len + 1 : line_len;
            current->push_back({p, copy_len});
        }

        p = (nl < end) ? nl + 1 : end;
    }

    if (!found_vs || !found_fs)
        return false;

    /* Build Vulkan GLSL for each stage: #version 450 + common + stage */
    auto build = [&](const std::vector<Span> &stage_lines) -> std::string {
        std::string out;
        out.reserve(len + 32);
        out += "#version 450\n";
        for (auto &s : common_lines)
            out.append(s.start, s.len);
        for (auto &s : stage_lines)
            out.append(s.start, s.len);
        if (!out.empty() && out.back() != '\n')
            out += '\n';
        return out;
    };

    vs_out = build(vs_lines);
    fs_out = build(fs_lines);
    return true;
}

/* ── Compile GLSL → SPIR-V via glslang ──────────────────────────── */

static bool compile_to_spirv(const char *glsl, glslang_stage_t stage,
                             std::vector<uint32_t> &spirv)
{
    const glslang_resource_t *res = glslang_default_resource();

    glslang_input_t input = {};
    input.language                = GLSLANG_SOURCE_GLSL;
    input.stage                   = stage;
    input.client                  = GLSLANG_CLIENT_VULKAN;
    input.client_version          = GLSLANG_TARGET_VULKAN_1_0;
    input.target_language         = GLSLANG_TARGET_SPV;
    input.target_language_version = GLSLANG_TARGET_SPV_1_0;
    input.code                    = glsl;
    input.default_version         = 450;
    input.default_profile         = GLSLANG_NO_PROFILE;
    input.force_default_version_and_profile = 0;
    input.forward_compatible      = 0;
    input.messages                = GLSLANG_MSG_DEFAULT_BIT;
    input.resource                = res;

    glslang_shader_t *shader = glslang_shader_create(&input);
    if (!shader) return false;

    if (!glslang_shader_preprocess(shader, &input)) {
        fprintf(stderr, TAG "preprocess failed: %s\n",
                glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return false;
    }

    if (!glslang_shader_parse(shader, &input)) {
        fprintf(stderr, TAG "parse failed: %s\n",
                glslang_shader_get_info_log(shader));
        glslang_shader_delete(shader);
        return false;
    }

    glslang_program_t *program = glslang_program_create();
    glslang_program_add_shader(program, shader);

    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                       GLSLANG_MSG_VULKAN_RULES_BIT)) {
        fprintf(stderr, TAG "link failed: %s\n",
                glslang_program_get_info_log(program));
        glslang_program_delete(program);
        glslang_shader_delete(shader);
        return false;
    }

    glslang_program_SPIRV_generate(program, stage);

    size_t size = glslang_program_SPIRV_get_size(program);
    spirv.resize(size);
    glslang_program_SPIRV_get(program, spirv.data());

    const char *spirv_msg = glslang_program_SPIRV_get_messages(program);
    if (spirv_msg && spirv_msg[0])
        fprintf(stderr, TAG "spirv-gen: %s\n", spirv_msg);

    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return true;
}

/* ── Cross-compile SPIR-V → target GLSL via SPIRV-Cross ─────────── */

static bool spirv_to_glsl(const std::vector<uint32_t> &spirv,
                          int gl_version, bool is_es,
                          std::string &glsl_out)
{
    spvc_context ctx = NULL;
    if (spvc_context_create(&ctx) != SPVC_SUCCESS)
        return false;

    spvc_parsed_ir ir = NULL;
    if (spvc_context_parse_spirv(ctx, spirv.data(), spirv.size(), &ir) != SPVC_SUCCESS) {
        fprintf(stderr, TAG "SPIRV-Cross parse failed: %s\n",
                spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return false;
    }

    spvc_compiler compiler = NULL;
    if (spvc_context_create_compiler(ctx, SPVC_BACKEND_GLSL, ir,
                                     SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                     &compiler) != SPVC_SUCCESS) {
        fprintf(stderr, TAG "SPIRV-Cross compiler create failed: %s\n",
                spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return false;
    }

    spvc_compiler_options options = NULL;
    spvc_compiler_create_compiler_options(compiler, &options);
    spvc_compiler_options_set_uint(options,
        SPVC_COMPILER_OPTION_GLSL_VERSION, (unsigned)gl_version);
    spvc_compiler_options_set_uint(options,
        SPVC_COMPILER_OPTION_GLSL_ES, is_es ? 1 : 0);
    spvc_compiler_options_set_uint(options,
        SPVC_COMPILER_OPTION_GLSL_EMIT_UNIFORM_BUFFER_AS_PLAIN_UNIFORMS, 1);
    spvc_compiler_install_compiler_options(compiler, options);

    const char *result = NULL;
    if (spvc_compiler_compile(compiler, &result) != SPVC_SUCCESS) {
        fprintf(stderr, TAG "SPIRV-Cross compile failed: %s\n",
                spvc_context_get_last_error_string(ctx));
        spvc_context_destroy(ctx);
        return false;
    }

    glsl_out = result;
    spvc_context_destroy(ctx);
    return true;
}

/* ── Public API ──────────────────────────────────────────────────── */

extern "C" bool libra_slang_compile(const char *source, size_t len,
                                    int gl_version, bool is_es,
                                    char *vs_out, size_t vs_size,
                                    char *fs_out, size_t fs_size)
{
    ensure_glslang_init();

    /* 1. Preprocess: split on #pragma stage, build per-stage Vulkan GLSL */
    std::string vs_vulkan, fs_vulkan;
    if (!preprocess_slang(source, len, vs_vulkan, fs_vulkan)) {
        fprintf(stderr, TAG "preprocess failed (no #pragma stage found)\n");
        release_glslang();
        return false;
    }

    /* 2. Compile each stage to SPIR-V */
    std::vector<uint32_t> vs_spirv, fs_spirv;
    if (!compile_to_spirv(vs_vulkan.c_str(), GLSLANG_STAGE_VERTEX, vs_spirv)) {
        fprintf(stderr, TAG "vertex stage SPIR-V compilation failed\n");
        release_glslang();
        return false;
    }
    if (!compile_to_spirv(fs_vulkan.c_str(), GLSLANG_STAGE_FRAGMENT, fs_spirv)) {
        fprintf(stderr, TAG "fragment stage SPIR-V compilation failed\n");
        release_glslang();
        return false;
    }

    /* 3. Cross-compile SPIR-V → target GLSL */
    std::string vs_glsl, fs_glsl;
    if (!spirv_to_glsl(vs_spirv, gl_version, is_es, vs_glsl)) {
        fprintf(stderr, TAG "vertex SPIRV-Cross failed\n");
        release_glslang();
        return false;
    }
    if (!spirv_to_glsl(fs_spirv, gl_version, is_es, fs_glsl)) {
        fprintf(stderr, TAG "fragment SPIRV-Cross failed\n");
        release_glslang();
        return false;
    }

    /* 4. Copy to output buffers */
    if (vs_glsl.size() + 1 > vs_size || fs_glsl.size() + 1 > fs_size) {
        fprintf(stderr, TAG "output buffer too small (vs=%zu fs=%zu)\n",
                vs_glsl.size(), fs_glsl.size());
        release_glslang();
        return false;
    }

    memcpy(vs_out, vs_glsl.c_str(), vs_glsl.size() + 1);
    memcpy(fs_out, fs_glsl.c_str(), fs_glsl.size() + 1);

    release_glslang();
    return true;
}
