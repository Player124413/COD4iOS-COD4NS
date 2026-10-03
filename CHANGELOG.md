# Changelog

## Unreleased — Android port

- Add an Android port under `ports/android/`: `Sys_*` platform layer, a Vulkan
  backend for the existing Direct3D 9 layer, a Direct3D 9 shader-model-3 to
  GLSL translator, MediaCodec cutscene playback, touch controls, a JNI bridge
  and a Kotlin launcher. Targets arm64-v8a, minSdk 26, Vulkan 1.0.
- Add a performance subsystem (`ports/android/perf/`) built around holding 60
  fps rather than peaking: p95 frame-cost prediction with sleep-then-spin
  pacing, dynamic resolution that gives way before frame rate does, a thermal
  governor driven by `PowerManager` status and headroom, cluster-aware thread
  placement and per-device quality profiles. 125 host assertions.
- Share the Direct3D 9 object model, controller input, HUD glyphs, the
  fastfile loader, cinematic playback and the touch-control state machine
  between the iOS and Android ports instead of duplicating them; four symbols
  are forwarded by `ports/android/engine/apple_bridge.cpp`.
- Cache translated SPIR-V and the driver's `VkPipelineCache` on disk, stamped
  with the driver version and device UUID, so shader and pipeline compilation
  is paid once rather than on every cold start.
- Add `tools/configure-android.sh`, `ports/android/scripts/convert_videos.sh`
  and `docs/ANDROID.md`.
- Retail game data is still never shipped or downloaded: the launcher imports
  the player's own PC installation into app-private storage.

Nothing in the Android port has been compiled against the NDK or run on a
device. The performance subsystem and the shader translator are covered by
host tests (`ctest -R android`); everything else is unverified. See the
"What has been verified" section of `docs/ANDROID.md`.

## 1.0.3 (build 4) — 3 October 2026

- Make multiplayer browser filtering work with empty Documents using 26 independently verified public server endpoints.
- Cache explicit authentication rejection and let recent local outcomes override the bundled baseline. Unknown servers are hidden by default; a successful direct join can add one locally.
- Default new iOS multiplayer profiles to `cl_maxpackets 125` and `snaps 40`, retaining `rate 25000` and `cl_packetdup 1`.
- Include the current campaign and multiplayer engines with all earlier port corrections, including the CoD4x wire/Huffman transport, mod/custom-map download handling, spawn/camera/HUD work, touch/controller input, aim assist, sprint/scoreboard toggles, audio cleanup, save handling and frame-rate changes.

## 1.0.2 (build 3) — 3 October 2026

- Bootstrap the public `cod4x_patchv2.ff` dependency from a pinned official revision, verify SHA-256, preserve an existing mismatched patch and recover other language copies locally.
- Send the same persistent per-install GUID in the non-Steam challenge and connection packets; generate new Apple identities with system randomness.

These notes describe included changes, not universal gameplay certification. Steam/official-client authentication is not implemented. Gameplay on every campaign mission and every server has not been verified.
