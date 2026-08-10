cmake_minimum_required(VERSION 3.22.1)
project(mobileglues LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MOBILEGLUES_BUILD_STANDALONE "Build the standalone libmobileglues shared library" ON)
option(MOBILEGLUES_AUTO_INIT "Run MobileGlues initialization from a shared-library constructor" ON)
option(MOBILEGLUES_ENABLE_PROFILING "Build the Perfetto profiling integration" OFF)

# Keep dependency options local to this directory tree.  CMP0077 NEW makes the
# normal variables below authoritative even for the older SPIRV-Cross project;
# this avoids polluting an embedding parent's cache while still producing the
# same minimal shader stack as the Android module.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(ENABLE_RTTI ON)
set(ENABLE_HLSL OFF)
set(BUILD_EXTERNAL OFF)
set(ENABLE_GLSLANG_BINARIES OFF)
set(ENABLE_OPT OFF)
set(ENABLE_GLSLANG_JS OFF)
set(BUILD_TESTING OFF)

set(SPIRV_CROSS_ENABLE_C_API ON)
set(SPIRV_CROSS_ENABLE_GLSL ON)
set(SPIRV_CROSS_ENABLE_REFLECT OFF)
set(SPIRV_CROSS_ENABLE_HLSL OFF)
set(SPIRV_CROSS_ENABLE_MSL OFF)
set(SPIRV_CROSS_ENABLE_CPP OFF)
set(SPIRV_CROSS_CLI OFF)
set(SPIRV_CROSS_STATIC ON)
set(SPIRV_CROSS_SHARED OFF)
set(SPIRV_CROSS_ENABLE_UTIL OFF)
set(SPIRV_CROSS_ENABLE_TESTS OFF)
set(SPIRV_CROSS_SKIP_INSTALL ON)

if(NOT TARGET glslang::glslang)
    add_subdirectory(3rdparty/glslang)
endif()
if(NOT TARGET spirv-cross-c)
    add_subdirectory(3rdparty/SPIRV-Cross)
endif()

# The official Android 2.0 plugin builds this shader stack as RelWithDebInfo:
# O2 + debug information + NDEBUG. OHOS names its shipping configuration
# Release, so preserve the same optimization/diagnostic contract explicitly;
# symbols are stripped only at the HAP packaging boundary.
if(NOT MSVC)
    foreach(MOBILEGLUES_SHADER_TARGET IN ITEMS
            glslang
            spirv-cross-core
            spirv-cross-glsl
            spirv-cross-c)
        if(TARGET ${MOBILEGLUES_SHADER_TARGET})
            target_compile_options(${MOBILEGLUES_SHADER_TARGET} PRIVATE -O2 -g)
        endif()
    endforeach()
endif()

# Single source-of-truth for both the Android/standalone library and embedded
# consumers.  init.cpp is intentionally a wrapper source rather than part of
# the core: hosts can initialize after preparing their writable directory.
set(MOBILEGLUES_CORE_SOURCES
    main.cpp
    gl/gl_stub.cpp
    gl/gl_native.cpp
    gl/gl.cpp
    gl/envvars.cpp
    gl/log.cpp
    gl/program.cpp
    gl/shader.cpp
    gl/framebuffer.cpp
    gl/texture.cpp
    gl/drawing.cpp
    gl/multidraw.cpp
    bench/multidraw_bench.cpp
    gl/mg.cpp
    gl/buffer.cpp
    gl/getter.cpp
    gl/enable.cpp
    gl/restart.cpp
    egl/context.cpp
    gl/pixel.cpp
    gl/transfer.cpp
    gl/random_string_gen.cpp
    gl/ExtWrappers/DSAWrapper.cpp
    gl/ExtWrappers/MultiBindWrapper.cpp
    gl/glsl/glsl_for_es.cpp
    gl/glsl/cache.cpp
    gl/FSR1/FSR1.cpp
    gl/vertexattrib.cpp
    glx/lookup.cpp
    egl/egl.cpp
    egl/loader.cpp
    gles/loader.cpp
    config/cJSON.c
    config/config.cpp
    config/gpu_utils.cpp
    config/settings.cpp
    config/stats.cpp
)

add_library(mobileglues_core OBJECT ${MOBILEGLUES_CORE_SOURCES})
add_library(MobileGlues::Core ALIAS mobileglues_core)

set_target_properties(mobileglues_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)

# These are private on purpose: an embedded host must not accidentally compile
# its own EGL/GLES translation units against MobileGlues' vendored headers.
target_include_directories(mobileglues_core BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/xxhash
)

if(MSVC)
    target_compile_options(mobileglues_core PRIVATE /Zc:preprocessor /Zc:__cplusplus)
else()
    target_compile_options(mobileglues_core PRIVATE
        -w
        -g
        -O2
        -funwind-tables
        -ffunction-sections
        -fdata-sections
        $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
    )
    target_compile_definitions(mobileglues_core PRIVATE _THREAD_SAFE)

    # Android RelWithDebInfo gets shared-library GC from NDK r27d.  Embedded
    # platforms consume this link contract explicitly so the effective result
    # stays equivalent even when their toolchain only enables GC for executables.
    target_link_options(mobileglues_core INTERFACE
        -Wl,-Bsymbolic-functions
        -Wl,--gc-sections
    )
endif()

if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Android NDK and OHOS BiSheng are Clang toolchains; ThinLTO is their
    # upstream release contract. Do not try_compile it on OHOS: Huawei's
    # toolchain file loses its SDK-root variable in CMake scratch projects even
    # though the real target accepts and links -flto=thin correctly.
    if(CMAKE_C_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(STATUS "MobileGlues: using Clang ThinLTO")
        target_compile_options(mobileglues_core PRIVATE -flto=thin)
        target_link_options(mobileglues_core INTERFACE -flto=thin)
        set(MOBILEGLUES_LTO_MODE "thin" CACHE INTERNAL "Effective MobileGlues LTO mode" FORCE)
    else()
        include(CheckIPOSupported)
        check_ipo_supported(RESULT MOBILEGLUES_IPO_SUPPORTED OUTPUT MOBILEGLUES_IPO_ERROR)
        if(NOT MOBILEGLUES_IPO_SUPPORTED)
            message(FATAL_ERROR "MobileGlues requires LTO for non-Debug builds: ${MOBILEGLUES_IPO_ERROR}")
        endif()
        message(STATUS "MobileGlues: using CMake IPO")
        set_property(TARGET mobileglues_core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set(MOBILEGLUES_LTO_MODE "ipo" CACHE INTERNAL "Effective MobileGlues LTO mode" FORCE)
    endif()
else()
    set(MOBILEGLUES_LTO_MODE "disabled-debug" CACHE INTERNAL "Effective MobileGlues LTO mode" FORCE)
endif()

target_link_libraries(mobileglues_core PUBLIC
    glslang::glslang
    spirv-cross-c
)

if(MOBILEGLUES_ENABLE_PROFILING)
    find_package(Threads REQUIRED)
    add_library(mobileglues_perfetto STATIC 3rdparty/perfetto/sdk/perfetto.cc)
    target_include_directories(mobileglues_perfetto PUBLIC 3rdparty/perfetto/sdk)
    target_link_libraries(mobileglues_core PRIVATE mobileglues_perfetto Threads::Threads)
    target_compile_definitions(mobileglues_core PRIVATE PROFILING=1)
else()
    target_compile_definitions(mobileglues_core PRIVATE PROFILING=0)
endif()

if(MOBILEGLUES_BUILD_STANDALONE)
    add_library(mobileglues SHARED init.cpp)
    add_library(MobileGlues::MobileGlues ALIAS mobileglues)
    target_link_libraries(mobileglues PRIVATE mobileglues_core)
    target_include_directories(mobileglues PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(mobileglues PRIVATE
        MOBILEGLUES_AUTO_INIT=$<BOOL:${MOBILEGLUES_AUTO_INIT}>
    )

    if(ANDROID)
        target_link_libraries(mobileglues PUBLIC android log)
    elseif(MACOS)
        target_link_libraries(mobileglues PUBLIC
            "-F${CMAKE_CURRENT_SOURCE_DIR}/external"
            "-framework libEGL"
            "-framework libGLESv2"
            "-framework Metal"
            "-framework Foundation"
            "-framework QuartzCore"
        )
    else()
        target_link_libraries(mobileglues PUBLIC ${CMAKE_DL_LIBS})
    endif()
endif()
