// Host tests for the Direct3D 9 shader model 3 to GLSL translator.
//
// The translator is pure C++ over a token array, so it can be driven here with
// hand-assembled bytecode. These tests check the translation decisions that
// are easy to get wrong and expensive to debug on a device: write masks,
// swizzles, source modifiers, the constant-folding of `def`, flow control
// nesting, and the Direct3D-to-Vulkan conventions (half-pixel offset, alpha
// test, shadow samplers).
//
// They check the generated GLSL as text. Compiling it would need shaderc,
// which only exists inside the NDK; the build that does have shaderc compiles
// every translated shader at load time anyway, so a syntax error shows up
// immediately there.

#include "../gfx/dx9_glsl_translator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace kisak::vk;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char *what, const char *file, int line)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s (%s:%d)\n", what, file, line);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)
#define CHECK_CONTAINS(haystack, needle)                                                                               \
    Check((haystack).find(needle) != std::string::npos, "expected \"" needle "\"", __FILE__, __LINE__)
#define CHECK_LACKS(haystack, needle)                                                                                  \
    Check((haystack).find(needle) == std::string::npos, "did not expect \"" needle "\"", __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// A tiny shader model 3 assembler, so the tests read as shaders rather than
// as hexadecimal.

constexpr uint32_t VersionToken(bool pixel, uint32_t major, uint32_t minor)
{
    return ((pixel ? 0xFFFFu : 0xFFFEu) << 16) | (major << 8) | minor;
}

constexpr uint32_t InstructionToken(uint32_t opcode, uint32_t length, uint32_t controls = 0)
{
    return opcode | (controls << 16) | (length << 24);
}

// Destination: register type, number, write mask (xyzw bits), modifiers.
constexpr uint32_t Dest(uint32_t type, uint32_t number, uint32_t mask = 0xF, uint32_t modifier = 0)
{
    return 0x80000000u | ((type & 0x7) << 28) | ((type & 0x18) << 8) | (number & 0x7FF) | ((mask & 0xF) << 16) |
           ((modifier & 0xF) << 20);
}

// Source: register type, number, swizzle (two bits per component), modifier.
constexpr uint32_t Src(uint32_t type, uint32_t number, uint32_t swizzle = 0xE4, uint32_t modifier = 0,
                       bool relative = false)
{
    return 0x80000000u | ((type & 0x7) << 28) | ((type & 0x18) << 8) | (number & 0x7FF) | ((swizzle & 0xFF) << 16) |
           ((modifier & 0xF) << 24) | (relative ? 0x2000u : 0u);
}

enum : uint32_t
{
    RegTemp = 0, RegInput = 1, RegConst = 2, RegTexture = 3, RegRastOut = 4, RegAttrOut = 5, RegOutput = 6,
    RegConstInt = 7, RegColorOut = 8, RegDepthOut = 9, RegSampler = 10, RegConstBool = 14, RegLoop = 15,
    RegMiscType = 17,
};

enum : uint32_t
{
    OpMov = 1, OpAdd = 2, OpMad = 4, OpMul = 5, OpRcp = 6, OpDp3 = 8, OpDp4 = 9, OpMin = 10, OpMax = 11,
    OpLrp = 18, OpFrc = 19, OpM4x4 = 20, OpLoop = 27, OpEndLoop = 29, OpDcl = 31, OpPow = 32, OpRep = 38,
    OpEndRep = 39, OpIf = 40, OpIfc = 41, OpElse = 42, OpEndIf = 43, OpBreakC = 45, OpTexKill = 65, OpTex = 66,
    OpDef = 81, OpCmp = 88, OpEnd = 0xFFFF,
};

uint32_t DeclToken(uint8_t usage, uint8_t index)
{
    return 0x80000000u | usage | (static_cast<uint32_t>(index) << 16);
}

uint32_t SamplerDeclToken(uint8_t textureType)
{
    return 0x80000000u | (static_cast<uint32_t>(textureType) << 27);
}

uint32_t FloatBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string Translate(const std::vector<uint32_t> &tokens, const TranslateOptions &options, bool expectSuccess = true)
{
    TranslatedShader out;
    const bool ok = TranslateShader(tokens.data(), tokens.size(), options, out);
    if (ok != expectSuccess)
    {
        ++g_failures;
        std::printf("FAIL: translation %s unexpectedly (%s)\n", ok ? "succeeded" : "failed", out.error.c_str());
    }
    ++g_checks;
    return ok ? out.source : out.error;
}

// ---------------------------------------------------------------------------

void TestVertexShaderBasics()
{
    // vs_3_0
    //   dcl_position v0
    //   dcl_texcoord0 v1
    //   dcl_position o0
    //   dcl_texcoord0 o1
    //   m4x4 o0, v0, c0
    //   mov o1, v1
    std::vector<uint32_t> tokens = {
        VersionToken(false, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsagePosition, 0), Dest(RegInput, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 1),
        InstructionToken(OpDcl, 2), DeclToken(kUsagePosition, 0), Dest(RegOutput, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegOutput, 1),
        InstructionToken(OpM4x4, 3), Dest(RegOutput, 0), Src(RegInput, 0), Src(RegConst, 0),
        InstructionToken(OpMov, 2), Dest(RegOutput, 1), Src(RegInput, 1),
        OpEnd,
    };

    TranslatedShader out;
    CHECK(TranslateShader(tokens.data(), tokens.size(), TranslateOptions{}, out));
    CHECK(!out.pixel);
    CHECK(out.inputs.size() == 2);
    CHECK(out.outputs.size() == 2);
    // m4x4 reads c0..c3.
    CHECK(out.constantCount >= 4);

    const std::string &source = out.source;
    CHECK_CONTAINS(source, "#version 450");
    // Position goes to attribute slot 0 and texcoord0 to slot 4.
    CHECK_CONTAINS(source, "layout(location = 0) in vec4 attr0");
    CHECK_CONTAINS(source, "layout(location = 4) in vec4 attr1");
    // texcoord0 varying is location 0 on the way out.
    CHECK_CONTAINS(source, "layout(location = 0) out vec4 vary1");
    CHECK_CONTAINS(source, "uniform ShaderConstants");
    // The matrix multiply expands to four dot products against c0..c3.
    CHECK_CONTAINS(source, "dot(vec4(attr0), c[0])");
    CHECK_CONTAINS(source, "dot(vec4(attr0), c[3])");
    // Direct3D's half-pixel offset is applied in clip space, scaled by w.
    CHECK_CONTAINS(source, "oPos.xy += params.halfPixel * oPos.w");
    CHECK_CONTAINS(source, "gl_Position = oPos");
    // Vulkan clip space is already Y-down like Direct3D; a flip would be a bug.
    CHECK_LACKS(source, "oPos.y = -oPos.y");
}

void TestPixelShaderBasics()
{
    // ps_3_0
    //   dcl_texcoord0 v0
    //   dcl_2d s0
    //   texld r0, v0, s0
    //   mov oC0, r0
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpDcl, 2), SamplerDeclToken(kTexture2D), Dest(RegSampler, 0),
        InstructionToken(OpTex, 3), Dest(RegTemp, 0), Src(RegInput, 0), Src(RegSampler, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };

    TranslatedShader out;
    CHECK(TranslateShader(tokens.data(), tokens.size(), TranslateOptions{}, out));
    CHECK(out.pixel);
    CHECK(out.samplers.size() == 1);
    CHECK(out.samplers[0].reg == 0);
    CHECK(out.samplers[0].type == kTexture2D);

    const std::string &source = out.source;
    CHECK_CONTAINS(source, "layout(set = 0, binding = 2) uniform sampler2D s0");
    CHECK_CONTAINS(source, "layout(location = 0) in vec4 vary0");
    CHECK_CONTAINS(source, "layout(location = 0) out vec4 outColor0");
    CHECK_CONTAINS(source, "texture(s0, vec4(vary0).xy)");
    // Alpha test is fixed-function in Direct3D 9 and has no Vulkan state, so
    // it has to run in the shader.
    CHECK_CONTAINS(source, "kisak_alphaTest");
    CHECK_CONTAINS(source, "discard");
    CHECK_CONTAINS(source, "outColor0 = oC[0]");
}

void TestWriteMasksAndSwizzles()
{
    // mov r0.xy, v0.zzzz  -> r0.xy = vec4(vary0.zzzz).xy
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        // swizzle .zzzz is 2 in every pair: 0b10101010 = 0xAA
        InstructionToken(OpMov, 2), Dest(RegTemp, 0, 0x3), Src(RegInput, 0, 0xAA),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, ".zzzz");
    CHECK_CONTAINS(source, "r0.xy =");
    // Writing two components must not assign a vec4 to a vec2.
    CHECK_CONTAINS(source, ").xy;");
}

void TestSourceModifiers()
{
    // add r0, -v0, v0_bx2
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpAdd, 3), Dest(RegTemp, 0), Src(RegInput, 0, 0xE4, 1 /* neg */),
        Src(RegInput, 0, 0xE4, 4 /* sign */),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "(-vary0)");
    // _bx2 is (x - 0.5) * 2.
    CHECK_CONTAINS(source, "((vary0 - 0.5) * 2.0)");
}

void TestSaturateModifier()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0, 0xF, 0x1 /* saturate */), Src(RegInput, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "kisak_sat(");
}

void TestDefConstantsAreFolded()
{
    // def c4, 1, 2, 3, 4 ; mul r0, v0, c4
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpDef, 5), Dest(RegConst, 4), FloatBits(1.0f), FloatBits(2.0f), FloatBits(3.0f),
        FloatBits(4.5f),
        InstructionToken(OpMul, 3), Dest(RegTemp, 0), Src(RegInput, 0), Src(RegConst, 4),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    // A def is part of the program, not uploaded state: it must become a
    // literal the compiler can fold, not a uniform read.
    CHECK_CONTAINS(source, "const vec4 cdef4 = vec4(1.0, 2.0, 3.0, 4.5)");
    CHECK_CONTAINS(source, "cdef4");
    CHECK_LACKS(source, "c[4]");
}

void TestFlowControl()
{
    // if_gt c0.x, c1.x { mov r0, c2 } else { mov r0, c3 } endif
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpIfc, 2, 1 /* gt */), Src(RegConst, 0), Src(RegConst, 1),
        InstructionToken(OpMov, 2), Dest(RegTemp, 0), Src(RegConst, 2),
        InstructionToken(OpElse, 0),
        InstructionToken(OpMov, 2), Dest(RegTemp, 0), Src(RegConst, 3),
        InstructionToken(OpEndIf, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "if (vec4(c[0]).x > vec4(c[1]).x) {");
    CHECK_CONTAINS(source, "} else {");
    CHECK_CONTAINS(source, "}");
}

void TestRepLoop()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDef, 5), Dest(RegConst, 0), FloatBits(4.0f), FloatBits(0.0f), FloatBits(0.0f),
        FloatBits(0.0f),
        InstructionToken(OpRep, 1), Src(RegConst, 0),
        InstructionToken(OpAdd, 3), Dest(RegTemp, 0), Src(RegTemp, 0), Src(RegConst, 1),
        InstructionToken(OpBreakC, 2, 4 /* lt */), Src(RegTemp, 0), Src(RegConst, 2),
        InstructionToken(OpEndRep, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "for (int rep0 = 0;");
    CHECK_CONTAINS(source, "break;");
    // Nesting must close: one open brace per loop, one close.
    std::size_t open = 0, close = 0;
    for (const char c : source)
    {
        if (c == '{') ++open;
        if (c == '}') ++close;
    }
    CHECK(open == close);
}

void TestLoopRegisterAndRelativeAddressing()
{
    // loop aL, i0 ; add r0, r0, c[aL + 2] ; endloop
    std::vector<uint32_t> tokens = {
        VersionToken(false, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsagePosition, 0), Dest(RegInput, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsagePosition, 0), Dest(RegOutput, 0),
        InstructionToken(OpLoop, 2), Dest(RegLoop, 0), Src(RegConstInt, 0),
        InstructionToken(OpAdd, 4), Dest(RegTemp, 0), Src(RegTemp, 0), Src(RegConst, 2, 0xE4, 0, true),
        Src(RegLoop, 0),
        InstructionToken(OpEndLoop, 0),
        InstructionToken(OpMov, 2), Dest(RegOutput, 0), Src(RegTemp, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "aL.x");
    // A relative index must be clamped: out-of-range reads are undefined in
    // Vulkan and fault on some drivers, where Direct3D 9 returned zero.
    CHECK_CONTAINS(source, "clamp(");
    // Relative addressing means the whole constant file has to be declared.
    CHECK_CONTAINS(source, "vec4 c[256]");
}

void TestShadowSampler()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpDcl, 2), SamplerDeclToken(kTexture2D), Dest(RegSampler, 3),
        InstructionToken(OpTex, 3), Dest(RegTemp, 0), Src(RegInput, 0), Src(RegSampler, 3),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegTemp, 0),
        OpEnd,
    };
    TranslateOptions options;
    options.depthSamplerMask = 1u << 3;
    const std::string source = Translate(tokens, options);
    CHECK_CONTAINS(source, "sampler2DShadow s3");
    // The comparison value rides in the coordinate for a shadow sampler.
    CHECK_CONTAINS(source, "texture(s3, vec4(vary0).xyz)");
}

void TestCubeAndVolumeSamplers()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpDcl, 2), SamplerDeclToken(kTextureCube), Dest(RegSampler, 1),
        InstructionToken(OpDcl, 2), SamplerDeclToken(kTextureVolume), Dest(RegSampler, 2),
        InstructionToken(OpTex, 3), Dest(RegTemp, 0), Src(RegInput, 0), Src(RegSampler, 1),
        InstructionToken(OpTex, 3), Dest(RegTemp, 1), Src(RegInput, 0), Src(RegSampler, 2),
        InstructionToken(OpAdd, 3), Dest(RegColorOut, 0), Src(RegTemp, 0), Src(RegTemp, 1),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "samplerCube s1");
    CHECK_CONTAINS(source, "sampler3D s2");
    CHECK_CONTAINS(source, "texture(s1, vec4(vary0).xyz)");
}

void TestMissingVaryingBecomesConstant()
{
    // Direct3D 9 allowed a pixel shader to read a varying nothing wrote.
    // Vulkan rejects the pipeline, so the translator has to substitute.
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 5), Dest(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegInput, 0),
        OpEnd,
    };
    TranslateOptions options;
    options.availableVaryings = 0; // vertex shader wrote nothing
    const std::string source = Translate(tokens, options);
    CHECK_CONTAINS(source, "const vec4 vary0 = vec4(0.0)");
    CHECK_LACKS(source, "in vec4 vary0");
}

void TestIntegerVertexAttributes()
{
    // Blend indices arrive as an unnormalised UBYTE4; Vulkan will not convert
    // them to float the way Direct3D 9 did.
    std::vector<uint32_t> tokens = {
        VersionToken(false, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageBlendIndices, 0), Dest(RegInput, 2),
        InstructionToken(OpDcl, 2), DeclToken(kUsagePosition, 0), Dest(RegOutput, 0),
        InstructionToken(OpMov, 2), Dest(RegOutput, 0), Src(RegInput, 2),
        OpEnd,
    };
    TranslateOptions options;
    options.unsignedIntInputMask = 1u << 2; // blendindices is attribute slot 2
    const std::string source = Translate(tokens, options);
    CHECK_CONTAINS(source, "in uvec4 attr2");
    CHECK_CONTAINS(source, "vec4(attr2)");
}

void TestTexkillAndDepthOutput()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpTexKill, 1), Src(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegDepthOut, 0), Src(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegInput, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "discard");
    CHECK_CONTAINS(source, "gl_FragDepth = oDepth");
}

void TestMultipleRenderTargets()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 2), Src(RegInput, 0),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "layout(location = 0) out vec4 outColor0");
    CHECK_CONTAINS(source, "layout(location = 2) out vec4 outColor2");
}

void TestVPosAndVFace()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpMov, 2), Dest(RegTemp, 0), Src(RegMiscType, 0),
        InstructionToken(OpMov, 2), Dest(RegTemp, 1), Src(RegMiscType, 1),
        InstructionToken(OpAdd, 3), Dest(RegColorOut, 0), Src(RegTemp, 0), Src(RegTemp, 1),
        OpEnd,
    };
    const std::string source = Translate(tokens, TranslateOptions{});
    CHECK_CONTAINS(source, "gl_FragCoord.xy");
    CHECK_CONTAINS(source, "gl_FrontFacing");
}

void TestRejectsBadInput()
{
    // Not a Direct3D shader at all.
    std::vector<uint32_t> garbage = { 0x12345678u, OpEnd };
    Translate(garbage, TranslateOptions{}, false);

    // Truncated: an instruction claiming more tokens than remain.
    std::vector<uint32_t> truncated = { VersionToken(true, 3, 0), InstructionToken(OpMov, 8), Dest(RegTemp, 0) };
    Translate(truncated, TranslateOptions{}, false);

    // Shader model 1 is older than anything the game ships.
    std::vector<uint32_t> ancient = { VersionToken(true, 1, 4), OpEnd };
    Translate(ancient, TranslateOptions{}, false);

    // Empty.
    TranslatedShader out;
    CHECK(!TranslateShader(nullptr, 0, TranslateOptions{}, out));
    CHECK(!out.error.empty());
}

void TestAttributeSlotsAreStable()
{
    // The vertex layout is created without reference to any shader, so these
    // numbers are a contract between the translator and the backend. Changing
    // one without the other produces silently wrong geometry.
    CHECK(AttributeSlot(kUsagePosition, 0) == 0);
    CHECK(AttributeSlot(kUsageBlendWeight, 0) == 1);
    CHECK(AttributeSlot(kUsageBlendIndices, 0) == 2);
    CHECK(AttributeSlot(kUsageNormal, 0) == 3);
    CHECK(AttributeSlot(kUsageTexCoord, 0) == 4);
    CHECK(AttributeSlot(kUsageTexCoord, 7) == 11);
    CHECK(AttributeSlot(kUsageTangent, 0) == 12);
    CHECK(AttributeSlot(kUsageBinormal, 0) == 13);
    CHECK(AttributeSlot(kUsageColor, 0) == 14);
    CHECK(AttributeSlot(kUsageColor, 1) == 15);
    CHECK(AttributeSlot(kUsageTexCoord, 9) == -1);
    CHECK(AttributeSlot(kUsageFog, 0) == -1);
}

void TestMediumPrecisionOption()
{
    std::vector<uint32_t> tokens = {
        VersionToken(true, 3, 0),
        InstructionToken(OpDcl, 2), DeclToken(kUsageTexCoord, 0), Dest(RegInput, 0),
        InstructionToken(OpMov, 2), Dest(RegColorOut, 0), Src(RegInput, 0),
        OpEnd,
    };
    TranslateOptions options;
    options.preferMediumPrecision = true;
    const std::string source = Translate(tokens, options);
    CHECK_CONTAINS(source, "precision mediump float");
}

} // namespace

int main()
{
    TestVertexShaderBasics();
    TestPixelShaderBasics();
    TestWriteMasksAndSwizzles();
    TestSourceModifiers();
    TestSaturateModifier();
    TestDefConstantsAreFolded();
    TestFlowControl();
    TestRepLoop();
    TestLoopRegisterAndRelativeAddressing();
    TestShadowSampler();
    TestCubeAndVolumeSamplers();
    TestMissingVaryingBecomesConstant();
    TestIntegerVertexAttributes();
    TestTexkillAndDepthOutput();
    TestMultipleRenderTargets();
    TestVPosAndVFace();
    TestRejectsBadInput();
    TestAttributeSlotsAreStable();
    TestMediumPrecisionOption();

    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
