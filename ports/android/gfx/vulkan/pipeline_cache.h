#pragma once

// Shader compilation and the on-disk pipeline cache.
//
// A cold launch of the game translates and compiles several hundred shaders
// and creates a pipeline for every state combination the first level uses.
// On a mid-range phone that is multiple seconds of stalls spread through the
// first minute of play - exactly the period where a dropped frame is most
// visible. Both halves are therefore cached on disk:
//
//   * VkPipelineCache blob, written to the app cache directory. The driver
//     reuses its own compiled code and pipeline creation becomes close to
//     free on the second run.
//   * A SPIR-V cache keyed by the hash of the GLSL, so shaderc only runs for
//     shaders never seen before. shaderc is the slower of the two by a wide
//     margin.
//
// The cache is keyed by driver version and device, because a driver update
// invalidates both.

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace kisak::vk::pipeline_cache {

// Points the cache at the app's getCacheDir(). Must be called before
// Initialize(); with an empty path the caches stay in memory only.
void SetDirectory(const std::string &path);

// Loads the driver blob, validating it against this device and driver. A
// mismatch returns a fresh empty cache rather than risking a driver crash on
// a foreign blob - some Mali versions do not validate the header themselves.
VkPipelineCache Load(VkDevice device, const std::string &directory, const VkPhysicalDeviceProperties &properties);

// Writes the blob back. Called on activity pause, because an app killed from
// the recents list never reaches Shutdown().
void Save(VkDevice device, VkPipelineCache cache, const std::string &directory);

// GLSL 4.50 to SPIR-V through shaderc, with the disk cache in front.
bool CompileGlsl(const std::string &source, bool pixel, std::vector<uint32_t> &spirv);

// Number of shaders compiled this run versus served from the cache, for the
// startup log line.
void GetShaderStats(uint32_t *compiled, uint32_t *cached);

} // namespace kisak::vk::pipeline_cache
