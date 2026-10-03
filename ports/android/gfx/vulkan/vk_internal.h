#pragma once

// Internal types for the Vulkan backend. Separated from gpu_backend.h because
// that header is included by the shared Direct3D 9 layer, which is compiled
// with the engine's Windows prelude - and <vulkan/vulkan.h> does not survive a
// translation unit where BOOL, near and far are macros.

#include "../gpu_backend.h"
// Shader reflection: ShaderSemantic, ShaderSampler and TranslatedShader are
// declared by the translator, and Shader below stores them.
#include "../dx9_glsl_translator.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace kisak::vk {

// How many frames the CPU may run ahead of the GPU.
//
// Two is the right number for a 60 fps phone. Three adds a frame of latency
// (16.7 ms, which is very noticeable with a touch control scheme) in exchange
// for absorbing jitter that the frame pacer already absorbs by predicting cost
// and starting late. One would serialise CPU and GPU and halve the throughput.
inline constexpr uint32_t kFramesInFlight = 2;

// Transient per-frame allocations (dynamic vertex data, uniform updates) come
// out of a ring rather than fresh allocations. 8 MB per frame covers the
// heaviest frames the engine produces, including the HUD, which draws
// everything through DrawPrimitiveUP.
inline constexpr VkDeviceSize kUploadRingBytes = 8 * 1024 * 1024;

struct MemoryBlock
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    void *mapped = nullptr;
    uint32_t typeIndex = 0;
};

struct Texture
{
    TextureDesc desc;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    // Per-level, per-face views, created lazily: a render target binding names
    // one mip of one face, and a view over the whole image cannot express that.
    std::unordered_map<uint32_t, VkImageView> targetViews;
    MemoryBlock memory;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    // Tracked so barriers can be emitted without a full render-graph: the
    // engine's usage pattern is simple enough that per-image state is exact.
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool isDepth = false;
    bool isSwapchainProxy = false;
    uint32_t swapchainIndex = 0;
};

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    MemoryBlock memory;
    VkDeviceSize size = 0;
    // Host-visible staging for the engine's lock/unlock pattern. The engine
    // locks a vertex buffer, writes with memcpy and unlocks, hundreds of times
    // per frame; a device-local buffer with a staging copy per unlock would be
    // slower than host-visible memory on every mobile part, all of which are
    // unified-memory designs where "device local" and "host visible" describe
    // the same physical RAM.
    void *mapped = nullptr;
};

struct Shader
{
    VkShaderModule module = VK_NULL_HANDLE;
    bool pixel = false;
    std::vector<ShaderSemantic> inputs;
    std::vector<ShaderSemantic> outputs;
    std::vector<ShaderSampler> samplers;
    uint32_t constantCount = 0;
    // Bit per varying the vertex shader writes, so a fragment shader can be
    // told which of its inputs will actually be fed.
    uint32_t writtenVaryings = 0;
    // SPIR-V kept so a pipeline can be rebuilt after a device loss without
    // re-running the translator.
    std::vector<uint32_t> spirv;
    uint64_t sourceHash = 0;
};

struct VertexLayout
{
    std::vector<VertexElement> elements;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    uint32_t unsignedIntMask = 0;
    uint32_t signedIntMask = 0;
    uint64_t hash = 0;
};

// Everything that distinguishes one VkPipeline from another, packed so the
// cache key is a hash of a POD rather than a walk over the draw state.
struct PipelineKey
{
    uint64_t vertexShader = 0;
    uint64_t pixelShader = 0;
    uint64_t layout = 0;
    uint64_t renderPass = 0;
    uint32_t renderState[6] = {};
    uint8_t primitive = 0;
    uint8_t colorTargets = 0;
    uint8_t sampleCount = 1;
    uint8_t pad = 0;

    bool operator==(const PipelineKey &other) const
    {
        return std::memcmp(this, &other, sizeof(PipelineKey)) == 0;
    }
};

struct PipelineKeyHash
{
    std::size_t operator()(const PipelineKey &key) const
    {
        // FNV-1a over the packed key. Not cryptographic; collisions would show
        // as the wrong pipeline, so the map stores the key and compares it.
        const auto *bytes = reinterpret_cast<const uint8_t *>(&key);
        std::size_t hash = 1469598103934665603ull;
        for (std::size_t i = 0; i < sizeof(PipelineKey); ++i)
        {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }
};

// One frame's worth of per-frame resources.
struct FrameContext
{
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkSemaphore rendered = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Upload ring.
    VkBuffer uploadBuffer = VK_NULL_HANDLE;
    MemoryBlock uploadMemory;
    VkDeviceSize uploadOffset = 0;

    // Timestamp queries bracketing the frame.
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    bool timestampsWritten = false;

    // Objects destroyed while this frame was in flight, released once its
    // fence signals. Freeing immediately would destroy resources the GPU is
    // still reading.
    std::vector<std::function<void()>> deferredDeletes;
};

} // namespace kisak::vk
