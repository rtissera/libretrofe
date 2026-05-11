add_executable(libra_test main.c)
target_link_libraries(libra_test PRIVATE libra)
# libra.h transitively includes libra_internal.h → libretro.h, which lives in
# the (private) deps/ tree. Mirror libra's own include path so the test compiles.
target_include_directories(libra_test PRIVATE ${CMAKE_SOURCE_DIR}/deps)

# ── rewind tests ──
# Each builds rewind.c directly so they have no dependency on the rest of
# libra (faster iteration, fewer link-time variables).
foreach(_t rewind_unit rewind_fuzz rewind_bench)
    add_executable(${_t} ${_t}.c ${CMAKE_SOURCE_DIR}/src/rewind.c)
    target_include_directories(${_t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${_t} PRIVATE -Wall -Wextra -Wpedantic -O2)
endforeach()

# Video helpers — same standalone approach.  video.c uses no other libra
# internals so we can build it directly into the test binary.
add_executable(video_unit video_unit.c ${CMAKE_SOURCE_DIR}/src/video.c)
target_include_directories(video_unit PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/deps)
target_compile_options(video_unit PRIVATE -Wall -Wextra -Wpedantic -O2)

# Audio resampler — same standalone build pattern.
foreach(_t audio_unit audio_fuzz)
    add_executable(${_t} ${_t}.c ${CMAKE_SOURCE_DIR}/src/audio.c)
    target_include_directories(${_t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${_t} PRIVATE -Wall -Wextra -Wpedantic -O2)
    target_link_libraries(${_t} PRIVATE m)
endforeach()

# Round-trip bench — REG-Station-style SW upload path against real EGL/GLES2.
# Optional: only built when EGL and GLESv2 are available at configure time.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(EGL_PC QUIET egl)
    pkg_check_modules(GLESV2_PC QUIET glesv2)
    if(EGL_PC_FOUND AND GLESV2_PC_FOUND)
        add_executable(roundtrip_bench roundtrip_bench.c)
        target_link_libraries(roundtrip_bench PRIVATE libra
            ${EGL_PC_LIBRARIES} ${GLESV2_PC_LIBRARIES})
        target_include_directories(roundtrip_bench PRIVATE
            ${CMAKE_SOURCE_DIR}/deps
            ${EGL_PC_INCLUDE_DIRS} ${GLESV2_PC_INCLUDE_DIRS})
        target_compile_options(roundtrip_bench PRIVATE -Wall -Wextra -O2
            ${EGL_PC_CFLAGS_OTHER} ${GLESV2_PC_CFLAGS_OTHER})
        message(STATUS "roundtrip_bench: enabled (EGL + GLESv2 found)")
    else()
        message(STATUS "roundtrip_bench: skipped (EGL or GLESv2 not found)")
    endif()
endif()
