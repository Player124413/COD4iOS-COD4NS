#include "dx9_glsl_translator.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace kisak::vk {

namespace {

// D3DSIO_* opcodes.
enum : uint32_t
{
    OpNop = 0, OpMov = 1, OpAdd = 2, OpSub = 3, OpMad = 4, OpMul = 5, OpRcp = 6, OpRsq = 7, OpDp3 = 8, OpDp4 = 9,
    OpMin = 10, OpMax = 11, OpSlt = 12, OpSge = 13, OpExp = 14, OpLog = 15, OpLit = 16, OpDst = 17, OpLrp = 18,
    OpFrc = 19, OpM4x4 = 20, OpM4x3 = 21, OpM3x4 = 22, OpM3x3 = 23, OpM3x2 = 24, OpCall = 25, OpCallNz = 26,
    OpLoop = 27, OpRet = 28, OpEndLoop = 29, OpLabel = 30, OpDcl = 31, OpPow = 32, OpCrs = 33, OpSgn = 34,
    OpAbs = 35, OpNrm = 36, OpSinCos = 37, OpRep = 38, OpEndRep = 39, OpIf = 40, OpIfc = 41, OpElse = 42,
    OpEndIf = 43, OpBreak = 44, OpBreakC = 45, OpMova = 46, OpDefB = 47, OpDefI = 48,
    OpTexCoord = 64, OpTexKill = 65, OpTex = 66, OpCnd = 80, OpDef = 81, OpCmp = 88, OpDp2Add = 90, OpDsx = 91,
    OpDsy = 92, OpTexLdd = 93, OpTexLdl = 95,
    OpComment = 0xFFFE, OpEnd = 0xFFFF,
};

// D3DSPR_* register types.
enum : uint32_t
{
    RegTemp = 0, RegInput = 1, RegConst = 2, RegTexture = 3, RegRastOut = 4, RegAttrOut = 5, RegOutput = 6,
    RegConstInt = 7, RegColorOut = 8, RegDepthOut = 9, RegSampler = 10, RegConstBool = 14, RegLoop = 15,
    RegMiscType = 17, RegLabel = 18, RegPredicate = 19,
};

// Source modifiers (D3DSPSM_*), in bits 24..27 of a source token.
enum : uint32_t
{
    ModNone = 0, ModNeg = 1, ModBias = 2, ModBiasNeg = 3, ModSign = 4, ModSignNeg = 5, ModComp = 6, ModX2 = 7,
    ModX2Neg = 8, ModDz = 9, ModDw = 10, ModAbs = 11, ModAbsNeg = 12, ModNot = 13,
};

// Destination modifiers (D3DSPDM_*), in bits 20..23 of a destination token.
constexpr uint32_t DstModSaturate = 0x1;
constexpr uint32_t DstModPartialPrecision = 0x2;

uint32_t RegisterType(uint32_t token) { return ((token >> 28) & 0x7) | ((token >> 8) & 0x18); }
uint32_t RegisterNumber(uint32_t token) { return token & 0x7FF; }
bool IsRelative(uint32_t token) { return (token & 0x2000) != 0; }
uint32_t WriteMask(uint32_t token) { return (token >> 16) & 0xF; }
uint32_t DestModifier(uint32_t token) { return (token >> 20) & 0xF; }
uint32_t Swizzle(uint32_t token) { return (token >> 16) & 0xFF; }
uint32_t SourceModifier(uint32_t token) { return (token >> 24) & 0xF; }

std::string FloatLiteral(float value)
{
    if (value != value)
        return "(0.0/0.0)";
    if (value == std::numeric_limits<float>::infinity())
        return "(1.0/0.0)";
    if (value == -std::numeric_limits<float>::infinity())
        return "(-1.0/0.0)";
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 9);
    std::string text(buffer, result.ptr);
    if (text.find_first_of(".e") == std::string::npos)
        text += ".0";
    return text;
}

int VaryingBit(uint8_t usage, uint8_t index)
{
    switch (usage)
    {
    case kUsageTexCoord: return index < 8 ? index : -1;
    case kUsageColor: return index < 2 ? 8 + index : -1;
    case kUsageFog: return index == 0 ? 10 : -1;
    case kUsageNormal: return index == 0 ? 11 : -1;
    case kUsageTangent: return index == 0 ? 12 : -1;
    case kUsageBinormal: return index == 0 ? 13 : -1;
    default: return -1;
    }
}

const char *UsageName(uint8_t usage)
{
    switch (usage)
    {
    case kUsagePosition: return "position";
    case kUsageBlendWeight: return "blendweight";
    case kUsageBlendIndices: return "blendindices";
    case kUsageNormal: return "normal";
    case kUsagePointSize: return "psize";
    case kUsageTexCoord: return "texcoord";
    case kUsageTangent: return "tangent";
    case kUsageBinormal: return "binormal";
    case kUsageTessFactor: return "tessfactor";
    case kUsagePositionT: return "positiont";
    case kUsageColor: return "color";
    case kUsageFog: return "fog";
    case kUsageDepth: return "depth";
    case kUsageSample: return "sample";
    default: return "usage";
    }
}

std::string SemanticName(uint8_t usage, uint8_t index)
{
    return std::string(UsageName(usage)) + std::to_string(index);
}

// Varyings are passed by a fixed location so the vertex and fragment stages
// link without the translator having to see both at once. Direct3D 9 matched
// by semantic; Vulkan matches by location, so the mapping has to be a pure
// function of the semantic.
int VaryingLocation(uint8_t usage, uint8_t index)
{
    const int bit = VaryingBit(usage, index);
    return bit < 0 ? -1 : bit;
}

struct Instruction
{
    uint32_t opcode = 0;
    uint32_t controls = 0;
    const uint32_t *params = nullptr;
    uint32_t length = 0;
};

class Translator
{
public:
    Translator(const uint32_t *tokens, size_t count, const TranslateOptions &options, TranslatedShader &out)
        : m_tokens(tokens), m_count(count), m_options(options), m_out(out)
    {
    }

    bool Run()
    {
        if (m_count < 2)
            return Fail("program too short");
        const uint32_t version = m_tokens[0];
        const uint32_t kind = version >> 16;
        if (kind != 0xFFFF && kind != 0xFFFE)
            return Fail("not a Direct3D 9 shader");
        m_out.pixel = kind == 0xFFFF;
        const uint32_t major = (version >> 8) & 0xFF;
        if (major < 2 || major > 3)
            return Fail("only shader model 2 and 3 are supported");

        // Two passes. The first collects declarations, constants and the
        // highest temp/constant register touched, because GLSL needs every
        // declaration before the first statement; the second emits the body.
        if (!Scan())
            return false;
        EmitPrologue();
        if (!EmitBody())
            return false;
        EmitEpilogue();
        m_out.source = m_header + m_body;
        return true;
    }

private:
    bool Fail(const char *reason)
    {
        m_failed = true;
        if (m_out.error.empty())
            m_out.error = reason;
        return false;
    }

    // -------------------------------------------------------------------
    // Token walking

    bool Next(Instruction &instruction)
    {
        if (m_at >= m_count)
            return false;
        const uint32_t token = m_tokens[m_at];
        instruction.opcode = token & 0xFFFF;
        instruction.controls = (token >> 16) & 0xFF;
        if (instruction.opcode == OpEnd)
            return false;
        uint32_t length = 0;
        if (instruction.opcode == OpComment)
        {
            length = (token >> 16) & 0x7FFF;
        }
        else
        {
            length = (token >> 24) & 0xF;
            // Shader model 1 encoded no length; 2 and 3 always do.
        }
        if (m_at + 1 + length > m_count)
        {
            Fail("instruction runs past the end of the program");
            return false;
        }
        instruction.params = m_tokens + m_at + 1;
        instruction.length = length;
        m_at += 1 + length;
        return true;
    }

    // -------------------------------------------------------------------
    // Pass one

    bool Scan()
    {
        m_at = 1;
        Instruction instruction;
        while (Next(instruction))
        {
            switch (instruction.opcode)
            {
            case OpComment:
                break;
            case OpDcl:
                if (instruction.length < 2)
                    return Fail("malformed dcl");
                ScanDeclaration(instruction);
                break;
            case OpDef:
                if (instruction.length < 5)
                    return Fail("malformed def");
                {
                    const uint32_t reg = RegisterNumber(instruction.params[0]);
                    float values[4];
                    std::memcpy(values, instruction.params + 1, sizeof(values));
                    m_floatDefs[reg] = { values[0], values[1], values[2], values[3] };
                }
                break;
            case OpDefI:
                if (instruction.length < 5)
                    return Fail("malformed defi");
                {
                    const uint32_t reg = RegisterNumber(instruction.params[0]);
                    int32_t values[4];
                    std::memcpy(values, instruction.params + 1, sizeof(values));
                    m_intDefs[reg] = { values[0], values[1], values[2], values[3] };
                }
                break;
            case OpDefB:
                if (instruction.length < 2)
                    return Fail("malformed defb");
                m_boolDefs[RegisterNumber(instruction.params[0])] = instruction.params[1] != 0;
                break;
            case OpM4x4:
            case OpM4x3:
            case OpM3x4:
            case OpM3x3:
            case OpM3x2:
                ScanRegisters(instruction);
                ScanMatrixRows(instruction);
                break;
            default:
                ScanRegisters(instruction);
                break;
            }
        }
        if (m_failed)
            return false;
        return true;
    }

    void ScanDeclaration(const Instruction &instruction)
    {
        const uint32_t usageToken = instruction.params[0];
        const uint32_t destination = instruction.params[1];
        const uint32_t type = RegisterType(destination);
        const uint32_t number = RegisterNumber(destination);

        if (type == RegSampler)
        {
            ShaderSampler sampler;
            sampler.reg = static_cast<uint16_t>(number);
            // dcl_2d / dcl_cube / dcl_volume live in bits 27..30.
            sampler.type = static_cast<uint8_t>((usageToken >> 27) & 0xF);
            m_out.samplers.push_back(sampler);
            m_samplerTypes[number] = sampler.type;
            return;
        }

        ShaderSemantic semantic;
        semantic.usage = static_cast<uint8_t>(usageToken & 0x1F);
        semantic.index = static_cast<uint8_t>((usageToken >> 16) & 0xF);
        semantic.reg = static_cast<uint16_t>(number);

        if (type == RegInput || (m_out.pixel && type == RegTexture))
        {
            m_out.inputs.push_back(semantic);
            m_declaredInputs[number] = semantic;
        }
        else if (type == RegOutput || type == RegAttrOut)
        {
            m_out.outputs.push_back(semantic);
            m_declaredOutputs[number] = semantic;
        }
    }

    // m4x4 dst, src, c0 reads c0..c3; only the base appears in the token
    // stream, so the constant count has to be extended by hand or the uniform
    // block is declared too small and the rows past the first read garbage.
    void ScanMatrixRows(const Instruction &instruction)
    {
        if (instruction.length < 3)
            return;
        const uint32_t matrixToken = instruction.params[2];
        if (RegisterType(matrixToken) != RegConst || IsRelative(matrixToken))
            return;
        uint32_t rows = 4;
        switch (instruction.opcode)
        {
        case OpM4x4: rows = 4; break;
        case OpM4x3: rows = 3; break;
        case OpM3x4: rows = 4; break;
        case OpM3x3: rows = 3; break;
        case OpM3x2: rows = 2; break;
        default: return;
        }
        m_out.constantCount = std::max(m_out.constantCount, RegisterNumber(matrixToken) + rows);
    }

    void ScanRegisters(const Instruction &instruction)
    {
        for (uint32_t i = 0; i < instruction.length; ++i)
        {
            const uint32_t token = instruction.params[i];
            // Relative addressing adds an extra token that is itself a
            // register reference; counting it as a plain operand would
            // misattribute its type.
            const uint32_t type = RegisterType(token);
            const uint32_t number = RegisterNumber(token);
            switch (type)
            {
            case RegTemp: m_maxTemp = std::max(m_maxTemp, number + 1); break;
            case RegConst:
                if (IsRelative(token))
                    m_relativeConstants = true;
                else
                    m_out.constantCount = std::max(m_out.constantCount, number + 1);
                break;
            case RegLoop: m_usesLoopRegister = true; break;
            case RegColorOut: m_maxColorOut = std::max(m_maxColorOut, number + 1); break;
            case RegDepthOut: m_writesDepth = true; break;
            default: break;
            }
        }
    }

    // -------------------------------------------------------------------
    // Pass two: declarations

    void Line(const char *text)
    {
        m_body += m_indent;
        m_body += text;
        m_body += '\n';
    }

    void Line(const std::string &text)
    {
        m_body += m_indent;
        m_body += text;
        m_body += '\n';
    }

    void Push() { m_indent += "    "; }
    void Pop()
    {
        if (m_indent.size() >= 4)
            m_indent.resize(m_indent.size() - 4);
    }

    void EmitPrologue()
    {
        m_header = "#version 450\n";
        m_header += "// generated from Direct3D 9 shader model bytecode by\n";
        m_header += "// ports/android/gfx/dx9_glsl_translator.cpp - do not edit\n";
        if (m_options.preferMediumPrecision && m_out.pixel)
            m_header += "precision mediump float;\n";

        // Float constants. One uniform block rather than push constants: the
        // engine rewrites up to 256 vec4 per draw, which is far beyond the
        // 128-byte push constant guarantee.
        const uint32_t constants = m_relativeConstants ? (m_out.pixel ? 224u : 256u)
                                                       : std::max<uint32_t>(m_out.constantCount, 1u);
        m_out.constantCount = constants;
        char buffer[256];
        std::snprintf(buffer, sizeof(buffer),
                      "layout(set = %u, binding = %u, std140) uniform ShaderConstants { vec4 c[%u]; };\n",
                      kDescriptorSet, m_out.pixel ? kFragmentConstantBinding : kVertexConstantBinding, constants);
        m_header += buffer;

        if (m_out.pixel)
        {
            std::snprintf(buffer, sizeof(buffer),
                          "layout(push_constant) uniform FragmentParams { int alphaFunc; float alphaRef; } params;\n");
            m_header += buffer;
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer),
                          "layout(push_constant) uniform VertexParams { vec2 halfPixel; } params;\n");
            m_header += buffer;
        }

        EmitSamplerDeclarations();
        EmitIoDeclarations();

        // Helper functions. Direct3D 9 semantics that GLSL does not share.
        m_header +=
            "float kisak_rcp(float v) { return v == 0.0 ? 3.402823466e38 : 1.0 / v; }\n"
            "float kisak_rsq(float v) { return v == 0.0 ? 3.402823466e38 : inversesqrt(abs(v)); }\n"
            // D3D log/exp are base 2 and clamp at zero rather than returning -inf.
            "float kisak_log2(float v) { return v == 0.0 ? -3.402823466e38 : log2(abs(v)); }\n"
            // frc is HLSL frac: x - floor(x), which differs from GLSL mod for negatives.
            "vec4 kisak_frc(vec4 v) { return v - floor(v); }\n"
            "vec4 kisak_sat(vec4 v) { return clamp(v, 0.0, 1.0); }\n"
            // pow(x, y) in SM3 is exp2(y * log2(abs(x))).
            "float kisak_pow(float x, float y) { return exp2(y * kisak_log2(x)); }\n"
            "vec4 kisak_cmp(vec4 cond, vec4 a, vec4 b) { return mix(b, a, step(vec4(0.0), cond)); }\n"
            "vec4 kisak_cnd(vec4 cond, vec4 a, vec4 b) { return mix(b, a, step(vec4(0.5), cond)); }\n"
            "vec4 kisak_sgn(vec4 v) { return sign(v); }\n"
            "vec4 kisak_lit(vec4 v) {\n"
            "    float d = v.y * (v.x > 0.0 ? 1.0 : 0.0);\n"
            "    return vec4(1.0, max(v.x, 0.0), v.x > 0.0 && v.y > 0.0 ? kisak_pow(v.y, v.w) : 0.0, 1.0);\n"
            "}\n"
            "vec4 kisak_dst(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }\n";

        if (m_out.pixel)
        {
            // Alpha test is fixed-function in Direct3D 9 and does not exist in
            // Vulkan at all, so it runs here, driven by a push constant. A
            // specialisation constant would let the driver fold the branch
            // away, but it would also multiply the pipeline count by eight.
            m_header +=
                "bool kisak_alphaTest(float a) {\n"
                "    switch (params.alphaFunc) {\n"
                "    case 1: return false;\n"                       // Never
                "    case 2: return a < params.alphaRef;\n"          // Less
                "    case 3: return a == params.alphaRef;\n"         // Equal
                "    case 4: return a <= params.alphaRef;\n"         // LessEqual
                "    case 5: return a > params.alphaRef;\n"          // Greater
                "    case 6: return a != params.alphaRef;\n"         // NotEqual
                "    case 7: return a >= params.alphaRef;\n"         // GreaterEqual
                "    default: return true;\n"                        // Always / disabled
                "    }\n"
                "}\n";
        }
    }

    void EmitSamplerDeclarations()
    {
        char buffer[256];
        for (const ShaderSampler &sampler : m_out.samplers)
        {
            const bool shadow = (m_options.depthSamplerMask & (1u << sampler.reg)) != 0;
            const char *type = "sampler2D";
            switch (sampler.type)
            {
            case kTextureCube: type = "samplerCube"; break;
            case kTextureVolume: type = "sampler3D"; break;
            default: type = shadow ? "sampler2DShadow" : "sampler2D"; break;
            }
            std::snprintf(buffer, sizeof(buffer), "layout(set = %u, binding = %u) uniform %s s%u;\n", kDescriptorSet,
                          kSamplerBindingBase + sampler.reg, type, sampler.reg);
            m_header += buffer;
        }
    }

    void EmitIoDeclarations()
    {
        char buffer[256];
        if (!m_out.pixel)
        {
            for (const auto &entry : m_declaredInputs)
            {
                const ShaderSemantic &semantic = entry.second;
                const int slot = AttributeSlot(semantic.usage, semantic.index);
                if (slot < 0)
                    continue;
                const char *type = "vec4";
                if (m_options.unsignedIntInputMask & (1u << slot))
                    type = "uvec4";
                else if (m_options.signedIntInputMask & (1u << slot))
                    type = "ivec4";
                std::snprintf(buffer, sizeof(buffer), "layout(location = %d) in %s attr%u; // %s\n", slot, type,
                              entry.first, SemanticName(semantic.usage, semantic.index).c_str());
                m_header += buffer;
            }
            for (const auto &entry : m_declaredOutputs)
            {
                const ShaderSemantic &semantic = entry.second;
                if (semantic.usage == kUsagePosition || semantic.usage == kUsagePositionT)
                    continue; // gl_Position
                if (semantic.usage == kUsagePointSize)
                    continue; // gl_PointSize
                const int location = VaryingLocation(semantic.usage, semantic.index);
                if (location < 0)
                    continue;
                std::snprintf(buffer, sizeof(buffer), "layout(location = %d) out vec4 vary%u; // %s\n", location,
                              entry.first, SemanticName(semantic.usage, semantic.index).c_str());
                m_header += buffer;
            }
        }
        else
        {
            for (const auto &entry : m_declaredInputs)
            {
                const ShaderSemantic &semantic = entry.second;
                const int location = VaryingLocation(semantic.usage, semantic.index);
                if (location < 0)
                    continue;
                const bool available = (m_options.availableVaryings & (1u << location)) != 0;
                if (!available)
                {
                    // Direct3D 9 let a pixel shader read a varying the vertex
                    // shader never wrote (it read as undefined, in practice
                    // zero). Vulkan fails pipeline creation, so synthesise a
                    // constant instead of refusing the shader.
                    std::snprintf(buffer, sizeof(buffer), "const vec4 vary%u = vec4(0.0); // %s not written\n",
                                  entry.first, SemanticName(semantic.usage, semantic.index).c_str());
                    m_header += buffer;
                    continue;
                }
                std::snprintf(buffer, sizeof(buffer), "layout(location = %d) in vec4 vary%u; // %s\n", location,
                              entry.first, SemanticName(semantic.usage, semantic.index).c_str());
                m_header += buffer;
            }
            const uint32_t targets = std::max<uint32_t>(m_maxColorOut, 1u);
            for (uint32_t i = 0; i < targets; ++i)
            {
                std::snprintf(buffer, sizeof(buffer), "layout(location = %u) out vec4 outColor%u;\n", i, i);
                m_header += buffer;
            }
        }
    }

    // -------------------------------------------------------------------
    // Pass two: body

    bool EmitBody()
    {
        m_body = "void main() {\n";
        m_indent = "    ";

        for (uint32_t i = 0; i < m_maxTemp; ++i)
            Line("vec4 r" + std::to_string(i) + " = vec4(0.0);");
        if (m_usesLoopRegister || m_relativeConstants)
            Line("ivec4 a0 = ivec4(0);");
        if (m_usesLoopRegister)
            Line("ivec4 aL = ivec4(0);");

        // `def` constants are baked in rather than uploaded: they are part of
        // the program, and folding them lets the compiler constant-fold.
        for (const auto &entry : m_floatDefs)
        {
            const auto &v = entry.second;
            Line("const vec4 cdef" + std::to_string(entry.first) + " = vec4(" + FloatLiteral(v[0]) + ", " +
                 FloatLiteral(v[1]) + ", " + FloatLiteral(v[2]) + ", " + FloatLiteral(v[3]) + ");");
        }
        for (const auto &entry : m_intDefs)
        {
            const auto &v = entry.second;
            Line("const ivec4 idef" + std::to_string(entry.first) + " = ivec4(" + std::to_string(v[0]) + ", " +
                 std::to_string(v[1]) + ", " + std::to_string(v[2]) + ", " + std::to_string(v[3]) + ");");
        }

        if (!m_out.pixel)
            Line("vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);");
        else
            Line("vec4 oC[4] = vec4[4](vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));");
        if (m_writesDepth)
            Line("float oDepth = gl_FragCoord.z;");

        m_at = 1;
        Instruction instruction;
        while (Next(instruction))
        {
            if (!EmitInstruction(instruction))
                return false;
        }
        return !m_failed;
    }

    void EmitEpilogue()
    {
        if (!m_out.pixel)
        {
            // Direct3D 9 samples pixel centres half a texel off from Vulkan's
            // convention. The offset is in clip space and therefore scales
            // with w. Unlike Metal, no Y flip is needed: Vulkan's clip space
            // already points down, matching Direct3D.
            Line("oPos.xy += params.halfPixel * oPos.w;");
            Line("gl_Position = oPos;");
        }
        else
        {
            Line("if (!kisak_alphaTest(oC[0].a)) discard;");
            const uint32_t targets = std::max<uint32_t>(m_maxColorOut, 1u);
            for (uint32_t i = 0; i < targets; ++i)
                Line("outColor" + std::to_string(i) + " = oC[" + std::to_string(i) + "];");
            if (m_writesDepth)
                Line("gl_FragDepth = oDepth;");
        }
        m_indent.clear();
        m_body += "}\n";
    }

    // Component mask as a GLSL swizzle suffix, or "" for all four.
    static std::string MaskSuffix(uint32_t mask)
    {
        if ((mask & 0xF) == 0xF)
            return "";
        std::string out = ".";
        if (mask & 1) out += 'x';
        if (mask & 2) out += 'y';
        if (mask & 4) out += 'z';
        if (mask & 8) out += 'w';
        return out;
    }

    static int MaskWidth(uint32_t mask)
    {
        return ((mask & 1) ? 1 : 0) + ((mask & 2) ? 1 : 0) + ((mask & 4) ? 1 : 0) + ((mask & 8) ? 1 : 0);
    }

    static std::string SwizzleSuffix(uint32_t swizzle)
    {
        static const char components[] = "xyzw";
        const uint32_t x = swizzle & 3;
        const uint32_t y = (swizzle >> 2) & 3;
        const uint32_t z = (swizzle >> 4) & 3;
        const uint32_t w = (swizzle >> 6) & 3;
        if (x == 0 && y == 1 && z == 2 && w == 3)
            return "";
        std::string out = ".";
        out += components[x];
        out += components[y];
        out += components[z];
        out += components[w];
        return out;
    }

    std::string SourceRegister(uint32_t token, const uint32_t *relativeToken)
    {
        const uint32_t type = RegisterType(token);
        const uint32_t number = RegisterNumber(token);
        std::string name;

        switch (type)
        {
        case RegTemp:
            name = "r" + std::to_string(number);
            break;
        case RegInput:
        case RegTexture:
            if (m_out.pixel)
            {
                name = m_declaredInputs.count(number) ? "vary" + std::to_string(number) : "vec4(0.0)";
            }
            else
            {
                const auto it = m_declaredInputs.find(number);
                if (it == m_declaredInputs.end())
                {
                    name = "vec4(0.0)";
                }
                else
                {
                    const int slot = AttributeSlot(it->second.usage, it->second.index);
                    const bool isUnsigned = slot >= 0 && (m_options.unsignedIntInputMask & (1u << slot));
                    const bool isSigned = slot >= 0 && (m_options.signedIntInputMask & (1u << slot));
                    name = "attr" + std::to_string(number);
                    if (isUnsigned || isSigned)
                        name = "vec4(" + name + ")";
                }
            }
            break;
        case RegConst:
            if (m_floatDefs.count(number) && !IsRelative(token))
            {
                name = "cdef" + std::to_string(number);
            }
            else if (IsRelative(token) && relativeToken)
            {
                // c[a0.x + n]: the loop counter and address register both land
                // here. Clamping keeps a bad index from reading out of the
                // block, which is undefined in Vulkan and a GPU fault on some
                // drivers rather than the zero Direct3D 9 returned.
                const uint32_t relativeType = RegisterType(*relativeToken);
                const std::string index = relativeType == RegLoop ? "aL.x" : "a0.x";
                name = "c[clamp(" + index + " + " + std::to_string(number) + ", 0, " +
                       std::to_string(static_cast<int>(m_out.constantCount) - 1) + ")]";
            }
            else
            {
                name = "c[" + std::to_string(number) + "]";
            }
            break;
        case RegConstInt:
            name = m_intDefs.count(number) ? "vec4(idef" + std::to_string(number) + ")" : "vec4(0.0)";
            break;
        case RegConstBool:
            name = m_boolDefs.count(number) && m_boolDefs[number] ? "vec4(1.0)" : "vec4(0.0)";
            break;
        case RegLoop:
            name = "vec4(aL)";
            break;
        case RegMiscType:
            // vPos (0) and vFace (1) in pixel shaders.
            name = number == 0 ? "vec4(gl_FragCoord.xy, 0.0, 0.0)"
                               : "vec4(gl_FrontFacing ? 1.0 : -1.0)";
            break;
        case RegSampler:
            name = "s" + std::to_string(number);
            return name; // samplers take no swizzle or modifier
        default:
            name = "vec4(0.0)";
            break;
        }

        name += SwizzleSuffix(Swizzle(token));
        return ApplyModifier(name, SourceModifier(token));
    }

    static std::string ApplyModifier(const std::string &value, uint32_t modifier)
    {
        switch (modifier)
        {
        case ModNone: return value;
        case ModNeg: return "(-" + value + ")";
        case ModBias: return "(" + value + " - 0.5)";
        case ModBiasNeg: return "(-(" + value + " - 0.5))";
        case ModSign: return "((" + value + " - 0.5) * 2.0)";
        case ModSignNeg: return "(-((" + value + " - 0.5) * 2.0))";
        case ModComp: return "(1.0 - " + value + ")";
        case ModX2: return "(" + value + " * 2.0)";
        case ModX2Neg: return "(-(" + value + " * 2.0))";
        case ModAbs: return "abs(" + value + ")";
        case ModAbsNeg: return "(-abs(" + value + "))";
        case ModNot: return "(1.0 - " + value + ")";
        // _dz and _dw are projective divides used by the texture instructions
        // that this translator handles through texldp instead.
        default: return value;
        }
    }

    // Writes `expression` into the destination described by `token`.
    void EmitAssignment(uint32_t token, const std::string &expression)
    {
        const uint32_t type = RegisterType(token);
        const uint32_t number = RegisterNumber(token);
        const uint32_t mask = WriteMask(token);
        const uint32_t modifier = DestModifier(token);

        std::string target;
        switch (type)
        {
        case RegTemp: target = "r" + std::to_string(number); break;
        case RegOutput:
        case RegAttrOut:
            if (m_out.pixel)
            {
                target = "oC[" + std::to_string(number) + "]";
            }
            else
            {
                const auto it = m_declaredOutputs.find(number);
                if (it != m_declaredOutputs.end() &&
                    (it->second.usage == kUsagePosition || it->second.usage == kUsagePositionT))
                    target = "oPos";
                else if (it != m_declaredOutputs.end() && it->second.usage == kUsagePointSize)
                    target = "gl_PointSize_tmp";
                else
                    target = "vary" + std::to_string(number);
            }
            break;
        case RegRastOut:
            // 0 oPos, 1 oFog, 2 oPts in shader model 2 vertex shaders.
            target = number == 0 ? "oPos" : "vec4_discard";
            break;
        case RegColorOut: target = "oC[" + std::to_string(number) + "]"; break;
        case RegDepthOut: target = "oDepth"; break;
        case RegPredicate: target = "p" + std::to_string(number); break;
        default: target = "r0"; break;
        }

        if (target == "vec4_discard" || target == "gl_PointSize_tmp")
            return; // fog and point size have no Vulkan equivalent here

        std::string value = expression;
        if (modifier & DstModSaturate)
            value = "kisak_sat(vec4(" + value + "))";

        const std::string suffix = MaskSuffix(mask);
        if (suffix.empty())
        {
            Line(target + " = vec4(" + value + ");");
        }
        else
        {
            // Narrow the right-hand side to the mask width so GLSL's strict
            // assignment typing accepts it.
            Line(target + suffix + " = vec4(" + value + ")" + suffix + ";");
        }
    }

    bool EmitInstruction(const Instruction &instruction)
    {
        switch (instruction.opcode)
        {
        case OpNop:
        case OpComment:
        case OpDcl:
        case OpDef:
        case OpDefI:
        case OpDefB:
        case OpLabel:
            return true;
        case OpEnd:
            return true;
        default:
            break;
        }

        // Operand layout: one destination then sources, except for the flow
        // control instructions which take only sources.
        const uint32_t *params = instruction.params;
        const uint32_t count = instruction.length;

        auto src = [&](uint32_t index) -> std::string {
            // Relative addressing inserts an extra token after the operand it
            // qualifies; Source() needs it to name the index register.
            const uint32_t token = params[index];
            const uint32_t *relative = IsRelative(token) && index + 1 < count ? &params[index + 1] : nullptr;
            return SourceRegister(token, relative);
        };
        auto srcIndex = [&](uint32_t start, uint32_t ordinal) -> uint32_t {
            // Walks past any relative-addressing tokens to find the nth source.
            uint32_t at = start;
            for (uint32_t i = 0; i < ordinal; ++i)
            {
                if (at < count && IsRelative(params[at]))
                    ++at;
                ++at;
            }
            return at;
        };

        switch (instruction.opcode)
        {
        case OpMov:
            if (count < 2) return Fail("mov needs two operands");
            EmitAssignment(params[0], src(srcIndex(1, 0)));
            return true;
        case OpMova:
            if (count < 2) return Fail("mova needs two operands");
            // Direct3D rounds to nearest; GLSL's int() truncates.
            Line("a0 = ivec4(round(vec4(" + src(srcIndex(1, 0)) + ")));");
            return true;
        case OpAdd:
            if (count < 3) return Fail("add needs three operands");
            EmitAssignment(params[0], "(" + src(srcIndex(1, 0)) + ") + (" + src(srcIndex(1, 1)) + ")");
            return true;
        case OpSub:
            if (count < 3) return Fail("sub needs three operands");
            EmitAssignment(params[0], "(" + src(srcIndex(1, 0)) + ") - (" + src(srcIndex(1, 1)) + ")");
            return true;
        case OpMul:
            if (count < 3) return Fail("mul needs three operands");
            EmitAssignment(params[0], "(" + src(srcIndex(1, 0)) + ") * (" + src(srcIndex(1, 1)) + ")");
            return true;
        case OpMad:
            if (count < 4) return Fail("mad needs four operands");
            EmitAssignment(params[0], "(" + src(srcIndex(1, 0)) + ") * (" + src(srcIndex(1, 1)) + ") + (" +
                                          src(srcIndex(1, 2)) + ")");
            return true;
        case OpLrp:
            if (count < 4) return Fail("lrp needs four operands");
            // lrp dst, a, b, c == c + a * (b - c), in that order.
            EmitAssignment(params[0], "mix(vec4(" + src(srcIndex(1, 2)) + "), vec4(" + src(srcIndex(1, 1)) +
                                          "), vec4(" + src(srcIndex(1, 0)) + "))");
            return true;
        case OpRcp:
            if (count < 2) return Fail("rcp needs two operands");
            EmitAssignment(params[0], "vec4(kisak_rcp(vec4(" + src(srcIndex(1, 0)) + ").x))");
            return true;
        case OpRsq:
            if (count < 2) return Fail("rsq needs two operands");
            EmitAssignment(params[0], "vec4(kisak_rsq(vec4(" + src(srcIndex(1, 0)) + ").x))");
            return true;
        case OpDp3:
            if (count < 3) return Fail("dp3 needs three operands");
            EmitAssignment(params[0], "vec4(dot(vec4(" + src(srcIndex(1, 0)) + ").xyz, vec4(" + src(srcIndex(1, 1)) +
                                          ").xyz))");
            return true;
        case OpDp4:
            if (count < 3) return Fail("dp4 needs three operands");
            EmitAssignment(params[0], "vec4(dot(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) + ")))");
            return true;
        case OpDp2Add:
            if (count < 4) return Fail("dp2add needs four operands");
            EmitAssignment(params[0], "vec4(dot(vec4(" + src(srcIndex(1, 0)) + ").xy, vec4(" + src(srcIndex(1, 1)) +
                                          ").xy) + vec4(" + src(srcIndex(1, 2)) + ").x)");
            return true;
        case OpMin:
            if (count < 3) return Fail("min needs three operands");
            EmitAssignment(params[0], "min(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) + "))");
            return true;
        case OpMax:
            if (count < 3) return Fail("max needs three operands");
            EmitAssignment(params[0], "max(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) + "))");
            return true;
        case OpSlt:
            if (count < 3) return Fail("slt needs three operands");
            EmitAssignment(params[0], "vec4(lessThan(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) +
                                          ")))");
            return true;
        case OpSge:
            if (count < 3) return Fail("sge needs three operands");
            EmitAssignment(params[0], "vec4(greaterThanEqual(vec4(" + src(srcIndex(1, 0)) + "), vec4(" +
                                          src(srcIndex(1, 1)) + ")))");
            return true;
        case OpExp:
            if (count < 2) return Fail("exp needs two operands");
            EmitAssignment(params[0], "vec4(exp2(vec4(" + src(srcIndex(1, 0)) + ").x))");
            return true;
        case OpLog:
            if (count < 2) return Fail("log needs two operands");
            EmitAssignment(params[0], "vec4(kisak_log2(vec4(" + src(srcIndex(1, 0)) + ").x))");
            return true;
        case OpPow:
            if (count < 3) return Fail("pow needs three operands");
            EmitAssignment(params[0], "vec4(kisak_pow(vec4(" + src(srcIndex(1, 0)) + ").x, vec4(" +
                                          src(srcIndex(1, 1)) + ").x))");
            return true;
        case OpFrc:
            if (count < 2) return Fail("frc needs two operands");
            EmitAssignment(params[0], "kisak_frc(vec4(" + src(srcIndex(1, 0)) + "))");
            return true;
        case OpAbs:
            if (count < 2) return Fail("abs needs two operands");
            EmitAssignment(params[0], "abs(vec4(" + src(srcIndex(1, 0)) + "))");
            return true;
        case OpSgn:
            if (count < 2) return Fail("sgn needs two operands");
            EmitAssignment(params[0], "kisak_sgn(vec4(" + src(srcIndex(1, 0)) + "))");
            return true;
        case OpNrm:
            if (count < 2) return Fail("nrm needs two operands");
            EmitAssignment(params[0], "vec4(normalize(vec4(" + src(srcIndex(1, 0)) + ").xyz), 0.0)");
            return true;
        case OpCrs:
            if (count < 3) return Fail("crs needs three operands");
            EmitAssignment(params[0], "vec4(cross(vec4(" + src(srcIndex(1, 0)) + ").xyz, vec4(" + src(srcIndex(1, 1)) +
                                          ").xyz), 0.0)");
            return true;
        case OpCmp:
            if (count < 4) return Fail("cmp needs four operands");
            EmitAssignment(params[0], "kisak_cmp(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) +
                                          "), vec4(" + src(srcIndex(1, 2)) + "))");
            return true;
        case OpCnd:
            if (count < 4) return Fail("cnd needs four operands");
            EmitAssignment(params[0], "kisak_cnd(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) +
                                          "), vec4(" + src(srcIndex(1, 2)) + "))");
            return true;
        case OpLit:
            if (count < 2) return Fail("lit needs two operands");
            EmitAssignment(params[0], "kisak_lit(vec4(" + src(srcIndex(1, 0)) + "))");
            return true;
        case OpDst:
            if (count < 3) return Fail("dst needs three operands");
            EmitAssignment(params[0], "kisak_dst(vec4(" + src(srcIndex(1, 0)) + "), vec4(" + src(srcIndex(1, 1)) + "))");
            return true;
        case OpSinCos:
            if (count < 2) return Fail("sincos needs operands");
            // Shader model 3 ignores the two coefficient sources.
            EmitAssignment(params[0], "vec4(cos(vec4(" + src(srcIndex(1, 0)) + ").x), sin(vec4(" +
                                          src(srcIndex(1, 0)) + ").x), 0.0, 0.0)");
            return true;
        case OpDsx:
            if (count < 2) return Fail("dsx needs two operands");
            EmitAssignment(params[0], "dFdx(vec4(" + src(srcIndex(1, 0)) + "))");
            return true;
        case OpDsy:
            if (count < 2) return Fail("dsy needs two operands");
            EmitAssignment(params[0], "dFdy(vec4(" + src(srcIndex(1, 0)) + "))");
            return true;

        case OpM4x4: return EmitMatrix(instruction, 4, 4);
        case OpM4x3: return EmitMatrix(instruction, 4, 3);
        case OpM3x4: return EmitMatrix(instruction, 3, 4);
        case OpM3x3: return EmitMatrix(instruction, 3, 3);
        case OpM3x2: return EmitMatrix(instruction, 3, 2);

        case OpTex:
        case OpTexCoord:
            return EmitTexture(instruction, false, false);
        case OpTexLdl:
            return EmitTexture(instruction, true, false);
        case OpTexLdd:
            return EmitTexture(instruction, false, true);
        case OpTexKill:
            if (count < 1) return Fail("texkill needs an operand");
            Line("if (any(lessThan(vec4(" + src(0) + ").xyz, vec3(0.0)))) discard;");
            return true;

        case OpIf:
            if (count < 1) return Fail("if needs an operand");
            Line("if (vec4(" + src(0) + ").x != 0.0) {");
            Push();
            return true;
        case OpIfc:
        {
            if (count < 2) return Fail("ifc needs two operands");
            const char *op = ComparisonOperator(instruction.controls);
            if (!op) return Fail("unknown comparison in ifc");
            Line(std::string("if (vec4(") + src(srcIndex(0, 0)) + ").x " + op + " vec4(" + src(srcIndex(0, 1)) +
                 ").x) {");
            Push();
            return true;
        }
        case OpElse:
            Pop();
            Line("} else {");
            Push();
            return true;
        case OpEndIf:
            Pop();
            Line("}");
            return true;
        case OpRep:
        {
            if (count < 1) return Fail("rep needs an operand");
            const std::string counter = "rep" + std::to_string(m_loopDepth++);
            Line("for (int " + counter + " = 0; " + counter + " < int(vec4(" + src(0) + ").x); ++" + counter + ") {");
            Push();
            return true;
        }
        case OpEndRep:
            if (m_loopDepth == 0) return Fail("endrep without rep");
            --m_loopDepth;
            Pop();
            Line("}");
            return true;
        case OpLoop:
        {
            if (count < 2) return Fail("loop needs two operands");
            // loop aL, iN: iN.x iterations, aL starts at iN.y and steps by iN.z.
            const std::string counter = "loop" + std::to_string(m_loopDepth++);
            const std::string control = src(srcIndex(1, 0));
            Line("{");
            Push();
            Line("ivec4 " + counter + "_c = ivec4(vec4(" + control + "));");
            Line("aL.x = " + counter + "_c.y;");
            Line("for (int " + counter + " = 0; " + counter + " < " + counter + "_c.x; ++" + counter +
                 ", aL.x += " + counter + "_c.z) {");
            Push();
            return true;
        }
        case OpEndLoop:
            if (m_loopDepth == 0) return Fail("endloop without loop");
            --m_loopDepth;
            Pop();
            Line("}");
            Pop();
            Line("}");
            return true;
        case OpBreak:
            Line("break;");
            return true;
        case OpBreakC:
        {
            if (count < 2) return Fail("breakc needs two operands");
            const char *op = ComparisonOperator(instruction.controls);
            if (!op) return Fail("unknown comparison in breakc");
            Line(std::string("if (vec4(") + src(srcIndex(0, 0)) + ").x " + op + " vec4(" + src(srcIndex(0, 1)) +
                 ").x) break;");
            return true;
        }
        case OpRet:
            // Only meaningful with subroutines, which the game's shaders do
            // not use; at the top level it is the end of main.
            return true;
        case OpCall:
        case OpCallNz:
            return Fail("subroutines are not supported");
        default:
            break;
        }

        char message[96];
        std::snprintf(message, sizeof(message), "unhandled opcode 0x%X", instruction.opcode);
        return Fail(message);
    }

    static const char *ComparisonOperator(uint32_t controls)
    {
        switch (controls)
        {
        case 1: return ">";
        case 2: return "==";
        case 3: return ">=";
        case 4: return "<";
        case 5: return "!=";
        case 6: return "<=";
        default: return nullptr;
        }
    }

    bool EmitMatrix(const Instruction &instruction, int sourceComponents, int rows)
    {
        if (instruction.length < 3)
            return Fail("matrix instruction needs three operands");
        const uint32_t *params = instruction.params;
        const std::string vector = SourceRegister(params[1], nullptr);
        const uint32_t matrixToken = params[2];
        const uint32_t matrixBase = RegisterNumber(matrixToken);

        // m4x4 dst, src, c[n] reads c[n]..c[n+3] as rows. Expanded rather than
        // built as a mat4, because the rows may be `def` constants that were
        // folded away and are no longer contiguous in the uniform block.
        const char *components = "xyzw";
        std::string result = "vec4(";
        for (int row = 0; row < rows; ++row)
        {
            if (row)
                result += ", ";
            const uint32_t reg = matrixBase + static_cast<uint32_t>(row);
            const std::string rowName =
                m_floatDefs.count(reg) ? "cdef" + std::to_string(reg) : "c[" + std::to_string(reg) + "]";
            if (sourceComponents == 4)
                result += "dot(vec4(" + vector + "), " + rowName + ")";
            else
                result += "dot(vec4(" + vector + ").xyz, " + rowName + ".xyz)";
        }
        for (int row = rows; row < 4; ++row)
            result += ", 0.0";
        result += ")";
        (void)components;
        EmitAssignment(params[0], result);
        return true;
    }

    bool EmitTexture(const Instruction &instruction, bool explicitLod, bool explicitGradient)
    {
        const uint32_t *params = instruction.params;
        const uint32_t count = instruction.length;
        if (count < 3)
            return Fail("texture instruction needs a destination, coordinate and sampler");

        const uint32_t samplerToken = params[explicitGradient ? 2 : 2];
        const uint32_t samplerReg = RegisterNumber(samplerToken);
        const auto typeIt = m_samplerTypes.find(samplerReg);
        const uint8_t samplerType =
            typeIt == m_samplerTypes.end() ? static_cast<uint8_t>(kTexture2D) : typeIt->second;
        const bool shadow = (m_options.depthSamplerMask & (1u << samplerReg)) != 0 && samplerType == kTexture2D;

        const std::string coordinate = SourceRegister(params[1], nullptr);
        const std::string sampler = "s" + std::to_string(samplerReg);

        // texld with the _dz/_dw source modifier is a projective fetch.
        const uint32_t coordinateModifier = SourceModifier(params[1]);
        const bool projective = coordinateModifier == ModDz || coordinateModifier == ModDw;

        std::string expression;
        if (shadow)
        {
            // Direct3D 9 compares against coord.z automatically when the
            // texture is a depth format; sampler2DShadow does the same, but
            // the comparison value is part of the coordinate.
            expression = "vec4(texture(" + sampler + ", vec4(" + coordinate + ").xyz))";
        }
        else if (explicitGradient)
        {
            if (count < 5)
                return Fail("texldd needs gradients");
            const std::string dx = SourceRegister(params[3], nullptr);
            const std::string dy = SourceRegister(params[4], nullptr);
            const char *swizzle = samplerType == kTextureCube || samplerType == kTextureVolume ? ".xyz" : ".xy";
            expression = "textureGrad(" + sampler + ", vec4(" + coordinate + ")" + swizzle + ", vec4(" + dx + ")" +
                         swizzle + ", vec4(" + dy + ")" + swizzle + ")";
        }
        else if (explicitLod)
        {
            const char *swizzle = samplerType == kTextureCube || samplerType == kTextureVolume ? ".xyz" : ".xy";
            expression = "textureLod(" + sampler + ", vec4(" + coordinate + ")" + swizzle + ", vec4(" + coordinate +
                         ").w)";
        }
        else if (projective && samplerType == kTexture2D)
        {
            expression = "textureProj(" + sampler + ", vec4(" + coordinate + "))";
        }
        else
        {
            switch (samplerType)
            {
            case kTextureCube: expression = "texture(" + sampler + ", vec4(" + coordinate + ").xyz)"; break;
            case kTextureVolume: expression = "texture(" + sampler + ", vec4(" + coordinate + ").xyz)"; break;
            default: expression = "texture(" + sampler + ", vec4(" + coordinate + ").xy)"; break;
            }
        }

        EmitAssignment(params[0], expression);
        return true;
    }

    const uint32_t *m_tokens;
    size_t m_count;
    const TranslateOptions &m_options;
    TranslatedShader &m_out;

    size_t m_at = 1;
    bool m_failed = false;

    std::string m_header;
    std::string m_body;
    std::string m_indent;

    uint32_t m_maxTemp = 0;
    uint32_t m_maxColorOut = 0;
    bool m_writesDepth = false;
    bool m_relativeConstants = false;
    bool m_usesLoopRegister = false;
    int m_loopDepth = 0;

    std::map<uint32_t, std::array<float, 4>> m_floatDefs;
    std::map<uint32_t, std::array<int32_t, 4>> m_intDefs;
    std::map<uint32_t, bool> m_boolDefs;
    std::map<uint32_t, ShaderSemantic> m_declaredInputs;
    std::map<uint32_t, ShaderSemantic> m_declaredOutputs;
    std::map<uint32_t, uint8_t> m_samplerTypes;
};

} // namespace

int AttributeSlot(uint8_t usage, uint8_t index)
{
    // Fixed table rather than first-come allocation: the vertex layout is
    // created independently of any shader, so both sides have to agree
    // without consulting each other. Matches the Metal translator's slots.
    switch (usage)
    {
    case kUsagePosition: return index == 0 ? 0 : -1;
    case kUsageBlendWeight: return index == 0 ? 1 : -1;
    case kUsageBlendIndices: return index == 0 ? 2 : -1;
    case kUsageNormal: return index == 0 ? 3 : -1;
    case kUsageTexCoord: return index < 8 ? 4 + index : -1;
    case kUsageTangent: return index == 0 ? 12 : -1;
    case kUsageBinormal: return index == 0 ? 13 : -1;
    case kUsageColor: return index < 2 ? 14 + index : -1;
    case kUsagePositionT: return index == 0 ? 0 : -1;
    default: return -1;
    }
}

bool TranslateShader(const uint32_t *tokens, size_t tokenCount, const TranslateOptions &options, TranslatedShader &out)
{
    out = TranslatedShader{};
    if (!tokens || tokenCount == 0)
    {
        out.error = "no bytecode";
        return false;
    }
    Translator translator(tokens, tokenCount, options, out);
    if (!translator.Run())
    {
        if (out.error.empty())
            out.error = "translation failed";
        return false;
    }
    return true;
}

} // namespace kisak::vk
