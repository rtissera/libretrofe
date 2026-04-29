add_executable(libra_test main.c)
target_link_libraries(libra_test PRIVATE libra)
# libra.h transitively includes libra_internal.h → libretro.h, which lives in
# the (private) deps/ tree. Mirror libra's own include path so the test compiles.
target_include_directories(libra_test PRIVATE ${CMAKE_SOURCE_DIR}/deps)
