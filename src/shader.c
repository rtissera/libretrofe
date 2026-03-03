/* SPDX-License-Identifier: MIT */
#include "libra_shader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ========================================================================= */
/* Helpers                                                                    */
/* ========================================================================= */

static void trim_line(char *line)
{
    int len = (int)strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                        line[len-1] == ' '  || line[len-1] == '\t'))
        line[--len] = '\0';
}

/* Parse "key = value" from a line. Returns false if no '=' found.
 * key and val point into the (modified) line buffer. */
static bool parse_kv(char *line, char **key_out, char **val_out)
{
    char *eq = strchr(line, '=');
    if (!eq) return false;
    *eq = '\0';

    char *key = line;
    while (*key == ' ' || *key == '\t') key++;
    char *kend = eq - 1;
    while (kend > key && (*kend == ' ' || *kend == '\t')) *kend-- = '\0';

    char *val = eq + 1;
    while (*val == ' ' || *val == '\t') val++;
    int vlen = (int)strlen(val);
    while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t'))
        val[--vlen] = '\0';
    if (vlen >= 2 && val[0] == '"' && val[vlen-1] == '"') {
        val[vlen-1] = '\0';
        val++;
    }

    *key_out = key;
    *val_out = val;
    return (*key != '\0');
}

/* Resolve a relative path against a base directory. */
static void resolve_path(char *out, size_t out_size,
                          const char *base_dir, const char *rel)
{
    if (rel[0] == '/') {
        snprintf(out, out_size, "%s", rel);
    } else {
        snprintf(out, out_size, "%s/%s", base_dir, rel);
    }
}

/* Check if string ends with suffix (case-insensitive) */
static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t sl = strlen(s);
    size_t xl = strlen(suffix);
    if (sl < xl) return false;
    for (size_t i = 0; i < xl; i++) {
        if (tolower((unsigned char)s[sl - xl + i]) !=
            tolower((unsigned char)suffix[i]))
            return false;
    }
    return true;
}

/* Parse wrap_mode string */
static int parse_wrap_mode(const char *s)
{
    if (!s) return LIBRA_WRAP_CLAMP;
    if (strcmp(s, "repeat") == 0)         return LIBRA_WRAP_REPEAT;
    if (strcmp(s, "mirrored_repeat") == 0) return LIBRA_WRAP_MIRRORED;
    if (strcmp(s, "clamp_to_edge") == 0)  return LIBRA_WRAP_CLAMP;
    if (strcmp(s, "clamp_to_border") == 0) return LIBRA_WRAP_CLAMP;
    return LIBRA_WRAP_CLAMP;
}

/* Parse scale_type string */
static int parse_scale_type(const char *s)
{
    if (!s) return LIBRA_SCALE_SOURCE;
    if (strcmp(s, "source") == 0)   return LIBRA_SCALE_SOURCE;
    if (strcmp(s, "viewport") == 0) return LIBRA_SCALE_VIEWPORT;
    if (strcmp(s, "absolute") == 0) return LIBRA_SCALE_ABSOLUTE;
    return LIBRA_SCALE_SOURCE;
}

/* Read entire file into malloc'd buffer (null-terminated). Caller frees. */
static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (out_len) *out_len = rd;
    return buf;
}

/* Safe string append helper for building shader sources */
static size_t sappend(char *dst, size_t pos, size_t cap, const char *src, size_t len)
{
    if (pos + len >= cap) return pos; /* silently truncate */
    memcpy(dst + pos, src, len);
    return pos + len;
}

static size_t sappend_str(char *dst, size_t pos, size_t cap, const char *src)
{
    return sappend(dst, pos, cap, src, strlen(src));
}

/* ========================================================================= */
/* Parameter extraction                                                       */
/* ========================================================================= */

unsigned libra_shader_extract_params(const char *source,
    libra_shader_param_t *params, unsigned max)
{
    if (!source || !params || max == 0)
        return 0;

    unsigned count = 0;
    const char *p = source;

    while ((p = strstr(p, "#pragma parameter")) != NULL) {
        /* Must be at start of line (or start of file) */
        if (p != source && p[-1] != '\n') { p++; continue; }

        const char *line_start = p;
        p += 17; /* skip "#pragma parameter" */

        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* Parse: id "label" default minimum maximum step */
        libra_shader_param_t param;
        memset(&param, 0, sizeof(param));

        /* id */
        const char *id_start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        size_t id_len = (size_t)(p - id_start);
        if (id_len == 0 || id_len >= sizeof(param.id)) { continue; }
        memcpy(param.id, id_start, id_len);
        param.id[id_len] = '\0';

        while (*p == ' ' || *p == '\t') p++;

        /* label in quotes */
        if (*p == '"') {
            p++;
            const char *label_start = p;
            while (*p && *p != '"' && *p != '\n') p++;
            size_t label_len = (size_t)(p - label_start);
            if (label_len >= sizeof(param.label))
                label_len = sizeof(param.label) - 1;
            memcpy(param.label, label_start, label_len);
            param.label[label_len] = '\0';
            if (*p == '"') p++;
        } else {
            /* No quotes — use id as label */
            snprintf(param.label, sizeof(param.label), "%s", param.id);
        }

        /* default, minimum, maximum, step */
        char *end;
        while (*p == ' ' || *p == '\t') p++;
        param.def = (float)strtod(p, &end); p = end;
        while (*p == ' ' || *p == '\t') p++;
        param.minimum = (float)strtod(p, &end); p = end;
        while (*p == ' ' || *p == '\t') p++;
        param.maximum = (float)strtod(p, &end); p = end;
        while (*p == ' ' || *p == '\t') p++;
        param.step = (float)strtod(p, &end); p = end;

        param.value = param.def;

        if (param.step <= 0.0f) param.step = 1.0f;

        /* Check for duplicates */
        bool dup = false;
        for (unsigned i = 0; i < count; i++) {
            if (strcmp(params[i].id, param.id) == 0) { dup = true; break; }
        }
        if (!dup && count < max) {
            params[count++] = param;
        }

        /* Advance past this line */
        (void)line_start;
    }

    return count;
}

/* ========================================================================= */
/* Preset parser                                                              */
/* ========================================================================= */

#define LIBRA_PRESET_MAX_DEPTH 8

static bool preset_load_internal(libra_shader_preset_t *out, const char *path, int depth);

bool libra_shader_preset_load(libra_shader_preset_t *out, const char *path)
{
    return preset_load_internal(out, path, 0);
}

static bool preset_load_internal(libra_shader_preset_t *out, const char *path, int depth)
{
    if (!out || !path) return false;
    if (depth > LIBRA_PRESET_MAX_DEPTH) {
        fprintf(stderr, "[shader] #reference depth exceeds %d\n", LIBRA_PRESET_MAX_DEPTH);
        return false;
    }

    /* Determine base directory */
    char base_dir[512];
    {
        const char *last_slash = strrchr(path, '/');
        if (last_slash) {
            size_t len = (size_t)(last_slash - path);
            if (len >= sizeof(base_dir))
                len = sizeof(base_dir) - 1;
            memcpy(base_dir, path, len);
            base_dir[len] = '\0';
        } else {
            snprintf(base_dir, sizeof(base_dir), ".");
        }
    }

    /* First pass: scan for #reference directive.
     * If found, recursively load the referenced preset as our base. */
    bool have_base = false;
    {
        FILE *rf = fopen(path, "r");
        if (!rf) return false;
        char rline[2048];
        while (fgets(rline, sizeof(rline), rf)) {
            trim_line(rline);
            if (strncmp(rline, "#reference", 10) != 0) continue;
            const char *rp = rline + 10;
            while (*rp == ' ' || *rp == '\t') rp++;
            /* Strip quotes */
            size_t rlen = strlen(rp);
            if (rlen >= 2 && rp[0] == '"' && rp[rlen-1] == '"') {
                rp++;
                rlen -= 2;
            }
            if (rlen == 0) continue;
            char ref_rel[512];
            snprintf(ref_rel, sizeof(ref_rel), "%.*s", (int)rlen, rp);
            char ref_path[1024];
            resolve_path(ref_path, sizeof(ref_path), base_dir, ref_rel);
            if (!preset_load_internal(out, ref_path, depth + 1)) {
                fclose(rf);
                return false;
            }
            have_base = true;
            break; /* only one #reference per file */
        }
        fclose(rf);
    }

    if (!have_base) {
        memset(out, 0, sizeof(*out));
        /* Set defaults for all passes */
        for (int i = 0; i < LIBRA_SHADER_MAX_PASSES; i++) {
            out->passes[i].filter_linear = -1; /* unset */
            out->passes[i].scale_x = 1.0f;
            out->passes[i].scale_y = 1.0f;
            out->passes[i].scale_type_x = LIBRA_SCALE_SOURCE;
            out->passes[i].scale_type_y = LIBRA_SCALE_SOURCE;
        }
    }

    /* Set base_dir to THIS file's directory (paths are relative to this file) */
    snprintf(out->base_dir, sizeof(out->base_dir), "%s", base_dir);

    /* Temporary LUT names for matching properties — rebuild from existing LUTs */
    char lut_names[LIBRA_SHADER_MAX_LUTS][64];
    memset(lut_names, 0, sizeof(lut_names));
    for (unsigned i = 0; i < out->lut_count; i++)
        snprintf(lut_names[i], 64, "%s", out->luts[i].name);

    FILE *f = fopen(path, "r");
    if (!f) return !have_base ? false : true;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        /* Skip #reference (already handled), other # comments, empty lines */
        if (line[0] == '\0') continue;
        if (line[0] == '#') continue;

        char *key, *val;
        if (!parse_kv(line, &key, &val))
            continue;

        /* shaders = N */
        if (strcmp(key, "shaders") == 0) {
            out->pass_count = (unsigned)atoi(val);
            if (out->pass_count > LIBRA_SHADER_MAX_PASSES)
                out->pass_count = LIBRA_SHADER_MAX_PASSES;
            continue;
        }

        /* textures = "name1;name2" */
        if (strcmp(key, "textures") == 0) {
            /* Child preset overrides base's texture list */
            if (have_base) {
                out->lut_count = 0;
                memset(lut_names, 0, sizeof(lut_names));
            }
            char tmp[512];
            snprintf(tmp, sizeof(tmp), "%s", val);
            char *tok = strtok(tmp, ";");
            while (tok && out->lut_count < LIBRA_SHADER_MAX_LUTS) {
                while (*tok == ' ') tok++;
                char *tend = tok + strlen(tok) - 1;
                while (tend > tok && *tend == ' ') *tend-- = '\0';
                snprintf(out->luts[out->lut_count].name, 64, "%s", tok);
                snprintf(lut_names[out->lut_count], 64, "%s", tok);
                out->luts[out->lut_count].filter_linear = 1;
                out->lut_count++;
                tok = strtok(NULL, ";");
            }
            continue;
        }

        /* Per-pass keys: shaderN, filter_linearN, etc. */
        {
            int idx = -1;

            /* Try to match "keyN" pattern */
            size_t klen = strlen(key);

            /* shader<N> */
            if (strncmp(key, "shader", 6) == 0 && klen > 6 && isdigit((unsigned char)key[6])) {
                idx = atoi(key + 6);
                if (idx >= 0 && idx < (int)out->pass_count) {
                    resolve_path(out->passes[idx].path, sizeof(out->passes[idx].path),
                                 out->base_dir, val);
                }
                continue;
            }

            /* filter_linear<N> */
            if (strncmp(key, "filter_linear", 13) == 0 && isdigit((unsigned char)key[13])) {
                idx = atoi(key + 13);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].filter_linear = (strcmp(val, "true") == 0 || atoi(val) != 0) ? 1 : 0;
                continue;
            }

            /* scale_type<N> (sets both x and y) */
            if (strncmp(key, "scale_type_x", 12) == 0 && isdigit((unsigned char)key[12])) {
                idx = atoi(key + 12);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].scale_type_x = parse_scale_type(val);
                continue;
            }
            if (strncmp(key, "scale_type_y", 12) == 0 && isdigit((unsigned char)key[12])) {
                idx = atoi(key + 12);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].scale_type_y = parse_scale_type(val);
                continue;
            }
            if (strncmp(key, "scale_type", 10) == 0 && isdigit((unsigned char)key[10])) {
                idx = atoi(key + 10);
                if (idx >= 0 && idx < (int)out->pass_count) {
                    int st = parse_scale_type(val);
                    out->passes[idx].scale_type_x = st;
                    out->passes[idx].scale_type_y = st;
                }
                continue;
            }

            /* scale<N>, scale_x<N>, scale_y<N> */
            if (strncmp(key, "scale_x", 7) == 0 && isdigit((unsigned char)key[7])) {
                idx = atoi(key + 7);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].scale_x = (float)atof(val);
                continue;
            }
            if (strncmp(key, "scale_y", 7) == 0 && isdigit((unsigned char)key[7])) {
                idx = atoi(key + 7);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].scale_y = (float)atof(val);
                continue;
            }
            if (strncmp(key, "scale", 5) == 0 && isdigit((unsigned char)key[5])) {
                idx = atoi(key + 5);
                if (idx >= 0 && idx < (int)out->pass_count) {
                    float s = (float)atof(val);
                    out->passes[idx].scale_x = s;
                    out->passes[idx].scale_y = s;
                }
                continue;
            }

            /* wrap_mode<N> */
            if (strncmp(key, "wrap_mode", 9) == 0 && isdigit((unsigned char)key[9])) {
                idx = atoi(key + 9);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].wrap_mode = parse_wrap_mode(val);
                continue;
            }

            /* mipmap_input<N> */
            if (strncmp(key, "mipmap_input", 12) == 0 && isdigit((unsigned char)key[12])) {
                idx = atoi(key + 12);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].mipmap_input = (strcmp(val, "true") == 0 || atoi(val) != 0) ? 1 : 0;
                continue;
            }

            /* float_framebuffer<N> */
            if (strncmp(key, "float_framebuffer", 17) == 0 && isdigit((unsigned char)key[17])) {
                idx = atoi(key + 17);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].float_framebuffer = (strcmp(val, "true") == 0 || atoi(val) != 0) ? 1 : 0;
                continue;
            }

            /* srgb_framebuffer<N> */
            if (strncmp(key, "srgb_framebuffer", 16) == 0 && isdigit((unsigned char)key[16])) {
                idx = atoi(key + 16);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].srgb_framebuffer = (strcmp(val, "true") == 0 || atoi(val) != 0) ? 1 : 0;
                continue;
            }

            /* frame_count_mod<N> */
            if (strncmp(key, "frame_count_mod", 15) == 0 && isdigit((unsigned char)key[15])) {
                idx = atoi(key + 15);
                if (idx >= 0 && idx < (int)out->pass_count)
                    out->passes[idx].frame_count_mod = atoi(val);
                continue;
            }

            /* alias<N> */
            if (strncmp(key, "alias", 5) == 0 && isdigit((unsigned char)key[5])) {
                idx = atoi(key + 5);
                if (idx >= 0 && idx < (int)out->pass_count)
                    snprintf(out->passes[idx].alias, sizeof(out->passes[idx].alias), "%s", val);
                continue;
            }
        }

        /* LUT texture properties: <name> = path, <name>_linear, <name>_mipmap, <name>_wrap_mode */
        for (unsigned i = 0; i < out->lut_count; i++) {
            if (lut_names[i][0] == '\0') continue;

            if (strcmp(key, lut_names[i]) == 0) {
                resolve_path(out->luts[i].path, sizeof(out->luts[i].path),
                             out->base_dir, val);
                break;
            }

            char prop[128];
            snprintf(prop, sizeof(prop), "%s_linear", lut_names[i]);
            if (strcmp(key, prop) == 0) {
                out->luts[i].filter_linear = (strcmp(val, "true") == 0 || atoi(val) != 0) ? 1 : 0;
                break;
            }

            snprintf(prop, sizeof(prop), "%s_mipmap", lut_names[i]);
            if (strcmp(key, prop) == 0) {
                out->luts[i].mipmap = (strcmp(val, "true") == 0 || atoi(val) != 0) ? 1 : 0;
                break;
            }

            snprintf(prop, sizeof(prop), "%s_wrap_mode", lut_names[i]);
            if (strcmp(key, prop) == 0) {
                out->luts[i].wrap_mode = parse_wrap_mode(val);
                break;
            }
        }
    }
    fclose(f);

    if (out->pass_count == 0)
        return false;

    /* Detect .glsl vs .slang from first shader's extension */
    out->is_slang = ends_with_ci(out->passes[0].path, ".slang") ? 1 : 0;

    /* Read each shader file and extract parameters */
    for (unsigned i = 0; i < out->pass_count; i++) {
        size_t src_len = 0;
        char *src = read_file(out->passes[i].path, &src_len);
        if (!src) continue;

        libra_shader_param_t tmp_params[LIBRA_SHADER_MAX_PARAMS];
        unsigned found = libra_shader_extract_params(src, tmp_params, LIBRA_SHADER_MAX_PARAMS);
        for (unsigned j = 0; j < found && out->param_count < LIBRA_SHADER_MAX_PARAMS; j++) {
            /* Skip duplicates across passes */
            bool dup = false;
            for (unsigned k = 0; k < out->param_count; k++) {
                if (strcmp(out->params[k].id, tmp_params[j].id) == 0) { dup = true; break; }
            }
            if (!dup)
                out->params[out->param_count++] = tmp_params[j];
        }
        free(src);
    }

    /* Apply parameter overrides from preset file (re-read) */
    f = fopen(path, "r");
    if (f) {
        char pline[2048];
        while (fgets(pline, sizeof(pline), f)) {
            trim_line(pline);
            if (pline[0] == '#' || pline[0] == '\0') continue;
            char *pk, *pv;
            if (!parse_kv(pline, &pk, &pv)) continue;
            for (unsigned i = 0; i < out->param_count; i++) {
                if (strcmp(out->params[i].id, pk) == 0) {
                    out->params[i].value = (float)atof(pv);
                    break;
                }
            }
        }
        fclose(f);
    }

    return true;
}

/* ========================================================================= */
/* .glsl splitter                                                             */
/* ========================================================================= */

bool libra_glsl_split(const char *source, size_t len,
    const char *version_line,
    char *vs_out, size_t vs_size, char *fs_out, size_t fs_size)
{
    if (!source || !vs_out || !fs_out || vs_size < 256 || fs_size < 256)
        return false;

    size_t vp = 0, fp = 0;
    size_t vl = strlen(version_line);

    /* Prepend version line */
    vp = sappend(vs_out, vp, vs_size, version_line, vl);
    fp = sappend(fs_out, fp, fs_size, version_line, vl);

    /* Add stage defines + PARAMETER_UNIFORM for runtime param control */
    vp = sappend_str(vs_out, vp, vs_size, "#define VERTEX\n#define PARAMETER_UNIFORM\n");
    fp = sappend_str(fs_out, fp, fs_size, "#define FRAGMENT\n#define PARAMETER_UNIFORM\n");

    /* Copy source, skipping #version lines (already prepended) and
     * #pragma parameter lines (not needed in compiled shader) */
    const char *p = source;
    const char *end = source + len;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!nl) nl = end;
        size_t line_len = (size_t)(nl - p);

        bool skip = false;
        if (line_len >= 8 && strncmp(p, "#version", 8) == 0)
            skip = true;
        if (line_len >= 17 && strncmp(p, "#pragma parameter", 17) == 0)
            skip = true;

        if (!skip) {
            /* Include newline */
            size_t copy_len = (nl < end) ? line_len + 1 : line_len;
            vp = sappend(vs_out, vp, vs_size, p, copy_len);
            fp = sappend(fs_out, fp, fs_size, p, copy_len);
        }

        p = (nl < end) ? nl + 1 : end;
    }

    /* Null-terminate */
    if (vp < vs_size) vs_out[vp] = '\0'; else vs_out[vs_size - 1] = '\0';
    if (fp < fs_size) fs_out[fp] = '\0'; else fs_out[fs_size - 1] = '\0';

    return true;
}

