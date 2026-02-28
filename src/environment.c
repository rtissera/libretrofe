// SPDX-License-Identifier: MIT
#include "environment.h"
#include "libra_internal.h"
#include "audio.h"
#include "vfs.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

/* File-scoped context pointer set before retro_run() and init calls */
static libra_ctx_t *s_ctx = NULL;

void libra_environment_set_ctx(libra_ctx_t *ctx)
{
    s_ctx = ctx;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Insert key/default only if key not already present.
 * val_list is a "|"-joined string of possible values (may be NULL).
 * Marks the option as visible by default. */
static void insert_option(libra_ctx_t *ctx, const char *key,
                           const char *default_value, const char *val_list)
{
    if (!key)
        return;
    for (unsigned i = 0; i < ctx->opt_count; i++) {
        if (strcmp(ctx->opt_keys[i], key) == 0)
            return; /* already exists — preserve current value */
    }
    if (ctx->opt_count >= LIBRA_MAX_OPTIONS)
        return;

    unsigned idx = ctx->opt_count;
    ctx->opt_keys[idx]     = strdup(key);
    ctx->opt_vals[idx]     = strdup(default_value ? default_value : "");
    ctx->opt_val_list[idx] = val_list ? strdup(val_list) : NULL;
    ctx->opt_visible[idx]  = true;
    ctx->opt_count++;
}

/* Build a "|"-joined value list from a retro_core_option_value array
 * (v1/v2 format).  Returns a malloc'd string; caller must free. */
static char *build_val_list(const struct retro_core_option_value *values)
{
    char buf[2048];
    size_t pos = 0;
    for (int i = 0;
         i < RETRO_NUM_CORE_OPTION_VALUES_MAX && values[i].value != NULL;
         i++) {
        if (i > 0 && pos < sizeof(buf) - 1)
            buf[pos++] = '|';
        const char *v = values[i].value;
        size_t len = strlen(v);
        if (pos + len >= sizeof(buf))
            break;
        memcpy(buf + pos, v, len);
        pos += len;
    }
    buf[pos] = '\0';
    return pos > 0 ? strdup(buf) : NULL;
}

/* Log callback forwarded to stderr */
static void log_printf(enum retro_log_level level, const char *fmt, ...)
{
    const char *prefix;
    va_list args;

    switch (level) {
        case RETRO_LOG_DEBUG: prefix = "[DEBUG]"; break;
        case RETRO_LOG_INFO:  prefix = "[INFO] "; break;
        case RETRO_LOG_WARN:  prefix = "[WARN] "; break;
        case RETRO_LOG_ERROR: prefix = "[ERROR]"; break;
        default:              prefix = "[?]    "; break;
    }

    fprintf(stderr, "libra %s ", prefix);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

/* Rumble callback — forwards to host */
static bool rumble_set_state(unsigned port,
                              enum retro_rumble_effect effect,
                              uint16_t strength)
{
    if (s_ctx && s_ctx->config.rumble)
        s_ctx->config.rumble(s_ctx->config.userdata,
                             port, (unsigned)effect, strength);
    return true;
}

/* -------------------------------------------------------------------------
 * Performance interface stubs
 * ---------------------------------------------------------------------- */

static retro_time_t perf_get_time_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (retro_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static retro_perf_tick_t perf_get_counter(void)
{
    return (retro_perf_tick_t)perf_get_time_usec();
}

static uint64_t perf_get_cpu_features(void) { return 0; }
static void perf_register(struct retro_perf_counter *counter)
{
    if (counter) counter->registered = true;
}
static void perf_start(struct retro_perf_counter *counter)
{
    if (counter) {
        counter->start    = perf_get_counter();
        counter->call_cnt++;
    }
}
static void perf_stop(struct retro_perf_counter *counter)
{
    if (counter)
        counter->total += perf_get_counter() - counter->start;
}
static void perf_log(void) { /* no-op */ }

/* -------------------------------------------------------------------------
 * Main environment callback
 * ---------------------------------------------------------------------- */

bool libra_environment_cb(unsigned cmd, void *data)
{
    libra_ctx_t *ctx = s_ctx;
    if (!ctx)
        return false;

    switch (cmd) {

        /* ---- Core status ------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            if (data) *(bool *)data = true;
            return true;

        case RETRO_ENVIRONMENT_SHUTDOWN:
            ctx->shutdown_requested = true;
            return true;

        case RETRO_ENVIRONMENT_GET_OVERSCAN:
            if (data) *(bool *)data = false; /* crop overscan */
            return true;

        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            if (data) ctx->support_no_game = *(const bool *)data;
            return true;

        /* ---- Video -------------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_ROTATION:
            if (data) ctx->rotation = *(const unsigned *)data & 3;
            return true;

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            ctx->pixel_format = *(const enum retro_pixel_format *)data;
            return true;

        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            if (ctx->core) {
                const struct retro_game_geometry *g =
                    (const struct retro_game_geometry *)data;
                ctx->core->av_info.geometry = *g;
            }
            return true;

        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            if (ctx->core) {
                const struct retro_system_av_info *av =
                    (const struct retro_system_av_info *)data;
                ctx->core->av_info = *av;
                if (ctx->audio)
                    libra_audio_set_src_rate(ctx->audio, av->timing.sample_rate);
            }
            return true;

        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            if (data) *(int *)data = 3;
            return true;

        /* ---- Directories / identity -------------------------------------- */

        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = ctx->system_dir;
            return true;

        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = ctx->save_dir;
            return true;

        case RETRO_ENVIRONMENT_GET_LIBRETRO_PATH:
            *(const char **)data = ctx->core_path;
            return true;

        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *(const char **)data = ctx->assets_dir;
            return true;

        case RETRO_ENVIRONMENT_GET_USERNAME:
            *(const char **)data = NULL;
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            if (data) *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
            return true;

        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            if (data) *(bool *)data = ctx->fast_forwarding;
            return true;

        /* ---- Logging / messages ------------------------------------------ */

        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback *cb = (struct retro_log_callback *)data;
            cb->log = log_printf;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const struct retro_message *msg = (const struct retro_message *)data;
            fprintf(stderr, "libra [MSG] %s\n", msg->msg);
            snprintf(ctx->message_buf, sizeof(ctx->message_buf), "%s", msg->msg);
            ctx->message_frames = msg->frames;
            ctx->message_pending = true;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            const struct retro_message_ext *msg =
                (const struct retro_message_ext *)data;
            const char *lvl;
            switch (msg->level) {
                case RETRO_LOG_DEBUG: lvl = "[DEBUG]"; break;
                case RETRO_LOG_WARN:  lvl = "[WARN] "; break;
                case RETRO_LOG_ERROR: lvl = "[ERROR]"; break;
                default:              lvl = "[INFO] "; break;
            }
            fprintf(stderr, "libra %s %s\n", lvl, msg->msg);
            /* Store for host OSD (skip pure-log messages) */
            if (msg->target != RETRO_MESSAGE_TARGET_LOG) {
                snprintf(ctx->message_buf, sizeof(ctx->message_buf), "%s", msg->msg);
                ctx->message_frames = msg->duration;
                ctx->message_pending = true;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            /* data may be NULL — return value alone signals support */
            if (data) *(bool *)data = true;
            return true;

        /* ---- Input -------------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
            return true;

        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
            return true;

        /* ---- Rumble ------------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE: {
            struct retro_rumble_interface *ri =
                (struct retro_rumble_interface *)data;
            ri->set_rumble_state = rumble_set_state;
            return true;
        }

        /* ---- VFS ---------------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE: {
            struct retro_vfs_interface_info *info =
                (struct retro_vfs_interface_info *)data;
            if (info->required_interface_version > LIBRA_VFS_VERSION)
                return false;
            info->required_interface_version = LIBRA_VFS_VERSION;
            info->iface = libra_vfs_get_interface();
            return true;
        }

        /* ---- Keyboard callback -------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
            const struct retro_keyboard_callback *cb =
                (const struct retro_keyboard_callback *)data;
            ctx->keyboard_cb = cb ? cb->callback : NULL;
            return true;
        }

        /* ---- Frame time --------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
            const struct retro_frame_time_callback *cb =
                (const struct retro_frame_time_callback *)data;
            if (cb) {
                ctx->frame_time_cb        = cb->callback;
                ctx->frame_time_reference = cb->reference;
            } else {
                ctx->frame_time_cb        = NULL;
                ctx->frame_time_reference = 0;
            }
            return true;
        }

        /* ---- Input queries ------------------------------------------------ */

        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
            if (data)
                *(uint64_t *)data = (1 << RETRO_DEVICE_JOYPAD)
                                  | (1 << RETRO_DEVICE_ANALOG)
                                  | (1 << RETRO_DEVICE_KEYBOARD);
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
            if (data) *(unsigned *)data = LIBRA_MAX_PORTS;
            return true;

        /* ---- Performance interface ---------------------------------------- */

        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
            struct retro_perf_callback *cb =
                (struct retro_perf_callback *)data;
            cb->get_time_usec  = perf_get_time_usec;
            cb->get_cpu_features = perf_get_cpu_features;
            cb->get_perf_counter = perf_get_counter;
            cb->perf_register    = perf_register;
            cb->perf_start       = perf_start;
            cb->perf_stop        = perf_stop;
            cb->perf_log         = perf_log;
            return true;
        }

        /* ---- Misc queries ------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            if (data) *(unsigned *)data = 1;
            return true;

        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
            if (data) ctx->serialization_quirks = *(const uint64_t *)data;
            return true;

        case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
            if (data) *(int *)data = RETRO_SAVESTATE_CONTEXT_NORMAL;
            return true;

        case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
            if (data) *(bool *)data = true;
            return true;

        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            if (data)
                *(float *)data = ctx->target_refresh_rate > 0.0f
                    ? ctx->target_refresh_rate : 60.0f;
            return true;

        /* ---- Memory maps -------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            const struct retro_memory_map *map =
                (const struct retro_memory_map *)data;
            if (map) {
                ctx->memory_descriptors      = map->descriptors;
                ctx->memory_descriptor_count  = map->num_descriptors;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
            return true;

        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;

        /* ---- LED interface (stub) ----------------------------------------- */

        case RETRO_ENVIRONMENT_GET_LED_INTERFACE: {
            /* Accept the interface but use a no-op callback.
             * Cores may refuse to load if we return false. */
            struct retro_led_interface *led =
                (struct retro_led_interface *)data;
            if (led) led->set_led_state = NULL; /* no LED support */
            return true;
        }

        /* ---- Core options ------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            if (data) *(unsigned *)data = 2;
            return true;

        case RETRO_ENVIRONMENT_SET_VARIABLES: {
            /* Legacy format: "description; val1|val2" — extract first value as
             * default and the whole "val1|val2|..." portion as val_list. */
            const struct retro_variable *vars =
                (const struct retro_variable *)data;
            if (!vars)
                return true;
            for (; vars->key != NULL; vars++) {
                const char *def      = "";
                const char *val_list = NULL;
                const char *semi = vars->value ? strchr(vars->value, ';') : NULL;
                if (semi) {
                    const char *vals = semi + 1;
                    while (*vals == ' ') vals++;
                    val_list = vals; /* "|"-joined list already */
                    const char *pipe = strchr(vals, '|');
                    char tmp[256] = "";
                    if (pipe) {
                        size_t len = (size_t)(pipe - vals);
                        if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
                        memcpy(tmp, vals, len);
                        tmp[len] = '\0';
                        def = tmp;
                    } else {
                        def = vals;
                    }
                }
                insert_option(ctx, vars->key, def, val_list);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            const struct retro_core_option_definition *defs =
                (const struct retro_core_option_definition *)data;
            if (!defs) return true;
            for (; defs->key != NULL; defs++) {
                char *vl = build_val_list(defs->values);
                insert_option(ctx, defs->key, defs->default_value, vl);
                free(vl);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
            const struct retro_core_options_intl *intl =
                (const struct retro_core_options_intl *)data;
            if (!intl || !intl->us) return true;
            for (const struct retro_core_option_definition *d = intl->us;
                 d->key != NULL; d++) {
                char *vl = build_val_list(d->values);
                insert_option(ctx, d->key, d->default_value, vl);
                free(vl);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
            const struct retro_core_options_v2 *opts =
                (const struct retro_core_options_v2 *)data;
            if (!opts || !opts->definitions) return true;
            for (const struct retro_core_option_v2_definition *d = opts->definitions;
                 d->key != NULL; d++) {
                char *vl = build_val_list(d->values);
                insert_option(ctx, d->key, d->default_value, vl);
                free(vl);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const struct retro_core_options_v2_intl *intl =
                (const struct retro_core_options_v2_intl *)data;
            if (!intl || !intl->us || !intl->us->definitions) return true;
            for (const struct retro_core_option_v2_definition *d =
                     intl->us->definitions; d->key != NULL; d++) {
                char *vl = build_val_list(d->values);
                insert_option(ctx, d->key, d->default_value, vl);
                free(vl);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            struct retro_variable *var = (struct retro_variable *)data;
            if (!var->key) { var->value = NULL; return false; }
            var->value = NULL;
            for (unsigned i = 0; i < ctx->opt_count; i++) {
                if (strcmp(ctx->opt_keys[i], var->key) == 0) {
                    var->value = ctx->opt_vals[i];
                    return true;
                }
            }
            return false;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            if (data) {
                *(bool *)data = ctx->opt_updated;
                ctx->opt_updated = false;
            }
            return true;

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY: {
            const struct retro_core_option_display *d =
                (const struct retro_core_option_display *)data;
            if (!d || !d->key)
                return false;
            for (unsigned i = 0; i < ctx->opt_count; i++) {
                if (strcmp(ctx->opt_keys[i], d->key) == 0) {
                    ctx->opt_visible[i] = d->visible;
                    return true;
                }
            }
            return false;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
            const struct retro_core_options_update_display_callback *cb =
                (const struct retro_core_options_update_display_callback *)data;
            ctx->options_update_display_cb = cb ? cb->callback : NULL;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_VARIABLE: {
            const struct retro_variable *var =
                (const struct retro_variable *)data;
            if (!var) return true; /* capability probe */
            if (!var->key || !var->value) return false;
            for (unsigned i = 0; i < ctx->opt_count; i++) {
                if (strcmp(ctx->opt_keys[i], var->key) == 0) {
                    if (strcmp(ctx->opt_vals[i], var->value) != 0) {
                        free(ctx->opt_vals[i]);
                        ctx->opt_vals[i] = strdup(var->value);
                        ctx->opt_updated = true;
                    }
                    return true;
                }
            }
            return false; /* key not found */
        }

        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
            if (data) *(unsigned *)data = 1;
            return true;

        /* ---- Disk control ------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
            const struct retro_disk_control_callback *cb =
                (const struct retro_disk_control_callback *)data;
            if (cb) { ctx->disk_cb = *cb; ctx->has_disk_cb = true; }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: {
            const struct retro_disk_control_ext_callback *cb =
                (const struct retro_disk_control_ext_callback *)data;
            if (cb) {
                ctx->disk_ext_cb     = *cb;
                ctx->has_disk_ext_cb = true;
                /* Mirror the 7 shared fields into the basic struct */
                ctx->disk_cb.set_eject_state     = cb->set_eject_state;
                ctx->disk_cb.get_eject_state     = cb->get_eject_state;
                ctx->disk_cb.get_image_index     = cb->get_image_index;
                ctx->disk_cb.set_image_index     = cb->set_image_index;
                ctx->disk_cb.get_num_images      = cb->get_num_images;
                ctx->disk_cb.replace_image_index = cb->replace_image_index;
                ctx->disk_cb.add_image_index     = cb->add_image_index;
                ctx->has_disk_cb = true;
            }
            return true;
        }

        /* ---- Subsystem ---------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO: {
            const struct retro_subsystem_info *info =
                (const struct retro_subsystem_info *)data;
            ctx->subsystem_info = info;
            ctx->subsystem_count = 0;
            if (info) {
                while (info[ctx->subsystem_count].ident != NULL)
                    ctx->subsystem_count++;
            }
            return true;
        }

        /* ---- Audio buffer / latency -------------------------------------- */

        case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
            const struct retro_audio_buffer_status_callback *cb =
                (const struct retro_audio_buffer_status_callback *)data;
            ctx->audio_buffer_status_cb = cb ? cb->callback : NULL;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
            if (data) ctx->min_audio_latency = *(const unsigned *)data;
            return true;

        /* ---- Fastforwarding override ------------------------------------- */

        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE: {
            const struct retro_fastforwarding_override *ffo =
                (const struct retro_fastforwarding_override *)data;
            if (ffo) {
                ctx->ff_override_active       = true;
                ctx->ff_override_ratio        = ffo->ratio;
                ctx->ff_override_fastforward  = ffo->fastforward;
                ctx->ff_override_notification = ffo->notification;
                ctx->ff_override_inhibit      = ffo->inhibit_toggle;
                ctx->fast_forwarding          = ffo->fastforward;
            }
            return true;
        }

        /* ---- Content info override --------------------------------------- */

        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
            ctx->content_info_override =
                (const struct retro_system_content_info_override *)data;
            return true;

        /* ---- Extended game info ------------------------------------------ */

        case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
            if (data && ctx->game_full_path) {
                *(const struct retro_game_info_ext **)data = &ctx->game_info_ext;
                return true;
            }
            return false;

        /* ---- Throttle state ---------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
            struct retro_throttle_state *ts =
                (struct retro_throttle_state *)data;
            if (ts) {
                ts->mode = ctx->throttle_mode;
                ts->rate = ctx->throttle_rate;
            }
            return true;
        }

        /* ---- Device power ------------------------------------------------ */

        case RETRO_ENVIRONMENT_GET_DEVICE_POWER: {
            struct retro_device_power *pw =
                (struct retro_device_power *)data;
            if (pw) {
                pw->state   = RETRO_POWERSTATE_PLUGGED_IN;
                pw->seconds = RETRO_POWERSTATE_NO_ESTIMATE;
                pw->percent = 100;
            }
            return true;
        }

        /* ---- Software framebuffer ---------------------------------------- */

        case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: {
            struct retro_framebuffer *fb =
                (struct retro_framebuffer *)data;
            if (!fb || fb->width == 0 || fb->height == 0)
                return false;

            /* Determine bytes per pixel from current pixel format */
            size_t bpp = (ctx->pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
            size_t pitch = fb->width * bpp;
            size_t needed = pitch * fb->height;

            if (needed > ctx->sw_framebuffer_size) {
                free(ctx->sw_framebuffer);
                ctx->sw_framebuffer = malloc(needed);
                if (!ctx->sw_framebuffer) {
                    ctx->sw_framebuffer_size = 0;
                    return false;
                }
                ctx->sw_framebuffer_size = needed;
            }

            fb->data         = ctx->sw_framebuffer;
            fb->pitch        = pitch;
            fb->format       = (enum retro_pixel_format)ctx->pixel_format;
            fb->memory_flags = 0;
            return true;
        }

        /* ---- Target sample rate ------------------------------------------ */

        case RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE:
            if (data)
                *(unsigned *)data = ctx->config.audio_output_rate
                    ? ctx->config.audio_output_rate : 48000;
            return true;

        /* ---- Identity / locale ------------------------------------------- */

        default:
            return false;
    }
}
