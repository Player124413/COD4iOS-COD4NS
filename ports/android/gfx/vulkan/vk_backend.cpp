// Vulkan backend for the Direct3D 9 layer on Android.
//
// Implements ports/android/gfx/gpu_backend.h, which the shared D3D9 object
// model (ports/ios/d3d9/d3d9_apple.cpp) calls through the `gpu::` alias.
//
// Design notes that are not obvious from the code:
//
//  * The engine's renderer is a Direct3D 9 immediate-mode consumer: it sets
//    state, binds resources and draws, with no notion of render passes or
//    barriers. Vulkan requires both. Rather than build a render graph, the
//    backend opens a render pass lazily on the first draw into a given target
//    set and closes it when the targets change or the frame ends. That matches
//    how the engine actually batches - it sets targets a handful of times per
//    frame - and keeps tile memory resident across the draws that matter.
//
//  * Pipelines are created on demand and cached by a packed state key, backed
//    by a VkPipelineCache serialised to disk. A cold first run on a mid-range
//    phone compiles a few hundred pipelines; without the disk cache that cost
//    is paid at every launch and shows up as multi-second hitches during the
//    first match.
//
//  * All memory is host-visible. Every mobile GPU is a unified memory design,
//    so a staging copy buys nothing and costs a copy. The exception is images,
//    which are device-local and tiled, and do go through a staging buffer.
//
//  * Nothing here calls into the engine. Logging goes to logcat directly,
//    because the renderer initialises before Com_Init.

#include "vk_internal.h"
#include "../dx9_glsl_translator.h"
#include "pipeline_cache.h"

#include <android/log.h>

#include "../../platform/android_log.h"
#include <android/native_window.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>

#define LOGI(...) KISAK_LOGI("KisakCOD-vk", __VA_ARGS__)
#define LOGE(...) KISAK_LOGE("KisakCOD-vk", __VA_ARGS__)

namespace kisak::vk {

namespace {

// ---------------------------------------------------------------------------
// State

struct Backend
{
    bool initialised = false;
    bool headless = true;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkPhysicalDeviceProperties deviceProperties{};

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainViews;
    uint32_t swapchainIndex = 0;
    bool swapchainValid = false;
    int swapInterval = 1;

    ANativeWindow *window = nullptr;

    std::array<FrameContext, kFramesInFlight> frames{};
    uint32_t frameIndex = 0;

    VkDescriptorSetLayout descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
    std::unordered_map<uint64_t, VkRenderPass> renderPasses;
    std::unordered_map<uint64_t, VkFramebuffer> framebuffers;
    std::unordered_map<uint64_t, VkSampler> samplers;

    // The render pass currently open, if any, and what it is targeting.
    VkRenderPass activePass = VK_NULL_HANDLE;
    VkFramebuffer activeFramebuffer = VK_NULL_HANDLE;
    uint64_t activeTargetKey = 0;
    VkExtent2D activeExtent{};

    // Resolution the scene renders at; the swapchain may be larger.
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t instanceVersion = VK_API_VERSION_1_0;
    // Set when the driver reports the window surface gone, cleared when a new
    // one arrives. Without it every later frame retries against the dead
    // surface and logs another VK_ERROR_SURFACE_LOST_KHR.
    bool surfaceLost = false;

    DeviceInfo info;
    std::string deviceNameStorage;
    std::string driverNameStorage;
    std::string cacheDirectory;
    int64_t lastGpuFrameNs = 0;
    double timestampPeriodNs = 0.0;

    std::mutex mutex;
};

Backend g;

// ---------------------------------------------------------------------------
// Helpers

bool Check(VkResult result, const char *what)
{
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR)
        return true;
    LOGE("%s failed: %d", what, static_cast<int>(result));
    return false;
}

uint64_t HashBytes(const void *data, std::size_t size, uint64_t seed = 1469598103934665603ull)
{
    const auto *bytes = static_cast<const uint8_t *>(data);
    uint64_t hash = seed;
    for (std::size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

VkFormat ToVkFormat(Format format)
{
    switch (format)
    {
    // Direct3D's A8R8G8B8 is BGRA in memory order, which Vulkan names
    // directly. Getting this backwards swaps red and blue everywhere and is
    // the classic first bug in a D3D-to-Vulkan port.
    case Format::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRX8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBX8: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::L8: return VK_FORMAT_R8_UNORM;
    case Format::A8: return VK_FORMAT_R8_UNORM;
    case Format::A8L8: return VK_FORMAT_R8G8_UNORM;
    case Format::R5G6B5: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case Format::X1R5G5B5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case Format::A1R5G5B5: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case Format::A4R4G4B4: return VK_FORMAT_R4G4B4A4_UNORM_PACK16;
    case Format::R16F: return VK_FORMAT_R16_SFLOAT;
    case Format::R32F: return VK_FORMAT_R32_SFLOAT;
    case Format::RG16F: return VK_FORMAT_R16G16_SFLOAT;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::DXT3: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::DXT5: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::D24S8: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D16: return VK_FORMAT_D16_UNORM;
    case Format::D32F: return VK_FORMAT_D32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

bool IsDepthFormat(Format format)
{
    return format == Format::D24S8 || format == Format::D16 || format == Format::D32F;
}

VkCompareOp ToCompareOp(Compare compare)
{
    switch (compare)
    {
    case Compare::Never: return VK_COMPARE_OP_NEVER;
    case Compare::Less: return VK_COMPARE_OP_LESS;
    case Compare::Equal: return VK_COMPARE_OP_EQUAL;
    case Compare::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case Compare::Greater: return VK_COMPARE_OP_GREATER;
    case Compare::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case Compare::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case Compare::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkBlendFactor ToBlendFactor(BlendFactor factor)
{
    switch (factor)
    {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InvDestAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::DestColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InvDestColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::SrcAlphaSat: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case BlendFactor::BlendFactor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::InvBlendFactor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    // BOTHSRCALPHA and BOTHINVSRCALPHA were removed in Direct3D 9 itself and
    // the game never sets them; mapping to the source factor keeps a
    // hypothetical caller rendering rather than failing pipeline creation.
    case BlendFactor::BothSrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::BothInvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp ToBlendOp(BlendOp op)
{
    switch (op)
    {
    case BlendOp::Add: return VK_BLEND_OP_ADD;
    case BlendOp::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOp::RevSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min: return VK_BLEND_OP_MIN;
    case BlendOp::Max: return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

VkStencilOp ToStencilOp(StencilOp op)
{
    switch (op)
    {
    case StencilOp::Keep: return VK_STENCIL_OP_KEEP;
    case StencilOp::Zero: return VK_STENCIL_OP_ZERO;
    case StencilOp::Replace: return VK_STENCIL_OP_REPLACE;
    case StencilOp::IncrSat: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case StencilOp::DecrSat: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case StencilOp::Invert: return VK_STENCIL_OP_INVERT;
    case StencilOp::Incr: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case StencilOp::Decr: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    }
    return VK_STENCIL_OP_KEEP;
}

VkPrimitiveTopology ToTopology(Primitive primitive)
{
    switch (primitive)
    {
    case Primitive::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case Primitive::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case Primitive::LineStrip: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case Primitive::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case Primitive::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case Primitive::TriangleFan: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkFormat ToAttributeFormat(VertexType type)
{
    switch (type)
    {
    case VertexType::Float1: return VK_FORMAT_R32_SFLOAT;
    case VertexType::Float2: return VK_FORMAT_R32G32_SFLOAT;
    case VertexType::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexType::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    // D3DDECLTYPE_D3DCOLOR is BGRA bytes normalised to 0..1.
    case VertexType::Color: return VK_FORMAT_B8G8R8A8_UNORM;
    case VertexType::UByte4: return VK_FORMAT_R8G8B8A8_UINT;
    case VertexType::UByte4N: return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexType::Short2: return VK_FORMAT_R16G16_SINT;
    case VertexType::Short4: return VK_FORMAT_R16G16B16A16_SINT;
    case VertexType::Short2N: return VK_FORMAT_R16G16_SNORM;
    case VertexType::Short4N: return VK_FORMAT_R16G16B16A16_SNORM;
    case VertexType::UShort2N: return VK_FORMAT_R16G16_UNORM;
    case VertexType::UShort4N: return VK_FORMAT_R16G16B16A16_UNORM;
    case VertexType::Float16_2: return VK_FORMAT_R16G16_SFLOAT;
    case VertexType::Float16_4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    // UDEC3/DEC3N are 10-10-10-2 packed; Vulkan's equivalent exists but is
    // optional on mobile, and the game's vertex formats do not use them.
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkSamplerAddressMode ToAddressMode(Address address)
{
    switch (address)
    {
    case Address::Wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case Address::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case Address::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case Address::Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case Address::MirrorOnce: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required)
{
    for (uint32_t i = 0; i < g.memoryProperties.memoryTypeCount; ++i)
    {
        if ((typeBits & (1u << i)) && (g.memoryProperties.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return UINT32_MAX;
}

bool AllocateMemory(const VkMemoryRequirements &requirements, VkMemoryPropertyFlags properties, MemoryBlock &out)
{
    uint32_t typeIndex = FindMemoryType(requirements.memoryTypeBits, properties);
    if (typeIndex == UINT32_MAX && (properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
        // Some drivers expose no coherent host-visible type for images.
        // Falling back to non-coherent means explicit flushes, which the
        // upload path already performs.
        typeIndex = FindMemoryType(requirements.memoryTypeBits, properties & ~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (typeIndex == UINT32_MAX)
    {
        LOGE("no memory type for 0x%x with 0x%x", requirements.memoryTypeBits, properties);
        return false;
    }

    VkMemoryAllocateInfo info{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = typeIndex;
    if (!Check(vkAllocateMemory(g.device, &info, nullptr, &out.memory), "vkAllocateMemory"))
        return false;
    out.offset = 0;
    out.size = requirements.size;
    out.typeIndex = typeIndex;
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(g.device, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    return true;
}

void FreeMemory(MemoryBlock &block)
{
    if (block.memory == VK_NULL_HANDLE)
        return;
    if (block.mapped)
        vkUnmapMemory(g.device, block.memory);
    vkFreeMemory(g.device, block.memory, nullptr);
    block = MemoryBlock{};
}

FrameContext &Frame()
{
    return g.frames[g.frameIndex];
}

// Defers a destroy until the frame currently in flight has completed.
void DeferDelete(std::function<void()> work)
{
    Frame().deferredDeletes.push_back(std::move(work));
}

// ---------------------------------------------------------------------------
// Device creation

bool CreateInstance()
{
    VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "KisakCOD";
    app.applicationVersion = 1;
    app.pEngineName = "KisakCOD";
    app.engineVersion = 1;
    // Ask for the highest version the loader offers, capped at what this
    // backend is written against. Pinning 1.0 was leaving the newer core
    // features unreachable even on a device that supports them, because the
    // loader clamps everything to the version the instance declares.
    //
    // 1.0 stays the floor rather than the requirement: minSdk is 26, and a
    // phone from that era may offer nothing newer. Raising the floor would
    // drop exactly the low-end devices this port is meant to run on.
    // vkEnumerateInstanceVersion is itself a 1.1 entry point, so its absence
    // is how a 1.0 loader announces itself.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
    {
        if (enumerateVersion(&loaderVersion) != VK_SUCCESS)
            loaderVersion = VK_API_VERSION_1_0;
    }
    app.apiVersion = std::min(loaderVersion, VK_API_VERSION_1_3);
    g.instanceVersion = app.apiVersion;

    const char *extensions[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME };

    VkInstanceCreateInfo info{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    return Check(vkCreateInstance(&info, nullptr, &g.instance), "vkCreateInstance");
}

bool PickPhysicalDevice()
{
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
    if (count == 0)
    {
        LOGE("no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g.instance, &count, devices.data());

    // Phones have exactly one GPU. The loop exists for emulators and
    // Chromebooks, where a software fallback may be listed alongside real
    // hardware and must never be chosen over it.
    int bestScore = -1;
    for (VkPhysicalDevice candidate : devices)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        int score = 0;
        switch (properties.deviceType)
        {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 100; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 90; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 40; break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: score = 1; break;
        default: score = 10; break;
        }
        if (score > bestScore)
        {
            bestScore = score;
            g.physicalDevice = candidate;
            g.deviceProperties = properties;
        }
    }
    if (g.physicalDevice == VK_NULL_HANDLE)
        return false;

    vkGetPhysicalDeviceMemoryProperties(g.physicalDevice, &g.memoryProperties);

    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(g.physicalDevice, &features);

    g.deviceNameStorage = g.deviceProperties.deviceName;
    g.info.deviceName = g.deviceNameStorage.c_str();
    g.info.apiVersion = g.deviceProperties.apiVersion;
    g.info.vendorId = g.deviceProperties.vendorID;
    g.info.deviceId = g.deviceProperties.deviceID;
    g.info.supportsBc = features.textureCompressionBC == VK_TRUE;
    g.info.supportsAstc = features.textureCompressionASTC_LDR == VK_TRUE;
    g.info.supportsEtc2 = features.textureCompressionETC2 == VK_TRUE;
    g.info.supportsTimestamps = g.deviceProperties.limits.timestampComputeAndGraphics == VK_TRUE;
    g.info.immediateMode = g.deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    g.timestampPeriodNs = static_cast<double>(g.deviceProperties.limits.timestampPeriod);

    for (uint32_t i = 0; i < g.memoryProperties.memoryHeapCount; ++i)
    {
        if (g.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            g.info.deviceLocalMemoryBytes = std::max<uint64_t>(g.info.deviceLocalMemoryBytes,
                                                               g.memoryProperties.memoryHeaps[i].size);
    }

    if (!g.info.supportsBc)
    {
        // The game's textures are DXT1/3/5 inside the fastfiles. Adreno and
        // most Mali parts expose BC; those that do not need a decode path,
        // which db_zoneload_android.cpp performs on the CPU at load time.
        LOGI("device does not expose BC texture compression; textures will be decoded on load");
    }
    return true;
}

bool CreateDevice()
{
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(g.physicalDevice, &count, families.data());

    g.queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < count; ++i)
    {
        VkBool32 presentSupported = VK_FALSE;
        if (g.surface != VK_NULL_HANDLE)
            vkGetPhysicalDeviceSurfaceSupportKHR(g.physicalDevice, i, g.surface, &presentSupported);
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (g.surface == VK_NULL_HANDLE || presentSupported))
        {
            g.queueFamily = i;
            break;
        }
    }
    if (g.queueFamily == UINT32_MAX)
    {
        LOGE("no graphics+present queue family");
        return false;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = g.queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(g.physicalDevice, &available);
    VkPhysicalDeviceFeatures enabled{};
    // Only the features the renderer actually uses. Enabling everything
    // available costs nothing on paper but changes driver fast paths on some
    // Mali versions.
    enabled.samplerAnisotropy = available.samplerAnisotropy;
    enabled.textureCompressionBC = available.textureCompressionBC;
    enabled.textureCompressionASTC_LDR = available.textureCompressionASTC_LDR;
    enabled.textureCompressionETC2 = available.textureCompressionETC2;
    enabled.depthClamp = available.depthClamp;
    enabled.depthBiasClamp = available.depthBiasClamp;
    enabled.fillModeNonSolid = available.fillModeNonSolid;
    enabled.independentBlend = available.independentBlend;
    enabled.shaderClipDistance = available.shaderClipDistance;

    const char *extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    VkDeviceCreateInfo info{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queueInfo;
    info.enabledExtensionCount = g.surface != VK_NULL_HANDLE ? 1 : 0;
    info.ppEnabledExtensionNames = extensions;
    info.pEnabledFeatures = &enabled;

    if (!Check(vkCreateDevice(g.physicalDevice, &info, nullptr, &g.device), "vkCreateDevice"))
        return false;
    vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
    return true;
}

bool CreateFrameContexts()
{
    for (uint32_t i = 0; i < kFramesInFlight; ++i)
    {
        FrameContext &frame = g.frames[i];

        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.queueFamilyIndex = g.queueFamily;
        // Reset the whole pool each frame rather than individual buffers:
        // it is one call instead of N and lets the driver recycle the backing
        // allocation wholesale.
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (!Check(vkCreateCommandPool(g.device, &poolInfo, nullptr, &frame.commandPool), "vkCreateCommandPool"))
            return false;

        VkCommandBufferAllocateInfo bufferInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        bufferInfo.commandPool = frame.commandPool;
        bufferInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        bufferInfo.commandBufferCount = 1;
        if (!Check(vkAllocateCommandBuffers(g.device, &bufferInfo, &frame.commandBuffer), "vkAllocateCommandBuffers"))
            return false;

        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // first wait must not block
        if (!Check(vkCreateFence(g.device, &fenceInfo, nullptr, &frame.fence), "vkCreateFence"))
            return false;

        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!Check(vkCreateSemaphore(g.device, &semaphoreInfo, nullptr, &frame.acquired), "vkCreateSemaphore") ||
            !Check(vkCreateSemaphore(g.device, &semaphoreInfo, nullptr, &frame.rendered), "vkCreateSemaphore"))
            return false;

        VkDescriptorPoolSize sizes[2]{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        sizes[0].descriptorCount = 4096;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = 16384;

        VkDescriptorPoolCreateInfo descriptorInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        descriptorInfo.maxSets = 4096;
        descriptorInfo.poolSizeCount = 2;
        descriptorInfo.pPoolSizes = sizes;
        if (!Check(vkCreateDescriptorPool(g.device, &descriptorInfo, nullptr, &frame.descriptorPool),
                   "vkCreateDescriptorPool"))
            return false;

        VkBufferCreateInfo uploadInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        uploadInfo.size = kUploadRingBytes;
        uploadInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        uploadInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!Check(vkCreateBuffer(g.device, &uploadInfo, nullptr, &frame.uploadBuffer), "vkCreateBuffer(upload)"))
            return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(g.device, frame.uploadBuffer, &requirements);
        if (!AllocateMemory(requirements,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            frame.uploadMemory))
            return false;
        vkBindBufferMemory(g.device, frame.uploadBuffer, frame.uploadMemory.memory, 0);

        if (g.info.supportsTimestamps)
        {
            VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
            queryInfo.queryCount = 2;
            vkCreateQueryPool(g.device, &queryInfo, nullptr, &frame.timestampPool);
        }
    }
    return true;
}

bool CreatePipelineLayout()
{
    // One descriptor set: a dynamic uniform buffer for the float constants,
    // then one combined image sampler per Direct3D sampler slot.
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    VkDescriptorSetLayoutBinding constants{};
    constants.binding = kVertexConstantBinding;
    constants.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    constants.descriptorCount = 1;
    constants.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    bindings.push_back(constants);

    constants.binding = kFragmentConstantBinding;
    constants.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings.push_back(constants);

    for (uint32_t i = 0; i < kMaxSamplers; ++i)
    {
        VkDescriptorSetLayoutBinding sampler{};
        sampler.binding = kSamplerBindingBase + i;
        sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler.descriptorCount = 1;
        // Slots 0-15 are pixel samplers and 16-19 are vertex texture fetch.
        sampler.stageFlags = i < kPixelSamplers ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        bindings.push_back(sampler);
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (!Check(vkCreateDescriptorSetLayout(g.device, &layoutInfo, nullptr, &g.descriptorLayout),
               "vkCreateDescriptorSetLayout"))
        return false;

    // Push constants carry the half-pixel offset and the alpha test, which
    // change per draw and are too small to justify a buffer.
    VkPushConstantRange ranges[2]{};
    ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ranges[0].offset = 0;
    ranges[0].size = kPushConstantSize;
    ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ranges[1].offset = kPushConstantSize;
    ranges[1].size = kPushConstantSize;

    VkPipelineLayoutCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pipelineInfo.setLayoutCount = 1;
    pipelineInfo.pSetLayouts = &g.descriptorLayout;
    pipelineInfo.pushConstantRangeCount = 2;
    pipelineInfo.pPushConstantRanges = ranges;
    return Check(vkCreatePipelineLayout(g.device, &pipelineInfo, nullptr, &g.pipelineLayout), "vkCreatePipelineLayout");
}

// ---------------------------------------------------------------------------
// Swapchain

void DestroySwapchain()
{
    for (VkImageView view : g.swapchainViews)
        vkDestroyImageView(g.device, view, nullptr);
    g.swapchainViews.clear();
    g.swapchainImages.clear();
    for (auto &entry : g.framebuffers)
        vkDestroyFramebuffer(g.device, entry.second, nullptr);
    g.framebuffers.clear();
    if (g.swapchain != VK_NULL_HANDLE)
    {
        vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
        g.swapchain = VK_NULL_HANDLE;
    }
    g.swapchainValid = false;
}

bool CreateSwapchain(uint32_t width, uint32_t height)
{
    // The window can disappear between the engine asking for a frame and this
    // running: rotation, backgrounding, or the activity being torn down. The
    // engine keeps asking, so without these two guards every subsequent frame
    // retries against a dead surface and logs another failure.
    if (g.surface == VK_NULL_HANDLE || g.surfaceLost)
        return false;

    VkSurfaceCapabilitiesKHR capabilities{};
    const VkResult caps = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.physicalDevice, g.surface, &capabilities);
    if (caps == VK_ERROR_SURFACE_LOST_KHR)
    {
        LOGE("the window surface was lost; waiting for a new one");
        g.surfaceLost = true;
        return false;
    }
    if (!Check(caps, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"))
        return false;

    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX)
    {
        extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0)
        return false; // surface is minimised; try again on the next resume

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physicalDevice, g.surface, &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.physicalDevice, g.surface, &formatCount, formats.data());

    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{ VK_FORMAT_R8G8B8A8_UNORM,
                                                                      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR }
                                                : formats[0];
    for (const VkSurfaceFormatKHR &format : formats)
    {
        // The engine's output is already gamma-encoded, so a UNORM surface is
        // correct; an SRGB one would apply the curve twice and wash the image
        // out, which is a very common mistake in D3D9 ports.
        if ((format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM) &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
        {
            chosen = format;
            break;
        }
    }

    uint32_t modeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physicalDevice, g.surface, &modeCount, nullptr);
    std::vector<VkPresentModeKHR> modes(modeCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.physicalDevice, g.surface, &modeCount, modes.data());

    // FIFO is the only mode guaranteed present, and it is also the right one:
    // the frame pacer is already targeting the refresh cadence, so mailbox
    // would only let it render frames the compositor discards, burning the
    // thermal budget that keeps 60 fps sustainable. FIFO_RELAXED is preferred
    // where available, because it avoids a full extra frame of stall after an
    // occasional overrun.
    VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (const VkPresentModeKHR mode : modes)
    {
        if (mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
            presentMode = mode;
    }

    uint32_t imageCount = std::max(capabilities.minImageCount, kFramesInFlight + 1);
    if (capabilities.maxImageCount > 0)
        imageCount = std::min(imageCount, capabilities.maxImageCount);

    VkSwapchainCreateInfoKHR info{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    info.surface = g.surface;
    info.minImageCount = imageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    // TRANSFER_DST as well as COLOR_ATTACHMENT: the final upscale from the
    // dynamic-resolution render target is a blit, not a draw.
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = presentMode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = g.swapchain;

    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (!Check(vkCreateSwapchainKHR(g.device, &info, nullptr, &created), "vkCreateSwapchainKHR"))
        return false;

    DestroySwapchain();
    g.swapchain = created;
    g.swapchainFormat = chosen.format;
    g.swapchainExtent = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &count, nullptr);
    g.swapchainImages.resize(count);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &count, g.swapchainImages.data());

    g.swapchainViews.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = g.swapchainImages[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = g.swapchainFormat;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (!Check(vkCreateImageView(g.device, &viewInfo, nullptr, &g.swapchainViews[i]), "vkCreateImageView"))
            return false;
    }

    if (g.renderWidth == 0 || g.renderHeight == 0)
    {
        g.renderWidth = extent.width;
        g.renderHeight = extent.height;
    }
    g.swapchainValid = true;
    LOGI("swapchain %ux%u, %u images, format %d, present mode %d", extent.width, extent.height, count,
         static_cast<int>(g.swapchainFormat), static_cast<int>(presentMode));
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface

bool Initialize(void *window)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.initialised)
        return !g.headless;
    g.initialised = true;
    g.headless = true;

    if (!window)
    {
        LOGI("no native window; renderer stays headless");
        return false;
    }
    g.window = static_cast<ANativeWindow *>(window);

    if (!CreateInstance())
        return false;

    VkAndroidSurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
    surfaceInfo.window = g.window;
    g.surfaceLost = false;
    if (!Check(vkCreateAndroidSurfaceKHR(g.instance, &surfaceInfo, nullptr, &g.surface), "vkCreateAndroidSurfaceKHR"))
        return false;

    if (!PickPhysicalDevice() || !CreateDevice() || !CreatePipelineLayout() || !CreateFrameContexts())
        return false;

    g.pipelineCache = pipeline_cache::Load(g.device, g.cacheDirectory, g.deviceProperties);

    const uint32_t width = static_cast<uint32_t>(ANativeWindow_getWidth(g.window));
    const uint32_t height = static_cast<uint32_t>(ANativeWindow_getHeight(g.window));
    if (!CreateSwapchain(width, height))
        return false;

    g.headless = false;
    // Usable version is the lower of the two: the driver cannot offer more
    // than the instance declared, and the instance cannot grant more than the
    // driver implements.
    const uint32_t usable = std::min(g.instanceVersion, g.info.apiVersion);
    LOGI("Vulkan ready: %s (driver API %u.%u.%u, instance %u.%u, using %u.%u)", g.info.deviceName,
         VK_VERSION_MAJOR(g.info.apiVersion), VK_VERSION_MINOR(g.info.apiVersion),
         VK_VERSION_PATCH(g.info.apiVersion), VK_VERSION_MAJOR(g.instanceVersion),
         VK_VERSION_MINOR(g.instanceVersion), VK_VERSION_MAJOR(usable), VK_VERSION_MINOR(usable));
    return true;
}

bool Available()
{
    return g.initialised && !g.headless;
}

void SurfaceChanged(void *window, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!Available())
        return;
    // Everything in flight references the old swapchain images.
    vkDeviceWaitIdle(g.device);

    if (window != g.window)
    {
        DestroySwapchain();
        if (g.surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
            g.surface = VK_NULL_HANDLE;
        }
        g.window = static_cast<ANativeWindow *>(window);
        if (!g.window)
            return; // activity paused: the device and all resources survive
        VkAndroidSurfaceCreateInfoKHR surfaceInfo{ VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR };
        surfaceInfo.window = g.window;
        g.surfaceLost = false;
        if (!Check(vkCreateAndroidSurfaceKHR(g.instance, &surfaceInfo, nullptr, &g.surface),
                   "vkCreateAndroidSurfaceKHR"))
            return;
    }
    CreateSwapchain(width, height);
}

const DeviceInfo &GetDeviceInfo()
{
    return g.info;
}

void SetRenderResolution(uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    g.renderWidth = std::max(8u, width);
    g.renderHeight = std::max(8u, height);
}

void GetRenderResolution(uint32_t *width, uint32_t *height)
{
    if (width)
        *width = g.renderWidth;
    if (height)
        *height = g.renderHeight;
}

int64_t LastGpuFrameTimeNs()
{
    return g.lastGpuFrameNs;
}

void SetSwapInterval(int interval)
{
    // Vulkan has no swap interval. FIFO already presents one image per
    // refresh, so a longer interval is produced by the frame pacer simply not
    // submitting - which it does by holding the frame start back. Recording
    // the value lets Present() skip a redundant acquire when the pacer has
    // decided this refresh is not ours.
    g.swapInterval = std::clamp(interval, 1, 4);
}

void SetCacheDirectory(const char *path)
{
    std::lock_guard<std::mutex> lock(g.mutex);
    g.cacheDirectory = path ? path : "";
    pipeline_cache::SetDirectory(g.cacheDirectory);
}

void SavePipelineCache()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    if (g.pipelineCache != VK_NULL_HANDLE)
        pipeline_cache::Save(g.device, g.pipelineCache, g.cacheDirectory);
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g.mutex);
    if (!g.initialised || g.device == VK_NULL_HANDLE)
        return;
    vkDeviceWaitIdle(g.device);
    pipeline_cache::Save(g.device, g.pipelineCache, g.cacheDirectory);

    for (auto &entry : g.pipelines)
        vkDestroyPipeline(g.device, entry.second, nullptr);
    g.pipelines.clear();
    for (auto &entry : g.renderPasses)
        vkDestroyRenderPass(g.device, entry.second, nullptr);
    g.renderPasses.clear();
    for (auto &entry : g.samplers)
        vkDestroySampler(g.device, entry.second, nullptr);
    g.samplers.clear();
    DestroySwapchain();

    for (FrameContext &frame : g.frames)
    {
        for (auto &work : frame.deferredDeletes)
            work();
        frame.deferredDeletes.clear();
        if (frame.timestampPool)
            vkDestroyQueryPool(g.device, frame.timestampPool, nullptr);
        if (frame.uploadBuffer)
            vkDestroyBuffer(g.device, frame.uploadBuffer, nullptr);
        FreeMemory(frame.uploadMemory);
        if (frame.descriptorPool)
            vkDestroyDescriptorPool(g.device, frame.descriptorPool, nullptr);
        if (frame.acquired)
            vkDestroySemaphore(g.device, frame.acquired, nullptr);
        if (frame.rendered)
            vkDestroySemaphore(g.device, frame.rendered, nullptr);
        if (frame.fence)
            vkDestroyFence(g.device, frame.fence, nullptr);
        if (frame.commandPool)
            vkDestroyCommandPool(g.device, frame.commandPool, nullptr);
    }

    if (g.pipelineCache)
        vkDestroyPipelineCache(g.device, g.pipelineCache, nullptr);
    if (g.pipelineLayout)
        vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
    if (g.descriptorLayout)
        vkDestroyDescriptorSetLayout(g.device, g.descriptorLayout, nullptr);
    vkDestroyDevice(g.device, nullptr);
    if (g.surface)
        vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
    vkDestroyInstance(g.instance, nullptr);

    // Not `g = Backend{}`: the struct holds the mutex this function is
    // standing on, and a mutex is neither copyable nor movable. The handles
    // are cleared by hand so a second Shutdown, or an Initialize after one,
    // starts from a known state.
    g.instance = VK_NULL_HANDLE;
    g.physicalDevice = VK_NULL_HANDLE;
    g.device = VK_NULL_HANDLE;
    g.queue = VK_NULL_HANDLE;
    g.surface = VK_NULL_HANDLE;
    g.swapchain = VK_NULL_HANDLE;
    g.descriptorLayout = VK_NULL_HANDLE;
    g.pipelineLayout = VK_NULL_HANDLE;
    g.pipelineCache = VK_NULL_HANDLE;
    g.activePass = VK_NULL_HANDLE;
    g.activeFramebuffer = VK_NULL_HANDLE;
    g.frames = {};
    g.frameIndex = 0;
    g.window = nullptr;
    g.renderWidth = 0;
    g.renderHeight = 0;
    g.swapchainValid = false;
    g.initialised = false;
    g.headless = true;
}

// ---------------------------------------------------------------------------
// Resources

Texture *CreateTexture(const TextureDesc &desc)
{
    if (!Available())
        return nullptr;
    std::lock_guard<std::mutex> lock(g.mutex);

    auto *texture = new Texture();
    texture->desc = desc;
    texture->format = ToVkFormat(desc.format);
    texture->isDepth = IsDepthFormat(desc.format);
    texture->aspect = texture->isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (desc.format == Format::D24S8)
        texture->aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;

    if (texture->format == VK_FORMAT_UNDEFINED)
    {
        LOGE("unsupported texture format %d", static_cast<int>(desc.format));
        delete texture;
        return nullptr;
    }

    VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType = desc.kind == TextureKind::Volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    info.format = texture->format;
    info.extent = { desc.width, desc.height, desc.kind == TextureKind::Volume ? desc.depth : 1 };
    info.mipLevels = std::max(1u, desc.levels);
    info.arrayLayers = desc.kind == TextureKind::Cube ? 6 : 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (desc.renderTarget)
        info.usage |= texture->isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (desc.kind == TextureKind::Cube)
        info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    if (!Check(vkCreateImage(g.device, &info, nullptr, &texture->image), "vkCreateImage"))
    {
        delete texture;
        return nullptr;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(g.device, texture->image, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, texture->memory))
    {
        vkDestroyImage(g.device, texture->image, nullptr);
        delete texture;
        return nullptr;
    }
    vkBindImageMemory(g.device, texture->image, texture->memory.memory, 0);

    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = texture->image;
    switch (desc.kind)
    {
    case TextureKind::Cube: viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE; break;
    case TextureKind::Volume: viewInfo.viewType = VK_IMAGE_VIEW_TYPE_3D; break;
    default: viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    viewInfo.format = texture->format;
    // D3DFMT_X8R8G8B8 reads alpha as one; Vulkan has no X format, so the
    // swizzle does it. Without this, every "opaque" surface drawn through an
    // X8 texture becomes transparent.
    if (desc.format == Format::BGRX8 || desc.format == Format::RGBX8 || desc.format == Format::X1R5G5B5)
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
    // D3DFMT_L8 broadcasts the single channel to rgb; D3DFMT_A8 puts it in a.
    if (desc.format == Format::L8)
    {
        viewInfo.components.r = viewInfo.components.g = viewInfo.components.b = VK_COMPONENT_SWIZZLE_R;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_ONE;
    }
    else if (desc.format == Format::A8)
    {
        viewInfo.components.r = viewInfo.components.g = viewInfo.components.b = VK_COMPONENT_SWIZZLE_ZERO;
        viewInfo.components.a = VK_COMPONENT_SWIZZLE_R;
    }
    viewInfo.subresourceRange = { texture->aspect, 0, info.mipLevels, 0, info.arrayLayers };
    // A depth+stencil view used for sampling may only carry one aspect.
    if (texture->isDepth)
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    Check(vkCreateImageView(g.device, &viewInfo, nullptr, &texture->view), "vkCreateImageView");

    return texture;
}

void DestroyTexture(Texture *texture)
{
    if (!texture || !Available())
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    // The GPU may still be reading this; release it when the in-flight frame
    // retires rather than now.
    DeferDelete([texture] {
        for (auto &entry : texture->targetViews)
            vkDestroyImageView(g.device, entry.second, nullptr);
        if (texture->view)
            vkDestroyImageView(g.device, texture->view, nullptr);
        if (texture->image && !texture->isSwapchainProxy)
            vkDestroyImage(g.device, texture->image, nullptr);
        FreeMemory(texture->memory);
        delete texture;
    });
}

Buffer *CreateBuffer(size_t length)
{
    if (!Available() || length == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(g.mutex);

    auto *buffer = new Buffer();
    buffer->size = length;

    VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.size = length;
    // One usage mask for every buffer: the Direct3D 9 layer does not tell the
    // backend whether a buffer is vertices or indices until it is bound.
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!Check(vkCreateBuffer(g.device, &info, nullptr, &buffer->buffer), "vkCreateBuffer"))
    {
        delete buffer;
        return nullptr;
    }

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(g.device, buffer->buffer, &requirements);
    if (!AllocateMemory(requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        buffer->memory))
    {
        vkDestroyBuffer(g.device, buffer->buffer, nullptr);
        delete buffer;
        return nullptr;
    }
    vkBindBufferMemory(g.device, buffer->buffer, buffer->memory.memory, 0);
    buffer->mapped = buffer->memory.mapped;
    return buffer;
}

void DestroyBuffer(Buffer *buffer)
{
    if (!buffer || !Available())
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    DeferDelete([buffer] {
        if (buffer->buffer)
            vkDestroyBuffer(g.device, buffer->buffer, nullptr);
        FreeMemory(buffer->memory);
        delete buffer;
    });
}

void UploadBuffer(Buffer *buffer, size_t offset, const uint8_t *data, size_t size, BufferUpdate update)
{
    (void)update;
    if (!buffer || !buffer->mapped || !data || size == 0)
        return;
    if (offset + size > buffer->size)
        return;
    // Host-visible and coherent: the write is visible to the GPU without a
    // flush, and because every mobile part has unified memory this is the
    // whole upload, not a staging step.
    std::memcpy(static_cast<uint8_t *>(buffer->mapped) + offset, data, size);
}

Shader *CreateShader(const uint32_t *tokens, size_t count)
{
    if (!Available() || !tokens || count == 0)
        return nullptr;

    TranslateOptions options;
    // The translator is told which varyings exist only when a pixel shader is
    // paired with its vertex shader at pipeline time; at module creation the
    // safe assumption is "all of them", and the pipeline cache key includes
    // the pair so a mismatch recompiles.
    TranslatedShader translated;
    if (!TranslateShader(tokens, count, options, translated))
    {
        LOGE("shader translation failed: %s", translated.error.c_str());
        return nullptr;
    }

    std::vector<uint32_t> spirv;
    if (!pipeline_cache::CompileGlsl(translated.source, translated.pixel, spirv))
    {
        LOGE("GLSL compilation failed for a %s shader", translated.pixel ? "pixel" : "vertex");
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g.mutex);
    auto *shader = new Shader();
    shader->pixel = translated.pixel;
    shader->inputs = translated.inputs;
    shader->outputs = translated.outputs;
    shader->samplers = translated.samplers;
    shader->constantCount = translated.constantCount;
    shader->glsl = translated.source;
    shader->spirv = std::move(spirv);
    shader->sourceHash = HashBytes(tokens, count * sizeof(uint32_t));

    for (const ShaderSemantic &output : shader->outputs)
    {
        const int bit = AttributeSlot(output.usage, output.index);
        if (bit >= 0 && bit < 32)
            shader->writtenVaryings |= 1u << bit;
    }

    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = shader->spirv.size() * sizeof(uint32_t);
    info.pCode = shader->spirv.data();
    if (!Check(vkCreateShaderModule(g.device, &info, nullptr, &shader->module), "vkCreateShaderModule"))
    {
        delete shader;
        return nullptr;
    }
    return shader;
}

void DestroyShader(Shader *shader)
{
    if (!shader || !Available())
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    DeferDelete([shader] {
        if (shader->module)
            vkDestroyShaderModule(g.device, shader->module, nullptr);
        delete shader;
    });
}

VertexLayout *CreateVertexLayout(const VertexElement *elements, size_t count)
{
    if (!elements || count == 0)
        return nullptr;

    auto *layout = new VertexLayout();
    layout->elements.assign(elements, elements + count);

    std::array<bool, kMaxStreams> usedStreams{};
    for (size_t i = 0; i < count; ++i)
    {
        const VertexElement &element = elements[i];
        const int slot = AttributeSlot(element.usage, element.usageIndex);
        if (slot < 0)
            continue; // a usage no shader can read; skipping keeps the layout valid

        VkVertexInputAttributeDescription attribute{};
        attribute.location = static_cast<uint32_t>(slot);
        attribute.binding = element.stream;
        attribute.offset = element.offset;
        attribute.format = ToAttributeFormat(element.type);
        if (attribute.format == VK_FORMAT_UNDEFINED)
            continue;
        layout->attributes.push_back(attribute);

        // Unnormalised integer formats reach the shader as integers; the
        // translator has to declare those inputs as uvec4/ivec4 and convert,
        // because Vulkan will not do the implicit conversion Direct3D did.
        if (element.type == VertexType::UByte4)
            layout->unsignedIntMask |= 1u << slot;
        else if (element.type == VertexType::Short2 || element.type == VertexType::Short4)
            layout->signedIntMask |= 1u << slot;

        if (element.stream < kMaxStreams)
            usedStreams[element.stream] = true;
    }

    for (uint32_t stream = 0; stream < kMaxStreams; ++stream)
    {
        if (!usedStreams[stream])
            continue;
        VkVertexInputBindingDescription binding{};
        binding.binding = stream;
        // The stride is a property of the bound stream, not the declaration,
        // so it is patched in at draw time from StreamBinding::stride.
        binding.stride = 0;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        layout->bindings.push_back(binding);
    }

    layout->hash = HashBytes(layout->attributes.data(),
                             layout->attributes.size() * sizeof(VkVertexInputAttributeDescription));
    layout->hash = HashBytes(&layout->unsignedIntMask, sizeof(uint32_t), layout->hash);
    layout->hash = HashBytes(&layout->signedIntMask, sizeof(uint32_t), layout->hash);
    return layout;
}

void DestroyVertexLayout(VertexLayout *layout)
{
    delete layout;
}

#include "vk_draw.inc"

} // namespace kisak::vk
