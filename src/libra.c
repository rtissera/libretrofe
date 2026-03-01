// SPDX-License-Identifier: MIT
#include "libra_internal.h"
#include "environment.h"
#include "input.h"
#include "netplay.h"
#include "rollback.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <stdio.h>
#include <ctype.h>

/* -------------------------------------------------------------------------
 * Global libretro callbacks — cores call these; we dispatch to s_ctx.
 * Using globals is standard practice for libretro frontends: only one core
 * can be active at a time per process anyway.
 * ---------------------------------------------------------------------- */

static libra_ctx_t *s_active = NULL;

static void video_refresh_cb(const void *data, unsigned width,
                              unsigned height, size_t pitch)
{
    if (!s_active)
        return;
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        /* HW frame: FBO already has the rendered image.
         * Notify host with data=NULL, pitch=0 as sentinel;
         * pixel_format = -1 signals a HW frame. */
        if (s_active->config.video)
            s_active->config.video(s_active->config.userdata,
                                   NULL, width, height, 0, -1);
        return;
    }
    if (!data) /* NULL frame = duplicate, honour GET_CAN_DUPE */
        return;
    if (!s_active->config.video)
        return;
    s_active->config.video(s_active->config.userdata, data,
                           width, height, pitch, s_active->pixel_format);
}

static void audio_sample_cb(int16_t left, int16_t right)
{
    if (s_active && s_active->audio)
        libra_audio_push(s_active->audio, left, right);
}

static size_t audio_sample_batch_cb(const int16_t *data, size_t frames)
{
    if (s_active && s_active->audio)
        libra_audio_push_batch(s_active->audio, data, frames);
    return frames;
}

static void input_poll_cb(void)
{
    if (s_active)
        libra_input_poll(s_active);
}

static int16_t input_state_cb(unsigned port, unsigned device,
                               unsigned index, unsigned id)
{
    if (s_active)
        return libra_input_state(s_active, port, device, index, id);
    return 0;
}

/* -------------------------------------------------------------------------
 * Path helpers
 * ---------------------------------------------------------------------- */

/* Extract directory, stem (name without ext), and lowercase extension.
 * All strings are strdup'd; caller frees via free_game_path_info(). */
static void parse_game_path(libra_ctx_t *ctx, const char *path)
{
    free(ctx->game_path); free(ctx->game_dir);
    free(ctx->game_name); free(ctx->game_ext);
    ctx->game_path = ctx->game_dir = ctx->game_name = ctx->game_ext = NULL;
    ctx->has_game_info_ext = false;

    if (!path) return;
    ctx->game_path = strdup(path);

    const char *sep  = strrchr(path, '/');
    const char *base;
    if (sep) {
        ctx->game_dir = strndup(path, (size_t)(sep - path));
        base = sep + 1;
    } else {
        ctx->game_dir = strdup(".");
        base = path;
    }

    const char *dot = strrchr(base, '.');
    if (dot && dot > base) {
        ctx->game_name = strndup(base, (size_t)(dot - base));
        ctx->game_ext  = strdup(dot + 1);
        for (char *p = ctx->game_ext; *p; p++)
            *p = (char)tolower((unsigned char)*p);
    } else {
        ctx->game_name = strdup(base);
        ctx->game_ext  = strdup("");
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

libra_ctx_t *libra_create(const libra_config_t *config)
{
    if (!config)
        return NULL;

    libra_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->config       = *config;
    ctx->pixel_format = RETRO_PIXEL_FORMAT_0RGB1555; /* default */

    /* Mark all options as uncategorized by default */
    for (int i = 0; i < LIBRA_MAX_OPTIONS; i++)
        ctx->opt_cat_idx[i] = -1;

    return ctx;
}

void libra_destroy(libra_ctx_t *ctx)
{
    if (!ctx)
        return;
    libra_unload_game(ctx);
    libra_unload_core(ctx);
    free(ctx->system_dir);
    free(ctx->save_dir);
    free(ctx->assets_dir);
    free(ctx->core_path);
    free(ctx->username);
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        free(ctx->opt_keys[i]);
        free(ctx->opt_vals[i]);
        free(ctx->opt_val_list[i]);
        free(ctx->opt_descs[i]);
    }
    for (unsigned i = 0; i < ctx->opt_cat_count; i++) {
        free(ctx->opt_cat_keys[i]);
        free(ctx->opt_cat_descs[i]);
    }
    libra_audio_destroy(ctx->audio);
    free(ctx->sw_fb);
    /* Free memory map descriptors */
    if (ctx->mem_descriptors) {
        for (unsigned i = 0; i < ctx->mem_descriptor_count; i++)
            free((void *)ctx->mem_descriptors[i].addrspace);
        free(ctx->mem_descriptors);
    }
    /* Free content info overrides */
    if (ctx->content_overrides) {
        for (unsigned i = 0; i < ctx->content_override_count; i++)
            free((void *)ctx->content_overrides[i].extensions);
        free(ctx->content_overrides);
    }
    libra_netplay_free(ctx->netplay);
    ctx->netplay = NULL;
    libra_rollback_free(ctx->rollback);
    ctx->rollback = NULL;
    if (s_active == ctx)
        s_active = NULL;
    free(ctx);
}

void libra_set_system_directory(libra_ctx_t *ctx, const char *path)
{
    if (!ctx) return;
    free(ctx->system_dir);
    ctx->system_dir = path ? strdup(path) : NULL;
}

void libra_set_save_directory(libra_ctx_t *ctx, const char *path)
{
    if (!ctx) return;
    free(ctx->save_dir);
    ctx->save_dir = path ? strdup(path) : NULL;
}

void libra_set_assets_directory(libra_ctx_t *ctx, const char *path)
{
    if (!ctx) return;
    free(ctx->assets_dir);
    ctx->assets_dir = path ? strdup(path) : NULL;
}

void libra_set_fast_forward(libra_ctx_t *ctx, bool enabled)
{
    if (ctx)
        ctx->fast_forwarding = enabled;
}

void libra_set_username(libra_ctx_t *ctx, const char *name)
{
    if (!ctx) return;
    free(ctx->username);
    ctx->username = name ? strdup(name) : NULL;
}

void libra_set_language(libra_ctx_t *ctx, unsigned language)
{
    if (ctx)
        ctx->language = language;
}

void libra_set_savestate_context(libra_ctx_t *ctx, unsigned context)
{
    if (ctx)
        ctx->savestate_context = context;
}

bool libra_load_core(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !path)
        return false;

    libra_unload_core(ctx);

    ctx->core = libra_core_load(path);
    if (!ctx->core)
        return false;

    free(ctx->core_path);
    ctx->core_path = strdup(path);

    /* Register environment callback (needs ctx set first) */
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    ctx->core->retro_set_environment(libra_environment_cb);

    /* Register remaining callbacks */
    ctx->core->retro_set_video_refresh(video_refresh_cb);
    ctx->core->retro_set_audio_sample(audio_sample_cb);
    ctx->core->retro_set_audio_sample_batch(audio_sample_batch_cb);
    ctx->core->retro_set_input_poll(input_poll_cb);
    ctx->core->retro_set_input_state(input_state_cb);

    ctx->core->retro_init();

    return true;
}

void libra_unload_core(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core)
        return;

    if (ctx->game_loaded)
        libra_unload_game(ctx);

    libra_environment_set_ctx(ctx);
    s_active = ctx;
    ctx->core->retro_deinit();

    libra_core_unload(ctx->core);
    ctx->core = NULL;

    free(ctx->core_path);
    ctx->core_path = NULL;

    if (s_active == ctx)
        s_active = NULL;
}

bool libra_load_game(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !ctx->core || ctx->game_loaded)
        return false;

    struct retro_game_info game = { 0 };
    game.path = path;

    /* Determine need_fullpath: check content info overrides first, then
     * fall back to the core's global sys_info.need_fullpath. */
    bool need_fullpath = ctx->core->sys_info.need_fullpath;
    if (ctx->content_overrides && ctx->content_override_count > 0 && path) {
        const char *dot = strrchr(path, '.');
        if (dot && dot[1]) {
            const char *ext = dot + 1;
            for (unsigned i = 0; i < ctx->content_override_count; i++) {
                const char *exts = ctx->content_overrides[i].extensions;
                if (!exts) continue;
                /* Check if ext matches any in the "|"-delimited list */
                char *tmp = strdup(exts);
                char *tok = strtok(tmp, "|");
                while (tok) {
                    if (strcasecmp(tok, ext) == 0) {
                        need_fullpath = ctx->content_overrides[i].need_fullpath;
                        free(tmp);
                        goto fullpath_resolved;
                    }
                    tok = strtok(NULL, "|");
                }
                free(tmp);
            }
        }
    }
fullpath_resolved:

    if (!need_fullpath && path) {
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            rewind(f);
            if (size > 0) {
                void *data = malloc((size_t)size);
                if (data && fread(data, 1, (size_t)size, f) == (size_t)size) {
                    game.data = data;
                    game.size = (size_t)size;
                }
                if (!game.data)
                    free(data);
            }
            fclose(f);
        }
    }

    /* Prepare game info ext (core may call GET_GAME_INFO_EXT during load) */
    parse_game_path(ctx, path);
    memset(&ctx->game_info_ext, 0, sizeof(ctx->game_info_ext));
    ctx->game_info_ext.full_path       = ctx->game_path;
    ctx->game_info_ext.dir             = ctx->game_dir;
    ctx->game_info_ext.name            = ctx->game_name;
    ctx->game_info_ext.ext             = ctx->game_ext;
    ctx->game_info_ext.data            = game.data;
    ctx->game_info_ext.size            = game.size;
    ctx->game_info_ext.file_in_archive = false;
    ctx->game_info_ext.persistent_data = false;
    ctx->has_game_info_ext             = true;

    s_active = ctx;
    libra_environment_set_ctx(ctx);
    bool ok = ctx->core->retro_load_game(&game);

    /* Data buffer is about to be freed — clear from game_info_ext */
    ctx->game_info_ext.data = NULL;
    ctx->game_info_ext.size = 0;
    free((void *)game.data);

    if (!ok) {
        fprintf(stderr, "libra: retro_load_game failed\n");
        return false;
    }

    ctx->core->retro_get_system_av_info(&ctx->core->av_info);

    /* Create audio resampler */
    libra_audio_destroy(ctx->audio);
    ctx->audio = libra_audio_create(
        ctx->core->av_info.timing.sample_rate,
        ctx->config.audio_output_rate ? ctx->config.audio_output_rate : 48000);

    /* Enable async audio if core registered a callback */
    if (ctx->has_audio_cb && ctx->audio_cb.set_state)
        ctx->audio_cb.set_state(true);

    ctx->game_loaded = true;
    return true;
}

void libra_unload_game(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return;

    /* Disable async audio before unload */
    if (ctx->has_audio_cb && ctx->audio_cb.set_state)
        ctx->audio_cb.set_state(false);

    /* Notify HW render core that context is being destroyed */
    if (ctx->has_hw_render && ctx->hw_render.context_destroy) {
        s_active = ctx;
        libra_environment_set_ctx(ctx);
        ctx->hw_render.context_destroy();
    }
    ctx->has_hw_render = false;
    memset(&ctx->hw_render, 0, sizeof(ctx->hw_render));
    ctx->hw_fbo = 0;
    ctx->hw_get_proc_address = NULL;

    s_active = ctx;
    libra_environment_set_ctx(ctx);
    ctx->core->retro_unload_game();
    ctx->game_loaded = false;

    /* SRAM pointer is no longer valid after unload */
    free(ctx->sram_shadow);
    ctx->sram_shadow      = NULL;
    ctx->sram_shadow_size = 0;

    /* Game info ext cleanup */
    free(ctx->game_path); ctx->game_path = NULL;
    free(ctx->game_dir);  ctx->game_dir  = NULL;
    free(ctx->game_name); ctx->game_name = NULL;
    free(ctx->game_ext);  ctx->game_ext  = NULL;
    ctx->has_game_info_ext = false;
    memset(&ctx->game_info_ext, 0, sizeof(ctx->game_info_ext));
}

void libra_run(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return;

    s_active = ctx;
    libra_environment_set_ctx(ctx);

    /* Notify the core of frame delta if it registered a frame_time callback */
    if (ctx->has_frame_time_cb && ctx->frame_time_cb.callback)
        ctx->frame_time_cb.callback(ctx->frame_time_cb.reference);

    ctx->core->retro_run();

    /* Async audio: core generates samples via this callback */
    if (ctx->has_audio_cb && ctx->audio_cb.callback)
        ctx->audio_cb.callback();

    /* Flush resampled audio to host */
    if (ctx->audio && ctx->config.audio) {
        libra_audio_flush(ctx->audio,
                          (libra_audio_output_fn)ctx->config.audio,
                          ctx->config.userdata);
    }

    /* Notify core of audio buffer status (for buffer-aware frame-skipping) */
    if (ctx->audio_buf_status_cb && ctx->audio) {
        unsigned occupancy = ctx->audio->count * 100 / LIBRA_RING_FRAMES;
        bool underrun_likely = ctx->audio->count < LIBRA_RING_FRAMES / 8;
        ctx->audio_buf_status_cb(true, occupancy, underrun_likely);
    }
}

void libra_reset(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return;

    s_active = ctx;
    libra_environment_set_ctx(ctx);
    ctx->core->retro_reset();
}

double libra_get_fps(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core)
        return 0.0;
    return ctx->core->av_info.timing.fps;
}

double libra_get_sample_rate(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core)
        return 0.0;
    return ctx->core->av_info.timing.sample_rate;
}

void libra_get_geometry(libra_ctx_t *ctx,
                        unsigned *base_w, unsigned *base_h, float *aspect)
{
    if (!ctx || !ctx->core)
        return;
    const struct retro_game_geometry *g = &ctx->core->av_info.geometry;
    if (base_w) *base_w = g->base_width;
    if (base_h) *base_h = g->base_height;
    if (aspect) *aspect = g->aspect_ratio > 0.0f
                          ? g->aspect_ratio
                          : (float)g->base_width / (float)g->base_height;
}

const char *libra_core_name(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core)
        return NULL;
    return ctx->core->sys_info.library_name;
}

const char *libra_core_extensions(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core)
        return NULL;
    return ctx->core->sys_info.valid_extensions;
}

const char *libra_get_option(libra_ctx_t *ctx, const char *key)
{
    if (!ctx || !key)
        return NULL;
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        if (strcmp(ctx->opt_keys[i], key) == 0)
            return ctx->opt_vals[i];
    }
    return NULL;
}

void libra_set_option(libra_ctx_t *ctx, const char *key, const char *value)
{
    if (!ctx || !key || !value)
        return;
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        if (strcmp(ctx->opt_keys[i], key) == 0) {
            free(ctx->opt_vals[i]);
            ctx->opt_vals[i] = strdup(value);
            ctx->opt_updated = true;
            return;
        }
    }
    /* Key not found: insert new */
    if (ctx->opt_count < LIBRA_MAX_OPTIONS) {
        ctx->opt_keys[ctx->opt_count] = strdup(key);
        ctx->opt_vals[ctx->opt_count] = strdup(value);
        ctx->opt_count++;
        ctx->opt_updated = true;
    }
}

/* -------------------------------------------------------------------------
 * Save states
 * ---------------------------------------------------------------------- */

bool libra_save_state(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !path)
        return false;
    if (!ctx->core->retro_serialize_size || !ctx->core->retro_serialize) {
        fprintf(stderr, "libra: core does not support save states\n");
        return false;
    }

    size_t size = ctx->core->retro_serialize_size();
    if (size == 0)
        return false;

    void *buf = malloc(size);
    if (!buf)
        return false;

    if (!ctx->core->retro_serialize(buf, size)) {
        fprintf(stderr, "libra: retro_serialize failed\n");
        free(buf);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "libra: cannot open '%s' for writing\n", path);
        free(buf);
        return false;
    }

    bool ok = (fwrite(buf, 1, size, f) == size);
    fclose(f);
    free(buf);
    if (!ok)
        fprintf(stderr, "libra: short write to '%s'\n", path);
    return ok;
}

bool libra_load_state(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !path)
        return false;
    if (!ctx->core->retro_serialize_size || !ctx->core->retro_unserialize) {
        fprintf(stderr, "libra: core does not support save states\n");
        return false;
    }

    size_t expected = ctx->core->retro_serialize_size();

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "libra: cannot open '%s' for reading\n", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    if (fsize <= 0 || (size_t)fsize < expected) {
        fprintf(stderr, "libra: state file '%s' is too small (%ld < %zu)\n",
                path, fsize, expected);
        fclose(f);
        return false;
    }

    void *buf = malloc((size_t)fsize);
    if (!buf) {
        fclose(f);
        return false;
    }

    bool ok = (fread(buf, 1, (size_t)fsize, f) == (size_t)fsize);
    fclose(f);

    if (ok)
        ok = ctx->core->retro_unserialize(buf, (size_t)fsize);
    else
        fprintf(stderr, "libra: short read from '%s'\n", path);

    free(buf);
    return ok;
}

/* -------------------------------------------------------------------------
 * In-memory serialization
 * ---------------------------------------------------------------------- */

size_t libra_serialize_size(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return 0;
    if (!ctx->core->retro_serialize_size)
        return 0;
    return ctx->core->retro_serialize_size();
}

bool libra_serialize(libra_ctx_t *ctx, void *data, size_t size)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !data || size == 0)
        return false;
    if (!ctx->core->retro_serialize)
        return false;
    return ctx->core->retro_serialize(data, size);
}

bool libra_unserialize(libra_ctx_t *ctx, const void *data, size_t size)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !data || size == 0)
        return false;
    if (!ctx->core->retro_unserialize)
        return false;
    return ctx->core->retro_unserialize(data, size);
}

/* -------------------------------------------------------------------------
 * Controller
 * ---------------------------------------------------------------------- */

void libra_set_controller(libra_ctx_t *ctx, unsigned port, unsigned device)
{
    if (!ctx || !ctx->core)
        return;
    if (port < 16) {
        ctx->port_devices[port] = device;
        if (port >= ctx->port_count)
            ctx->port_count = port + 1;
    }
    if (!ctx->core->retro_set_controller_port_device) {
        fprintf(stderr, "libra: core does not implement retro_set_controller_port_device\n");
        return;
    }
    ctx->core->retro_set_controller_port_device(port, device);
}

/* -------------------------------------------------------------------------
 * Disk control
 * ---------------------------------------------------------------------- */

bool libra_get_disk_count(libra_ctx_t *ctx, unsigned *count)
{
    if (!ctx || !count)
        return false;
    if (!ctx->has_disk_cb || !ctx->disk_cb.get_num_images)
        return false;
    *count = ctx->disk_cb.get_num_images();
    return true;
}

bool libra_swap_disk(libra_ctx_t *ctx, unsigned index)
{
    if (!ctx || !ctx->has_disk_cb)
        return false;

    struct retro_disk_control_callback *cb = &ctx->disk_cb;
    if (!cb->set_eject_state || !cb->set_image_index)
        return false;

    /* Eject if not already ejected */
    if (cb->get_eject_state && !cb->get_eject_state())
        cb->set_eject_state(true);
    else if (!cb->get_eject_state)
        cb->set_eject_state(true);

    cb->set_image_index(index);
    cb->set_eject_state(false);
    return true;
}

/* -------------------------------------------------------------------------
 * Cheats
 * ---------------------------------------------------------------------- */

void libra_clear_cheats(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core)
        return;
    if (!ctx->core->retro_cheat_reset) {
        fprintf(stderr, "libra: core does not implement retro_cheat_reset\n");
        return;
    }
    ctx->core->retro_cheat_reset();
}

void libra_set_cheat(libra_ctx_t *ctx, unsigned index, bool enabled, const char *code)
{
    if (!ctx || !ctx->core || !code)
        return;
    if (!ctx->core->retro_cheat_set) {
        fprintf(stderr, "libra: core does not implement retro_cheat_set\n");
        return;
    }
    ctx->core->retro_cheat_set(index, enabled, code);
}

/* -------------------------------------------------------------------------
 * Option visibility
 * ---------------------------------------------------------------------- */

bool libra_is_option_visible(libra_ctx_t *ctx, const char *key)
{
    if (!ctx || !key)
        return true;
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        if (strcmp(ctx->opt_keys[i], key) == 0)
            return ctx->opt_visible[i];
    }
    return true; /* unknown key — assume visible */
}

unsigned libra_option_count(libra_ctx_t *ctx)
{
    return ctx ? ctx->opt_count : 0;
}

const char *libra_option_key(libra_ctx_t *ctx, unsigned index)
{
    if (!ctx || index >= ctx->opt_count)
        return NULL;
    return ctx->opt_keys[index];
}

const char *libra_option_value(libra_ctx_t *ctx, unsigned index)
{
    if (!ctx || index >= ctx->opt_count)
        return NULL;
    return ctx->opt_vals[index];
}

bool libra_option_cycle(libra_ctx_t *ctx, unsigned index, int direction)
{
    if (!ctx || index >= ctx->opt_count)
        return false;

    const char *vl = ctx->opt_val_list[index];
    if (!vl || !*vl)
        return false;

    /* Tokenise a copy of the val_list by '|' */
    char *copy = strdup(vl);
    if (!copy)
        return false;

    char  *tokens[LIBRA_MAX_OPTIONS];
    int    count   = 0;
    int    cur_idx = -1;
    const char *cur = ctx->opt_vals[index];

    char *tok = strtok(copy, "|");
    while (tok && count < LIBRA_MAX_OPTIONS) {
        tokens[count] = tok;
        if (cur && strcmp(tok, cur) == 0)
            cur_idx = count;
        count++;
        tok = strtok(NULL, "|");
    }

    if (count == 0) { free(copy); return false; }
    if (cur_idx < 0) cur_idx = 0;

    int next = ((cur_idx + direction) % count + count) % count;

    /* strdup before free(copy) because tokens[] point into copy */
    char *new_val = strdup(tokens[next]);
    free(copy);

    if (!new_val)
        return false;

    free(ctx->opt_vals[index]);
    ctx->opt_vals[index] = new_val;
    ctx->opt_updated     = true;
    return true;
}

/* -------------------------------------------------------------------------
 * Core option categories
 * ---------------------------------------------------------------------- */

unsigned libra_option_category_count(libra_ctx_t *ctx)
{
    return ctx ? ctx->opt_cat_count : 0;
}

const char *libra_option_category_key(libra_ctx_t *ctx, unsigned cat_index)
{
    if (!ctx || cat_index >= ctx->opt_cat_count)
        return NULL;
    return ctx->opt_cat_keys[cat_index];
}

const char *libra_option_category_desc(libra_ctx_t *ctx, unsigned cat_index)
{
    if (!ctx || cat_index >= ctx->opt_cat_count)
        return NULL;
    return ctx->opt_cat_descs[cat_index];
}

int libra_option_category_index(libra_ctx_t *ctx, unsigned opt_index)
{
    if (!ctx || opt_index >= ctx->opt_count)
        return -1;
    return ctx->opt_cat_idx[opt_index];
}

const char *libra_option_desc(libra_ctx_t *ctx, unsigned opt_index)
{
    if (!ctx || opt_index >= ctx->opt_count)
        return NULL;
    return ctx->opt_descs[opt_index];
}

/* -------------------------------------------------------------------------
 * Subsystem / multi-ROM loading
 * ---------------------------------------------------------------------- */

bool libra_load_game_special(libra_ctx_t *ctx, unsigned game_type,
                              const char **paths, unsigned num_paths)
{
    if (!ctx || !ctx->core || ctx->game_loaded)
        return false;
    if (!ctx->core->retro_load_game_special) {
        fprintf(stderr, "libra: core does not implement retro_load_game_special\n");
        return false;
    }
    if (!paths || num_paths == 0)
        return false;

    /* Build a retro_game_info array — pass full paths only.
     * Subsystem ROMs almost always have need_fullpath=true; for those that
     * don't the core will still receive the path and can open it itself. */
    struct retro_game_info *infos = calloc(num_paths, sizeof(*infos));
    if (!infos)
        return false;

    for (unsigned i = 0; i < num_paths; i++)
        infos[i].path = paths[i];

    s_active = ctx;
    libra_environment_set_ctx(ctx);
    bool ok = ctx->core->retro_load_game_special(game_type, infos, num_paths);
    free(infos);

    if (!ok) {
        fprintf(stderr, "libra: retro_load_game_special failed\n");
        return false;
    }

    ctx->core->retro_get_system_av_info(&ctx->core->av_info);

    libra_audio_destroy(ctx->audio);
    ctx->audio = libra_audio_create(
        ctx->core->av_info.timing.sample_rate,
        ctx->config.audio_output_rate ? ctx->config.audio_output_rate : 48000);

    ctx->game_loaded = true;
    return true;
}

/* -------------------------------------------------------------------------
 * SRAM
 * ---------------------------------------------------------------------- */

bool libra_save_sram(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !path)
        return false;
    if (!ctx->core->retro_get_memory_data || !ctx->core->retro_get_memory_size) {
        fprintf(stderr, "libra: core does not expose memory interface\n");
        return false;
    }

    size_t size = ctx->core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0)
        return false; /* core has no SRAM */

    void *data = ctx->core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return false;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "libra: cannot open '%s' for writing\n", path);
        return false;
    }

    bool ok = (fwrite(data, 1, size, f) == size);
    fclose(f);
    if (!ok)
        fprintf(stderr, "libra: short write to '%s'\n", path);
    return ok;
}

bool libra_load_sram(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !path)
        return false;
    if (!ctx->core->retro_get_memory_data || !ctx->core->retro_get_memory_size) {
        fprintf(stderr, "libra: core does not expose memory interface\n");
        return false;
    }

    size_t size = ctx->core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0)
        return false;

    void *dest = ctx->core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!dest)
        return false;

    FILE *f = fopen(path, "rb");
    if (!f)
        return false; /* no SRAM file yet — not an error */

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);

    /* Read up to `size` bytes; a truncated file is a soft error */
    size_t to_read = ((size_t)fsize < size) ? (size_t)fsize : size;
    bool ok = (fread(dest, 1, to_read, f) == to_read);
    fclose(f);
    if (!ok)
        fprintf(stderr, "libra: short read from '%s'\n", path);
    return ok;
}

bool libra_save_sram_if_dirty(libra_ctx_t *ctx, const char *path)
{
    if (!ctx || !ctx->core || !ctx->game_loaded || !path)
        return false;
    if (!ctx->core->retro_get_memory_data || !ctx->core->retro_get_memory_size)
        return false;

    size_t size = ctx->core->retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    if (size == 0)
        return false;

    uint8_t *data = (uint8_t *)ctx->core->retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    if (!data)
        return false;

    /* First call or size changed: establish baseline, no write */
    if (!ctx->sram_shadow || ctx->sram_shadow_size != size) {
        free(ctx->sram_shadow);
        ctx->sram_shadow = (uint8_t *)malloc(size);
        if (!ctx->sram_shadow) {
            ctx->sram_shadow_size = 0;
            return false;
        }
        memcpy(ctx->sram_shadow, data, size);
        ctx->sram_shadow_size = size;
        return false;
    }

    /* Clean — nothing to do */
    if (memcmp(ctx->sram_shadow, data, size) == 0)
        return false;

    /* Dirty — flush to disk and update shadow */
    bool ok = libra_save_sram(ctx, path);
    if (ok)
        memcpy(ctx->sram_shadow, data, size);
    return ok;
}

/* -------------------------------------------------------------------------
 * Core memory access
 * ---------------------------------------------------------------------- */

void *libra_get_memory_data(libra_ctx_t *ctx, unsigned id)
{
    if (!ctx || !ctx->core || !ctx->game_loaded ||
        !ctx->core->retro_get_memory_data)
        return NULL;
    return ctx->core->retro_get_memory_data(id);
}

size_t libra_get_memory_size(libra_ctx_t *ctx, unsigned id)
{
    if (!ctx || !ctx->core || !ctx->game_loaded ||
        !ctx->core->retro_get_memory_size)
        return 0;
    return ctx->core->retro_get_memory_size(id);
}

unsigned libra_memory_map_count(libra_ctx_t *ctx)
{
    return ctx ? ctx->mem_descriptor_count : 0;
}

uint32_t libra_memory_map_read(libra_ctx_t *ctx, uint32_t address,
                                uint8_t *buffer, uint32_t num_bytes)
{
    if (!ctx || !buffer || num_bytes == 0 || !ctx->mem_descriptors)
        return 0;

    for (unsigned i = 0; i < ctx->mem_descriptor_count; i++) {
        const struct retro_memory_descriptor *d = &ctx->mem_descriptors[i];
        if (!d->ptr || d->len == 0)
            continue;

        /* Match: if select is non-zero, use it; otherwise range-check */
        if (d->select) {
            if ((address & d->select) != (d->start & d->select))
                continue;
        } else {
            if (address < d->start || address >= d->start + d->len)
                continue;
        }

        /* Translate emulated address to offset within descriptor buffer:
         * offset = (address & ~disconnect) - start + descriptor->offset */
        size_t off = (size_t)((address & ~d->disconnect) - d->start) + d->offset;
        if (off >= d->len)
            continue;

        uint32_t avail = (uint32_t)(d->len - off);
        uint32_t to_read = num_bytes < avail ? num_bytes : avail;
        memcpy(buffer, (const uint8_t *)d->ptr + off, to_read);
        return to_read;
    }

    return 0;
}

bool libra_is_shutdown_requested(libra_ctx_t *ctx)
{
    return ctx && ctx->shutdown_requested;
}

unsigned libra_get_rotation(libra_ctx_t *ctx)
{
    return ctx ? ctx->rotation : 0;
}

void libra_keyboard_event(libra_ctx_t *ctx, bool down,
                           unsigned keycode, uint32_t character,
                           uint16_t key_modifiers)
{
    if (ctx && ctx->keyboard_cb)
        ctx->keyboard_cb(down, keycode, character, key_modifiers);
}

/* -------------------------------------------------------------------------
 * Hardware rendering API
 * ---------------------------------------------------------------------- */

void libra_set_preferred_hw_render(libra_ctx_t *ctx, unsigned context_type)
{
    if (ctx) ctx->preferred_hw_context = context_type;
}

void libra_set_hw_render_support(libra_ctx_t *ctx, unsigned context_type,
                                  unsigned max_major, unsigned max_minor)
{
    if (!ctx || context_type == 0 || context_type >= 8)
        return;
    ctx->hw_caps[context_type].supported  = true;
    ctx->hw_caps[context_type].max_major  = max_major;
    ctx->hw_caps[context_type].max_minor  = max_minor;
    ctx->hw_caps_set = true;
}

bool libra_hw_render_enabled(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render;
}

unsigned libra_hw_render_context_type(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render
           ? (unsigned)ctx->hw_render.context_type : 0;
}

unsigned libra_hw_render_version_major(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render ? ctx->hw_render.version_major : 0;
}

unsigned libra_hw_render_version_minor(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render ? ctx->hw_render.version_minor : 0;
}

bool libra_hw_render_bottom_left_origin(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render && ctx->hw_render.bottom_left_origin;
}

bool libra_hw_render_depth(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render && ctx->hw_render.depth;
}

bool libra_hw_render_stencil(libra_ctx_t *ctx)
{
    return ctx && ctx->has_hw_render && ctx->hw_render.stencil;
}

void libra_hw_render_set_fbo(libra_ctx_t *ctx, unsigned fbo)
{
    if (ctx) ctx->hw_fbo = fbo;
}

void libra_hw_render_set_get_proc_address(libra_ctx_t *ctx,
                                           libra_get_proc_address_t proc)
{
    if (ctx)
        ctx->hw_get_proc_address = (retro_hw_get_proc_address_t)proc;
}

void libra_hw_render_context_reset(libra_ctx_t *ctx)
{
    if (ctx && ctx->has_hw_render && ctx->hw_render.context_reset) {
        s_active = ctx;
        libra_environment_set_ctx(ctx);
        ctx->hw_render.context_reset();
    }
}

void libra_hw_render_context_destroy(libra_ctx_t *ctx)
{
    if (ctx && ctx->has_hw_render && ctx->hw_render.context_destroy) {
        s_active = ctx;
        libra_environment_set_ctx(ctx);
        ctx->hw_render.context_destroy();
    }
}

/* -------------------------------------------------------------------------
 * Netplay (core-controlled multiplayer)
 * ---------------------------------------------------------------------- */

static void ensure_netplay(libra_ctx_t *ctx)
{
    if (!ctx->netplay)
        ctx->netplay = libra_netplay_alloc(ctx);
}

bool libra_has_netpacket(libra_ctx_t *ctx)
{
    return ctx && ctx->has_netpacket;
}

const char *libra_netpacket_core_version(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->has_netpacket)
        return NULL;
    return ctx->netpacket_cb.protocol_version;
}

bool libra_netplay_host(libra_ctx_t *ctx, uint16_t port,
                        libra_net_message_cb_t msg_cb)
{
    if (!ctx) return false;
    ensure_netplay(ctx);
    if (!ctx->netplay) return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return libra_np_host(ctx->netplay, port, msg_cb);
}

bool libra_netplay_join(libra_ctx_t *ctx, const char *host_ip,
                        uint16_t port, libra_net_message_cb_t msg_cb)
{
    if (!ctx) return false;
    ensure_netplay(ctx);
    if (!ctx->netplay) return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return libra_np_join(ctx->netplay, host_ip, port, msg_cb);
}

bool libra_netplay_poll(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return libra_np_poll(ctx->netplay);
}

void libra_netplay_disconnect(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    libra_np_disconnect(ctx->netplay);
}

bool libra_netplay_active(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return false;
    return libra_np_active(ctx->netplay);
}

bool libra_netplay_connected(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return false;
    return libra_np_connected(ctx->netplay);
}

bool libra_netplay_is_host(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return false;
    return libra_np_is_host(ctx->netplay);
}

bool libra_netplay_is_waiting(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return false;
    return libra_np_is_waiting(ctx->netplay);
}

uint16_t libra_netplay_client_id(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return 0;
    return libra_np_client_id(ctx->netplay);
}

unsigned libra_netplay_peer_count(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->netplay) return 0;
    return libra_np_peer_count(ctx->netplay);
}

/* -------------------------------------------------------------------------
 * Rollback netplay (savestate-based)
 * ---------------------------------------------------------------------- */

static void ensure_rollback(libra_ctx_t *ctx)
{
    if (!ctx->rollback)
        ctx->rollback = libra_rollback_alloc(ctx);
}

bool libra_rollback_host(libra_ctx_t *ctx, uint16_t port,
                         libra_net_message_cb_t msg_cb)
{
    if (!ctx || !ctx->core || !ctx->game_loaded) return false;
    if (libra_serialize_size(ctx) == 0) return false;
    ensure_rollback(ctx);
    if (!ctx->rollback) return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return libra_rb_host(ctx->rollback, port, msg_cb);
}

bool libra_rollback_join(libra_ctx_t *ctx, const char *host_ip,
                         uint16_t port, libra_net_message_cb_t msg_cb)
{
    if (!ctx || !ctx->core || !ctx->game_loaded) return false;
    if (libra_serialize_size(ctx) == 0) return false;
    ensure_rollback(ctx);
    if (!ctx->rollback) return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return libra_rb_join(ctx->rollback, host_ip, port, msg_cb);
}

bool libra_rollback_run(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return libra_rb_run(ctx->rollback);
}

void libra_rollback_disconnect(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    libra_rb_disconnect(ctx->rollback);
}

bool libra_rollback_active(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return false;
    return libra_rb_active(ctx->rollback);
}

bool libra_rollback_connected(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return false;
    return libra_rb_connected(ctx->rollback);
}

bool libra_rollback_is_host(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return false;
    return libra_rb_is_host(ctx->rollback);
}

bool libra_rollback_is_replay(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return false;
    return libra_rb_is_replay(ctx->rollback);
}

void libra_rollback_set_input_latency(libra_ctx_t *ctx, unsigned frames)
{
    if (!ctx) return;
    ensure_rollback(ctx);
    if (ctx->rollback)
        libra_rb_set_input_latency(ctx->rollback, frames);
}

unsigned libra_rollback_input_latency(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->rollback) return 0;
    return libra_rb_input_latency(ctx->rollback);
}
