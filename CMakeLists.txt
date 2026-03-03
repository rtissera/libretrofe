# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(libra VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_SHARED_LIBS "Build shared library (ON) or static (OFF)" ON)

add_library(libra
    src/libra.c
    src/core.c
    src/environment.c
    src/audio.c
    src/input.c
    src/menu.c
    src/vfs.c
    src/netplay.c
    src/netsock.c
    src/rollback.c
    src/rewind.c
    src/shader.c
    deps/miniz.c
    deps/zstd.c
)

target_include_directories(libra
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        src
        deps
)

target_compile_options(libra PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(libra PRIVATE ${CMAKE_DL_LIBS} m)

if(BUILD_SHARED_LIBS)
    set_target_properties(libra PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION 0
    )
endif()

option(BUILD_TESTS "Build smoke-test executable" ON)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()

include(GNUInstallDirs)
install(TARGETS libra
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/libra.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
