// SPDX-License-Identifier: MIT
#ifndef LIBRA_INTERNAL_H
#define LIBRA_INTERNAL_H

#include "libra.h"
#include "core.h"
#include "audio.h"
#include "libretro.h"

#define LIBRA_MAX_OPTIONS    128
#define LIBRA_MAX_CATEGORIES  32

struct libra_ctx {
    libra_config_t  config;
    libra_core_t   *core;       /* NULL when no core loaded */
    int             pixel_format;
    bool            game_loaded;
    bool            shutdown_requested;

    char           *system_dir;
    char           *save_dir;
    char           *assets_dir;
    char           *core_path;
    char           *playlist_dir;
    char           *file_browser_dir;

    bool            fast_forwarding;
    unsigned        throttle_mode;     /* RETRO_THROTTLE_* for GET_THROTTLE_STATE */

    char           *username;
    unsigned        language;          /* RETRO_LANGUAGE_*; default 0 = ENGLISH */
    unsigned        savestate_context; /* RETRO_SAVESTATE_CONTEXT_* */

    /* Core options: simple fixed-size key/value store */
    char           *opt_keys[LIBRA_MAX_OPTIONS];
    char           *opt_vals[LIBRA_MAX_OPTIONS];
    char           *opt_val_list[LIBRA_MAX_OPTIONS]; /* "|"-joined possible values */
    bool            opt_visible[LIBRA_MAX_OPTIONS]; /* per-key visibility hint */
    int             opt_cat_idx[LIBRA_MAX_OPTIONS]; /* per-option → category index, -1 = none */
    char           *opt_descs[LIBRA_MAX_OPTIONS];   /* human-readable desc from core */
    unsigned        opt_count;
    bool            opt_updated;

    /* Core option categories */
    char           *opt_cat_keys[LIBRA_MAX_CATEGORIES];
    char           *opt_cat_descs[LIBRA_MAX_CATEGORIES];
    unsigned        opt_cat_count;

    /* SRAM dirty-detection shadow copy */
    uint8_t        *sram_shadow;
    size_t          sram_shadow_size;

    /* Audio resampler state */
    libra_audio_t  *audio;

    /* Disk control (populated by SET_DISK_CONTROL_INTERFACE or _EXT_) */
    struct retro_disk_control_callback     disk_cb;
    struct retro_disk_control_ext_callback disk_ext_cb;
    bool has_disk_cb;
    bool has_disk_ext_cb;

    /* Subsystem info (static data owned by the core) */
    const struct retro_subsystem_info *subsystem_info;
    unsigned                           subsystem_count;

    /* Keyboard callback (SET_KEYBOARD_CALLBACK) */
    retro_keyboard_event_t keyboard_cb;

    /* Screen rotation (SET_ROTATION): 0=0°, 1=90°CCW, 2=180°, 3=270°CCW */
    unsigned rotation;

    /* Frame time callback (SET_FRAME_TIME_CALLBACK) */
    struct retro_frame_time_callback frame_time_cb;
    bool has_frame_time_cb;

    /* Netpacket interface (core-controlled multiplayer) */
    struct retro_netpacket_callback netpacket_cb;
    bool has_netpacket;

    /* Audio callback (SET_AUDIO_CALLBACK — async audio cores) */
    struct retro_audio_callback audio_cb;
    bool has_audio_cb;

    /* Audio buffer status callback (SET_AUDIO_BUFFER_STATUS_CALLBACK) */
    retro_audio_buffer_status_callback_t audio_buf_status_cb;

    /* Minimum audio latency hint from core (SET_MINIMUM_AUDIO_LATENCY) */
    unsigned min_audio_latency_ms;

    /* SDL audio queue depth in bytes (updated by host each frame) */
    unsigned audio_queue_bytes;

    /* Core-requested fast-forwarding override (SET_FASTFORWARDING_OVERRIDE) */
    struct retro_fastforwarding_override ff_override;
    bool has_ff_override;

    /* Core options update display callback (SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK) */
    retro_core_options_update_display_callback_t opt_update_display_cb;

    /* Core reports achievement support (SET_SUPPORT_ACHIEVEMENTS) */
    bool supports_achievements;

    /* Serialization quirks (SET_SERIALIZATION_QUIRKS) */
    uint64_t serialization_quirks;

    /* Content info overrides (SET_CONTENT_INFO_OVERRIDE) */
    struct retro_system_content_info_override *content_overrides;
    unsigned content_override_count;

    /* SET_SUPPORT_NO_GAME: core can run without content */
    bool support_no_game;

    /* SET_PERFORMANCE_LEVEL: core-reported computational complexity (0 = low) */
    unsigned performance_level;

    /* SET_PROC_ADDRESS_CALLBACK: core-provided function lookup */
    retro_get_proc_address_t core_get_proc_address;

    /* Input descriptors (SET_INPUT_DESCRIPTORS) — deep copy */
    struct retro_input_descriptor *input_descriptors;
    unsigned input_descriptor_count;

    /* Hardware rendering (SET_HW_RENDER) */
    struct retro_hw_render_callback hw_render;
    bool            has_hw_render;
    unsigned        hw_fbo;              /* FBO id set by host */
    retro_hw_get_proc_address_t hw_get_proc_address; /* host-provided */
    unsigned        preferred_hw_context; /* host-set; 0 = OPENGL_CORE default */

    /* Per-type device capabilities (host-registered via libra_set_hw_render_support).
     * Indexed by retro_hw_context_type (0=NONE .. 7). */
    struct {
        bool     supported;
        unsigned max_major;
        unsigned max_minor;
    } hw_caps[8];
    bool            hw_caps_set;   /* true once any cap registered */

    /* Software framebuffer for GET_CURRENT_SOFTWARE_FRAMEBUFFER */
    void           *sw_fb;
    size_t          sw_fb_size;

    /* Game info ext for GET_GAME_INFO_EXT */
    char           *game_path;
    char           *game_dir;
    char           *game_name;
    char           *game_ext;
    struct retro_game_info_ext game_info_ext;
    bool            has_game_info_ext;

    /* Memory map (deep copy of core-provided descriptors) */
    struct retro_memory_descriptor *mem_descriptors;
    unsigned                        mem_descriptor_count;

    /* Per-port device types (from SET_CONTROLLER_INFO + retro_set_controller_port_device) */
    unsigned port_devices[16];  /* RETRO_DEVICE_* per port; default JOYPAD */
    unsigned port_count;        /* number of ports with known devices */

    /* Networking state (owned by netplay.c) */
    struct libra_netplay *netplay;

    /* Rewind state (owned by rewind.c) */
    struct libra_rewind *rewind;

    /* Rollback state (owned by rollback.c) */
    struct libra_rollback *rollback;

    /* Input override for rollback replay */
    bool     input_override_active;
    uint32_t input_override[16];  /* one word per port (joypad bitmask) */
};

#endif /* LIBRA_INTERNAL_H */
