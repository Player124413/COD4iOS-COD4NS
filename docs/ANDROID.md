# Android port

A port of the engine to 64-bit Android phones, built to hold a stable 60 fps
on anything from a 2019 mid-range part upwards.

No game content is included and none is downloaded. You supply your own Call
of Duty 4 PC installation, exactly as the iOS port requires.

---

## What exists

| Area | Where | Notes |
| --- | --- | --- |
| Performance subsystem | `ports/android/perf/` | Frame pacing, dynamic resolution, thermal response, CPU placement, per-device profiles. Unit tested on the host. |
| `Sys_*` platform layer | `ports/android/platform/` | Timing, critical sections, virtual memory, events, storage, sockets, HTTP downloads, master server query. |
| Engine entry point | `ports/android/engine/android_sys.cpp` | Replaces `src/win32`. Owns the paced frame loop. |
| Direct3D 9 → Vulkan | `ports/android/gfx/` | Shader translator, Vulkan backend, pipeline cache. The 2000-line D3D9 object model from the iOS port is reused unmodified. |
| Cutscenes | `ports/android/engine/video_player_android.cpp` | MediaCodec, behind the same C API the iOS AVFoundation player implements. |
| App shell | `ports/android/app/` | JNI bridge and touch controls. |
| Launcher | `ports/android/launcher/` | Kotlin. Game data import, mode selection, performance settings. |

Shared with the iOS port rather than duplicated: the Direct3D 9 layer
(`ports/ios/d3d9/d3d9_apple.cpp`), controller input and HUD glyphs, the
fastfile loader (`ports/ios/zoneload/`), cinematic playback, the touch control
state machine and the CoD4X network transport. Four symbols needed a name
forward, which is all of `ports/android/engine/apple_bridge.cpp`.

---

## Targets

* **ABI:** `arm64-v8a` only. The engine is a 64-bit port of 32-bit Windows
  code; a 32-bit build would reintroduce the pointer-size bugs the port exists
  to fix, with none of the benefit.
* **minSdk 26** (Android 8.0). Newer platform features — AChoreographer (29),
  thermal status (29/30), ADPF performance hints (33) — are resolved with
  `dlsym` at runtime and simply do not engage on older devices.
* **Graphics:** Vulkan 1.0. There is no GLES fallback in this tree; a device
  with no Vulkan driver is below the performance floor the port targets
  anyway.

---

## Building

### Prerequisites

* Android Studio (Ladybug or newer) or the command-line SDK tools
* NDK r27 (`27.2.12479018` is what the Gradle build pins)
* JDK 17
* CMake 3.26+ and Ninja. Install the SDK's with
  `sdkmanager --install "cmake;3.30.5"`. **Not** AGP's default 3.22.1:
  `third-party/openal-soft` links through `$<BUILD_LOCAL_INTERFACE:...>`,
  which CMake only learned in 3.26, and an older one fails at generate time
  with several copies of "Expression did not evaluate to a known generator
  expression". `ports/android/CMakeLists.txt` checks the version up front and
  says so.

### shaderc

The renderer translates the game's Direct3D 9 shader bytecode to GLSL and
compiles it to SPIR-V at load time with shaderc, which ships as source inside
the NDK and has to be built once:

```sh
cd "$ANDROID_NDK_HOME/sources/third_party/shaderc"
"$ANDROID_NDK_HOME/ndk-build" NDK_PROJECT_PATH=. \
    APP_BUILD_SCRIPT=Android.mk APP_STL=c++_static \
    APP_ABI=arm64-v8a APP_PLATFORM=android-26 libshaderc_combined
```

That takes a few minutes and then never again. The CMake configure step fails
with this exact command in the error message if it is missing.

### The app

The repository does not carry a Gradle wrapper, so use an installed Gradle
8.9 or newer (AGP 8.7 requires it), or open `ports/android/launcher` in
Android Studio, which generates a wrapper for you:

```sh
cd ports/android/launcher
gradle assembleRelease
```

The APK lands in `ports/android/launcher/app/build/outputs/apk/release/`.
Gradle drives the same `CMakeLists.txt` the script below uses, so there is one
build description, not two.

Three properties are worth knowing:

| Property | Default | What it does |
| --- | --- | --- |
| `-Pkisak.mp=false` | `true` | Skips the multiplayer library. SP and MP are separate compiles of the whole engine, so this halves the build. |
| `-Pkisak.ccache=true` | `false` | Routes the native compile through `ccache`. Off by default because a machine without it would fail at configure time. |
| `-Pkisak.cmake=<ver>` | `3.30.5` | Which SDK CMake to use. The floor is 3.26, see Prerequisites. |

The release build is always signed, so the APK is installable. Set
`KISAK_KEYSTORE`, `KISAK_KEYSTORE_PASSWORD`, `KISAK_KEY_ALIAS` and
`KISAK_KEY_PASSWORD` in the environment to sign with a real key; with none of
them set it falls back to the debug key, which works for sideloading but
changes between machines.

### Native only

For working on the C++ without Gradle in the loop:

```sh
bash tools/configure-android.sh
cmake --build build/android --parallel
```

This produces `libkisakcod_sp.so` and `libkisakcod_mp.so` and nothing else.

### Host tests

The parts of the port that are plain C++ — the performance subsystem, the
shader translator, and the gamepad/touch input merge — are tested on any
machine with a C++17 compiler:

```sh
sudo apt-get install -y ninja-build zlib1g-dev
cmake -S . -B build/host -G Ninja -DKISAK_BUILD_PORT_TESTS=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/host --parallel \
    --target kisakcod_android_perf_tests \
    --target kisakcod_android_shader_tests \
    --target kisakcod_android_input_tests
ctest --test-dir build/host -R android_ --output-on-failure
```

zlib is a configure-time dependency of `ports/ios/assets`, which is pulled in
by `KISAK_BUILD_PORT_TESTS` even though these three targets do not use it.
Building the whole tree instead of the three targets also builds the engine
core, which needs GCC 13 or newer for `<format>`.

### CI

`.github/workflows/android-apk.yml` builds a signed, installable arm64 APK on
every push that touches the port, and on demand from the Actions tab. It runs
the host tests first as a two-minute gate, then:

* installs NDK `27.2.12479018`, SDK CMake 3.30.5 and Gradle 8.10.2, and
  checks the CMake it got is really 3.26 or newer,
* builds shaderc inside the NDK and caches it against the NDK version,
* compiles the engine through `ccache`, cached between runs,
* checks the resulting APK actually contains `libkisakcod_sp.so` (and
  `libkisakcod_mp.so`) for `arm64-v8a` and nothing for any other ABI, and
  prints the signing certificate,
* uploads the APK, the R8 mapping and the native debug symbols.

The APK is signed with a debug key unless the repository has these secrets,
in which case it is signed properly:

| Secret | Contents |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | `base64 -w0 release.jks` |
| `ANDROID_KEYSTORE_PASSWORD` | Keystore password |
| `ANDROID_KEY_ALIAS` | Key alias inside the keystore |
| `ANDROID_KEY_PASSWORD` | Key password |

The NDK and SDK CMake versions appear in both the Gradle build and the
workflow. `tools/check-android-pins.sh` fails the build if they disagree; run
it locally after changing either.

---

## Game data

The launcher asks for a folder and copies out only what the engine reads:

```
main/*.iwd            base archives
main/<mod>/*.iwd|.ff  mods and custom maps
zone/<language>/*.ff  fastfiles
localization.txt      names the language
video/*.mp4|.mp3      converted cutscenes, if you made them
```

Everything else in a retail install — Windows binaries, the original Bink
movies, the patcher — is skipped, which is several gigabytes not copied.

The files go to app-private internal storage (`filesDir/cod4`). That is not a
preference: scoped storage means a `content://` URI is not a path, and the
engine's file layer opens paths. An interrupted import resumes rather than
restarting, and nothing is ever written outside the app sandbox.

### Cutscenes

Bink has no Android runtime, so convert the movies first:

```sh
ports/android/scripts/convert_videos.sh /path/to/cod4
```

Movies with no converted file report finished immediately — the same outcome a
failed `BinkOpen` produced — so menus and level transitions carry on.

---

## How 60 fps is held

The target is not "fast on a flagship". It is **60 fps sustained on a mid-range
part, after twenty minutes, when the phone is warm**. Those are different
problems, and the second one is the one that is usually lost.

### 1. The frame rate is paced, not maximised

`ports/android/perf/frame_pacer.{h,cpp}`

The loop does not render as fast as it can. It predicts the next frame's cost
from a **p95** of recent frames — not the mean, which systematically
under-reserves — sleeps until roughly 12% before the deadline, then spins for
the last 1.2 ms because `nanosleep` cannot be trusted at that granularity.

Rendering faster than the display refreshes produces frames the compositor
discards while the SoC heats up, and the heat is what costs the frame rate ten
minutes later. `com_maxfps` is set to 0 after `Com_Init` so the engine's own
limiter does not beat against this one.

### 2. Resolution gives way before frame rate does

`ports/android/perf/resolution_scaler.{h,cpp}`

When a frame goes over 92% of its budget the render resolution drops a step
(0.05, floor 0.60); below 75% it climbs back, with a 10-frame dwell down and a
45-frame confirmation up so the picture does not pulse. On a sharp drop it
jumps straight to `scale / sqrt(load / threshold)` rather than stepping, because
walking down one step per 10 frames through a grenade is ten dropped frames.

Scaling only engages when the GPU is the bottleneck. Dropping resolution on a
CPU-bound frame costs image quality and buys nothing.

### 3. Thermals are planned for, not reacted to

`ports/android/perf/thermal_governor.{h,cpp}`

`PowerManager`'s thermal status and `getThermalHeadroom()` drive a ceiling on
resolution and, only when that is exhausted, on the target frame rate:

| Status | Resolution ceiling | Frame rate |
| --- | --- | --- |
| Light | 0.90 | 60 |
| Moderate | 0.75 | 60 |
| Severe | floor | 45 |
| Critical+ | floor | floor |

Headroom ≥ 0.97 is treated as Severe before the status flag catches up.
Recovery is one step per 600 frames, and frame rate is restored before
resolution — a softer picture at 60 is better than a sharp one at 45.

### 4. Threads are placed on the right cores

`ports/android/perf/cpu_topology.{h,cpp}`

Clusters are discovered from `cpu_capacity`, falling back to
`cpuinfo_max_freq`. The render thread is pinned to the prime and big cores,
workers to the big cores, audio and background to the little ones. Left alone,
the scheduler migrates the render thread to a little core during a quiet frame
and the next busy frame starts at 1/3 the throughput.

### 5. The device is profiled before it is trusted

`ports/android/perf/device_profile.{h,cpp}`

GPU model, RAM, core counts and maximum frequency pick a quality tier and a
starting resolution. Every tier targets 60 fps; what differs is what is spent
to get there. Unknown hardware gets Medium, which is recoverable in both
directions.

### 5b. One input path, two sources

`ports/android/engine/android_input.cpp`

Android has no equivalent of iOS's GameController callback, so key and motion
events are accumulated from the UI thread and sampled once per frame by the
engine thread. A physical pad and the on-screen controls are never summed: a
thumb resting in the stick region would fight a centred stick. The pad wins
whenever one is attached, and the overlay hides itself.

Two details are worth knowing before debugging it:

* Android's Y axis is down-positive and the engine's is up-positive, so both
  sticks are negated on the way in.
* `Snapshot::connected` means "this input source is live", not "a physical pad
  is attached" — the shared controller code drops the whole snapshot when it
  is false. Touch samples set it too; `Snapshot::touch` is what distinguishes
  them.

Keycode and axis translation lives in Kotlin (`GamepadInput.kt`), so the
native side is handed a bit position in `kisak::controller::Button` and never
sees an Android keycode. D-pad hat releases are synthesised there, because
returning the hat to centre emits no `KeyEvent`.

### 6. Shader and pipeline compilation is paid once

`ports/android/gfx/vulkan/pipeline_cache.{h,cpp}`

Translated SPIR-V and the driver's `VkPipelineCache` are both written to the
app cache directory, stamped with the driver version and device UUID so a
system update invalidates them. Without this, a few hundred pipeline creations
land as multi-second hitches during the first match, every launch. The cache is
written on activity pause, because an app swiped out of recents never reaches
`onDestroy`.

### 7. The renderer is written for a tiler

`ports/android/gfx/vulkan/`

* Depth is `STORE_OP_DONT_CARE` — the tile depth buffer is never written back.
* Clears are `vkCmdClearAttachments` inside the pass, which is tile-local, not
  `vkCmdClearColorImage`, which flushes and refills every tile.
* Render passes stay open across the draws that share a target set.
* All buffers are host-visible: every mobile GPU is a unified-memory design,
  so a staging copy is a copy for nothing.
* Two frames in flight, not three. Three hides jitter the pacer already
  handles and costs 16.7 ms of input latency, which is very noticeable with a
  touch control scheme.

### 8. The shader translator does not leave work for the driver

`ports/android/gfx/dx9_glsl_translator.{h,cpp}`

`def` constants are folded as literals rather than uploaded. Only the constant
registers a shader actually declares are copied per draw, not all 256. The
alpha test is a push constant rather than a specialisation constant, which
would have multiplied the pipeline count by eight. Relative constant addressing
is clamped, because Direct3D 9 returned zero out of range and Vulkan faults.

---

## What has been verified, and what has not

**Verified**, by running it:

* `ports/android/perf/` — 125 assertions, `ctest -R android_perf`
* `ports/android/gfx/dx9_glsl_translator.cpp` — 93 assertions,
  `ctest -R android_shader_translator`, covering write masks, swizzles, source
  modifiers, `def` folding, flow control nesting, loop registers and relative
  addressing, shadow and cube samplers, missing varyings, integer attributes,
  multiple render targets, `vPos`/`vFace`, and rejection of malformed input.
* `ports/android/engine/android_input.cpp` — 51 assertions,
  `ctest -R android_input_merge`, running the real merge against a stubbed
  engine: axis sign and clamping, trigger-to-button thresholds, out-of-range
  button indices, state reset on connect and disconnect, label coverage, and
  when the on-screen controls appear.

**Not verified** — no NDK, JDK, Gradle or Android device was available in the
environment this was written in:

* The native cross-compile. The CMake is written against the NDK toolchain but
  has not been run through it. `.github/workflows/android-apk.yml` is the
  first thing that will: its host-test job has been run locally against this
  tree, but the APK job has not, because the environment this was written in
  has no NDK, JDK or Android SDK.
* The Vulkan backend, the JNI bridge, the MediaCodec video player and the
  Kotlin launcher have not been compiled. The JNI and Kotlin sides have been
  checked against each other by name — all 32 `external fun native*`
  declarations have exactly one matching `JNI_METHOD`, and every
  `GetMethodID` callback has a matching Kotlin method — but a name match is
  not a compile.
* No controller has been plugged into anything. The merge logic is tested;
  the keycode table in `GamepadInput.kt` is not, and a pad that reports its
  buttons unusually will need that table extended.
* No frame has been rendered and no frame rate has been measured. The
  performance work above is reasoned from how these parts behave, and the
  numbers in the tables are the policy the code implements — not measurements.

Expect the first NDK build to surface compile errors, mostly around the
Windows compatibility prelude meeting NDK headers. That is the same class of
problem the iOS port solved, and `ports/android/compat/kisak_android_prelude.h`
is where it is handled.

---

## Following the iOS port

The engine in `src/` is shared. Android reuses the iOS port wholesale where
the code is portable: the whole 64-bit zone loader, the controller model,
the cutscene glue, the Direct3D 9 shim and the CoD4X transport are compiled
straight out of `ports/ios`. What `ports/android` adds is only what the
platform forces: a Vulkan backend in place of Metal, a Kotlin/JNI shell in
place of UIKit, MediaCodec in place of AVFoundation, and the performance
director.

The dangerous part is the shared engine. Every `#ifdef __APPLE__` in `src/`
is a fork where iOS takes the ported path and Android silently keeps the
original 32-bit Windows one, and nothing fails at build time. That is what
caused the zone-loader crash, the dead controller look input and the loaded
sound crash.

So the default is inverted: shared code should say
`#if defined(__APPLE__) || defined(__ANDROID__)`. Anything that must stay
Apple-only is listed in `APPLE_ONLY_POLICY` in
`tools/check-android-sources.py` with a reason and a count, and a new
unclassified guard fails CI.

Not every guard is safe to flip. The iPhone canvas reshaping in
`src/ui/ui_atoms.cpp` collapsed the entire Android main menu when it was
enabled, so anything tuned to a specific screen has to be validated on a
device before it is shared.

## Logs and crashes

The engine writes a plain text log to internal storage:

```
/data/data/ovh.kisak.cod4/files/logs/kisakcod-log.txt
kisakcod-log-previous.txt
```

Internal storage rather than the shared volume, because the crash handler
writes from a signal handler on a process that is already dying, and the
emulated FUSE volume can block there. The launcher is the way in: **View log**
offers Copy, Share and Save to Downloads, and switches between the last run
and the one before it. The launcher also says so plainly on its front page
when the previous run ended in a crash.

The file is rotated when the engine starts, not when the launcher opens, so a
log from a run that crashed is still there after the player goes back to look
at it.

### What a crash looks like

`KisakAndroid_InstallCrashHandler` (`ports/android/platform/android_log.cpp`)
catches `SIGSEGV`, `SIGBUS`, `SIGFPE`, `SIGILL`, `SIGABRT`, `SIGTRAP` and
`SIGSYS`, appends a report, and then re-raises the signal so the system still
writes its own tombstone:

```
==== CRASH ====
signal:  SIGSEGV (invalid memory access)
code:    1
address: 0x0
thread:  kisak-engine tid 12345
backtrace:
  #00  0x7a1c3f4190  /data/app/.../libkisakcod_sp.so+0x1c4190
  ...
==== END CRASH ====
```

Writes go to a raw file descriptor, never stdio: a signal can arrive inside
an `fwrite`, and re-entering stdio from the handler deadlocks on its lock. An
alternate signal stack is installed **per thread**, because `sigaltstack` is
per thread while `sigaction` is not, and the engine thread is both the one
that recurses deeply enough to overflow and the one that would otherwise have
nowhere to run the handler.

### Turning a backtrace into source lines

Release libraries are stripped, so `dli_sname` is usually empty and the useful
part of each frame is the library plus the offset. Keep the unstripped
`.so` that the build produces and feed it the offset:

```
llvm-symbolizer --obj=app/build/intermediates/cxx/Release/*/obj/arm64-v8a/libkisakcod_sp.so 0x1c4190
```

The offset in the log is already relative to the library's load address, so it
goes in as printed - no arithmetic with `/proc/self/maps` needed.

## Licensing

GPL v3, as the rest of the repository. No Activision assets or code are
included. The touch control layout is adapted from MC360-Recomp under
BSD-3-Clause; see `ports/ios/app/TouchControls.LICENSE`.
