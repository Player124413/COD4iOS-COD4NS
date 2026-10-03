#pragma once

// Direct3D 9 shader model 3 bytecode to Vulkan GLSL.
//
// COD4 ships its HLSL precompiled as ps_3_0/vs_3_0 bytecode inside the
// fastfiles, so a port has to consume bytecode, not source. The iOS port
// translates it to Metal Shading Language (ports/ios/d3d9/metal/dx9_msl_translator.h);
// this is the same job for Vulkan, emitting GLSL 4.50 that shaderc compiles to
// SPIR-V at load time and the pipeline cache keeps across runs.
//
// The two translators share a public surface on purpose - the structs below
// are deliberately the same shape as the MSL ones - so the D3D9 layer and the
// backends stay symmetrical. They do not share an implementation, because the
// two output languages differ in exactly the places that matter:
//
//   * Metal addresses resources by argument index; Vulkan uses descriptor set
//     and binding, so samplers and the constant buffer need a layout decided
//     here rather than at bind time.
//   * GLSL has no `saturate`, no per-component `select`, and its `mod` differs
//     from HLSL's `frc` for negative inputs.
//   * Vulkan clip space is already Y-down, so the Y flip that MSL needs is
//     wrong here; the half-pixel offset is still required.
//   * `texldd`/`texldl` map onto textureGrad/textureLod, which in GLSL may not
//     appear under non-uniform control flow, so the translator hoists them.
//
// Depends only on the standard library, so it is validated offline by
// ports/android/tests/ShaderTranslatorTests.cpp.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kisak::vk {

// Direct3D 9 declaration usages (D3DDECLUSAGE_*).
enum : uint8_t
{
    kUsagePosition = 0,
    kUsageBlendWeight = 1,
    kUsageBlendIndices = 2,
    kUsageNormal = 3,
    kUsagePointSize = 4,
    kUsageTexCoord = 5,
    kUsageTangent = 6,
    kUsageBinormal = 7,
    kUsageTessFactor = 8,
    kUsagePositionT = 9,
    kUsageColor = 10,
    kUsageFog = 11,
    kUsageDepth = 12,
    kUsageSample = 13,
};

// Sampler texture types from `dcl_2d`, `dcl_cube` and `dcl_volume`.
enum : uint8_t
{
    kTextureUnknown = 0,
    kTexture2D = 2,
    kTextureCube = 3,
    kTextureVolume = 4,
};

struct ShaderSemantic
{
    uint8_t usage = 0;
    uint8_t index = 0;
    uint16_t reg = 0;
};

struct ShaderSampler
{
    uint16_t reg = 0;
    uint8_t type = kTextureUnknown;
};

struct TranslateOptions
{
    // Samplers (by register) bound to depth textures. Direct3D 9 drivers
    // return a hardware shadow comparison when a depth texture is sampled;
    // these become sampler2DShadow and compare against coord.z.
    uint32_t depthSamplerMask = 0;
    // Varyings the paired vertex shader writes (pixel shaders only). Bits 0-7
    // texcoord0-7, 8-9 color0-1, 10 fog, 11 normal, 12 tangent, 13 binormal.
    // Inputs outside the mask read as zero instead of failing linkage, which
    // Direct3D 9 allowed and some of the game's material shaders rely on.
    uint32_t availableVaryings = 0xFFFFFFFF;
    // Vertex inputs fed by unnormalised integer formats (bits by AttributeSlot).
    // Vulkan will not convert those to float attributes the way Direct3D 9
    // does, so they are declared as uvec4/ivec4 and converted in the shader.
    uint32_t unsignedIntInputMask = 0;
    uint32_t signedIntInputMask = 0;
    // Emit `precision mediump float` defaults. Worth 10-20% of fragment cost
    // on Mali and PowerVR parts, and harmless on Adreno, but it changes
    // results, so the backend only sets it for the lower quality tiers.
    bool preferMediumPrecision = false;
};

struct TranslatedShader
{
    bool pixel = false;
    std::string source;
    std::vector<ShaderSemantic> inputs;
    std::vector<ShaderSemantic> outputs;
    std::vector<ShaderSampler> samplers;
    uint32_t constantCount = 0; // highest float constant register read, plus one
    std::string error;
};

// Descriptor layout shared with the Vulkan backend. One set, because the
// engine rebinds everything per draw anyway and a second set would only add
// descriptor-set-change traffic.
inline constexpr uint32_t kDescriptorSet = 0;
inline constexpr uint32_t kVertexConstantBinding = 0;   // uniform block, 256 vec4
inline constexpr uint32_t kFragmentConstantBinding = 1; // uniform block, 224 vec4
inline constexpr uint32_t kPushConstantSize = 16;       // VertexParams or FragmentParams
inline constexpr uint32_t kSamplerBindingBase = 2;      // 2..21 for the 20 samplers
inline constexpr const char *kShaderEntryPoint = "main";

// Sizes of the two uniform blocks. Direct3D 9 shader model 3 defines 256
// float4 vertex constants and 224 pixel constants; the blocks are declared at
// full size so one descriptor layout serves every shader, and only the
// registers a given shader reads are actually uploaded.
inline constexpr uint32_t kVertexConstantRegisters = 256;
inline constexpr uint32_t kFragmentConstantRegisters = 224;
inline constexpr uint32_t kVertexConstantBytes = kVertexConstantRegisters * 16;
inline constexpr uint32_t kFragmentConstantBytes = kFragmentConstantRegisters * 16;

// Push-constant layouts, matching the generated GLSL blocks.
struct VertexParams
{
    float halfPixel[2]; // added to clip-space xy, scaled by w
    float pad[2];
};

struct FragmentParams
{
    int32_t alphaFunc; // D3DCMPFUNC, 0 when alpha test is disabled
    float alphaRef;    // 0..1
    float pad[2];
};

// Vertex attribute location for a declaration usage, or -1 when unmapped.
// Must agree with the Metal translator's AttributeSlot so one vertex
// declaration description works for both backends.
int AttributeSlot(uint8_t usage, uint8_t index);

// Translates one shader. `tokens` must hold the whole program through the end
// token. Returns false with `out.error` set when the program uses a feature
// the translator does not handle.
bool TranslateShader(const uint32_t *tokens, size_t tokenCount, const TranslateOptions &options, TranslatedShader &out);

} // namespace kisak::vk
