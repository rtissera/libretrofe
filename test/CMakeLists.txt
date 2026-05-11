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
