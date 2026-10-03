#!/bin/bash
# Configures a native-only Android build of the engine libraries.
#
# This is the path for working on the C++ without Gradle in the way: it
# produces libkisakcod_sp.so and libkisakcod_mp.so and nothing else. The full
# app, including the launcher, is built with
#
#     cd ports/android/launcher && ./gradlew assembleRelease
#
# which drives the same CMakeLists.txt through the Android Gradle plugin.
#
# Requires an NDK (r26 or later; r27 is what the Gradle build pins) and
# shaderc built inside it - see docs/ANDROID.md.

set -euo pipefail

source_root="$(cd "$(dirname "$0")/.." && pwd)"
build_root="${KISAK_ANDROID_BUILD_DIR:-$source_root/build/android}"

ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
if [ -z "$ndk" ] && [ -n "${ANDROID_HOME:-}" ]; then
    # Pick the highest-numbered NDK the SDK has installed.
    ndk="$(find "$ANDROID_HOME/ndk" -maxdepth 1 -mindepth 1 -type d 2>/dev/null | sort -V | tail -1)"
fi
if [ -z "$ndk" ] || [ ! -d "$ndk" ]; then
    echo "No NDK found. Set ANDROID_NDK_HOME, or install one through the SDK manager." >&2
    exit 1
fi

toolchain="$ndk/build/cmake/android.toolchain.cmake"
if [ ! -f "$toolchain" ]; then
    echo "No toolchain file at $toolchain - is that really an NDK?" >&2
    exit 1
fi

# API 26 is the floor: AAudio, the behaviour the Vulkan loader guarantees and
# the display-rate controls all land there.
api="${KISAK_ANDROID_API:-26}"
build_type="${KISAK_ANDROID_BUILD_TYPE:-Release}"

shaderc_dir="$ndk/sources/third_party/shaderc"
if [ ! -d "$shaderc_dir/libs" ]; then
    cat >&2 <<EOF
shaderc is present in the NDK but has not been built. The renderer compiles
the translated GLSL to SPIR-V with it at load time, so it is required.

    cd "$shaderc_dir"
    "\$ANDROID_NDK_HOME/ndk-build" NDK_PROJECT_PATH=. \\
        APP_BUILD_SCRIPT=Android.mk APP_STL=c++_static \\
        APP_ABI=arm64-v8a APP_PLATFORM=android-$api libshaderc_combined

That takes a few minutes once and then never again.
EOF
    exit 1
fi

cmake -S "$source_root" -B "$build_root" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$api" \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DKISAK_BUILD_MP=ON \
    -DFETCHCONTENT_SOURCE_DIR_OPENAL="$source_root/third-party/openal-soft"

printf '\nConfigured in %s\n' "$build_root"
printf 'Build with: cmake --build %s --parallel\n' "$build_root"
