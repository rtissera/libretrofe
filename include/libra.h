// SPDX-License-Identifier: MIT
#ifndef LIBRA_H
#define LIBRA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct libra_ctx libra_ctx_t;

/* pixel_format values match retro_pixel_format enum:
 * 0 = XRGB1555, 1 = XRGB8888, 2 = RGB565 */
typedef void    (*libra_video_cb_t)(void *ud, const void *data,
                    unsigned w, unsigned h, size_t pitch, int pixel_format);
typedef void    (*libra_audio_cb_t)(void *ud, const int16_t *data, size_t frames);
typedef void    (*libra_input_poll_cb_t)(void *ud);
typedef int16_t (*libra_input_state_cb_t)(void *ud,
                    unsigned port, unsigned device, unsigned index, unsigned id);
/* effect: 0=strong, 1=weak (matches retro_rumble_effect); strength: 0-65535 */
typedef void    (*libra_rumble_cb_t)(void *ud,
                    unsigned port, unsigned effect, uint16_t strength);
/* LED state change: led = LED index, state = 0 or 1 */
typedef void    (*libra_led_cb_t)(void *ud, int led, int state);
/* Sensor state: action = RETRO_SENSOR_*_ENABLE/DISABLE, rate = Hz */
typedef bool    (*libra_sensor_set_state_cb_t)(void *ud, unsigned port,
                    unsigned action, unsigned rate);
/* Sensor read: id = RETRO_SENSOR_ACCELEROMETER_X..ILLUMINANCE */
typedef float   (*libra_sensor_get_input_cb_t)(void *ud, unsigned port, unsigned id);

/* Microphone callbacks (all optional; NULL = no mic support) */
typedef void   *(*libra_mic_open_cb_t)(void *ud, unsigned rate);
typedef void    (*libra_mic_close_cb_t)(void *ud, void *mic);
typedef bool    (*libra_mic_set_state_cb_t)(void *ud, void *mic, bool active);
typedef bool    (*libra_mic_get_state_cb_t)(void *ud, const void *mic);
typedef int     (*libra_mic_read_cb_t)(void *ud, void *mic, int16_t *samples, size_t num_samples);

typedef struct {
    libra_video_cb_t       video;
    libra_audio_cb_t       audio;
    libra_input_poll_cb_t  input_poll;
    libra_input_state_cb_t input_state;
    libra_rumble_cb_t      rumble;          /* optional; NULL = no rumble */
    libra_led_cb_t         led;             /* optional; NULL = no LED */
    libra_sensor_set_state_cb_t sensor_set_state; /* optional */
    libra_sensor_get_input_cb_t sensor_get_input; /* optional */
    libra_mic_open_cb_t    mic_open;        /* optional; NULL = no mic */
    libra_mic_close_cb_t   mic_close;
    libra_mic_set_state_cb_t mic_set_state;
    libra_mic_get_state_cb_t mic_get_state;
    libra_mic_read_cb_t    mic_read;
    void                  *userdata;
    unsigned               audio_output_rate;  /* target rate Hz, e.g. 48000 */
} libra_config_t;

/* Lifecycle */
libra_ctx_t *libra_create(const libra_config_t *config);
void         libra_destroy(libra_ctx_t *ctx);

/* Directories (set before loading core) */
void libra_set_system_directory(libra_ctx_t *ctx, const char *path);
void libra_set_save_directory(libra_ctx_t *ctx, const char *path);
void libra_set_assets_directory(libra_ctx_t *ctx, const char *path);

/* Fast-forward mode — cores may skip non-essential work when true */
void libra_set_fast_forward(libra_ctx_t *ctx, bool enabled);

/* Throttle mode reported to cores via GET_THROTTLE_STATE.
 * Values: 0=NONE, 1=FRAME_STEPPING, 2=FAST_FORWARD, 3=SLOW_MOTION, 4=REWINDING */
void libra_set_throttle_mode(libra_ctx_t *ctx, unsigned mode);

/* Optional directories */
void libra_set_playlist_directory(libra_ctx_t *ctx, const char *path);
void libra_set_file_browser_directory(libra_ctx_t *ctx, const char *path);

/* Username reported to cores via GET_USERNAME (NULL = anonymous) */
void libra_set_username(libra_ctx_t *ctx, const char *name);

/* Language reported to cores via GET_LANGUAGE (RETRO_LANGUAGE_* enum, default 0 = ENGLISH) */
void libra_set_language(libra_ctx_t *ctx, unsigned language);

/* Savestate context reported to cores via GET_SAVESTATE_CONTEXT.
 * Values: 0=NORMAL, 1=RUNAHEAD_SAME_INSTANCE, 2=RUNAHEAD_SAME_BINARY, 3=ROLLBACK_NETPLAY */
void libra_set_savestate_context(libra_ctx_t *ctx, unsigned context);

/* Core */
bool libra_load_core(libra_ctx_t *ctx, const char *path);
void libra_unload_core(libra_ctx_t *ctx);

/* Game */
bool libra_load_game(libra_ctx_t *ctx, const char *path);
void libra_unload_game(libra_ctx_t *ctx);

/* Per-frame loop */
void libra_run(libra_ctx_t *ctx);

/* Soft reset */
void libra_reset(libra_ctx_t *ctx);

/* Queries (valid after libra_load_game) */
double      libra_get_fps(libra_ctx_t *ctx);
double      libra_get_sample_rate(libra_ctx_t *ctx);
void        libra_get_geometry(libra_ctx_t *ctx,
                unsigned *base_w, unsigned *base_h, float *aspect);
const char *libra_core_name(libra_ctx_t *ctx);
const char *libra_core_extensions(libra_ctx_t *ctx);

/* Core options — key/value get/set */
const char *libra_get_option(libra_ctx_t *ctx, const char *key);
void        libra_set_option(libra_ctx_t *ctx, const char *key, const char *value);

/* Core option iteration (for building a quick-menu UI) */
unsigned    libra_option_count(libra_ctx_t *ctx);
const char *libra_option_key  (libra_ctx_t *ctx, unsigned index);
const char *libra_option_value(libra_ctx_t *ctx, unsigned index);
/* Cycle the option at index by direction (+1 = next, -1 = prev), wrapping.
 * Sets opt_updated so the core will see the change next retro_run().
 * Returns false if no value list is available for this option. */
bool        libra_option_cycle(libra_ctx_t *ctx, unsigned index, int direction);

/* Core option categories (populated from SET_CORE_OPTIONS_V2) */
unsigned    libra_option_category_count(libra_ctx_t *ctx);
const char *libra_option_category_key  (libra_ctx_t *ctx, unsigned cat_index);
const char *libra_option_category_desc (libra_ctx_t *ctx, unsigned cat_index);
int         libra_option_category_index(libra_ctx_t *ctx, unsigned opt_index);
const char *libra_option_desc          (libra_ctx_t *ctx, unsigned opt_index);

/* Dynamic audio rate control — host feeds SDL queue depth each frame */
void libra_set_audio_queue_depth(libra_ctx_t *ctx, unsigned bytes);
void libra_set_audio_target_queue(libra_ctx_t *ctx, unsigned target_frames);

/* Save states */
bool libra_save_state(libra_ctx_t *ctx, const char *path);
bool libra_load_state(libra_ctx_t *ctx, const char *path);

/* In-memory serialization (for run-ahead / rewind) */
size_t libra_serialize_size(libra_ctx_t *ctx);
bool   libra_serialize(libra_ctx_t *ctx, void *data, size_t size);
bool   libra_unserialize(libra_ctx_t *ctx, const void *data, size_t size);

/* SRAM (battery-backed save RAM) */
bool libra_save_sram(libra_ctx_t *ctx, const char *path);
bool libra_load_sram(libra_ctx_t *ctx, const char *path);
/* Write SRAM to path only if contents changed since last save/load.
 * The first call after libra_load_game() establishes the baseline (no write).
 * Returns true if a write was performed. */
bool libra_save_sram_if_dirty(libra_ctx_t *ctx, const char *path);

/* Controller port — call after libra_load_game; device constants from libretro.h */
void libra_set_controller(libra_ctx_t *ctx, unsigned port, unsigned device);

/* Disk control — swap to disk at index (0-based); ejects then re-inserts */
bool libra_get_disk_count(libra_ctx_t *ctx, unsigned *count);
bool libra_swap_disk(libra_ctx_t *ctx, unsigned index);

/* Cheats */
void libra_clear_cheats(libra_ctx_t *ctx);
void libra_set_cheat(libra_ctx_t *ctx, unsigned index, bool enabled, const char *code);

/* Core memory access (for RetroAchievements / memory inspection)
 * id: RETRO_MEMORY_SAVE_RAM (0), RETRO_MEMORY_RTC (1),
 *     RETRO_MEMORY_SYSTEM_RAM (2), RETRO_MEMORY_VIDEO_RAM (3) */
void  *libra_get_memory_data(libra_ctx_t *ctx, unsigned id);
size_t  libra_get_memory_size(libra_ctx_t *ctx, unsigned id);

/* Memory map descriptors (from SET_MEMORY_MAPS).
 * Returns 0 if the core did not provide a memory map.
 * Each descriptor has: ptr, start, select, disconnect, len, offset. */
unsigned libra_memory_map_count(libra_ctx_t *ctx);

/* Read from the core's address space using memory map descriptors.
 * Translates emulated address → host pointer via descriptor matching.
 * Returns number of bytes read (0 if address is unmapped). */
uint32_t libra_memory_map_read(libra_ctx_t *ctx, uint32_t address,
                                uint8_t *buffer, uint32_t num_bytes);

/* Core option visibility (updated dynamically by core during retro_run) */
bool libra_is_option_visible(libra_ctx_t *ctx, const char *key);

/* Input descriptors (populated by core via SET_INPUT_DESCRIPTORS).
 * Returns the number of descriptors.  For each index 0..count-1:
 * port, device, index, id identify the input; description is human-readable. */
unsigned    libra_input_descriptor_count(libra_ctx_t *ctx);
const char *libra_input_descriptor_description(libra_ctx_t *ctx, unsigned idx);
void        libra_input_descriptor_info(libra_ctx_t *ctx, unsigned idx,
                unsigned *port, unsigned *device, unsigned *index, unsigned *id);

/* True if the core called SET_SUPPORT_NO_GAME (can run without content) */
bool libra_supports_no_game(libra_ctx_t *ctx);

/* Subsystem / multi-ROM loading (e.g. Game Boy link, Sufami Turbo)
 * game_type: matches retro_subsystem_info::id supplied by the core
 * paths[]:   one path per required ROM slot, in the order the core declared them */
bool libra_load_game_special(libra_ctx_t *ctx, unsigned game_type,
                              const char **paths, unsigned num_paths);

/* Returns true if the core called RETRO_ENVIRONMENT_SHUTDOWN */
bool libra_is_shutdown_requested(libra_ctx_t *ctx);

/* Screen rotation requested by core: 0=0°, 1=90°CCW, 2=180°, 3=270°CCW */
unsigned libra_get_rotation(libra_ctx_t *ctx);

/* ---- Hardware rendering (GL/GLES cores) --------------------------------
 *
 * Cores that use RETRO_ENVIRONMENT_SET_HW_RENDER render 3D frames directly
 * into a GL framebuffer.  The host must create a GL context and FBO, then
 * call the lifecycle functions below.
 *
 * pixel_format == -1 in the video callback signals a HW frame (data=NULL).
 */

typedef void *(*libra_get_proc_address_t)(const char *sym);

/* Host tells libra what HW context type to advertise to cores via
 * GET_PREFERRED_HW_RENDER.  Call before libra_load_core().
 * Values match retro_hw_context_type (e.g. 3 = OPENGL_CORE, 2 = OPENGLES2).
 * Default (0 or uncalled) = OPENGL_CORE. */
void libra_set_preferred_hw_render(libra_ctx_t *ctx, unsigned context_type);

/* Register device capability: the host can create context_type up to
 * version max_major.max_minor.  Call before libra_load_core().
 * When caps are registered, SET_HW_RENDER rejects unsupported types/versions.
 * context_type values: 1=OPENGL, 2=OPENGLES2, 3=OPENGL_CORE,
 * 4=OPENGLES3, 5=OPENGLES_VERSION */
void libra_set_hw_render_support(libra_ctx_t *ctx, unsigned context_type,
                                  unsigned max_major, unsigned max_minor);

/* Queries (valid after libra_load_game) */
bool     libra_hw_render_enabled(libra_ctx_t *ctx);
unsigned libra_hw_render_context_type(libra_ctx_t *ctx);
unsigned libra_hw_render_version_major(libra_ctx_t *ctx);
unsigned libra_hw_render_version_minor(libra_ctx_t *ctx);
bool     libra_hw_render_bottom_left_origin(libra_ctx_t *ctx);
bool     libra_hw_render_depth(libra_ctx_t *ctx);
bool     libra_hw_render_stencil(libra_ctx_t *ctx);

/* Host sets these before calling context_reset */
void libra_hw_render_set_fbo(libra_ctx_t *ctx, unsigned fbo);
void libra_hw_render_set_get_proc_address(libra_ctx_t *ctx,
         libra_get_proc_address_t proc);

/* Lifecycle: host calls after creating GL context + FBO */
void libra_hw_render_context_reset(libra_ctx_t *ctx);
/* Host calls before destroying GL context */
void libra_hw_render_context_destroy(libra_ctx_t *ctx);

/* Forward a keyboard event to the core (for cores using SET_KEYBOARD_CALLBACK).
 * keycode: RETROK_* value from libretro.h
 * character: Unicode codepoint of the key (0 if non-printable)
 * key_modifiers: RETROKMOD_* bitmask */
void libra_keyboard_event(libra_ctx_t *ctx, bool down,
                           unsigned keycode, uint32_t character,
                           uint16_t key_modifiers);

/* ---- Netpacket (core-controlled multiplayer) ----------------------------
 *
 * Wire protocol is compatible with RetroArch (CORE_PACKET_INTERFACE mode).
 * Only supported for cores that register RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE.
 */

/* Callback: the networking layer wants to display an OSD message */
typedef void (*libra_net_message_cb_t)(void *userdata, const char *msg);

/* True if the loaded core registered a netpacket interface */
bool        libra_has_netpacket(libra_ctx_t *ctx);
/* Core-supplied protocol version string, or NULL */
const char *libra_netpacket_core_version(libra_ctx_t *ctx);

/* Host: start listening.  msg_cb is called for status/error messages. */
bool libra_netplay_host(libra_ctx_t *ctx, uint16_t port,
                        libra_net_message_cb_t msg_cb);

/* Client: connect to host_ip:port (blocks briefly for handshake). */
bool libra_netplay_join(libra_ctx_t *ctx, const char *host_ip,
                        uint16_t port, libra_net_message_cb_t msg_cb);

/* Call once per frame before libra_run().
 * Accepts connections (host), reads packets, dispatches to core.
 * Returns false if the connection was lost. */
bool libra_netplay_poll(libra_ctx_t *ctx);

/* Disconnect and tear down. */
void libra_netplay_disconnect(libra_ctx_t *ctx);

/* True if a netplay session is active (hosting or connected). */
bool libra_netplay_active(libra_ctx_t *ctx);
/* True if peer is connected and playing. */
bool libra_netplay_connected(libra_ctx_t *ctx);
/* True if we are the host. */
bool libra_netplay_is_host(libra_ctx_t *ctx);
/* True if host is waiting for a peer. */
bool libra_netplay_is_waiting(libra_ctx_t *ctx);
/* Our client_id (0 = host, >= 1 = client). */
uint16_t libra_netplay_client_id(libra_ctx_t *ctx);
/* Number of playing peers (excludes ourselves). */
unsigned libra_netplay_peer_count(libra_ctx_t *ctx);

/* ---- Rollback netplay (savestate-based, wire-compatible with RetroArch) --
 *
 * For cores that support serialization but NOT the netpacket interface.
 * Uses GGPO-style rollback: each peer captures local input, exchanges it
 * over TCP, and when a prediction was wrong, loads a savestate and replays.
 */

bool libra_rollback_host(libra_ctx_t *ctx, uint16_t port,
                         libra_net_message_cb_t msg_cb);
bool libra_rollback_join(libra_ctx_t *ctx, const char *host_ip,
                         uint16_t port, libra_net_message_cb_t msg_cb);

/* Call instead of libra_run() when rollback is active.
 * Handles input exchange, rollback detection, replay, and core execution.
 * Returns false if connection lost. */
bool libra_rollback_run(libra_ctx_t *ctx);

void libra_rollback_disconnect(libra_ctx_t *ctx);

bool     libra_rollback_active(libra_ctx_t *ctx);
bool     libra_rollback_connected(libra_ctx_t *ctx);
bool     libra_rollback_is_host(libra_ctx_t *ctx);
/* True during rollback replay (host should mute OSD / skip non-essential work) */
bool     libra_rollback_is_replay(libra_ctx_t *ctx);

void     libra_rollback_set_input_latency(libra_ctx_t *ctx, unsigned frames);
unsigned libra_rollback_input_latency(libra_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* LIBRA_H */
