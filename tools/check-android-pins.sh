#!/bin/bash
# Checks that the tool versions pinned in the Gradle build and the ones the CI
# workflow installs are the same.
#
# They are declared twice because neither file can read the other: Gradle
# needs the NDK and CMake versions at configure time, and the workflow needs
# them before Gradle exists to ask. When they drift, the failure is slow and
# confusing - Gradle downloads a second NDK mid-build, or stops with
# "NDK not configured" after the host tests have already passed.
#
# Run it from anywhere; it is also the first thing the APK workflow does.

set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
gradle_app="$root/ports/android/launcher/app/build.gradle.kts"
gradle_root="$root/ports/android/launcher/build.gradle.kts"
workflow="$root/.github/workflows/android-apk.yml"

for f in "$gradle_app" "$gradle_root" "$workflow"; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done

status=0

# extract <file> <regex>  -> first capture group, or empty
extract() {
    sed -n -E "s/.*$2.*/\1/p" "$1" | head -1
}

compare() {
    local label="$1" want="$2" got="$3"
    if [ -z "$want" ] || [ -z "$got" ]; then
        printf '  %-18s COULD NOT READ (gradle=%q workflow=%q)\n' "$label" "$want" "$got"
        status=1
    elif [ "$want" != "$got" ]; then
        printf '  %-18s MISMATCH  gradle=%s  workflow=%s\n' "$label" "$want" "$got"
        status=1
    else
        printf '  %-18s %s\n' "$label" "$want"
    fi
}

echo "Android version pins:"

compare "NDK" \
    "$(extract "$gradle_app" 'ndkVersion = "([^"]+)"')" \
    "$(extract "$workflow" 'NDK_VERSION: "([^"]+)"')"

sdk_cmake_gradle="$(extract "$gradle_app" 'sdkCmakeVersion.*\?: "([0-9][0-9.]*)"')"
compare "SDK CMake" \
    "$sdk_cmake_gradle" \
    "$(extract "$workflow" 'SDK_CMAKE_VERSION: "([^"]+)"')"

# third-party/openal-soft links through $<BUILD_LOCAL_INTERFACE:...>, added in
# CMake 3.26. AGP's default is 3.22.1, so this is easy to regress by deleting
# the pin and never noticing until generate time.
if [ -n "$sdk_cmake_gradle" ] &&
   [ "$(printf '3.26\n%s\n' "$sdk_cmake_gradle" | sort -V | head -1)" != "3.26" ]; then
    printf '  %-18s %s is below the 3.26 OpenAL Soft needs\n' "SDK CMake floor" "$sdk_cmake_gradle"
    status=1
fi

compare "compileSdk" \
    "$(extract "$gradle_app" 'compileSdk = ([0-9]+)')" \
    "$(extract "$workflow" 'COMPILE_SDK: "([0-9]+)"')"

compare "minSdk" \
    "$(extract "$gradle_app" 'minSdk = ([0-9]+)')" \
    "$(extract "$workflow" 'ANDROID_API: "([0-9]+)"')"

# AGP does not have to equal the Gradle version, but it does set a floor:
# AGP 8.7 needs Gradle 8.9 or newer. Checked as a floor, not an equality.
agp="$(extract "$gradle_root" 'com\.android\.application"\) version "([^"]+)"')"
gradle_version="$(extract "$workflow" 'GRADLE_VERSION: "([^"]+)"')"
if [ -z "$agp" ] || [ -z "$gradle_version" ]; then
    echo "  AGP/Gradle         COULD NOT READ"
    status=1
else
    case "$agp" in
        8.7.*) floor="8.9" ;;
        8.8.*) floor="8.10.2" ;;
        8.9.*) floor="8.11.1" ;;
        *)     floor="" ;;
    esac
    if [ -z "$floor" ]; then
        printf '  %-18s AGP %s is not in this script'"'"'s table - add it\n' "AGP/Gradle" "$agp"
        status=1
    elif [ "$(printf '%s\n%s\n' "$floor" "$gradle_version" | sort -V | head -1)" != "$floor" ]; then
        printf '  %-18s AGP %s needs Gradle >= %s, workflow installs %s\n' \
            "AGP/Gradle" "$agp" "$floor" "$gradle_version"
        status=1
    else
        printf '  %-18s AGP %s with Gradle %s (floor %s)\n' "AGP/Gradle" "$agp" "$gradle_version" "$floor"
    fi
fi

# The engine is a 64-bit port; a second ABI here would mean a 32-bit build was
# reintroduced, which ports/android/CMakeLists.txt refuses anyway.
abis="$(grep -c 'abiFilters' "$gradle_app" || true)"
if [ "$abis" != "1" ] || ! grep -q 'abiFilters += "arm64-v8a"' "$gradle_app"; then
    echo "  ABI                EXPECTED exactly: abiFilters += \"arm64-v8a\""
    status=1
else
    echo "  ABI                arm64-v8a"
fi

if [ "$status" = 0 ]; then
    echo "All pins agree."
else
    echo
    echo "Fix the mismatches above: $gradle_app and $workflow must agree." >&2
fi
exit "$status"
