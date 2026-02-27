# libra

A minimal, self-contained C library for embedding libretro cores as an in-process frontend. Designed to be integrated into existing applications (emulator frontends, media centers, etc.) without pulling in RetroArch or its GPL dependencies.

## Features

- **Core lifecycle** — load/unload cores and games via `dlopen`
- **Video** — pixel format negotiation (XRGB1555, XRGB8888, RGB565), rotation support
- **Audio** — ring buffer with built-in linear resampler, configurable output rate
- **Input** — up to 8 ports, joypad + analog + keyboard
- **Save states** — save/load to arbitrary paths
- **SRAM** — save/load with dirty detection (`libra_save_sram_if_dirty`)
- **Core options** — get/set/cycle with dynamic visibility support
- **Disk control** — multi-disk swap for CD-based systems
- **Cheats** — set/clear cheat codes
- **Subsystem/multi-ROM** — Game Boy link, Sufami Turbo, etc.
- **VFS v3** — full virtual filesystem (file I/O, stat, mkdir, directory iteration)
- **Keyboard callback** — dispatch host key events to cores (DOSBox, ScummVM, VICE)
- **Frame time callback** — accurate frame timing for cores that request it
- **Performance interface** — `clock_gettime`-based counters
- **Memory access** — `libra_get_memory_data/size` for achievements integration
- **Rumble** — per-port strong/weak motor control
- **~47 environment callbacks** handled

## Building

```bash
mkdir build && cd build
cmake .. -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Static library:
```bash
cmake .. -DBUILD_SHARED_LIBS=OFF
```

Cross-compilation (toolchain files included for armhf, arm64, riscv64):
```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchains/arm64.cmake
```

## Usage

```c
#include <libra.h>

// Define your callbacks
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

    // Game loop
    while (running) {
        libra_run(ctx);  // calls your video/audio/input callbacks
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
│   ├── core.c / core.h     # dlopen/dlsym core loader
│   ├── environment.c / .h  # RETRO_ENVIRONMENT_* dispatch
│   ├── audio.c / .h        # Ring buffer + linear resampler
│   ├── input.c / .h        # Input state helpers
│   └── vfs.c / .h          # Virtual filesystem (v3)
└── test/                   # Smoke test
```

## Requirements

- C11 compiler
- Linux (POSIX) — uses `dlopen`, `clock_gettime`, `opendir`/`readdir`
- CMake 3.20+

## License

MIT — see [LICENSE](LICENSE).

`deps/libretro.h` is the public libretro API specification, also MIT-licensed.
