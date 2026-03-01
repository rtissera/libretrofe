# libra

A self-contained C library for embedding [libretro](https://www.libretro.com/) cores as an in-process frontend. Designed to be integrated into existing applications (emulator frontends, media centers, etc.) without pulling in RetroArch or its GPL dependencies.

MIT licensed. All code is original — only the public `libretro.h` API header (also MIT) is included.

## Features

### Core lifecycle
- Load/unload libretro cores via `dlopen`
- Load/unload games (including multi-ROM subsystems like Game Boy link, Sufami Turbo)
- Soft reset, shutdown detection

### Video
- Pixel format negotiation (XRGB1555, XRGB8888, RGB565)
- Screen rotation (0°/90°/180°/270°)
- Software framebuffer (zero-copy rendering for compatible cores)
- **Hardware rendering** — GL Core (3.3+), GL Compat (2.1+), GLES (2.0–3.2) with cascading device capability probing, FBO lifecycle, shared context support

### Audio
- Ring buffer with built-in linear resampler
- Configurable output rate (e.g. 48000 Hz)
- Async audio callback support (for cores like PPSSPP)
- Frame time callback for accurate timing

### Input
- Up to 16 ports, joypad + analog + mouse + lightgun + pointer + keyboard
- Input bitmask support for efficient polling
- Rumble (strong/weak motor control per port)
- Input override mechanism (used internally for rollback replay)

### Save system
- Save states to file with zlib compression (LBZ1 format)
- In-memory serialization for run-ahead and rewind
- SRAM save/load with dirty detection (`libra_save_sram_if_dirty`)

### Core options
- v1 and v2 option parsing with category support
- Get/set/cycle with dynamic visibility
- Category enumeration API for building menu UIs

### Memory access
- `libra_get_memory_data`/`libra_get_memory_size` for direct memory access
- Memory map descriptor storage and address translation (`libra_memory_map_read`)
- Designed for RetroAchievements integration (rc_client compatible)

### Netplay — Core packet mode
- For cores with `SET_NETPACKET_INTERFACE` (e.g. Dinothawr)
- Wire-compatible with RetroArch (CORE_PACKET_INTERFACE mode)
- Host + up to 15 clients, relay support
- TCP on port 55435

### Netplay — Rollback mode
- For cores with serialization support (most cores)
- GGPO-style savestate-based rollback
- Wire-compatible with RetroArch protocol 5–7 (INPUT_FRAME_SYNC)
- 128-frame ring buffer with input prediction
- CRC-32 desync detection, compressed savestate exchange
- Configurable input latency (0–10 frames)

### Miscellaneous
- Disk control (multi-disk swap for CD-based systems)
- Cheats (set/clear cheat codes)
- VFS v2 (POSIX file I/O, stat, mkdir, directory iteration)
- Performance interface (`clock_gettime`-based counters)
- Keyboard forwarding (for cores like DOSBox, ScummVM, VICE)
- Battery/power status (Linux sysfs)
- Device power, throttle state, fast-forward control
- **57 environment callbacks** handled, 0 critical gaps

## Building

```bash
mkdir build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Dependencies

- C11 compiler
- Linux (POSIX) — uses `dlopen`, `clock_gettime`, `opendir`/`readdir`
- CMake 3.20+
- zlib (for save state compression, CRC-32, netplay savestate exchange)

### Static library

```bash
cmake .. -DBUILD_SHARED_LIBS=OFF
```

### Cross-compilation

Toolchain files included for armhf, arm64, riscv64:

```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/arm64.cmake
```

## Usage

```c
#include <libra.h>

void my_video(void *ud, const void *data, unsigned w, unsigned h,
              size_t pitch, int pixel_format) { /* ... */ }
void my_audio(void *ud, const int16_t *data, size_t frames) { /* ... */ }
void my_input_poll(void *ud) { /* ... */ }
int16_t my_input_state(void *ud, unsigned port, unsigned device,
                       unsigned index, unsigned id) { /* ... */ }

int main(void) {
    libra_config_t cfg = {
        .video       = my_video,
        .audio       = my_audio,
        .input_poll  = my_input_poll,
        .input_state = my_input_state,
        .audio_output_rate = 48000,
    };

    libra_ctx_t *ctx = libra_create(&cfg);
    libra_set_system_directory(ctx, "/path/to/bios");
    libra_set_save_directory(ctx, "/path/to/saves");

    libra_load_core(ctx, "/path/to/core_libretro.so");
    libra_load_game(ctx, "/path/to/rom.nes");

    while (running) {
        libra_run(ctx);
    }

    libra_unload_game(ctx);
    libra_unload_core(ctx);
    libra_destroy(ctx);
}
```

## Project structure

```
libra/
├── CMakeLists.txt
├── cmake/toolchains/       # Cross-compilation toolchain files
├── deps/libretro.h         # libretro API header (MIT)
├── include/libra.h         # Public API — the only header you need
├── src/
│   ├── libra.c             # Lifecycle, queries, save states, SRAM
│   ├── libra_internal.h    # Internal context structure
│   ├── core.c / core.h     # dlopen/dlsym core loader
│   ├── environment.c / .h  # RETRO_ENVIRONMENT_* dispatch (57 commands)
│   ├── audio.c / .h        # Ring buffer + linear resampler
│   ├── input.c / .h        # Input state + rollback override
│   ├── vfs.c / .h          # Virtual filesystem (v2)
│   ├── netsock.c / .h      # Shared TCP helpers + wire protocol constants
│   ├── netplay.c / .h      # Core packet netplay (SET_NETPACKET_INTERFACE)
│   └── rollback.c / .h     # Rollback netplay (GGPO-style, savestate-based)
└── test/                   # Smoke test
```

## License

MIT — see [LICENSE](LICENSE).

`deps/libretro.h` is the public libretro API specification, also MIT-licensed.
