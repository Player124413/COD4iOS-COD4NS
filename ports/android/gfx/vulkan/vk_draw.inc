// Draw submission for the Vulkan backend.
//
// Included at the end of vk_backend.cpp rather than compiled separately,
// because it works on the single `Backend g` instance in that file's
// anonymous namespace. Splitting it would mean exporting the whole backend
// state through a header for no benefit.
//
// The shape of the problem: the Direct3D 9 layer above calls Draw() with a
// complete DrawState every time - it has no notion of "what changed". Doing
// the same to Vulkan would mean rebuilding descriptor sets and re-binding
// everything per draw, which on a tiler is the difference between 60 and 25
// fps. So this file keeps a shadow of what is currently bound and only emits
// the commands the new state actually requires.

namespace {

// Shadow of the last state submitted to the command buffer this frame.
struct BoundState
{
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkIndexType indexType = VK_INDEX_TYPE_UINT16;
    std::array<VkBuffer, kMaxStreams> vertexBuffers{};
    std::array<VkDeviceSize, kMaxStreams> vertexOffsets{};
    Viewport viewport{};
    Rect scissor{};
    bool viewportValid = false;
    bool scissorValid = false;
    uint8_t stencilRef = 0xFF;
    bool stencilRefValid = false;
    float vertexPush[4] = { 1e30f, 0, 0, 0 };
    float fragmentPush[4] = { 1e30f, 0, 0, 0 };
    // Descriptor contents, so an identical binding set does not allocate and
    // write a new descriptor set.
    uint64_t descriptorHash = 0;

    void Reset()
    {
        *this = BoundState{};
    }
};

BoundState g_bound;

// A draw that cannot get a pipeline, a render pass, ring space or a
// descriptor set is dropped on the floor, which looks exactly like a black
// screen from the outside. Count the reasons and report them, otherwise the
// only way to tell "the engine submitted nothing" from "every draw was
// rejected" is a debugger on the device.
struct DrawStats
{
    uint32_t requested;
    uint32_t submitted;
    uint32_t noPipeline;
    uint32_t noPass;
    uint32_t noRingSpace;
    uint32_t noDescriptor;
    bool haveViewport;
    Viewport viewport;
};
DrawStats g_drawStats;
uint32_t g_drawStatsFrame;
uint32_t g_emptyFrames;

// Records what the engine asked for, so a frame that produced nothing can be
// told apart from a frame whose geometry was transformed into nothing.
void NoteDraw(const DrawState &state)
{
    ++g_drawStats.requested;
    if (!g_drawStats.haveViewport)
    {
        g_drawStats.haveViewport = true;
        g_drawStats.viewport = state.viewport;
    }
}
bool g_frameOpen = false;

// ---------------------------------------------------------------------------
// Render passes and framebuffers
//
// A VkRenderPass is identified by its attachment formats and load/store ops;
// a VkFramebuffer by the actual views. Both are cached, because the engine
// cycles through a small fixed set of target configurations.

uint64_t TargetSetKey(const DrawState &state)
{
    uint64_t key = 1469598103934665603ull;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        const Texture *texture = state.color[i].texture;
        const uint64_t id = reinterpret_cast<uint64_t>(texture);
        key = HashBytes(&id, sizeof(id), key);
        key = HashBytes(&state.color[i].level, sizeof(uint32_t), key);
        key = HashBytes(&state.color[i].face, sizeof(uint32_t), key);
    }
    const uint64_t depthId = reinterpret_cast<uint64_t>(state.depth.texture);
    key = HashBytes(&depthId, sizeof(depthId), key);
    return key;
}

uint64_t RenderPassKey(const DrawState &state)
{
    uint64_t key = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        const VkFormat format = state.color[i].texture ? state.color[i].texture->format : VK_FORMAT_UNDEFINED;
        key = HashBytes(&format, sizeof(format), key);
    }
    const VkFormat depthFormat = state.depth.texture ? state.depth.texture->format : VK_FORMAT_UNDEFINED;
    key = HashBytes(&depthFormat, sizeof(depthFormat), key);
    return key;
}

VkRenderPass GetRenderPass(const DrawState &state)
{
    const uint64_t key = RenderPassKey(state);
    const auto it = g.renderPasses.find(key);
    if (it != g.renderPasses.end())
        return it->second;

    std::vector<VkAttachmentDescription> attachments;
    std::vector<VkAttachmentReference> colorRefs;
    VkAttachmentReference depthRef{ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED };

    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        if (!state.color[i].texture)
            break;
        VkAttachmentDescription attachment{};
        attachment.format = state.color[i].texture->format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        // LOAD, not CLEAR: the engine issues its own Clear() calls and relies
        // on content surviving between passes (the HUD draws over the scene).
        // DONT_CARE here would discard the scene whenever the target set is
        // re-bound mid-frame.
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRefs.push_back({ static_cast<uint32_t>(attachments.size()),
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
        attachments.push_back(attachment);
    }

    if (state.depth.texture)
    {
        VkAttachmentDescription attachment{};
        attachment.format = state.depth.texture->format;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        // The depth buffer is never read after the frame. Discarding it tells
        // a tiler not to write the tile depth back to main memory, which is
        // a measurable bandwidth saving on every Mali and Adreno part.
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef = { static_cast<uint32_t>(attachments.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        attachments.push_back(attachment);
    }

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
    subpass.pColorAttachments = colorRefs.empty() ? nullptr : colorRefs.data();
    subpass.pDepthStencilAttachment = depthRef.attachment == VK_ATTACHMENT_UNUSED ? nullptr : &depthRef;

    // Two external dependencies so a pass that samples the previous pass's
    // output sees finished writes, and so a later sample sees this one's.
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;

    VkRenderPass pass = VK_NULL_HANDLE;
    if (!Check(vkCreateRenderPass(g.device, &info, nullptr, &pass), "vkCreateRenderPass"))
        return VK_NULL_HANDLE;
    g.renderPasses.emplace(key, pass);
    return pass;
}

// One mip level of one cube face needs its own view to be an attachment.
VkImageView GetTargetView(Texture *texture, uint32_t face, uint32_t level)
{
    if (!texture)
        return VK_NULL_HANDLE;
    if (face == 0 && level == 0 && texture->desc.kind == TextureKind::Texture2D)
        return texture->view;

    const uint32_t key = (face << 16) | level;
    const auto it = texture->targetViews.find(key);
    if (it != texture->targetViews.end())
        return it->second;

    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = texture->image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = texture->format;
    info.subresourceRange = { texture->aspect, level, 1, face, 1 };

    VkImageView view = VK_NULL_HANDLE;
    if (!Check(vkCreateImageView(g.device, &info, nullptr, &view), "vkCreateImageView(target)"))
        return VK_NULL_HANDLE;
    texture->targetViews.emplace(key, view);
    return view;
}

VkFramebuffer GetFramebuffer(const DrawState &state, VkRenderPass pass, VkExtent2D &extentOut)
{
    const uint64_t key = TargetSetKey(state);
    const auto it = g.framebuffers.find(key);

    std::vector<VkImageView> views;
    uint32_t width = 0;
    uint32_t height = 0;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        if (!state.color[i].texture)
            break;
        views.push_back(GetTargetView(state.color[i].texture, state.color[i].face, state.color[i].level));
        const uint32_t levelWidth = std::max(1u, state.color[i].texture->desc.width >> state.color[i].level);
        const uint32_t levelHeight = std::max(1u, state.color[i].texture->desc.height >> state.color[i].level);
        width = width ? std::min(width, levelWidth) : levelWidth;
        height = height ? std::min(height, levelHeight) : levelHeight;
    }
    if (state.depth.texture)
    {
        views.push_back(GetTargetView(state.depth.texture, state.depth.face, state.depth.level));
        const uint32_t levelWidth = std::max(1u, state.depth.texture->desc.width >> state.depth.level);
        const uint32_t levelHeight = std::max(1u, state.depth.texture->desc.height >> state.depth.level);
        width = width ? std::min(width, levelWidth) : levelWidth;
        height = height ? std::min(height, levelHeight) : levelHeight;
    }
    if (views.empty() || width == 0 || height == 0)
        return VK_NULL_HANDLE;

    extentOut = { width, height };
    if (it != g.framebuffers.end())
        return it->second;

    VkFramebufferCreateInfo info{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    info.renderPass = pass;
    info.attachmentCount = static_cast<uint32_t>(views.size());
    info.pAttachments = views.data();
    info.width = width;
    info.height = height;
    info.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    if (!Check(vkCreateFramebuffer(g.device, &info, nullptr, &framebuffer), "vkCreateFramebuffer"))
        return VK_NULL_HANDLE;
    g.framebuffers.emplace(key, framebuffer);
    return framebuffer;
}

void EndPass()
{
    if (g.activePass == VK_NULL_HANDLE)
        return;
    vkCmdEndRenderPass(Frame().commandBuffer);
    g.activePass = VK_NULL_HANDLE;
    g.activeFramebuffer = VK_NULL_HANDLE;
    g.activeTargetKey = 0;
    // Everything recorded inside a pass is scoped to it.
    g_bound.Reset();
}

bool BeginPass(const DrawState &state)
{
    const uint64_t key = TargetSetKey(state);
    if (g.activePass != VK_NULL_HANDLE && key == g.activeTargetKey)
        return true;
    EndPass();

    VkRenderPass pass = GetRenderPass(state);
    if (pass == VK_NULL_HANDLE)
        return false;
    VkExtent2D extent{};
    VkFramebuffer framebuffer = GetFramebuffer(state, pass, extent);
    if (framebuffer == VK_NULL_HANDLE)
        return false;

    // Attachments must be in the layout the pass declares. Textures that were
    // sampled last frame are in SHADER_READ_ONLY_OPTIMAL.
    auto transition = [&](Texture *texture, VkImageLayout target, VkPipelineStageFlags stage, VkAccessFlags access) {
        if (!texture || texture->layout == target)
            return;
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = texture->layout;
        barrier.newLayout = target;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture->image;
        barrier.subresourceRange = { texture->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = access;
        vkCmdPipelineBarrier(Frame().commandBuffer,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, stage, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        texture->layout = target;
    };

    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        if (!state.color[i].texture)
            break;
        transition(state.color[i].texture, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    transition(state.depth.texture, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderPassBeginInfo info{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    info.renderPass = pass;
    info.framebuffer = framebuffer;
    info.renderArea = { { 0, 0 }, extent };
    vkCmdBeginRenderPass(Frame().commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);

    g.activePass = pass;
    g.activeFramebuffer = framebuffer;
    g.activeTargetKey = key;
    g.activeExtent = extent;
    g_bound.Reset();
    return true;
}

// ---------------------------------------------------------------------------
// Pipelines

// Packs the parts of RenderState that affect pipeline creation into six
// words, so the cache key is a memcmp and not a field-by-field compare.
void PackRenderState(const RenderState &render, uint8_t colorTargets, uint32_t out[6])
{
    std::memset(out, 0, sizeof(uint32_t) * 6);
    out[0] = (render.depthEnable ? 1u : 0u) | (render.depthWrite ? 2u : 0u) |
             (static_cast<uint32_t>(render.depthFunc) << 2) | (render.blendEnable ? (1u << 6) : 0u) |
             (render.separateAlphaBlend ? (1u << 7) : 0u) | (static_cast<uint32_t>(render.colorWriteMask) << 8) |
             (static_cast<uint32_t>(render.cull) << 12) | (render.wireframe ? (1u << 14) : 0u) |
             (render.alphaTest ? (1u << 15) : 0u) | (static_cast<uint32_t>(render.alphaFunc) << 16) |
             (render.stencilEnable ? (1u << 20) : 0u) | (render.twoSidedStencil ? (1u << 21) : 0u) |
             (static_cast<uint32_t>(colorTargets) << 22);
    out[1] = static_cast<uint32_t>(render.srcColor) | (static_cast<uint32_t>(render.destColor) << 8) |
             (static_cast<uint32_t>(render.colorOp) << 16) | (static_cast<uint32_t>(render.srcAlpha) << 20);
    out[2] = static_cast<uint32_t>(render.destAlpha) | (static_cast<uint32_t>(render.alphaOp) << 8) |
             (static_cast<uint32_t>(render.stencilReadMask) << 12) |
             (static_cast<uint32_t>(render.stencilWriteMask) << 20);
    out[3] = static_cast<uint32_t>(render.stencilFront.fail) |
             (static_cast<uint32_t>(render.stencilFront.depthFail) << 4) |
             (static_cast<uint32_t>(render.stencilFront.pass) << 8) |
             (static_cast<uint32_t>(render.stencilFront.func) << 12) |
             (static_cast<uint32_t>(render.stencilBack.fail) << 16) |
             (static_cast<uint32_t>(render.stencilBack.depthFail) << 20) |
             (static_cast<uint32_t>(render.stencilBack.pass) << 24) |
             (static_cast<uint32_t>(render.stencilBack.func) << 28);
    // Depth bias is a float but takes only a handful of distinct values; the
    // bits go in whole rather than being quantised, because a wrong bias is a
    // z-fighting bug that is painful to track down.
    std::memcpy(&out[4], &render.depthBias, sizeof(float));
    std::memcpy(&out[5], &render.slopeScaledDepthBias, sizeof(float));
}

VkPipeline GetPipeline(const DrawState &state, Primitive primitive, VkRenderPass pass)
{
    PipelineKey key{};
    key.vertexShader = state.vertexShader ? state.vertexShader->sourceHash : 0;
    key.pixelShader = state.pixelShader ? state.pixelShader->sourceHash : 0;
    key.layout = state.layout ? state.layout->hash : 0;
    key.renderPass = reinterpret_cast<uint64_t>(pass);
    key.primitive = static_cast<uint8_t>(primitive);
    key.sampleCount = 1;
    uint8_t colorTargets = 0;
    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        if (state.color[i].texture)
            colorTargets = static_cast<uint8_t>(i + 1);
    }
    key.colorTargets = colorTargets;
    PackRenderState(state.render, colorTargets, key.renderState);
    // The stride is part of the vertex input state, so it belongs in the key.
    for (uint32_t i = 0; i < kMaxStreams; ++i)
        key.layout = HashBytes(&state.streams[i].stride, sizeof(uint32_t), key.layout);

    const auto it = g.pipelines.find(key);
    if (it != g.pipelines.end())
        return it->second;

    if (!state.vertexShader || !state.pixelShader || !state.layout)
        return VK_NULL_HANDLE;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = state.vertexShader->module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = state.pixelShader->module;
    stages[1].pName = "main";

    // Only bind the streams this draw actually uses, with their real strides.
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::array<bool, kMaxStreams> live{};
    for (const VkVertexInputAttributeDescription &attribute : state.layout->attributes)
    {
        if (attribute.binding < kMaxStreams && state.streams[attribute.binding].buffer != nullptr)
        {
            attributes.push_back(attribute);
            live[attribute.binding] = true;
        }
    }
    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        if (!live[i])
            continue;
        VkVertexInputBindingDescription binding{};
        binding.binding = i;
        binding.stride = state.streams[i].stride;
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings.push_back(binding);
    }

    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = ToTopology(primitive);

    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = state.render.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    switch (state.render.cull)
    {
    case Cull::None: raster.cullMode = VK_CULL_MODE_NONE; break;
    case Cull::Clockwise: raster.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    case Cull::CounterClockwise: raster.cullMode = VK_CULL_MODE_BACK_BIT; break;
    }
    // Direct3D's default winding is clockwise-front. Vulkan's Y-down clip
    // space matches Direct3D, so the winding carries over unchanged - this is
    // the half of the convention that does NOT need flipping, and flipping it
    // "to be safe" inverts every backface in the game.
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = state.render.depthBias != 0.0f || state.render.slopeScaledDepthBias != 0.0f;
    // Direct3D expresses the constant bias as a fraction of the depth range;
    // Vulkan wants it in units of the smallest resolvable difference.
    raster.depthBiasConstantFactor = state.render.depthBias * 16777216.0f;
    raster.depthBiasSlopeFactor = state.render.slopeScaledDepthBias;

    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO
    };
    depthStencil.depthTestEnable = state.render.depthEnable ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = state.render.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = ToCompareOp(state.render.depthFunc);
    depthStencil.stencilTestEnable = state.render.stencilEnable ? VK_TRUE : VK_FALSE;
    depthStencil.front.failOp = ToStencilOp(state.render.stencilFront.fail);
    depthStencil.front.passOp = ToStencilOp(state.render.stencilFront.pass);
    depthStencil.front.depthFailOp = ToStencilOp(state.render.stencilFront.depthFail);
    depthStencil.front.compareOp = ToCompareOp(state.render.stencilFront.func);
    depthStencil.front.compareMask = state.render.stencilReadMask;
    depthStencil.front.writeMask = state.render.stencilWriteMask;
    depthStencil.back = depthStencil.front;
    if (state.render.twoSidedStencil)
    {
        depthStencil.back.failOp = ToStencilOp(state.render.stencilBack.fail);
        depthStencil.back.passOp = ToStencilOp(state.render.stencilBack.pass);
        depthStencil.back.depthFailOp = ToStencilOp(state.render.stencilBack.depthFail);
        depthStencil.back.compareOp = ToCompareOp(state.render.stencilBack.func);
    }

    std::array<VkPipelineColorBlendAttachmentState, kMaxColorTargets> blendAttachments{};
    for (uint32_t i = 0; i < kMaxColorTargets; ++i)
    {
        VkPipelineColorBlendAttachmentState &attachment = blendAttachments[i];
        attachment.blendEnable = state.render.blendEnable ? VK_TRUE : VK_FALSE;
        attachment.srcColorBlendFactor = ToBlendFactor(state.render.srcColor);
        attachment.dstColorBlendFactor = ToBlendFactor(state.render.destColor);
        attachment.colorBlendOp = ToBlendOp(state.render.colorOp);
        if (state.render.separateAlphaBlend)
        {
            attachment.srcAlphaBlendFactor = ToBlendFactor(state.render.srcAlpha);
            attachment.dstAlphaBlendFactor = ToBlendFactor(state.render.destAlpha);
            attachment.alphaBlendOp = ToBlendOp(state.render.alphaOp);
        }
        else
        {
            // Without D3DRS_SEPARATEALPHABLENDENABLE the alpha channel uses
            // the colour factors, not One/Zero.
            attachment.srcAlphaBlendFactor = attachment.srcColorBlendFactor;
            attachment.dstAlphaBlendFactor = attachment.dstColorBlendFactor;
            attachment.alphaBlendOp = attachment.colorBlendOp;
        }
        attachment.colorWriteMask = 0;
        if (state.render.colorWriteMask & 1) attachment.colorWriteMask |= VK_COLOR_COMPONENT_R_BIT;
        if (state.render.colorWriteMask & 2) attachment.colorWriteMask |= VK_COLOR_COMPONENT_G_BIT;
        if (state.render.colorWriteMask & 4) attachment.colorWriteMask |= VK_COLOR_COMPONENT_B_BIT;
        if (state.render.colorWriteMask & 8) attachment.colorWriteMask |= VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = colorTargets;
    blend.pAttachments = blendAttachments.data();

    // Viewport, scissor, stencil reference and blend constants change far
    // more often than anything else; making them dynamic keeps the pipeline
    // count down by roughly an order of magnitude.
    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                             VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 4;
    dynamic.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo info{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewportState;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = g.pipelineLayout;
    info.renderPass = pass;
    info.subpass = 0;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (!Check(vkCreateGraphicsPipelines(g.device, g.pipelineCache, 1, &info, nullptr, &pipeline),
               "vkCreateGraphicsPipelines"))
        return VK_NULL_HANDLE;
    g.pipelines.emplace(key, pipeline);
    return pipeline;
}

// ---------------------------------------------------------------------------
// Samplers

VkSampler GetSampler(const SamplerState &sampler, bool shadow)
{
    uint64_t key = HashBytes(&sampler, sizeof(SamplerState));
    key = HashBytes(&shadow, sizeof(bool), key);
    const auto it = g.samplers.find(key);
    if (it != g.samplers.end())
        return it->second;

    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = sampler.magFilter == Filter::Point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = sampler.minFilter == Filter::Point ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = sampler.mipFilter == Filter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                          : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = ToAddressMode(sampler.addressU);
    info.addressModeV = ToAddressMode(sampler.addressV);
    info.addressModeW = ToAddressMode(sampler.addressW);
    // D3DTEXF_NONE for the mip filter means "use level 0 only", which Vulkan
    // expresses as a zero lod range rather than a filter mode.
    info.maxLod = sampler.mipFilter == Filter::None ? 0.25f : VK_LOD_CLAMP_NONE;
    info.anisotropyEnable = sampler.maxAnisotropy > 1 ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = std::min<float>(sampler.maxAnisotropy, g.deviceProperties.limits.maxSamplerAnisotropy);
    info.borderColor = (sampler.borderColor >> 24) >= 0x80 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                           : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (shadow)
    {
        info.compareEnable = VK_TRUE;
        // The engine's shadow maps store depth and compare with less-equal.
        info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    }

    VkSampler created = VK_NULL_HANDLE;
    if (!Check(vkCreateSampler(g.device, &info, nullptr, &created), "vkCreateSampler"))
        return VK_NULL_HANDLE;
    g.samplers.emplace(key, created);
    return created;
}

// ---------------------------------------------------------------------------
// Upload ring

// Suballocates from this frame's upload buffer. Returns the byte offset, or
// SIZE_MAX if the ring is exhausted - which means the frame is drawing more
// dynamic data than kUploadRingBytes and the draw is dropped rather than
// corrupting another draw's data.
VkDeviceSize RingAllocate(VkDeviceSize size, VkDeviceSize alignment, void **mapped)
{
    FrameContext &frame = Frame();
    const VkDeviceSize aligned = (frame.uploadOffset + alignment - 1) & ~(alignment - 1);
    if (aligned + size > kUploadRingBytes)
    {
        LOGE("upload ring exhausted (%llu bytes requested)", static_cast<unsigned long long>(size));
        return VK_WHOLE_SIZE;
    }
    frame.uploadOffset = aligned + size;
    *mapped = static_cast<uint8_t *>(frame.uploadMemory.mapped) + aligned;
    return aligned;
}

// ---------------------------------------------------------------------------
// Descriptors

VkDescriptorSet BuildDescriptorSet(const DrawState &state, uint32_t vertexConstantOffset,
                                   uint32_t fragmentConstantOffset)
{
    (void)vertexConstantOffset;
    (void)fragmentConstantOffset;

    // The hash covers only what lands in the set: the buffer is the same
    // every time (the offsets are dynamic), so only the images and samplers
    // matter. Most consecutive draws differ in neither and reuse the set.
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < kMaxSamplers; ++i)
    {
        const uint64_t id = reinterpret_cast<uint64_t>(state.textures[i]);
        hash = HashBytes(&id, sizeof(id), hash);
        if (state.textures[i])
            hash = HashBytes(&state.samplers[i], sizeof(SamplerState), hash);
    }
    if (hash == g_bound.descriptorHash && g_bound.descriptorSet != VK_NULL_HANDLE)
        return g_bound.descriptorSet;

    FrameContext &frame = Frame();
    VkDescriptorSetAllocateInfo allocate{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocate.descriptorPool = frame.descriptorPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g.descriptorLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g.device, &allocate, &set) != VK_SUCCESS)
    {
        // The pool is sized for a heavy frame; running out means this frame is
        // heavier still. Reusing the previous set keeps the frame rendering
        // with slightly wrong textures rather than dropping it entirely.
        LOGE("descriptor pool exhausted this frame");
        return g_bound.descriptorSet;
    }

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    bufferInfos.reserve(2);
    imageInfos.reserve(kMaxSamplers);

    // Constants: one dynamic uniform buffer per stage, both pointing at the
    // upload ring with a per-draw offset.
    for (uint32_t stage = 0; stage < 2; ++stage)
    {
        VkDescriptorBufferInfo buffer{};
        buffer.buffer = frame.uploadBuffer;
        buffer.offset = 0;
        buffer.range = stage == 0 ? kVertexConstantBytes : kFragmentConstantBytes;
        bufferInfos.push_back(buffer);

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = set;
        write.dstBinding = stage == 0 ? kVertexConstantBinding : kFragmentConstantBinding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        write.pBufferInfo = &bufferInfos.back();
        writes.push_back(write);
    }

    for (uint32_t i = 0; i < kMaxSamplers; ++i)
    {
        Texture *texture = state.textures[i];
        // Vulkan requires every declared binding to be written, even for a
        // slot the shader never reads: an unwritten binding is undefined
        // behaviour and crashes the validation layers and some drivers. A
        // null slot gets whatever texture is at slot 0, or is skipped if the
        // frame has nothing bound at all.
        if (!texture)
            continue;

        const bool shadow = texture->isDepth;
        VkDescriptorImageInfo image{};
        image.sampler = GetSampler(state.samplers[i], shadow);
        image.imageView = texture->view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.push_back(image);

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = set;
        write.dstBinding = kSamplerBindingBase + i;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfos.back();
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(g.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    g_bound.descriptorHash = hash;
    return set;
}

// Moves sampled textures out of the attachment layout before a draw reads
// them. Called before BeginPass, since a layout transition inside a render
// pass is illegal.
void TransitionSampledTextures(const DrawState &state)
{
    for (uint32_t i = 0; i < kMaxSamplers; ++i)
    {
        Texture *texture = state.textures[i];
        if (!texture || texture->layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            continue;
        // A texture that is simultaneously a render target and a source would
        // be a feedback loop; the engine does not do it, and skipping keeps
        // the pass open.
        bool isTarget = texture == state.depth.texture;
        for (uint32_t t = 0; t < kMaxColorTargets && !isTarget; ++t)
            isTarget = texture == state.color[t].texture;
        if (isTarget)
            continue;

        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = texture->layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture->image;
        barrier.subresourceRange = { texture->isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : texture->aspect, 0,
                                     VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(Frame().commandBuffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                 VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
}

uint32_t VertexCount(Primitive primitive, uint32_t primitiveCount)
{
    switch (primitive)
    {
    case Primitive::PointList: return primitiveCount;
    case Primitive::LineList: return primitiveCount * 2;
    case Primitive::LineStrip: return primitiveCount + 1;
    case Primitive::TriangleList: return primitiveCount * 3;
    case Primitive::TriangleStrip: return primitiveCount + 2;
    case Primitive::TriangleFan: return primitiveCount + 2;
    }
    return 0;
}

// Begins the command buffer for this frame if a draw arrives before any
// explicit frame start. The Direct3D layer has no BeginFrame, so the first
// operation of the frame opens it.
void EnsureFrameOpen()
{
    if (g_frameOpen)
        return;
    FrameContext &frame = Frame();

    // Wait for this slot's previous submission, then recycle everything that
    // was waiting on it.
    vkWaitForFences(g.device, 1, &frame.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g.device, 1, &frame.fence);

    for (auto &work : frame.deferredDeletes)
        work();
    frame.deferredDeletes.clear();

    if (frame.timestampPool && frame.timestampsWritten)
    {
        uint64_t timestamps[2] = {};
        if (vkGetQueryPoolResults(g.device, frame.timestampPool, 0, 2, sizeof(timestamps), timestamps,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            const double delta = static_cast<double>(timestamps[1] - timestamps[0]);
            g.lastGpuFrameNs = static_cast<int64_t>(delta * g.timestampPeriodNs);
        }
        frame.timestampsWritten = false;
    }

    vkResetCommandPool(g.device, frame.commandPool, 0);
    frame.uploadOffset = 0;

    vkResetDescriptorPool(g.device, frame.descriptorPool, 0);

    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.commandBuffer, &begin);

    if (frame.timestampPool)
    {
        vkCmdResetQueryPool(frame.commandBuffer, frame.timestampPool, 0, 2);
        vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, frame.timestampPool, 0);
    }

    g_bound.Reset();
    g_frameOpen = true;
}

// Binds everything the draw needs, skipping whatever already matches.
bool BindDrawState(const DrawState &state, Primitive primitive)
{
    VkCommandBuffer cmd = Frame().commandBuffer;

    VkPipeline pipeline = GetPipeline(state, primitive, g.activePass);
    if (pipeline == VK_NULL_HANDLE)
    {
        ++g_drawStats.noPipeline;
        return false;
    }
    if (pipeline != g_bound.pipeline)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_bound.pipeline = pipeline;
    }

    // Constants into the ring.
    uint32_t vertexOffset = 0;
    uint32_t fragmentOffset = 0;
    const VkDeviceSize alignment = std::max<VkDeviceSize>(
        g.deviceProperties.limits.minUniformBufferOffsetAlignment, 16);
    if (state.vertexConstants)
    {
        void *mapped = nullptr;
        const VkDeviceSize offset = RingAllocate(kVertexConstantBytes, alignment, &mapped);
        if (offset == VK_WHOLE_SIZE)
        {
            ++g_drawStats.noRingSpace;
            return false;
        }
        // Only the registers the shader declares are copied: uploading all
        // 256 float4s per draw is 4 KB of memory traffic for shaders that
        // typically read a dozen.
        const std::size_t used =
            state.vertexShader ? std::min<std::size_t>(state.vertexShader->constantCount, 256) * 16 : kVertexConstantBytes;
        std::memcpy(mapped, state.vertexConstants, used);
        vertexOffset = static_cast<uint32_t>(offset);
    }
    if (state.pixelConstants)
    {
        void *mapped = nullptr;
        const VkDeviceSize offset = RingAllocate(kFragmentConstantBytes, alignment, &mapped);
        if (offset == VK_WHOLE_SIZE)
        {
            ++g_drawStats.noRingSpace;
            return false;
        }
        const std::size_t used =
            state.pixelShader ? std::min<std::size_t>(state.pixelShader->constantCount, 224) * 16 : kFragmentConstantBytes;
        std::memcpy(mapped, state.pixelConstants, used);
        fragmentOffset = static_cast<uint32_t>(offset);
    }

    VkDescriptorSet set = BuildDescriptorSet(state, vertexOffset, fragmentOffset);
    if (set == VK_NULL_HANDLE)
    {
        ++g_drawStats.noDescriptor;
        return false;
    }
    const uint32_t dynamicOffsets[2] = { vertexOffset, fragmentOffset };
    // Rebound unconditionally, because the dynamic offsets change per draw
    // even when the set does not.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipelineLayout, kDescriptorSet, 1, &set, 2,
                            dynamicOffsets);
    g_bound.descriptorSet = set;

    // Push constants: half-pixel offset for the vertex stage, alpha test for
    // the fragment stage.
    const float halfPixelX = g.activeExtent.width ? 1.0f / static_cast<float>(g.activeExtent.width) : 0.0f;
    const float halfPixelY = g.activeExtent.height ? 1.0f / static_cast<float>(g.activeExtent.height) : 0.0f;
    // Direct3D 9 sampled texels at pixel centres offset by half a pixel from
    // where Vulkan does. The engine's 2D drawing assumes the Direct3D rule,
    // so the vertex shader shifts clip space by half a pixel.
    const float vertexPush[4] = { -halfPixelX, -halfPixelY, 0.0f, 0.0f };
    if (std::memcmp(vertexPush, g_bound.vertexPush, sizeof(vertexPush)) != 0)
    {
        vkCmdPushConstants(cmd, g.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(vertexPush), vertexPush);
        std::memcpy(g_bound.vertexPush, vertexPush, sizeof(vertexPush));
    }

    const float fragmentPush[4] = {
        state.render.alphaTest ? static_cast<float>(state.render.alphaFunc) : static_cast<float>(Compare::Always),
        static_cast<float>(state.render.alphaRef) / 255.0f, 0.0f, 0.0f
    };
    if (std::memcmp(fragmentPush, g_bound.fragmentPush, sizeof(fragmentPush)) != 0)
    {
        vkCmdPushConstants(cmd, g.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, kPushConstantSize,
                           sizeof(fragmentPush), fragmentPush);
        std::memcpy(g_bound.fragmentPush, fragmentPush, sizeof(fragmentPush));
    }

    // Vertex streams.
    for (uint32_t i = 0; i < kMaxStreams; ++i)
    {
        const StreamBinding &stream = state.streams[i];
        VkBuffer buffer = stream.buffer ? stream.buffer->buffer : VK_NULL_HANDLE;
        const VkDeviceSize offset = stream.offset;
        if (buffer == VK_NULL_HANDLE)
            continue;
        if (buffer == g_bound.vertexBuffers[i] && offset == g_bound.vertexOffsets[i])
            continue;
        vkCmdBindVertexBuffers(cmd, i, 1, &buffer, &offset);
        g_bound.vertexBuffers[i] = buffer;
        g_bound.vertexOffsets[i] = offset;
    }

    if (state.indices)
    {
        const VkIndexType type = state.indices32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        if (state.indices->buffer != g_bound.indexBuffer || type != g_bound.indexType)
        {
            vkCmdBindIndexBuffer(cmd, state.indices->buffer, 0, type);
            g_bound.indexBuffer = state.indices->buffer;
            g_bound.indexType = type;
        }
    }

    // Viewport. Direct3D's viewport origin is top-left with Y growing down,
    // which is also Vulkan's, so no flip is needed here either.
    if (!g_bound.viewportValid || std::memcmp(&state.viewport, &g_bound.viewport, sizeof(Viewport)) != 0)
    {
        VkViewport viewport{};
        viewport.x = state.viewport.x;
        viewport.y = state.viewport.y;
        viewport.width = state.viewport.width;
        viewport.height = state.viewport.height;
        viewport.minDepth = state.viewport.minZ;
        viewport.maxDepth = state.viewport.maxZ;
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        g_bound.viewport = state.viewport;
        g_bound.viewportValid = true;
    }

    // Scissor. Vulkan always has one; with the test disabled it covers the
    // whole target.
    Rect scissor = state.scissor;
    if (!state.render.scissorTest)
        scissor = { 0, 0, static_cast<int32_t>(g.activeExtent.width), static_cast<int32_t>(g.activeExtent.height) };
    if (!g_bound.scissorValid || std::memcmp(&scissor, &g_bound.scissor, sizeof(Rect)) != 0)
    {
        VkRect2D rect{};
        rect.offset.x = std::max(0, scissor.left);
        rect.offset.y = std::max(0, scissor.top);
        rect.extent.width = static_cast<uint32_t>(std::max(0, scissor.right - rect.offset.x));
        rect.extent.height = static_cast<uint32_t>(std::max(0, scissor.bottom - rect.offset.y));
        rect.extent.width = std::min(rect.extent.width, g.activeExtent.width - rect.offset.x);
        rect.extent.height = std::min(rect.extent.height, g.activeExtent.height - rect.offset.y);
        vkCmdSetScissor(cmd, 0, 1, &rect);
        g_bound.scissor = scissor;
        g_bound.scissorValid = true;
    }

    if (!g_bound.stencilRefValid || state.render.stencilRef != g_bound.stencilRef)
    {
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, state.render.stencilRef);
        g_bound.stencilRef = state.render.stencilRef;
        g_bound.stencilRefValid = true;
    }
    return true;
}

} // namespace

void UploadTexture(Texture *texture, uint32_t face, uint32_t level, const uint8_t *bits, size_t size)
{
    if (!Available() || !texture || !bits || size == 0)
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    EnsureFrameOpen();
    // A copy cannot be recorded inside a render pass.
    EndPass();

    void *mapped = nullptr;
    VkDeviceSize offset = RingAllocate(size, 16, &mapped);
    VkBuffer source = Frame().uploadBuffer;
    VkBuffer temporary = VK_NULL_HANDLE;
    MemoryBlock temporaryMemory;

    if (offset == VK_WHOLE_SIZE)
    {
        // A texture larger than the ring (the sky cube, mostly) gets a
        // one-shot staging buffer rather than failing the upload.
        VkBufferCreateInfo info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateBuffer(g.device, &info, nullptr, &temporary), "vkCreateBuffer(staging)"))
            return;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(g.device, temporary, &requirements);
        if (!AllocateMemory(requirements,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            temporaryMemory))
        {
            vkDestroyBuffer(g.device, temporary, nullptr);
            return;
        }
        vkBindBufferMemory(g.device, temporary, temporaryMemory.memory, 0);
        mapped = temporaryMemory.mapped;
        source = temporary;
        offset = 0;
        DeferDelete([temporary, temporaryMemory]() mutable {
            vkDestroyBuffer(g.device, temporary, nullptr);
            FreeMemory(temporaryMemory);
        });
    }

    std::memcpy(mapped, bits, size);

    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.oldLayout = texture->layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture->image;
    barrier.subresourceRange = { texture->aspect, level, 1, face, 1 };
    barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(Frame().commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = { texture->aspect, level, face, 1 };
    copy.imageExtent = { std::max(1u, texture->desc.width >> level), std::max(1u, texture->desc.height >> level),
                         texture->desc.kind == TextureKind::Volume ? std::max(1u, texture->desc.depth >> level) : 1 };
    vkCmdCopyBufferToImage(Frame().commandBuffer, source, texture->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &copy);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(Frame().commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    texture->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void Draw(const DrawState &state, Primitive primitive, uint32_t primitiveCount, bool indexed, int32_t baseVertex,
          uint32_t start)
{
    if (!Available() || primitiveCount == 0)
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    EnsureFrameOpen();

    TransitionSampledTextures(state);
    NoteDraw(state);
    if (!BeginPass(state))
    {
        ++g_drawStats.noPass;
        return;
    }
    if (!BindDrawState(state, primitive))
        return;
    ++g_drawStats.submitted;

    const uint32_t vertices = VertexCount(primitive, primitiveCount);
    if (indexed)
        vkCmdDrawIndexed(Frame().commandBuffer, vertices, 1, start, baseVertex, 0);
    else
        vkCmdDraw(Frame().commandBuffer, vertices, 1, start, 0);
}

void DrawUserPrimitives(const DrawState &state, Primitive primitive, uint32_t primitiveCount, const void *vertices,
                        uint32_t stride)
{
    if (!Available() || primitiveCount == 0 || !vertices || stride == 0)
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    EnsureFrameOpen();
    NoteDraw(state);

    const uint32_t vertexCount = VertexCount(primitive, primitiveCount);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(vertexCount) * stride;

    void *mapped = nullptr;
    const VkDeviceSize offset = RingAllocate(bytes, 16, &mapped);
    if (offset == VK_WHOLE_SIZE)
    {
        ++g_drawStats.noRingSpace;
        return;
    }
    std::memcpy(mapped, vertices, bytes);

    // DrawPrimitiveUP has no buffer object, so a local DrawState points
    // stream 0 at the ring. The HUD and most debug drawing come through here.
    DrawState local = state;
    Buffer ring{};
    ring.buffer = Frame().uploadBuffer;
    ring.size = kUploadRingBytes;
    local.streams[0].buffer = &ring;
    local.streams[0].offset = static_cast<uint32_t>(offset);
    local.streams[0].stride = stride;
    for (uint32_t i = 1; i < kMaxStreams; ++i)
        local.streams[i] = StreamBinding{};
    local.indices = nullptr;

    TransitionSampledTextures(local);
    if (!BeginPass(local) || !BindDrawState(local, primitive))
        return;
    ++g_drawStats.submitted;
    vkCmdDraw(Frame().commandBuffer, vertexCount, 1, 0, 0);

    // The shadow holds a pointer to the stack Buffer above; forget it so the
    // next draw cannot compare against a dangling address.
    g_bound.vertexBuffers[0] = VK_NULL_HANDLE;
}

void Clear(const DrawState &state, bool color, bool depth, bool stencil, uint32_t argb, float z, uint8_t stencilValue)
{
    if (!Available() || (!color && !depth && !stencil))
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    EnsureFrameOpen();
    if (!BeginPass(state))
        return;

    // vkCmdClearAttachments inside the pass, not a separate clear pass: on a
    // tiler this is a tile-local operation with no memory traffic at all,
    // whereas a vkCmdClearColorImage would flush and refill every tile.
    std::vector<VkClearAttachment> attachments;
    if (color)
    {
        VkClearAttachment attachment{};
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        // D3DCOLOR is ARGB in a 32-bit word.
        attachment.clearValue.color.float32[0] = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
        attachment.clearValue.color.float32[1] = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
        attachment.clearValue.color.float32[2] = static_cast<float>(argb & 0xFF) / 255.0f;
        attachment.clearValue.color.float32[3] = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
        for (uint32_t i = 0; i < kMaxColorTargets; ++i)
        {
            if (!state.color[i].texture)
                break;
            attachment.colorAttachment = i;
            attachments.push_back(attachment);
        }
    }
    if ((depth || stencil) && state.depth.texture)
    {
        VkClearAttachment attachment{};
        if (depth)
            attachment.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (stencil && state.depth.texture->desc.format == Format::D24S8)
            attachment.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        attachment.clearValue.depthStencil.depth = z;
        attachment.clearValue.depthStencil.stencil = stencilValue;
        if (attachment.aspectMask)
            attachments.push_back(attachment);
    }
    if (attachments.empty())
        return;

    VkClearRect rect{};
    rect.rect = { { 0, 0 }, g.activeExtent };
    rect.baseArrayLayer = 0;
    rect.layerCount = 1;
    vkCmdClearAttachments(Frame().commandBuffer, static_cast<uint32_t>(attachments.size()), attachments.data(), 1,
                          &rect);
}

void Blit(const TargetBinding &source, const TargetBinding &destination)
{
    if (!Available() || !source.texture || !destination.texture)
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    EnsureFrameOpen();
    EndPass();

    VkCommandBuffer cmd = Frame().commandBuffer;

    auto transition = [&](Texture *texture, VkImageLayout target, VkAccessFlags access) {
        if (texture->layout == target)
            return;
        VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        barrier.oldLayout = texture->layout;
        barrier.newLayout = target;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture->image;
        barrier.subresourceRange = { texture->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = access;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        texture->layout = target;
    };

    transition(source.texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);
    transition(destination.texture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT);

    const uint32_t sourceWidth = std::max(1u, source.texture->desc.width >> source.level);
    const uint32_t sourceHeight = std::max(1u, source.texture->desc.height >> source.level);
    const uint32_t destWidth = std::max(1u, destination.texture->desc.width >> destination.level);
    const uint32_t destHeight = std::max(1u, destination.texture->desc.height >> destination.level);

    if (sourceWidth == destWidth && sourceHeight == destHeight &&
        source.texture->format == destination.texture->format)
    {
        // Same size and format: a copy is cheaper than a blit on every mobile
        // part, and avoids any filtering question.
        VkImageCopy copy{};
        copy.srcSubresource = { source.texture->aspect, source.level, source.face, 1 };
        copy.dstSubresource = { destination.texture->aspect, destination.level, destination.face, 1 };
        copy.extent = { sourceWidth, sourceHeight, 1 };
        vkCmdCopyImage(cmd, source.texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.texture->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    else
    {
        VkImageBlit blit{};
        blit.srcSubresource = { source.texture->aspect, source.level, source.face, 1 };
        blit.srcOffsets[1] = { static_cast<int32_t>(sourceWidth), static_cast<int32_t>(sourceHeight), 1 };
        blit.dstSubresource = { destination.texture->aspect, destination.level, destination.face, 1 };
        blit.dstOffsets[1] = { static_cast<int32_t>(destWidth), static_cast<int32_t>(destHeight), 1 };
        // Linear for the dynamic-resolution upscale. Nearest would make the
        // resolution changes obvious as a sudden pixelation; linear makes them
        // read as a mild softening, which is the entire point of scaling
        // resolution instead of dropping frames.
        vkCmdBlitImage(cmd, source.texture->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination.texture->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       source.texture->isDepth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    }
}

void Present(const TargetBinding &backBuffer)
{
    // Reported unconditionally at a low rate: a frame that submits nothing is
    // exactly the case worth seeing, so this must not be conditional on
    // something having gone wrong.
    ++g_drawStatsFrame;
    if (g_drawStatsFrame <= 3 || (g_drawStatsFrame % 240) == 0)
    {
        const uint32_t dropped = g_drawStats.requested - g_drawStats.submitted;
        LOGI("frame %u: %u draws requested, %u submitted, dropped %u (pipeline %u, pass %u, ring %u, descriptor %u); "
             "viewport %.1f,%.1f %.1fx%.1f z %.2f..%.2f",
             g_drawStatsFrame, g_drawStats.requested, g_drawStats.submitted, dropped, g_drawStats.noPipeline,
             g_drawStats.noPass, g_drawStats.noRingSpace, g_drawStats.noDescriptor,
             g_drawStats.haveViewport ? g_drawStats.viewport.x : -1.0f,
             g_drawStats.haveViewport ? g_drawStats.viewport.y : -1.0f,
             g_drawStats.haveViewport ? g_drawStats.viewport.width : -1.0f,
             g_drawStats.haveViewport ? g_drawStats.viewport.height : -1.0f,
             g_drawStats.haveViewport ? g_drawStats.viewport.minZ : -1.0f,
             g_drawStats.haveViewport ? g_drawStats.viewport.maxZ : -1.0f);
        if (!g_frameOpen)
            LOGI("frame %u: nothing opened a render pass", g_drawStatsFrame);
        // The blit at the end of this function is what actually puts anything
        // on screen. A null target, or a zero-sized source, is a black screen
        // no matter how much was drawn into it.
        LOGI("frame %u: target %s %ux%u, blit source %ux%u, swapchain %ux%u", g_drawStatsFrame,
             backBuffer.texture ? "ok" : "MISSING",
             backBuffer.texture ? backBuffer.texture->desc.width : 0u,
             backBuffer.texture ? backBuffer.texture->desc.height : 0u,
             g.renderWidth, g.renderHeight, g.swapchainExtent.width, g.swapchainExtent.height);
    }
    if (g_drawStats.requested == 0)
    {
        ++g_emptyFrames;
        if (g_emptyFrames == 1 || g_emptyFrames == 30 || (g_emptyFrames % 600) == 0)
            LOGE("the engine submitted no draw calls (%u frame%s in a row)", g_emptyFrames,
                 g_emptyFrames == 1 ? "" : "s");
    }
    else if (g_emptyFrames)
    {
        LOGI("drawing resumed after %u empty frame%s", g_emptyFrames, g_emptyFrames == 1 ? "" : "s");
        g_emptyFrames = 0;
    }
    g_drawStats = DrawStats{};

    if (!Available())
        return;
    std::lock_guard<std::mutex> lock(g.mutex);
    // A frame the engine left empty still has to reach the screen. Bailing
    // out here presents nothing at all, so the surface keeps whatever it last
    // held - undefined memory on Android - and the result is a frozen image
    // that is indistinguishable from a rendered one. Open a frame and let the
    // blit below put a defined picture up.
    EnsureFrameOpen();
    EndPass();

    FrameContext &frame = Frame();
    VkCommandBuffer cmd = frame.commandBuffer;

    uint32_t imageIndex = 0;
    VkResult acquired = VK_ERROR_OUT_OF_DATE_KHR;
    if (g.swapchainValid)
        acquired = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, frame.acquired, VK_NULL_HANDLE,
                                         &imageIndex);

    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR)
    {
        // Rotation or a resize landed between frames. Finish the command
        // buffer without presenting, rebuild and pick up next frame: dropping
        // one frame is invisible, and presenting to a stale swapchain is not
        // allowed.
        if (frame.timestampPool)
        {
            vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool, 1);
            frame.timestampsWritten = true;
        }
        vkEndCommandBuffer(cmd);
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vkQueueSubmit(g.queue, 1, &submit, frame.fence);
        g_frameOpen = false;
        g.frameIndex = (g.frameIndex + 1) % kFramesInFlight;
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
        {
            vkDeviceWaitIdle(g.device);
            CreateSwapchain(g.swapchainExtent.width, g.swapchainExtent.height);
        }
        return;
    }
    if (acquired != VK_SUCCESS)
    {
        LOGE("vkAcquireNextImageKHR: %d", static_cast<int>(acquired));
        return;
    }
    g.swapchainIndex = imageIndex;

    // Copy the scene target to the swapchain image. This is where dynamic
    // resolution becomes visible: the scene was rendered at renderWidth x
    // renderHeight and is scaled up to the display here, in one blit the
    // display controller would otherwise have done for free - but doing it
    // explicitly is what lets the resolution change every frame without
    // reallocating the swapchain.
    VkImage swapchainImage = g.swapchainImages[imageIndex];

    VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; // previous contents are not needed
    toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransfer.image = swapchainImage;
    toTransfer.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toTransfer);

    if (backBuffer.texture)
    {
        Texture *scene = backBuffer.texture;
        if (scene->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            barrier.oldLayout = scene->layout;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = scene->image;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1 };
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &barrier);
            scene->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        }

        const uint32_t sourceWidth = std::min(g.renderWidth, scene->desc.width);
        const uint32_t sourceHeight = std::min(g.renderHeight, scene->desc.height);

        VkImageBlit blit{};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { static_cast<int32_t>(sourceWidth), static_cast<int32_t>(sourceHeight), 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { static_cast<int32_t>(g.swapchainExtent.width),
                               static_cast<int32_t>(g.swapchainExtent.height), 1 };
        const VkFilter filter = (sourceWidth == g.swapchainExtent.width &&
                                 sourceHeight == g.swapchainExtent.height)
                                    ? VK_FILTER_NEAREST
                                    : VK_FILTER_LINEAR;
        vkCmdBlitImage(cmd, scene->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImage,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    }
    else
    {
        // Nothing rendered this frame (loading screen before the first
        // target exists). Clear so the surface is not undefined, which on
        // some drivers shows the previous app's framebuffer.
        VkClearColorValue black{};
        VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    }

    VkImageMemoryBarrier toPresent{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = swapchainImage;
    toPresent.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &toPresent);

    if (frame.timestampPool)
    {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, frame.timestampPool, 1);
        frame.timestampsWritten = true;
    }

    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &frame.acquired;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &frame.rendered;
    if (!Check(vkQueueSubmit(g.queue, 1, &submit, frame.fence), "vkQueueSubmit"))
    {
        g_frameOpen = false;
        return;
    }

    VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &frame.rendered;
    present.swapchainCount = 1;
    present.pSwapchains = &g.swapchain;
    present.pImageIndices = &imageIndex;
    const VkResult presented = vkQueuePresentKHR(g.queue, &present);

    g_frameOpen = false;
    g.frameIndex = (g.frameIndex + 1) % kFramesInFlight;

    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
    {
        vkDeviceWaitIdle(g.device);
        CreateSwapchain(g.swapchainExtent.width, g.swapchainExtent.height);
    }
    else if (presented != VK_SUCCESS)
    {
        LOGE("vkQueuePresentKHR: %d", static_cast<int>(presented));
    }
}
