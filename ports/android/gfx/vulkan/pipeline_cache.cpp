#include "pipeline_cache.h"

#include <android/log.h>
#include <shaderc/shaderc.hpp>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KisakCOD-vk", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KisakCOD-vk", __VA_ARGS__)

namespace kisak::vk::pipeline_cache {

namespace {

// Bump when the translator's output changes in a way that makes previously
// cached SPIR-V wrong. Forgetting to bump this produces the worst class of
// bug in the port: a shader fix that works on a clean install and not on an
// upgrade.
constexpr uint32_t kShaderCacheVersion = 3;
constexpr uint32_t kShaderCacheMagic = 0x4B534843; // "KSHC"

struct ShaderCacheHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t count;
    uint32_t reserved;
};

std::string g_directory;
std::mutex g_mutex;
// Remembered at Load() so Save() can stamp the blob with the same identity
// Load() will check, without needing the physical device handle again.
VkPhysicalDeviceProperties g_deviceProperties{};
bool g_haveDeviceProperties = false;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_shaderCache;
bool g_shaderCacheLoaded = false;
bool g_shaderCacheDirty = false;
uint32_t g_compiled = 0;
uint32_t g_servedFromCache = 0;

uint64_t Hash(const void *data, std::size_t size, uint64_t seed = 1469598103934665603ull)
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

bool ReadFile(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return false;
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (size <= 0)
    {
        std::fclose(file);
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const bool ok = std::fread(out.data(), 1, out.size(), file) == out.size();
    std::fclose(file);
    return ok;
}

// Writes to a temporary and renames. A cache file truncated by the system
// killing the app mid-write is worse than no cache file: the driver may read
// the partial blob and crash.
bool WriteFileAtomic(const std::string &path, const void *data, std::size_t size)
{
    const std::string temporary = path + ".tmp";
    FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    const bool written = std::fwrite(data, 1, size, file) == size;
    std::fflush(file);
    ::fsync(fileno(file));
    std::fclose(file);
    if (!written)
    {
        ::unlink(temporary.c_str());
        return false;
    }
    return ::rename(temporary.c_str(), path.c_str()) == 0;
}

void LoadShaderCache()
{
    if (g_shaderCacheLoaded || g_directory.empty())
        return;
    g_shaderCacheLoaded = true;

    std::vector<uint8_t> blob;
    if (!ReadFile(g_directory + "/shaders.spv.cache", blob) || blob.size() < sizeof(ShaderCacheHeader))
        return;

    ShaderCacheHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != kShaderCacheMagic || header.version != kShaderCacheVersion)
    {
        LOGI("shader cache version mismatch; recompiling");
        return;
    }

    std::size_t offset = sizeof(ShaderCacheHeader);
    for (uint32_t i = 0; i < header.count; ++i)
    {
        if (offset + sizeof(uint64_t) + sizeof(uint32_t) > blob.size())
            break;
        uint64_t key = 0;
        uint32_t words = 0;
        std::memcpy(&key, blob.data() + offset, sizeof(key));
        offset += sizeof(key);
        std::memcpy(&words, blob.data() + offset, sizeof(words));
        offset += sizeof(words);
        if (offset + words * sizeof(uint32_t) > blob.size())
            break;
        std::vector<uint32_t> spirv(words);
        std::memcpy(spirv.data(), blob.data() + offset, words * sizeof(uint32_t));
        offset += words * sizeof(uint32_t);
        g_shaderCache.emplace(key, std::move(spirv));
    }
    LOGI("shader cache: %zu entries", g_shaderCache.size());
}

void SaveShaderCache()
{
    if (!g_shaderCacheDirty || g_directory.empty())
        return;

    std::vector<uint8_t> blob;
    ShaderCacheHeader header{ kShaderCacheMagic, kShaderCacheVersion, static_cast<uint32_t>(g_shaderCache.size()), 0 };
    blob.resize(sizeof(header));
    std::memcpy(blob.data(), &header, sizeof(header));

    for (const auto &entry : g_shaderCache)
    {
        const uint64_t key = entry.first;
        const uint32_t words = static_cast<uint32_t>(entry.second.size());
        const std::size_t base = blob.size();
        blob.resize(base + sizeof(key) + sizeof(words) + words * sizeof(uint32_t));
        std::memcpy(blob.data() + base, &key, sizeof(key));
        std::memcpy(blob.data() + base + sizeof(key), &words, sizeof(words));
        std::memcpy(blob.data() + base + sizeof(key) + sizeof(words), entry.second.data(),
                    words * sizeof(uint32_t));
    }

    if (WriteFileAtomic(g_directory + "/shaders.spv.cache", blob.data(), blob.size()))
        g_shaderCacheDirty = false;
}

// Identifies the exact driver, so a system update throws the blob away.
struct CacheStamp
{
    uint32_t magic;
    uint32_t vendorId;
    uint32_t deviceId;
    uint32_t driverVersion;
    uint8_t uuid[VK_UUID_SIZE];
};

} // namespace

void SetDirectory(const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_directory = path;
}

VkPipelineCache Load(VkDevice device, const std::string &directory, const VkPhysicalDeviceProperties &properties)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!directory.empty())
        g_directory = directory;
    g_deviceProperties = properties;
    g_haveDeviceProperties = true;

    std::vector<uint8_t> blob;
    const void *initialData = nullptr;
    std::size_t initialSize = 0;

    if (!g_directory.empty() && ReadFile(g_directory + "/pipeline.cache", blob) && blob.size() > sizeof(CacheStamp))
    {
        CacheStamp stamp{};
        std::memcpy(&stamp, blob.data(), sizeof(stamp));
        const bool matches = stamp.magic == kShaderCacheMagic && stamp.vendorId == properties.vendorID &&
                             stamp.deviceId == properties.deviceID &&
                             stamp.driverVersion == properties.driverVersion &&
                             std::memcmp(stamp.uuid, properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
        if (matches)
        {
            initialData = blob.data() + sizeof(CacheStamp);
            initialSize = blob.size() - sizeof(CacheStamp);
            LOGI("pipeline cache: %zu bytes", initialSize);
        }
        else
        {
            LOGI("pipeline cache is from another driver; starting empty");
        }
    }

    VkPipelineCacheCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    info.initialDataSize = initialSize;
    info.pInitialData = initialData;

    VkPipelineCache cache = VK_NULL_HANDLE;
    if (vkCreatePipelineCache(device, &info, nullptr, &cache) != VK_SUCCESS)
    {
        // A rejected blob is recoverable; an empty cache is not an error.
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        vkCreatePipelineCache(device, &info, nullptr, &cache);
    }
    return cache;
}

void Save(VkDevice device, VkPipelineCache cache, const std::string &directory)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!directory.empty())
        g_directory = directory;
    SaveShaderCache();

    if (cache == VK_NULL_HANDLE || g_directory.empty())
        return;

    std::size_t size = 0;
    if (vkGetPipelineCacheData(device, cache, &size, nullptr) != VK_SUCCESS || size == 0)
        return;

    std::vector<uint8_t> blob(sizeof(CacheStamp) + size);
    if (vkGetPipelineCacheData(device, cache, &size, blob.data() + sizeof(CacheStamp)) != VK_SUCCESS)
        return;

    // Stamp with the identity Load() will verify. Without a real stamp the
    // blob is rejected on the next launch and the cache never does anything.
    if (!g_haveDeviceProperties)
        return;
    CacheStamp stamp{};
    stamp.magic = kShaderCacheMagic;
    stamp.vendorId = g_deviceProperties.vendorID;
    stamp.deviceId = g_deviceProperties.deviceID;
    stamp.driverVersion = g_deviceProperties.driverVersion;
    std::memcpy(stamp.uuid, g_deviceProperties.pipelineCacheUUID, VK_UUID_SIZE);
    blob.resize(sizeof(CacheStamp) + size);
    std::memcpy(blob.data(), &stamp, sizeof(stamp));

    WriteFileAtomic(g_directory + "/pipeline.cache", blob.data(), blob.size());
}

bool CompileGlsl(const std::string &source, bool pixel, std::vector<uint32_t> &spirv)
{
    const uint64_t key = Hash(source.data(), source.size(), pixel ? 0x9E3779B97F4A7C15ull : 1469598103934665603ull);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        LoadShaderCache();
        const auto it = g_shaderCache.find(key);
        if (it != g_shaderCache.end())
        {
            spirv = it->second;
            ++g_servedFromCache;
            return true;
        }
    }

    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    // Vulkan 1.0 is the floor the instance asks for, so the SPIR-V must not
    // require anything newer.
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_0);
    options.SetTargetSpirv(shaderc_spirv_version_1_0);
    // Performance, not size: the shaders are small and compile time is
    // already cached away, while every saved instruction runs millions of
    // times per frame.
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetSourceLanguage(shaderc_source_language_glsl);
    options.SetAutoBindUniforms(false);
    options.SetAutoMapLocations(false);

    const shaderc_shader_kind kind = pixel ? shaderc_glsl_fragment_shader : shaderc_glsl_vertex_shader;
    const shaderc::SpvCompilationResult result =
        compiler.CompileGlslToSpv(source, kind, pixel ? "pixel.frag" : "vertex.vert", options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success)
    {
        LOGE("shaderc: %s", result.GetErrorMessage().c_str());
        // Dumping the source is worth the log spam: a translation bug is
        // otherwise invisible, and the GLSL is the only evidence.
        LOGE("----- generated GLSL -----\n%s", source.c_str());
        return false;
    }

    spirv.assign(result.cbegin(), result.cend());

    std::lock_guard<std::mutex> lock(g_mutex);
    g_shaderCache[key] = spirv;
    g_shaderCacheDirty = true;
    ++g_compiled;
    return true;
}

void GetShaderStats(uint32_t *compiled, uint32_t *cached)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (compiled)
        *compiled = g_compiled;
    if (cached)
        *cached = g_servedFromCache;
}

} // namespace kisak::vk::pipeline_cache
