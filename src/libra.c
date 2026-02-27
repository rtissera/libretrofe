// SPDX-License-Identifier: MIT
#include "libra_internal.h"
#include "environment.h"
#include "input.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* -------------------------------------------------------------------------
 * Global libretro callbacks — cores call these; we dispatch to s_ctx.
 * Using globals is standard practice for libretro frontends: only one core
 * can be active at a time per process anyway.
 * ---------------------------------------------------------------------- */

static libra_ctx_t *s_active = NULL;

static void video_refresh_cb(const void *data, unsigned width,
                              unsigned height, size_t pitch)
{
    if (!s_active || !s_active->config.video)
        return;
    if (!data) /* NULL frame = duplicate, honour GET_CAN_DUPE */
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
 * Helpers
 * ---------------------------------------------------------------------- */

static int64_t get_time_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
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
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        free(ctx->opt_keys[i]);
        free(ctx->opt_vals[i]);
        free(ctx->opt_val_list[i]);
    }
    libra_audio_destroy(ctx->audio);
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

    /* If the core needs full path, we just pass it without data.
     * Cores that need the data buffer set need_fullpath = false. */
    if (!ctx->core->sys_info.need_fullpath && path) {
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

    s_active = ctx;
    libra_environment_set_ctx(ctx);
    bool ok = ctx->core->retro_load_game(&game);
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

    ctx->game_loaded = true;
    return true;
}

void libra_unload_game(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return;

    s_active = ctx;
    libra_environment_set_ctx(ctx);
    ctx->core->retro_unload_game();
    ctx->game_loaded = false;

    /* SRAM pointer is no longer valid after unload */
    free(ctx->sram_shadow);
    ctx->sram_shadow      = NULL;
    ctx->sram_shadow_size = 0;

    /* Reset per-game state */
    ctx->frame_time_last       = 0;
    ctx->memory_descriptors    = NULL;
    ctx->memory_descriptor_count = 0;
}

void libra_run(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return;

    s_active = ctx;
    libra_environment_set_ctx(ctx);

    /* Dispatch frame time callback if core registered one */
    if (ctx->frame_time_cb) {
        int64_t now = get_time_usec();
        int64_t delta;
        if (ctx->frame_time_last == 0)
            delta = ctx->frame_time_reference;
        else
            delta = now - ctx->frame_time_last;
        /* During fast-forward pass the ideal reference time */
        if (ctx->fast_forwarding && ctx->frame_time_reference > 0)
            delta = ctx->frame_time_reference;
        ctx->frame_time_last = now;
        ctx->frame_time_cb(delta);
    }

    ctx->core->retro_run();

    /* Flush resampled audio to host */
    if (ctx->audio && ctx->config.audio) {
        libra_audio_flush(ctx->audio,
                          (libra_audio_output_fn)ctx->config.audio,
                          ctx->config.userdata);
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
 * Controller
 * ---------------------------------------------------------------------- */

void libra_set_controller(libra_ctx_t *ctx, unsigned port, unsigned device)
{
    if (!ctx || !ctx->core)
        return;
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

bool libra_is_shutdown_requested(libra_ctx_t *ctx)
{
    return ctx && ctx->shutdown_requested;
}

/* -------------------------------------------------------------------------
 * Rotation
 * ---------------------------------------------------------------------- */

unsigned libra_get_rotation(libra_ctx_t *ctx)
{
    return ctx ? ctx->rotation : 0;
}

/* -------------------------------------------------------------------------
 * Keyboard dispatch
 * ---------------------------------------------------------------------- */

void libra_dispatch_keyboard(libra_ctx_t *ctx, bool down,
                              unsigned keycode, uint32_t character,
                              uint16_t key_modifiers)
{
    if (!ctx)
        return;
    /* Forward to core's keyboard callback if registered */
    if (ctx->keyboard_cb)
        ctx->keyboard_cb(down, keycode, character, key_modifiers);
    /* Also forward to host callback if registered */
    if (ctx->config.keyboard)
        ctx->config.keyboard(ctx->config.userdata, down, keycode,
                             character, key_modifiers);
}

/* -------------------------------------------------------------------------
 * Memory access
 * ---------------------------------------------------------------------- */

void *libra_get_memory_data(libra_ctx_t *ctx, unsigned id)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return NULL;
    if (!ctx->core->retro_get_memory_data)
        return NULL;
    return ctx->core->retro_get_memory_data(id);
}

size_t libra_get_memory_size(libra_ctx_t *ctx, unsigned id)
{
    if (!ctx || !ctx->core || !ctx->game_loaded)
        return 0;
    if (!ctx->core->retro_get_memory_size)
        return 0;
    return ctx->core->retro_get_memory_size(id);
}

/* -------------------------------------------------------------------------
 * Misc queries
 * ---------------------------------------------------------------------- */

bool libra_supports_no_game(libra_ctx_t *ctx)
{
    return ctx && ctx->support_no_game;
}

unsigned libra_get_memory_map(libra_ctx_t *ctx, const void **out)
{
    if (!ctx || !ctx->memory_descriptors || ctx->memory_descriptor_count == 0) {
        if (out) *out = NULL;
        return 0;
    }
    if (out) *out = ctx->memory_descriptors;
    return ctx->memory_descriptor_count;
}

void libra_set_target_refresh_rate(libra_ctx_t *ctx, float rate)
{
    if (ctx)
        ctx->target_refresh_rate = rate;
}

bool libra_update_option_visibility(libra_ctx_t *ctx)
{
    if (!ctx || !ctx->options_update_display_cb)
        return false;
    s_active = ctx;
    libra_environment_set_ctx(ctx);
    return ctx->options_update_display_cb();
}
