// SPDX-License-Identifier: MIT
#ifndef LIBRA_INTERNAL_H
#define LIBRA_INTERNAL_H

#include "libra.h"
#include "core.h"
#include "audio.h"
#include "libretro.h"

#define LIBRA_MAX_OPTIONS 256
#define LIBRA_MAX_PORTS   8
#define LIBRA_MAX_INPUT_DESCS 256
#define LIBRA_MAX_CTRL_TYPES  16

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

    bool            fast_forwarding;

    /* Core options: simple fixed-size key/value store */
    char           *opt_keys[LIBRA_MAX_OPTIONS];
    char           *opt_vals[LIBRA_MAX_OPTIONS];
    char           *opt_val_list[LIBRA_MAX_OPTIONS]; /* "|"-joined possible values */
    bool            opt_visible[LIBRA_MAX_OPTIONS]; /* per-key visibility hint */
    unsigned        opt_count;
    bool            opt_updated;

    /* Core options update display callback (core requests visibility refresh) */
    retro_core_options_update_display_callback_t options_update_display_cb;

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

    /* Video rotation (0-3: 0°, 90°, 180°, 270° counter-clockwise) */
    unsigned        rotation;

    /* Keyboard callback (used by DOSBox, ScummVM, VICE, etc.) */
    retro_keyboard_event_t keyboard_cb;

    /* Frame time callback (called before each retro_run) */
    retro_frame_time_callback_t frame_time_cb;
    retro_usec_t    frame_time_reference;   /* ideal frame duration in usec */
    retro_usec_t    frame_time_last;        /* timestamp of last retro_run */

    /* Serialization quirks bitmask from core */
    uint64_t        serialization_quirks;

    /* Core declared it can run without content */
    bool            support_no_game;

    /* Memory map provided by core (pointers owned by core, valid while loaded) */
    const struct retro_memory_descriptor *memory_descriptors;
    unsigned        memory_descriptor_count;

    /* Target display refresh rate (set by host, reported to core) */
    float           target_refresh_rate;

    /* Message from core (SET_MESSAGE / SET_MESSAGE_EXT) */
    char            message_buf[512];
    unsigned        message_frames;
    bool            message_pending;

    /* Audio buffer status callback (core polls buffer occupancy) */
    retro_audio_buffer_status_callback_t audio_buffer_status_cb;

    /* Minimum audio latency requested by core (ms) */
    unsigned        min_audio_latency;

    /* Fastforwarding override from core */
    bool            ff_override_active;
    float           ff_override_ratio;
    bool            ff_override_fastforward;
    bool            ff_override_notification;
    bool            ff_override_inhibit;

    /* Content info override from core (pointer owned by core) */
    const struct retro_system_content_info_override *content_info_override;

    /* GET_GAME_INFO_EXT data (built at game load) */
    char           *game_full_path;
    char           *game_dir;
    char           *game_name;    /* basename without extension */
    char           *game_ext;     /* extension, lowercase */
    struct retro_game_info_ext game_info_ext;

    /* Throttle state for GET_THROTTLE_STATE */
    unsigned        throttle_mode;
    float           throttle_rate;

    /* Software framebuffer for GET_CURRENT_SOFTWARE_FRAMEBUFFER */
    void           *sw_framebuffer;
    size_t          sw_framebuffer_size;

    /* Audio callback (for async audio cores like DOSBox, ScummVM) */
    retro_audio_callback_t           audio_cb;
    retro_audio_set_state_callback_t audio_set_state_cb;

    /* Input descriptors (deep-copied from core) */
    struct {
        unsigned port, device, index, id;
        char *desc;
    } input_descs[LIBRA_MAX_INPUT_DESCS];
    unsigned input_desc_count;

    /* Controller info per port (deep-copied from core) */
    struct {
        char *desc;
        unsigned id;
    } ctrl_types[LIBRA_MAX_PORTS][LIBRA_MAX_CTRL_TYPES];
    unsigned ctrl_type_count[LIBRA_MAX_PORTS];

    /* Audio/video enable flags for run-ahead (default 3 = both) */
    int audio_video_enable;

    /* Username (host-set, for GET_USERNAME) */
    char *username;

    /* Language (for GET_LANGUAGE, default 0 = RETRO_LANGUAGE_ENGLISH) */
    unsigned language;

    /* Option descriptions (parallel to opt_keys/opt_vals) */
    char *opt_desc[LIBRA_MAX_OPTIONS];

    /* Geometry change flag (set by SET_GEOMETRY / SET_SYSTEM_AV_INFO) */
    bool geometry_changed;
};

#endif /* LIBRA_INTERNAL_H */
