// SPDX-License-Identifier: MIT
#include "environment.h"
#include "libra_internal.h"
#include "audio.h"
#include "vfs.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>

/* Thread-local context pointer set before retro_run() and init calls */
static libra_ctx_t *s_ctx = NULL;

void libra_environment_set_ctx(libra_ctx_t *ctx)
{
    s_ctx = ctx;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ---------------------------------------------------------------------- */

/* Resolve a category key to its index in opt_cat_keys[], inserting if needed.
 * Returns -1 if category_key is NULL. */
static int resolve_category(libra_ctx_t *ctx, const char *category_key,
                             const char *category_desc)
{
    if (!category_key || !*category_key)
        return -1;
    for (unsigned i = 0; i < ctx->opt_cat_count; i++) {
        if (strcmp(ctx->opt_cat_keys[i], category_key) == 0)
            return (int)i;
    }
    if (ctx->opt_cat_count >= LIBRA_MAX_CATEGORIES)
        return -1;
    unsigned idx = ctx->opt_cat_count;
    ctx->opt_cat_keys[idx]  = strdup(category_key);
    ctx->opt_cat_descs[idx] = category_desc ? strdup(category_desc) : strdup(category_key);
    ctx->opt_cat_count++;
    return (int)idx;
}

/* Insert key/default only if key not already present.
 * val_list is a "|"-joined string of possible values (may be NULL).
 * category_key / desc / opt_desc may be NULL.
 * Marks the option as visible by default. */
static void insert_option(libra_ctx_t *ctx, const char *key,
                           const char *default_value, const char *val_list,
                           const char *category_key, const char *opt_desc)
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
    ctx->opt_cat_idx[idx]  = resolve_category(ctx, category_key, NULL);
    ctx->opt_descs[idx]    = opt_desc ? strdup(opt_desc) : NULL;
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

/* Hardware rendering trampolines */
static uintptr_t hw_get_current_framebuffer(void)
{
    return s_ctx ? (uintptr_t)s_ctx->hw_fbo : 0;
}

static retro_proc_address_t hw_get_proc_address_trampoline(const char *sym)
{
    if (s_ctx && s_ctx->hw_get_proc_address)
        return s_ctx->hw_get_proc_address(sym);
    return NULL;
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

/* LED callback — forwards to host */
static void led_set_state(int led, int state)
{
    if (s_ctx && s_ctx->config.led)
        s_ctx->config.led(s_ctx->config.userdata, led, state);
}

/* Sensor callbacks — forward to host */
static bool sensor_set_state(unsigned port,
                              enum retro_sensor_action action,
                              unsigned rate)
{
    if (s_ctx && s_ctx->config.sensor_set_state)
        return s_ctx->config.sensor_set_state(s_ctx->config.userdata,
                                               port, (unsigned)action, rate);
    return false;
}

static float sensor_get_input(unsigned port, unsigned id)
{
    if (s_ctx && s_ctx->config.sensor_get_input)
        return s_ctx->config.sensor_get_input(s_ctx->config.userdata, port, id);
    return 0.0f;
}

/* Microphone interface — opaque handle wraps host's mic pointer */
struct retro_microphone {
    void    *host_mic;     /* handle returned by host mic_open callback */
    unsigned rate;
    bool     active;
};

static retro_microphone_t *mic_open(const retro_microphone_params_t *params)
{
    if (!s_ctx || !s_ctx->config.mic_open)
        return NULL;
    unsigned rate = (params && params->rate) ? params->rate : 44100;
    void *host = s_ctx->config.mic_open(s_ctx->config.userdata, rate);
    if (!host)
        return NULL;
    retro_microphone_t *mic = (retro_microphone_t *)calloc(1, sizeof(*mic));
    if (!mic) return NULL;
    mic->host_mic = host;
    mic->rate     = rate;
    mic->active   = false;
    return mic;
}

static void mic_close(retro_microphone_t *microphone)
{
    if (!microphone) return;
    if (s_ctx && s_ctx->config.mic_close)
        s_ctx->config.mic_close(s_ctx->config.userdata, microphone->host_mic);
    free(microphone);
}

static bool mic_get_params(const retro_microphone_t *microphone,
                            retro_microphone_params_t *params)
{
    if (!microphone || !params) return false;
    params->rate = microphone->rate;
    return true;
}

static bool mic_set_state(retro_microphone_t *microphone, bool state)
{
    if (!microphone) return false;
    if (s_ctx && s_ctx->config.mic_set_state) {
        bool ok = s_ctx->config.mic_set_state(s_ctx->config.userdata,
                                               microphone->host_mic, state);
        if (ok) microphone->active = state;
        return ok;
    }
    return false;
}

static bool mic_get_state(const retro_microphone_t *microphone)
{
    if (!microphone) return false;
    if (s_ctx && s_ctx->config.mic_get_state)
        return s_ctx->config.mic_get_state(s_ctx->config.userdata,
                                            microphone->host_mic);
    return microphone->active;
}

static int mic_read(retro_microphone_t *microphone,
                     int16_t *samples, size_t num_samples)
{
    if (!microphone || !samples || num_samples == 0) return -1;
    if (s_ctx && s_ctx->config.mic_read)
        return s_ctx->config.mic_read(s_ctx->config.userdata,
                                       microphone->host_mic,
                                       samples, num_samples);
    return -1;
}

/* -------------------------------------------------------------------------
 * Linux battery status via /sys/class/power_supply
 * ---------------------------------------------------------------------- */

static bool read_sysfs_line(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = (fgets(buf, (int)bufsz, f) != NULL);
    fclose(f);
    if (ok) { char *nl = strchr(buf, '\n'); if (nl) *nl = '\0'; }
    return ok;
}

static bool read_device_power(struct retro_device_power *power)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) {
        /* No power_supply sysfs — assume plugged-in desktop */
        power->state   = RETRO_POWERSTATE_PLUGGED_IN;
        power->seconds = RETRO_POWERSTATE_NO_ESTIMATE;
        power->percent = 100;
        return true;
    }

    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[512], val[64];
        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/type", ent->d_name);
        if (!read_sysfs_line(path, val, sizeof(val))) continue;
        if (strcmp(val, "Battery") != 0) continue;

        found = true;

        /* capacity → percent */
        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/capacity", ent->d_name);
        power->percent = read_sysfs_line(path, val, sizeof(val))
                         ? (int8_t)atoi(val) : -1;

        /* status → state */
        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/status", ent->d_name);
        if (read_sysfs_line(path, val, sizeof(val))) {
            if      (strcmp(val, "Discharging")  == 0)
                power->state = RETRO_POWERSTATE_DISCHARGING;
            else if (strcmp(val, "Charging")     == 0)
                power->state = RETRO_POWERSTATE_CHARGING;
            else if (strcmp(val, "Full")         == 0)
                power->state = RETRO_POWERSTATE_CHARGED;
            else
                power->state = RETRO_POWERSTATE_PLUGGED_IN;
        } else {
            power->state = RETRO_POWERSTATE_PLUGGED_IN;
        }

        /* time_to_empty_now → seconds (only when discharging) */
        if (power->state == RETRO_POWERSTATE_DISCHARGING) {
            snprintf(path, sizeof(path),
                     "/sys/class/power_supply/%s/time_to_empty_now",
                     ent->d_name);
            power->seconds = read_sysfs_line(path, val, sizeof(val))
                             ? atoi(val) : RETRO_POWERSTATE_NO_ESTIMATE;
        } else {
            power->seconds = RETRO_POWERSTATE_NO_ESTIMATE;
        }
        break; /* use first battery */
    }
    closedir(d);

    if (!found) {
        /* No battery → plugged-in desktop */
        power->state   = RETRO_POWERSTATE_PLUGGED_IN;
        power->seconds = RETRO_POWERSTATE_NO_ESTIMATE;
        power->percent = 100;
    }
    return true;
}

/* -------------------------------------------------------------------------
 * Performance counter stubs
 * ---------------------------------------------------------------------- */

static retro_time_t RETRO_CALLCONV perf_get_time_usec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (retro_time_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static retro_perf_tick_t RETRO_CALLCONV perf_get_counter(void)
{
    return (retro_perf_tick_t)perf_get_time_usec();
}

static uint64_t RETRO_CALLCONV perf_get_cpu_features(void)
{
    uint64_t flags = 0;

#if defined(__x86_64__) || defined(__i386__) || defined(_M_IX86) || defined(_M_X64)
    /* CPUID-based detection for x86/x86_64 */
    unsigned int eax, ebx, ecx, edx;

    /* leaf 1: basic features */
    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(1));

    if (edx & (1 << 23)) flags |= RETRO_SIMD_MMX;
    if (edx & (1 << 25)) flags |= RETRO_SIMD_SSE;
    if (edx & (1 << 26)) flags |= RETRO_SIMD_SSE2;
    if (edx & (1 << 15)) flags |= RETRO_SIMD_CMOV;
    if (ecx & (1 <<  0)) flags |= RETRO_SIMD_SSE3;
    if (ecx & (1 <<  9)) flags |= RETRO_SIMD_SSSE3;
    if (ecx & (1 << 19)) flags |= RETRO_SIMD_SSE4;
    if (ecx & (1 << 20)) flags |= RETRO_SIMD_SSE42;
    if (ecx & (1 << 25)) flags |= RETRO_SIMD_AES;
    if (ecx & (1 << 28)) flags |= RETRO_SIMD_AVX;
    if (ecx & (1 << 23)) flags |= RETRO_SIMD_POPCNT;
    if (ecx & (1 << 22)) flags |= RETRO_SIMD_MOVBE;

    /* leaf 1 extended: MMXEXT (AMD) */
    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(0x80000001));
    if (edx & (1 << 22)) flags |= RETRO_SIMD_MMXEXT;

    /* leaf 7: extended features */
    __asm__ __volatile__("cpuid"
        : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
        : "a"(7), "c"(0));
    if (ebx & (1 << 5)) flags |= RETRO_SIMD_AVX2;

#elif defined(__aarch64__) || defined(_M_ARM64)
    /* AArch64 always has NEON/ASIMD */
    flags |= RETRO_SIMD_NEON | RETRO_SIMD_ASIMD;

#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    flags |= RETRO_SIMD_NEON;
#  if defined(__ARM_FEATURE_FMA) || __ARM_ARCH >= 7
    flags |= RETRO_SIMD_VFPV3;
#  endif
#  if defined(__ARM_FEATURE_FMA)
    flags |= RETRO_SIMD_VFPV4;
#  endif
#endif

    return flags;
}
static void RETRO_CALLCONV perf_log(void) {}
static void RETRO_CALLCONV perf_register(struct retro_perf_counter *c)
{
    if (c) c->registered = true;
}
static void RETRO_CALLCONV perf_start(struct retro_perf_counter *c)
{
    if (c) c->start = perf_get_counter();
}
static void RETRO_CALLCONV perf_stop(struct retro_perf_counter *c)
{
    if (c) c->total += perf_get_counter() - c->start;
}

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

        case RETRO_ENVIRONMENT_SET_ROTATION:
            if (data) ctx->rotation = *(const unsigned *)data & 3;
            return true;

        /* ---- Video -------------------------------------------------------- */

        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            ctx->pixel_format = *(const enum retro_pixel_format *)data;
            return true;

        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            struct retro_hw_render_callback *cb =
                (struct retro_hw_render_callback *)data;
            if (!cb) return false;

            /* Only accept GL/GLES context types */
            switch (cb->context_type) {
            case RETRO_HW_CONTEXT_OPENGL:
            case RETRO_HW_CONTEXT_OPENGLES2:
            case RETRO_HW_CONTEXT_OPENGL_CORE:
            case RETRO_HW_CONTEXT_OPENGLES3:
            case RETRO_HW_CONTEXT_OPENGLES_VERSION:
                break;
            default:
                return false; /* Vulkan/D3D not supported */
            }

            /* Validate against host-registered device capabilities */
            if (ctx->hw_caps_set) {
                unsigned ct = (unsigned)cb->context_type;
                if (ct >= 8 || !ctx->hw_caps[ct].supported)
                    return false;

                /* Determine the minimum version this request requires */
                unsigned req_maj = cb->version_major;
                unsigned req_min = cb->version_minor;
                if (ct == RETRO_HW_CONTEXT_OPENGLES2) {
                    req_maj = 2; req_min = 0;
                } else if (ct == RETRO_HW_CONTEXT_OPENGLES3) {
                    req_maj = 3; req_min = 0;
                } else if (ct == RETRO_HW_CONTEXT_OPENGL && req_maj == 0) {
                    req_maj = 2; req_min = 0; /* legacy GL, no explicit version */
                }

                if (req_maj > ctx->hw_caps[ct].max_major ||
                    (req_maj == ctx->hw_caps[ct].max_major &&
                     req_min > ctx->hw_caps[ct].max_minor))
                    return false;
            }

            ctx->hw_render     = *cb;
            ctx->has_hw_render = true;

            /* Frontend fills in these two function pointers */
            cb->get_current_framebuffer = hw_get_current_framebuffer;
            cb->get_proc_address        = hw_get_proc_address_trampoline;
            return true;
        }

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
            /* bit 0 = video, bit 1 = audio, bit 2 = fast-forward hint */
            if (data) {
                if (ctx->fast_forwarding)
                    *(int *)data = (1 << 1) | (1 << 2); /* audio + ff hint, skip video */
                else
                    *(int *)data = (1 << 0) | (1 << 1); /* video + audio */
            }
            return true;

        case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER: {
            struct retro_framebuffer *fb = (struct retro_framebuffer *)data;
            if (!fb) return false;
            unsigned bpp = (ctx->pixel_format == RETRO_PIXEL_FORMAT_XRGB8888)
                           ? 4 : 2;
            size_t pitch  = (size_t)fb->width * bpp;
            size_t needed = pitch * fb->height;
            if (needed > ctx->sw_fb_size) {
                free(ctx->sw_fb);
                ctx->sw_fb = malloc(needed);
                if (!ctx->sw_fb) { ctx->sw_fb_size = 0; return false; }
                ctx->sw_fb_size = needed;
            }
            fb->data         = ctx->sw_fb;
            fb->pitch        = pitch;
            fb->format       = (enum retro_pixel_format)ctx->pixel_format;
            fb->memory_flags = 0;
            return true;
        }

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
            *(const char **)data = ctx->username;
            return true;

        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            if (data) *(unsigned *)data = ctx->language;
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
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
            const struct retro_message_ext *msg =
                (const struct retro_message_ext *)data;
            /* Route to log or OSD — we just forward to stderr */
            const char *lvl;
            switch (msg->level) {
                case RETRO_LOG_DEBUG: lvl = "[DEBUG]"; break;
                case RETRO_LOG_WARN:  lvl = "[WARN] "; break;
                case RETRO_LOG_ERROR: lvl = "[ERROR]"; break;
                default:              lvl = "[INFO] "; break;
            }
            fprintf(stderr, "libra %s %s\n", lvl, msg->msg);
            return true;
        }

        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            /* data may be NULL — return value alone signals support */
            if (data) *(bool *)data = true;
            return true;

        /* ---- Input / timing ------------------------------------------------ */

        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
            const struct retro_input_descriptor *desc =
                (const struct retro_input_descriptor *)data;
            /* Free previous descriptors */
            if (ctx->input_descriptors) {
                for (unsigned i = 0; i < ctx->input_descriptor_count; i++)
                    free((void *)ctx->input_descriptors[i].description);
                free(ctx->input_descriptors);
                ctx->input_descriptors = NULL;
                ctx->input_descriptor_count = 0;
            }
            if (!desc) return true;
            /* Count entries (terminated by NULL description) */
            unsigned count = 0;
            while (desc[count].description) count++;
            if (count == 0) return true;
            ctx->input_descriptors = calloc(count, sizeof(*ctx->input_descriptors));
            if (!ctx->input_descriptors) return true;
            ctx->input_descriptor_count = count;
            for (unsigned i = 0; i < count; i++) {
                ctx->input_descriptors[i].port   = desc[i].port;
                ctx->input_descriptors[i].device  = desc[i].device;
                ctx->input_descriptors[i].index   = desc[i].index;
                ctx->input_descriptors[i].id      = desc[i].id;
                ctx->input_descriptors[i].description = strdup(desc[i].description);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
            const struct retro_controller_info *info =
                (const struct retro_controller_info *)data;
            if (info) {
                unsigned p = 0;
                while (info[p].types && p < 16) {
                    /* Default each port to JOYPAD if not already set */
                    if (ctx->port_devices[p] == 0)
                        ctx->port_devices[p] = RETRO_DEVICE_JOYPAD;
                    p++;
                }
                if (p > ctx->port_count)
                    ctx->port_count = p;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK: {
            const struct retro_keyboard_callback *kb =
                (const struct retro_keyboard_callback *)data;
            ctx->keyboard_cb = kb ? kb->callback : NULL;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK: {
            const struct retro_audio_callback *cb =
                (const struct retro_audio_callback *)data;
            if (cb) {
                ctx->audio_cb     = *cb;
                ctx->has_audio_cb = true;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK: {
            const struct retro_frame_time_callback *ft =
                (const struct retro_frame_time_callback *)data;
            if (ft) {
                ctx->frame_time_cb   = *ft;
                ctx->has_frame_time_cb = true;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE: {
            struct retro_perf_callback *cb =
                (struct retro_perf_callback *)data;
            cb->get_time_usec  = perf_get_time_usec;
            cb->get_cpu_features = perf_get_cpu_features;
            cb->get_perf_counter = perf_get_counter;
            cb->perf_register  = perf_register;
            cb->perf_start     = perf_start;
            cb->perf_stop      = perf_stop;
            cb->perf_log       = perf_log;
            return true;
        }

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

        /* ---- Misc no-ops / simple queries --------------------------------- */

        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS: {
            const struct retro_memory_map *map = (const struct retro_memory_map *)data;
            if (!map || !map->descriptors || map->num_descriptors == 0)
                return true;
            /* Free previous descriptors */
            free(ctx->mem_descriptors);
            ctx->mem_descriptors = NULL;
            ctx->mem_descriptor_count = 0;
            /* Deep copy (ptr values are host pointers owned by core — remain valid) */
            size_t sz = map->num_descriptors * sizeof(struct retro_memory_descriptor);
            ctx->mem_descriptors = (struct retro_memory_descriptor *)malloc(sz);
            if (ctx->mem_descriptors) {
                memcpy(ctx->mem_descriptors, map->descriptors, sz);
                ctx->mem_descriptor_count = map->num_descriptors;
                /* addrspace strings point into core-owned data — strdup them */
                for (unsigned i = 0; i < ctx->mem_descriptor_count; i++) {
                    if (ctx->mem_descriptors[i].addrspace)
                        ctx->mem_descriptors[i].addrspace =
                            strdup(ctx->mem_descriptors[i].addrspace);
                }
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
            if (data) ctx->supports_achievements = *(const bool *)data;
            return true;

        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            if (data) ctx->performance_level = *(const unsigned *)data;
            return true;

        case RETRO_ENVIRONMENT_GET_OVERSCAN:
            if (data) *(bool *)data = false;
            return true;

        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            if (data) ctx->support_no_game = *(const bool *)data;
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_DEVICE_CAPABILITIES:
            if (data) *(uint64_t *)data = (1 << RETRO_DEVICE_JOYPAD)
                                        | (1 << RETRO_DEVICE_MOUSE)
                                        | (1 << RETRO_DEVICE_KEYBOARD)
                                        | (1 << RETRO_DEVICE_LIGHTGUN)
                                        | (1 << RETRO_DEVICE_ANALOG)
                                        | (1 << RETRO_DEVICE_POINTER);
            return true;

        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
            if (data) *(unsigned *)data = 4;
            return true;

        case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION:
            if (data) *(unsigned *)data = 1;
            return true;

        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            if (data) *(unsigned *)data = 1;
            return true;

        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: {
            /* Read quirk flags from core, clear unsupported bits, write back */
            uint64_t *quirks = (uint64_t *)data;
            if (!quirks) return true;
            const uint64_t supported =
                RETRO_SERIALIZATION_QUIRK_INCOMPLETE |
                RETRO_SERIALIZATION_QUIRK_MUST_INITIALIZE |
                RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE |
                RETRO_SERIALIZATION_QUIRK_FRONT_VARIABLE_SIZE;
            *quirks &= supported;
            ctx->serialization_quirks = *quirks;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE: {
            const struct retro_system_content_info_override *overrides =
                (const struct retro_system_content_info_override *)data;
            /* Free previous overrides */
            if (ctx->content_overrides) {
                for (unsigned i = 0; i < ctx->content_override_count; i++)
                    free((void *)ctx->content_overrides[i].extensions);
                free(ctx->content_overrides);
                ctx->content_overrides = NULL;
                ctx->content_override_count = 0;
            }
            if (!overrides) return true; /* NULL = query support */
            /* Count entries (terminated by NULL extensions) */
            unsigned count = 0;
            while (overrides[count].extensions) count++;
            if (count == 0) return true;
            ctx->content_overrides = calloc(count, sizeof(*ctx->content_overrides));
            if (!ctx->content_overrides) return true;
            ctx->content_override_count = count;
            for (unsigned i = 0; i < count; i++) {
                ctx->content_overrides[i].extensions =
                    strdup(overrides[i].extensions);
                ctx->content_overrides[i].need_fullpath =
                    overrides[i].need_fullpath;
                ctx->content_overrides[i].persistent_data =
                    overrides[i].persistent_data;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
            if (!ctx->has_game_info_ext) return false;
            *(const struct retro_game_info_ext **)data = &ctx->game_info_ext;
            return true;

        case RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE: {
            const struct retro_fastforwarding_override *ff =
                (const struct retro_fastforwarding_override *)data;
            if (!ff) return true; /* NULL = query support */
            ctx->ff_override = *ff;
            ctx->has_ff_override = true;
            /* Apply core's request unless host has inhibit control */
            ctx->fast_forwarding = ff->fastforward;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_JIT_CAPABLE:
            if (data) *(bool *)data = true;
            return true;

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK: {
            const struct retro_core_options_update_display_callback *cb =
                (const struct retro_core_options_update_display_callback *)data;
            ctx->opt_update_display_cb = cb ? cb->callback : NULL;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
            if (data) ctx->min_audio_latency_ms = *(const unsigned *)data;
            return true;

        case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK: {
            const struct retro_audio_buffer_status_callback *cb =
                (const struct retro_audio_buffer_status_callback *)data;
            ctx->audio_buf_status_cb = cb ? cb->callback : NULL;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT:
            if (data) *(unsigned *)data = ctx->savestate_context;
            return true;

        case RETRO_ENVIRONMENT_GET_THROTTLE_STATE: {
            struct retro_throttle_state *ts =
                (struct retro_throttle_state *)data;
            if (!ts) return true;
            ts->mode = ctx->throttle_mode;
            float fps = ctx->core
                        ? (float)ctx->core->av_info.timing.fps : 0.0f;
            switch (ctx->throttle_mode) {
            case RETRO_THROTTLE_FAST_FORWARD:
                ts->rate = 0.0f; break;
            case RETRO_THROTTLE_SLOW_MOTION:
                ts->rate = fps * 0.5f; break;
            case RETRO_THROTTLE_REWINDING:
            case RETRO_THROTTLE_FRAME_STEPPING:
                ts->rate = fps; break;
            default: /* RETRO_THROTTLE_NONE */
                ts->rate = fps; break;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_GET_DEVICE_POWER: {
            struct retro_device_power *pw =
                (struct retro_device_power *)data;
            if (!pw) return true;
            return read_device_power(pw);
        }

        case RETRO_ENVIRONMENT_GET_TARGET_REFRESH_RATE:
            if (data && ctx->core)
                *(float *)data = (float)ctx->core->av_info.timing.fps;
            return true;

        case RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE:
            if (data)
                *(unsigned *)data = ctx->config.audio_output_rate
                                    ? ctx->config.audio_output_rate : 48000;
            return true;

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
                insert_option(ctx, vars->key, def, val_list, NULL, NULL);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
            const struct retro_core_option_definition *defs =
                (const struct retro_core_option_definition *)data;
            if (!defs) return true;
            for (; defs->key != NULL; defs++) {
                char *vl = build_val_list(defs->values);
                insert_option(ctx, defs->key, defs->default_value, vl,
                              NULL, NULL);
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
                insert_option(ctx, d->key, d->default_value, vl,
                              NULL, NULL);
                free(vl);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
            const struct retro_core_options_v2 *opts =
                (const struct retro_core_options_v2 *)data;
            if (!opts || !opts->definitions) return true;
            /* Pre-register categories */
            if (opts->categories) {
                for (const struct retro_core_option_v2_category *c = opts->categories;
                     c->key != NULL; c++)
                    resolve_category(ctx, c->key, c->desc);
            }
            for (const struct retro_core_option_v2_definition *d = opts->definitions;
                 d->key != NULL; d++) {
                char *vl = build_val_list(d->values);
                const char *desc = d->desc_categorized ? d->desc_categorized
                                                       : d->desc;
                insert_option(ctx, d->key, d->default_value, vl,
                              d->category_key, desc);
                free(vl);
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
            const struct retro_core_options_v2_intl *intl =
                (const struct retro_core_options_v2_intl *)data;
            if (!intl || !intl->us || !intl->us->definitions) return true;
            /* Pre-register categories */
            if (intl->us->categories) {
                for (const struct retro_core_option_v2_category *c =
                         intl->us->categories; c->key != NULL; c++)
                    resolve_category(ctx, c->key, c->desc);
            }
            for (const struct retro_core_option_v2_definition *d =
                     intl->us->definitions; d->key != NULL; d++) {
                char *vl = build_val_list(d->values);
                const char *desc = d->desc_categorized ? d->desc_categorized
                                                       : d->desc;
                insert_option(ctx, d->key, d->default_value, vl,
                              d->category_key, desc);
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

        case RETRO_ENVIRONMENT_SET_VARIABLE: {
            const struct retro_variable *var =
                (const struct retro_variable *)data;
            if (!var || !var->key || !var->value)
                return false;
            for (unsigned i = 0; i < ctx->opt_count; i++) {
                if (strcmp(ctx->opt_keys[i], var->key) == 0) {
                    free(ctx->opt_vals[i]);
                    ctx->opt_vals[i] = strdup(var->value);
                    ctx->opt_updated = true;
                    return true;
                }
            }
            return false;
        }

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

        /* ---- Netpacket (core-controlled multiplayer) ---------------------- */

        case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE: {
            const struct retro_netpacket_callback *cb =
                (const struct retro_netpacket_callback *)data;
            if (cb) {
                ctx->netpacket_cb  = *cb;
                ctx->has_netpacket = true;
            }
            return true;
        }

        case RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT:
            /* Host always creates a shared GL context (SDL_GL_SHARE_WITH_CURRENT_CONTEXT=1),
             * so this is unconditionally supported. */
            return true;

        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: {
            unsigned *preferred = (unsigned *)data;
            if (preferred)
                *preferred = ctx->preferred_hw_context
                             ? ctx->preferred_hw_context
                             : RETRO_HW_CONTEXT_OPENGL_CORE;
            return true;
        }

        case RETRO_ENVIRONMENT_SET_PROC_ADDRESS_CALLBACK: {
            const struct retro_get_proc_address_interface *iface =
                (const struct retro_get_proc_address_interface *)data;
            ctx->core_get_proc_address = iface ? iface->get_proc_address : NULL;
            return true;
        }

        /* ---- LED ---------------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_LED_INTERFACE: {
            struct retro_led_interface *led =
                (struct retro_led_interface *)data;
            if (!led) return false;
            if (!ctx->config.led) return false;
            led->set_led_state = led_set_state;
            return true;
        }

        /* ---- Sensor ------------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_SENSOR_INTERFACE: {
            struct retro_sensor_interface *si =
                (struct retro_sensor_interface *)data;
            if (!si) return false;
            if (!ctx->config.sensor_set_state && !ctx->config.sensor_get_input)
                return false;
            si->set_sensor_state = sensor_set_state;
            si->get_sensor_input = sensor_get_input;
            return true;
        }

        /* ---- Microphone --------------------------------------------------- */

        case RETRO_ENVIRONMENT_GET_MICROPHONE_INTERFACE: {
            struct retro_microphone_interface *mi =
                (struct retro_microphone_interface *)data;
            if (!mi) return false;
            if (!ctx->config.mic_open) return false;
            if (mi->interface_version > RETRO_MICROPHONE_INTERFACE_VERSION)
                return false;
            mi->interface_version = RETRO_MICROPHONE_INTERFACE_VERSION;
            mi->open_mic      = mic_open;
            mi->close_mic     = mic_close;
            mi->get_params    = mic_get_params;
            mi->set_mic_state = mic_set_state;
            mi->get_mic_state = mic_get_state;
            mi->read_mic      = mic_read;
            return true;
        }

        /* ---- HW render context negotiation -------------------------------- */

        case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
            /* Spec says frontend must return true even when the current API
             * doesn't use context negotiation (so the core knows we're modern).
             * We only support GL, so just acknowledge and ignore the data. */
            return true;

        case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT: {
            struct retro_hw_render_context_negotiation_interface *iface =
                (struct retro_hw_render_context_negotiation_interface *)data;
            if (iface) {
                iface->interface_type = RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN;
                iface->interface_version = 0; /* 0 = not supported for this API */
            }
            return true;
        }

        /* ---- Optional directories ---------------------------------------- */

        case RETRO_ENVIRONMENT_GET_PLAYLIST_DIRECTORY:
            if (data) *(const char **)data = ctx->playlist_dir;
            return true;

        case RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY:
            if (data) *(const char **)data = ctx->file_browser_dir;
            return true;

        /* ---- Identity / locale ------------------------------------------- */

        default:
            return false;
    }
}
