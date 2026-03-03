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

bool libra_shader_preset_load(libra_shader_preset_t *out, const char *path)
{
    if (!out || !path) return false;
    memset(out, 0, sizeof(*out));

    /* Determine base directory */
    {
        const char *last_slash = strrchr(path, '/');
        if (last_slash) {
            size_t len = (size_t)(last_slash - path);
            if (len >= sizeof(out->base_dir))
                len = sizeof(out->base_dir) - 1;
            memcpy(out->base_dir, path, len);
            out->base_dir[len] = '\0';
        } else {
            snprintf(out->base_dir, sizeof(out->base_dir), ".");
        }
    }

    /* Set defaults for all passes */
    for (int i = 0; i < LIBRA_SHADER_MAX_PASSES; i++) {
        out->passes[i].filter_linear = -1; /* unset */
        out->passes[i].scale_x = 1.0f;
        out->passes[i].scale_y = 1.0f;
        out->passes[i].scale_type_x = LIBRA_SCALE_SOURCE;
        out->passes[i].scale_type_y = LIBRA_SCALE_SOURCE;
    }

    /* Temporary LUT names for matching properties */
    char lut_names[LIBRA_SHADER_MAX_LUTS][64];
    memset(lut_names, 0, sizeof(lut_names));

    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        trim_line(line);
        if (line[0] == '#' || line[0] == '\0')
            continue;

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

/* ========================================================================= */
/* .slang -> GLSL transpiler                                                  */
/* ========================================================================= */

/* Helper: skip past layout(...) qualifier at current position.
 * Returns pointer to first non-space char after the closing ')'. */
static const char *skip_layout(const char *p)
{
    if (strncmp(p, "layout", 6) != 0)
        return p;
    const char *paren = strchr(p, '(');
    if (!paren) return p;
    int depth = 1;
    const char *q = paren + 1;
    while (*q && depth > 0) {
        if (*q == '(') depth++;
        else if (*q == ')') depth--;
        q++;
    }
    while (*q == ' ' || *q == '\t') q++;
    return q;
}

/* Helper: check if line starts with a layout qualifier */
static bool has_layout(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "layout", 6) == 0 && (p[6] == '(' || p[6] == ' ');
}

/* Helper: find and flatten a UBO/push_constant block.
 * Scans from the current source position for "uniform BlockName { ... } instanceName;"
 * or "uniform ... { ... };" patterns. Outputs individual "uniform type name;" lines.
 * Returns the number of bytes consumed from src, or 0 if not a block. */
static size_t flatten_block(const char *src, const char *src_end,
                            char *out, size_t *out_pos, size_t out_cap,
                            char *replacements, size_t *rep_pos, size_t rep_cap)
{
    /* Look for opening brace */
    const char *brace = NULL;
    const char *p = src;
    while (p < src_end && *p != '\n') {
        if (*p == '{') { brace = p; break; }
        p++;
    }
    if (!brace) return 0;

    /* Find closing brace */
    const char *close = NULL;
    int depth = 1;
    const char *q = brace + 1;
    while (q < src_end && depth > 0) {
        if (*q == '{') depth++;
        else if (*q == '}') { depth--; if (depth == 0) { close = q; break; } }
        q++;
    }
    if (!close) return 0;

    /* Find instance name after closing brace (optional) */
    const char *semi = close + 1;
    while (semi < src_end && (*semi == ' ' || *semi == '\t')) semi++;
    char inst_name[64] = {0};
    if (semi < src_end && *semi != ';' && isalpha((unsigned char)*semi)) {
        const char *ns = semi;
        while (semi < src_end && (isalnum((unsigned char)*semi) || *semi == '_')) semi++;
        size_t nlen = (size_t)(semi - ns);
        if (nlen < sizeof(inst_name))
            memcpy(inst_name, ns, nlen);
    }
    /* Skip to semicolon */
    while (semi < src_end && *semi != ';' && *semi != '\n') semi++;
    if (semi < src_end && *semi == ';') semi++;
    if (semi < src_end && *semi == '\n') semi++;

    /* Extract member declarations from inside braces */
    const char *member = brace + 1;
    while (member < close) {
        /* Skip whitespace/newlines */
        while (member < close && (*member == ' ' || *member == '\t' ||
                                   *member == '\n' || *member == '\r'))
            member++;
        if (member >= close) break;

        /* Find end of this member line (semicolon) */
        const char *mem_end = member;
        while (mem_end < close && *mem_end != ';') mem_end++;
        if (mem_end >= close) break;

        /* Skip layout qualifiers on members */
        const char *mstart = member;
        while (*mstart == ' ' || *mstart == '\t') mstart++;
        if (has_layout(mstart))
            mstart = skip_layout(mstart);

        /* Output "uniform <member_decl>;" */
        size_t mlen = (size_t)(mem_end - mstart);
        /* Trim leading spaces */
        while (mlen > 0 && (*mstart == ' ' || *mstart == '\t')) { mstart++; mlen--; }

        if (mlen > 0) {
            *out_pos = sappend_str(out, *out_pos, out_cap, "uniform ");
            *out_pos = sappend(out, *out_pos, out_cap, mstart, mlen);
            *out_pos = sappend_str(out, *out_pos, out_cap, ";\n");

            /* If there's an instance name, record replacement: "inst.member" -> "member"
             * Extract just the variable name (last word before any array brackets) */
            if (inst_name[0]) {
                /* Find the variable name in the member declaration */
                const char *name_end = mstart + mlen;
                /* Back up past array brackets */
                if (name_end > mstart && *(name_end-1) == ']') {
                    while (name_end > mstart && *name_end != '[') name_end--;
                }
                /* Back up past trailing spaces */
                while (name_end > mstart && (*(name_end-1) == ' ' || *(name_end-1) == '\t'))
                    name_end--;
                /* Find start of name */
                const char *name_start = name_end;
                while (name_start > mstart &&
                       (isalnum((unsigned char)*(name_start-1)) || *(name_start-1) == '_'))
                    name_start--;

                if (name_start < name_end) {
                    /* Store "inst_name.member_name\0member_name\0" */
                    size_t iname_len = strlen(inst_name);
                    size_t vname_len = (size_t)(name_end - name_start);
                    size_t entry_len = iname_len + 1 + vname_len + 1 + vname_len + 1;
                    if (*rep_pos + entry_len < rep_cap) {
                        /* "inst.var" */
                        memcpy(replacements + *rep_pos, inst_name, iname_len);
                        *rep_pos += iname_len;
                        replacements[(*rep_pos)++] = '.';
                        memcpy(replacements + *rep_pos, name_start, vname_len);
                        *rep_pos += vname_len;
                        replacements[(*rep_pos)++] = '\0';
                        /* "var" */
                        memcpy(replacements + *rep_pos, name_start, vname_len);
                        *rep_pos += vname_len;
                        replacements[(*rep_pos)++] = '\0';
                    }
                }
            }
        }

        member = mem_end + 1;
    }

    return (size_t)(semi - src);
}

/* Apply instance name replacements: "inst.member" -> "member" */
static void apply_replacements(char *buf, size_t buf_size,
                                const char *replacements, size_t rep_len)
{
    if (rep_len == 0) return;

    /* Parse replacement pairs from packed buffer */
    const char *p = replacements;
    const char *end = replacements + rep_len;
    while (p < end) {
        const char *from = p;
        size_t from_len = strlen(from);
        p += from_len + 1;
        if (p >= end) break;
        const char *to = p;
        size_t to_len = strlen(to);
        p += to_len + 1;

        if (from_len == 0) continue;

        /* Replace all occurrences in buf */
        char *scan = buf;
        while ((scan = strstr(scan, from)) != NULL) {
            /* Make sure it's not part of a larger identifier */
            if (scan > buf) {
                char prev = *(scan - 1);
                if (isalnum((unsigned char)prev) || prev == '_') { scan++; continue; }
            }
            char after = scan[from_len];
            if (isalnum((unsigned char)after) || after == '_') { scan++; continue; }

            /* Perform replacement */
            size_t remaining = strlen(scan + from_len);
            if (to_len <= from_len) {
                memcpy(scan, to, to_len);
                memmove(scan + to_len, scan + from_len, remaining + 1);
            } else {
                size_t scan_off = (size_t)(scan - buf);
                if (scan_off + to_len + remaining >= buf_size) { scan++; continue; }
                memmove(scan + to_len, scan + from_len, remaining + 1);
                memcpy(scan, to, to_len);
            }
            scan += to_len;
        }
    }
}

bool libra_slang_to_glsl(const char *source, size_t len,
    const char *version_line, bool is_gles2,
    char *vs_out, size_t vs_size, char *fs_out, size_t fs_size)
{
    if (!source || !vs_out || !fs_out || vs_size < 256 || fs_size < 256)
        return false;

    /* Phase 1: Split on #pragma stage vertex / fragment */
    const char *common_start = source;
    const char *common_end = NULL;
    const char *vs_start = NULL, *vs_end = NULL;
    const char *fs_start = NULL, *fs_end = NULL;
    const char *src_end = source + len;

    const char *p = source;
    while (p < src_end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(src_end - p));
        if (!nl) nl = src_end;

        /* Check for #pragma stage */
        const char *tp = p;
        while (tp < nl && (*tp == ' ' || *tp == '\t')) tp++;
        if (strncmp(tp, "#pragma stage ", 14) == 0) {
            const char *stage = tp + 14;
            while (*stage == ' ') stage++;
            if (strncmp(stage, "vertex", 6) == 0) {
                if (!common_end) common_end = p;
                if (fs_start && !fs_end) fs_end = p; /* close previous FS section */
                vs_start = (nl < src_end) ? nl + 1 : src_end;
            } else if (strncmp(stage, "fragment", 8) == 0) {
                if (!common_end) common_end = p;
                if (vs_start && !vs_end) vs_end = p;
                fs_start = (nl < src_end) ? nl + 1 : src_end;
            }
        }

        p = (nl < src_end) ? nl + 1 : src_end;
    }

    /* Close last open section */
    if (vs_start && !vs_end) vs_end = src_end;
    if (fs_start && !fs_end) fs_end = src_end;
    if (!common_end) common_end = source; /* no pragma found — everything is common */

    /* If no stage pragmas at all, treat as plain GLSL (copy to both) */
    if (!vs_start && !fs_start) {
        return libra_glsl_split(source, len, version_line,
                                vs_out, vs_size, fs_out, fs_size);
    }

    /* Phase 2: Build VS and FS sources with transformations */
    size_t vp = 0, fp = 0;
    size_t vl = strlen(version_line);

    vp = sappend(vs_out, vp, vs_size, version_line, vl);
    fp = sappend(fs_out, fp, fs_size, version_line, vl);

    /* Replacement buffer for instance name flattening */
    char replacements[4096];
    size_t rep_pos = 0;

    /* Process common section and each stage section through the same pipeline */
    struct section { const char *start; const char *end; int dest; /* 0=both, 1=vs, 2=fs */ };
    struct section sections[3];
    int nsections = 0;
    sections[nsections++] = (struct section){ common_start, common_end, 0 };
    if (vs_start) sections[nsections++] = (struct section){ vs_start, vs_end, 1 };
    if (fs_start) sections[nsections++] = (struct section){ fs_start, fs_end, 2 };

    for (int si = 0; si < nsections; si++) {
        const char *sp = sections[si].start;
        const char *se = sections[si].end;
        int dest = sections[si].dest;

        while (sp < se) {
            const char *nl = (const char *)memchr(sp, '\n', (size_t)(se - sp));
            if (!nl) nl = se;
            size_t line_len = (size_t)(nl - sp);
            size_t copy_len = (nl < se) ? line_len + 1 : line_len;

            /* Skip #version, #pragma parameter, #pragma stage, #pragma name, #pragma format */
            const char *tp = sp;
            while (tp < nl && (*tp == ' ' || *tp == '\t')) tp++;

            bool skip = false;
            if (strncmp(tp, "#version", 8) == 0) skip = true;
            if (strncmp(tp, "#pragma parameter", 17) == 0) skip = true;
            if (strncmp(tp, "#pragma stage", 13) == 0) skip = true;
            if (strncmp(tp, "#pragma name", 12) == 0) skip = true;
            if (strncmp(tp, "#pragma format", 14) == 0) skip = true;

            if (skip) { sp = (nl < se) ? nl + 1 : se; continue; }

            /* Check for UBO / push_constant block: line contains '{' after uniform */
            if (strstr(sp, "uniform") && line_len > 0) {
                /* Check if this starts a block (has { on this line or is followed by {) */
                const char *after_layout = tp;
                if (has_layout(after_layout))
                    after_layout = skip_layout(after_layout);

                /* Check for "uniform <name> {" or "push_constant ... {" */
                bool is_block = false;
                if (strncmp(after_layout, "uniform", 7) == 0) {
                    /* Look for '{' on this line */
                    for (const char *c = after_layout; c < nl; c++) {
                        if (*c == '{') { is_block = true; break; }
                    }
                }

                if (is_block) {
                    size_t consumed;
                    if (dest == 0 || dest == 1) {
                        consumed = flatten_block(sp, se, vs_out, &vp, vs_size,
                                                 replacements, &rep_pos, sizeof(replacements));
                    }
                    if (dest == 0 || dest == 2) {
                        size_t dummy_rep = rep_pos; /* avoid double recording */
                        consumed = flatten_block(sp, se, fs_out, &fp, fs_size,
                                                 replacements, &dummy_rep, sizeof(replacements));
                    }
                    if (consumed > 0) {
                        sp += consumed;
                        continue;
                    }
                }
            }

            /* Strip layout qualifiers from declarations */
            if (has_layout(tp)) {
                const char *after = skip_layout(tp);
                /* Output indentation + rest of line */
                size_t indent = (size_t)(tp - sp);
                size_t rest_len = (size_t)(nl - after);
                if (dest == 0 || dest == 1) {
                    vp = sappend(vs_out, vp, vs_size, sp, indent);
                    vp = sappend(vs_out, vp, vs_size, after, rest_len);
                    vp = sappend_str(vs_out, vp, vs_size, "\n");
                }
                if (dest == 0 || dest == 2) {
                    fp = sappend(fs_out, fp, fs_size, sp, indent);
                    fp = sappend(fs_out, fp, fs_size, after, rest_len);
                    fp = sappend_str(fs_out, fp, fs_size, "\n");
                }
            } else {
                /* Pass through unchanged */
                if (dest == 0 || dest == 1)
                    vp = sappend(vs_out, vp, vs_size, sp, copy_len);
                if (dest == 0 || dest == 2)
                    fp = sappend(fs_out, fp, fs_size, sp, copy_len);
            }

            sp = (nl < se) ? nl + 1 : se;
        }
    }

    /* Null-terminate before replacements */
    if (vp < vs_size) vs_out[vp] = '\0'; else vs_out[vs_size - 1] = '\0';
    if (fp < fs_size) fs_out[fp] = '\0'; else fs_out[fs_size - 1] = '\0';

    /* Phase 3: Apply instance name replacements */
    apply_replacements(vs_out, vs_size, replacements, rep_pos);
    apply_replacements(fs_out, fs_size, replacements, rep_pos);

    /* Phase 4: GLES 2.0 compatibility transforms */
    if (is_gles2) {
        /* VS: "in " -> "attribute ", "out " -> "varying " */
        /* FS: "in " -> "varying ", "out vec4 ..." -> remove (use gl_FragColor),
         *     "texture(" -> "texture2D(" */

        /* Simple line-by-line in-place rewrite for VS */
        /* Note: This is a simplified approach. For production quality,
         * a proper token-based rewrite would be better. */
        /* For now, the transpiler targets GLES 3.0+ / GL 3.30+,
         * so GLES 2.0 path is left as a stub for future expansion. */
    }

    return true;
}
