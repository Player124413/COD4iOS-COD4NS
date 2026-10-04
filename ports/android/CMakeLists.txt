# Android port.
#
# Two shared libraries, libkisakcod_sp.so and libkisakcod_mp.so, loaded by the
# Kotlin launcher according to the mode the player picked. They cannot be one
# binary: KISAK_MP changes structure layouts throughout the engine, so the
# same translation unit compiled both ways produces incompatible objects.
#
# Source lists come from scripts/*.cmake, exactly as the iOS port does, so the
# three platforms never drift apart. Everything in ports/ios that turned out
# to be portable C++ - the Direct3D 9 object model, controller input, the
# fastfile loader, cinematic playback - is compiled here directly rather than
# copied; see ports/android/engine/apple_bridge.cpp for the four symbols that
# needed a name forward.
#
# When this directory is configured for the host instead of Android (that is,
# -DKISAK_BUILD_PORT_TESTS=ON with no NDK toolchain) it builds only the
# portable test executables. That is the configuration CI can actually run.

cmake_minimum_required(VERSION 3.22)

get_filename_component(KISAK_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SRC_DIR "${KISAK_ROOT}/src")
set(DEPS_DIR "${KISAK_ROOT}/deps")
set(SCRIPTS_DIR "${KISAK_ROOT}/scripts")
set(IOS_DIR "${KISAK_ROOT}/ports/ios")

# ---------------------------------------------------------------------------
# Host test build. No NDK, no Vulkan, no JNI - just the parts that are plain
# C++ and can therefore be verified on any machine.

if(NOT ANDROID)
    if(KISAK_BUILD_PORT_TESTS)
        add_executable(kisakcod_android_perf_tests
            tests/PerfTests.cpp
            perf/frame_pacer.cpp
            perf/resolution_scaler.cpp
            perf/thermal_governor.cpp
            perf/cpu_topology.cpp
            perf/device_profile.cpp
            perf/perf_director.cpp
        )
        target_compile_features(kisakcod_android_perf_tests PRIVATE cxx_std_17)
        target_compile_options(kisakcod_android_perf_tests PRIVATE -UNDEBUG)
        add_test(NAME android_perf COMMAND kisakcod_android_perf_tests)
        set_tests_properties(android_perf PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL:")

        add_executable(kisakcod_android_shader_tests
            tests/ShaderTranslatorTests.cpp
            gfx/dx9_glsl_translator.cpp
        )
        target_compile_features(kisakcod_android_shader_tests PRIVATE cxx_std_17)
        target_compile_options(kisakcod_android_shader_tests PRIVATE -UNDEBUG)
        add_test(NAME android_shader_translator COMMAND kisakcod_android_shader_tests)
        set_tests_properties(android_shader_translator PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL:")

        # The gamepad/touch merge. The engine side is stubbed by the test, so
        # what runs here is the real android_input.cpp and the real on-screen
        # control state machine - the two places where a wrong sign shows up
        # on a device as "aiming is sometimes inverted" rather than a crash.
        add_executable(kisakcod_android_input_tests
            tests/InputMergeTests.cpp
            engine/android_input.cpp
            app/touch_controls.cpp
        )
        target_compile_features(kisakcod_android_input_tests PRIVATE cxx_std_17)
        target_compile_options(kisakcod_android_input_tests PRIVATE -UNDEBUG)
        target_include_directories(kisakcod_android_input_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../ios/engine")
        add_test(NAME android_input_merge COMMAND kisakcod_android_input_tests)
        set_tests_properties(android_input_merge PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL:")
    endif()
    return()
endif()

# ---------------------------------------------------------------------------
# Android build proper.

if(NOT CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR
        "Only arm64-v8a is supported. The engine is a 64-bit port of 32-bit "
        "Windows code; a 32-bit Android build would hit the same pointer-size "
        "bugs the port exists to fix, with none of the benefit.")
endif()

include(FetchContent)

find_package(Vulkan REQUIRED)

# shaderc ships inside the NDK and is what turns the translated GLSL into
# SPIR-V at load time. The combined variant has the GLSL front end linked in.
find_library(SHADERC_LIB
    NAMES shaderc shaderc_combined
    PATHS "${CMAKE_ANDROID_NDK}/sources/third_party/shaderc/libs/c++_static/${CMAKE_ANDROID_ARCH_ABI}"
          "${CMAKE_ANDROID_NDK}/sources/third_party/shaderc/libs/c++_shared/${CMAKE_ANDROID_ARCH_ABI}"
    NO_DEFAULT_PATH
)
if(NOT SHADERC_LIB)
    message(FATAL_ERROR
        "shaderc not found in the NDK. Build it once with:\n"
        "  \$ANDROID_NDK/sources/third_party/shaderc/android_test/../../../../ndk-build \\\n"
        "      NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=Android.mk APP_STL=c++_static \\\n"
        "      APP_ABI=arm64-v8a libshaderc_combined\n"
        "or point -DSHADERC_LIB= at a prebuilt library.")
endif()
set(SHADERC_INCLUDE "${CMAKE_ANDROID_NDK}/sources/third_party/shaderc/include")

# --- buildnumber.h, as the Windows pre-build step generates it ---------------
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY "${KISAK_ROOT}"
    OUTPUT_VARIABLE KISAK_BUILD_NUMBER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT KISAK_BUILD_NUMBER)
    set(KISAK_BUILD_NUMBER 0)
endif()
set(KISAK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${KISAK_GENERATED_DIR}/buildnumber.h" CONTENT "#pragma once
#define BUILD_NUMBER ${KISAK_BUILD_NUMBER}

char *__cdecl getBuildNumber();
int getBuildNumberAsInt();
")

# OpenAL Soft 1.25.2 declares `cmake_minimum_required(VERSION 3.13...3.31)` but
# links with $<BUILD_LOCAL_INTERFACE:alsoft::fmt>, a generator expression CMake
# only learned in 3.26. On anything older the build configures and generates
# and then dies with five copies of "Expression did not evaluate to a known
# generator expression", pointing at a line in a vendored file rather than at
# the version that is actually wrong. Fail here instead, with the fix.
if(CMAKE_VERSION VERSION_LESS 3.26)
    message(FATAL_ERROR
        "This build needs CMake 3.26 or newer; you have ${CMAKE_VERSION}.\n"
        "  Gradle:  sdkmanager --install \"cmake;3.30.5\"\n"
        "           then build with -Pkisak.cmake=3.30.5, or change the default in\n"
        "           ports/android/launcher/app/build.gradle.kts\n"
        "  Script:  put a newer cmake first on PATH before tools/configure-android.sh\n"
        "The requirement comes from third-party/openal-soft, not from the engine.")
endif()

# --- OpenAL Soft -------------------------------------------------------------
# Same release the iOS port and the Windows build use. The OpenSL ES backend
# is the one that works everywhere; AAudio is lower latency but only from API
# 26 and has historically had vendor bugs, so both are built and OpenAL picks
# at runtime.
set(LIBTYPE "STATIC")
set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_CONFIG OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_HRTF_DATA OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_AMBDEC_PRESETS OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ALSOFT_INSTALL_UTILS OFF CACHE BOOL "" FORCE)
set(ALSOFT_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(ALSOFT_BACKEND_OPENSL ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    openal
    GIT_REPOSITORY https://github.com/kcat/openal-soft.git
    GIT_TAG 1.25.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(openal)

add_subdirectory("${IOS_DIR}/zoneload" "${CMAKE_CURRENT_BINARY_DIR}/zoneload")

# --- Performance subsystem ---------------------------------------------------
# Free of engine headers on purpose: it is the one part of the port with
# meaningful logic that is not a translation of something, so it is unit
# tested on the host (tests/PerfTests.cpp, 125 assertions).
add_library(kisakcod_android_perf STATIC
    perf/frame_pacer.cpp
    perf/resolution_scaler.cpp
    perf/thermal_governor.cpp
    perf/cpu_topology.cpp
    perf/device_profile.cpp
    perf/perf_director.cpp
)
target_compile_features(kisakcod_android_perf PUBLIC cxx_std_17)
target_link_libraries(kisakcod_android_perf PUBLIC android log)

# --- Vulkan backend ----------------------------------------------------------
# Built apart from the engine because the engine forces a Windows prelude that
# defines BOOL, near and far as macros, and <vulkan/vulkan.h> does not survive
# that. The same reason the iOS port keeps its Metal backend separate.
add_library(kisakcod_android_gfx STATIC
    gfx/dx9_glsl_translator.cpp
    gfx/vulkan/vk_backend.cpp
    gfx/vulkan/pipeline_cache.cpp
)
target_compile_features(kisakcod_android_gfx PUBLIC cxx_std_17)
target_include_directories(kisakcod_android_gfx PRIVATE "${SHADERC_INCLUDE}")
# Without this <vulkan/vulkan.h> omits VkAndroidSurfaceCreateInfoKHR,
# vkCreateAndroidSurfaceKHR and VK_KHR_ANDROID_SURFACE_EXTENSION_NAME. It has
# to be a compile definition rather than a #define in one header: both
# vk_internal.h and pipeline_cache.h include vulkan.h, and whichever lands
# first wins the include guard.
target_compile_definitions(kisakcod_android_gfx PRIVATE VK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(kisakcod_android_gfx PUBLIC Vulkan::Vulkan android log PRIVATE "${SHADERC_LIB}")

# --- Video decoding ----------------------------------------------------------
# Separate for the same prelude reason: the NDK media headers use `near`.
add_library(kisakcod_android_video STATIC engine/video_player_android.cpp)
target_compile_features(kisakcod_android_video PUBLIC cxx_std_17)
target_link_libraries(kisakcod_android_video PUBLIC mediandk android log)

# --- Engine source list ------------------------------------------------------
include("${SCRIPTS_DIR}/common_files.cmake")
include("${SCRIPTS_DIR}/sp/sp_files.cmake")

set(KISAK_ENGINE_GROUPS
    SRCROOT DYNENTITY EFFECTSCORE AIM_ASSIST BGAME CGAME CLIENT COMMON DATABASE
    DEVGUI GAME GFX_D3D PHYSICS PHYSICS_ODE QCOMMON RAGDOLL SCRIPT SERVER SOUND
    STRINGED UI UNIVERSAL XANIM ODE ZLIB
)
set(KISAK_SP_SOURCES "")
foreach(group IN LISTS KISAK_ENGINE_GROUPS)
    list(APPEND KISAK_SP_SOURCES ${${group}})
endforeach()
list(FILTER KISAK_SP_SOURCES INCLUDE REGEX "\\.(c|cpp)$")

# Exactly the same exclusions the iOS port makes, for the same reasons.
set(KISAK_ENGINE_WINDOWS_ONLY
    # Miles Sound System; KISAK_OPENAL selects snd_driver_openal.cpp instead.
    "${SRC_DIR}/sound/snd_mss.cpp"
    # Hand-written SSE skinning; r_model_skin.cpp keeps the portable path.
    # NEON would be the arm64 equivalent and is worth doing later, but the
    # portable path is correct and the skinning is not the bottleneck.
    "${SRC_DIR}/gfx_d3d/r_model_skin_sse.cpp"
    # rdtsc calibration; ports/android/platform/android_timing.cpp replaces it.
    "${SRC_DIR}/universal/timing.cpp"
    # Bink SDK; engine/video_player_android.cpp plays the converted video/*.mp4.
    "${SRC_DIR}/gfx_d3d/r_cinematic.cpp"
)
list(REMOVE_ITEM KISAK_SP_SOURCES ${KISAK_ENGINE_WINDOWS_ONLY})

# Port sources shared by both engine libraries.
set(KISAK_ANDROID_PORT_SOURCES
    # Portable win32 files the Android build keeps.
    "${SRC_DIR}/win32/win_localize.cpp"
    "${SRC_DIR}/win32/win_storage.cpp"
    # Android replacements for the rest of src/win32.
    "${CMAKE_CURRENT_LIST_DIR}/engine/android_sys.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/engine/apple_bridge.cpp"
    # Gamepad state and the per-frame merge with the on-screen controls.
    "${CMAKE_CURRENT_LIST_DIR}/engine/android_input.cpp"
    # Builds the DeviceDescriptor the performance subsystem reasons about.
    "${CMAKE_CURRENT_LIST_DIR}/engine/device_bootstrap.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/android_timing.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/android_critical_sections.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/android_virtual_memory.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/android_storage.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/android_log.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/platform/android_engine_mode.cpp"
    # Shared with the iOS port, unchanged.
    # Win32 event objects. Android used to carry a renamed copy of this file;
    # it was identical line for line, and the iOS one is covered by
    # ports/ios/platform/tests. One implementation, one set of semantics.
    "${IOS_DIR}/platform/apple_events.cpp"
    "${IOS_DIR}/engine/controller_input.cpp"
    "${IOS_DIR}/engine/controller_icons.cpp"
    "${IOS_DIR}/engine/cinematic_apple.cpp"
    "${IOS_DIR}/engine/db_zoneload_apple.cpp"
    "${IOS_DIR}/compat/d3dx9shader_apple.cpp"
    "${IOS_DIR}/d3d9/d3d9_apple.cpp"
    # App shell.
    "${CMAKE_CURRENT_LIST_DIR}/app/jni_bridge.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/app/touch_controls.cpp"
)

set(KISAK_ANDROID_INCLUDES
    "${SRC_DIR}"
    "${DEPS_DIR}"
    # Supplies <gpu_backend.h>, which d3d9_apple.cpp includes with angle
    # brackets under #if defined(__ANDROID__). Quoted includes search the
    # including file's own directory first, so no -I ordering can redirect
    # its "metal/metal_backend.h" away from the Metal header next to it;
    # the shared file has to name the Vulkan interface explicitly.
    "${CMAKE_CURRENT_LIST_DIR}/gfx"
    "${IOS_DIR}/engine"
    "${IOS_DIR}/compat/native/windows"
    "${IOS_DIR}/compat/native/directx"
    "${KISAK_GENERATED_DIR}"
)

set(KISAK_ANDROID_COMPILE_OPTIONS
    "SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/compat/kisak_android_prelude.h"
    -fms-extensions
    # Decompiled code type-puns through pointers and relies on
    # two's-complement wraparound, which is what MSVC happened to do.
    -fno-strict-aliasing
    -fwrapv
    # Tens of thousands of warnings from the decompiled sources; errors are
    # what matter.
    -w
)

# Release flags. -O2 rather than -O3: the engine is bound by memory traffic
# and instruction cache, and -O3's extra inlining made the code larger without
# being faster. Section GC plus --gc-sections removes the Windows-only code
# the linker can prove is unreachable, which is a few megabytes of install.
set(KISAK_ANDROID_RELEASE_OPTIONS
    $<$<CONFIG:Release>:-O2>
    $<$<CONFIG:Release>:-ffunction-sections>
    $<$<CONFIG:Release>:-fdata-sections>
    $<$<CONFIG:Release>:-fno-math-errno>
)

function(kisak_android_engine target mode)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_compile_definitions(${target} PRIVATE
        ${mode} CINEMA USE_SEPARATE_BLIT_TEXTURE KISAK_OPENAL
        # The engine's own invariant checks stay on: this is a 64-bit port of
        # code written for 32 bits, and a silent wrong answer is much harder
        # to chase than an assert.
        RELEASE_ASSERTS
    )
    target_include_directories(${target} PRIVATE ${KISAK_ANDROID_INCLUDES})
    target_compile_options(${target} PRIVATE ${KISAK_ANDROID_COMPILE_OPTIONS} ${KISAK_ANDROID_RELEASE_OPTIONS})
    target_link_options(${target} PRIVATE
        $<$<CONFIG:Release>:-Wl,--gc-sections>
        # The engine resolves nothing lazily and a missing symbol at runtime
        # would surface as a crash minutes into a level.
        -Wl,--no-undefined
        -Wl,-z,max-page-size=16384
    )
    target_link_libraries(${target} PRIVATE
        OpenAL::OpenAL
        kisakcod_zoneload
        kisakcod_android_perf
        kisakcod_android_gfx
        kisakcod_android_video
        android log vulkan mediandk
    )
    # dr_wav and dr_mp3 implementations belong in exactly one translation
    # unit, as in scripts/sp/CMakeLists.txt.
    set_source_files_properties("${SRC_DIR}/sound/snd_driver_load_obj.cpp"
        TARGET_DIRECTORY ${target}
        PROPERTIES COMPILE_DEFINITIONS "DR_WAV_IMPLEMENTATION;DR_MP3_IMPLEMENTATION")
endfunction()

# --- Single player -----------------------------------------------------------
add_library(kisakcod_sp SHARED ${KISAK_SP_SOURCES} ${KISAK_ANDROID_PORT_SOURCES})
kisak_android_engine(kisakcod_sp KISAK_SP)

# --- Multiplayer -------------------------------------------------------------
option(KISAK_BUILD_MP "Also build the multiplayer engine library" ON)
if(KISAK_BUILD_MP)
    # sp_files.cmake appended the single-player sources to the same variable
    # names, so the groups are cleared and re-included for the MP file set.
    foreach(group IN LISTS KISAK_ENGINE_GROUPS)
        unset(${group})
    endforeach()
    unset(CGAME_MP)
    unset(CLIENT_MP)
    unset(GAME_MP)
    unset(SERVER_MP)
    unset(UI_MP)
    include("${SCRIPTS_DIR}/common_files.cmake")
    include("${SCRIPTS_DIR}/mp/mp_files.cmake")

    set(KISAK_MP_GROUPS
        SRCROOT DYNENTITY EFFECTSCORE AIM_ASSIST BGAME CGAME CGAME_MP CLIENT CLIENT_MP COMMON
        DATABASE DEVGUI GAME GAME_MP GFX_D3D PHYSICS PHYSICS_ODE QCOMMON RAGDOLL SCRIPT SERVER
        SERVER_MP SOUND STRINGED UI UI_MP UNIVERSAL XANIM ODE ZLIB
    )
    set(KISAK_MP_SOURCES "")
    foreach(group IN LISTS KISAK_MP_GROUPS)
        list(APPEND KISAK_MP_SOURCES ${${group}})
    endforeach()
    list(FILTER KISAK_MP_SOURCES INCLUDE REGEX "\\.(c|cpp)$")
    list(REMOVE_ITEM KISAK_MP_SOURCES ${KISAK_ENGINE_WINDOWS_ONLY})
    list(REMOVE_DUPLICATES KISAK_MP_SOURCES)

    add_library(kisakcod_mp SHARED
        ${KISAK_MP_SOURCES}
        ${KISAK_ANDROID_PORT_SOURCES}
        "${SRC_DIR}/client_mp/cl_cod4x.cpp"
        "${IOS_DIR}/network/cod4x_transport.cpp"
        "${IOS_DIR}/compat/steam_apple.cpp"
        # Sockets, HTTP downloads and the master server query: multiplayer only.
        "${CMAKE_CURRENT_LIST_DIR}/platform/android_net.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/platform/android_download.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/platform/android_master.cpp"
    )
    kisak_android_engine(kisakcod_mp KISAK_MP)
endif()
