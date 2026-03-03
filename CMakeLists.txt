# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.20)
project(libra VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
    src/slang_spirv.cpp
    src/video.c
    src/image.c
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

# Allow LTO on subdirectory targets (suppresses CMP0069 warnings)
set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

# ── Size-optimize the SPIR-V deps (not hot-path, run once per shader load) ──
# Save parent flags, override with -Os for deps, then restore.
set(_SAVED_C_FLAGS   "${CMAKE_C_FLAGS_RELEASE}")
set(_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS_RELEASE}")
string(REPLACE "-O3" "-Os" CMAKE_C_FLAGS_RELEASE   "${CMAKE_C_FLAGS_RELEASE}")
string(REPLACE "-O3" "-Os" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")

# glslang — minimal build (no HLSL, no SPIRV-Tools optimizer, no SPV remapper)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(deps/glslang EXCLUDE_FROM_ALL)

# SPIRV-Cross — GLSL backend + C API only, no RTTI, no exceptions (assert on error)
set(SPIRV_CROSS_STATIC ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SHARED OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_EXCEPTIONS_TO_ASSERTIONS ON CACHE BOOL "" FORCE)
add_subdirectory(deps/SPIRV-Cross EXCLUDE_FROM_ALL)

# Restore parent optimization level
set(CMAKE_C_FLAGS_RELEASE   "${_SAVED_C_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "${_SAVED_CXX_FLAGS}")

# Hide symbols from deps and disable RTTI (no dynamic_cast/typeid used)
foreach(_tgt glslang glslang-default-resource-limits SPIRV
             spirv-cross-core spirv-cross-glsl spirv-cross-c)
    if(TARGET ${_tgt})
        target_compile_options(${_tgt} PRIVATE -fvisibility=hidden -fno-rtti)
    endif()
endforeach()

target_link_libraries(libra PUBLIC
    glslang::glslang glslang::glslang-default-resource-limits
    glslang::SPIRV spirv-cross-c)

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
