#include "menu_effects.hpp"
#include "TicoSlang.h"
#include "glass_shaders.hpp"
#include "glass_uniforms.hpp"
#include <imgui_impl_vulkan.h>
#include <array>
#include <cstdio>
#include <cstring>

namespace woplay_menu {
namespace {
using namespace gbastation::rendering;
using TicoVulkan::Buffer;
using TicoVulkan::Image;

struct BlurUniforms {
    ImVec2 inverseResolution, direction;
    float center = .2270270270F, count = 2, pass = 1, pad = 0;
    float weights[12] = {.3162162162F,.0702702703F};
    float offsets[12] = {1.3846153846F,3.2307692308F};
};
static_assert(sizeof(BlurUniforms) == 128);

VkDevice device() { return TicoVulkan::Ctx().device; }

VkPipeline createPipeline(VkRenderPass pass, VkPipelineLayout layout, const char* vertex, const char* fragment,
                          bool glass) {
    TicoSlang::ShaderSource shader;
    shader.vertex = vertex;
    shader.fragment = fragment;
    TicoSlang::CompiledPass compiled;
    std::string failure;
    if (!TicoSlang::Compile(shader,compiled,failure)) {
        std::fprintf(stderr,"WoPlay Vulkan glass shader: %s\n",failure.c_str());
        return VK_NULL_HANDLE;
    }
    const auto module = [](const auto& spirv) {
        VkShaderModule result = VK_NULL_HANDLE;
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.codeSize = spirv.size()*sizeof(std::uint32_t);
        info.pCode = spirv.data();
        vkCreateShaderModule(device(),&info,nullptr,&result);
        return result;
    };
    const auto vert = module(compiled.vertexSpirv);
    const auto frag = module(compiled.fragmentSpirv);
    VkPipeline result = VK_NULL_HANDLE;
    if (vert && frag) {
        const VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vert,"main"},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,frag,"main"}
        };
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.F;
        VkPipelineMultisampleStateCreateInfo sample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        sample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState color{};
        color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                               VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
        color.blendEnable = glass;
        color.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &color;
        constexpr VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = states;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &sample;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = layout;
        info.renderPass = pass;
        vkCreateGraphicsPipelines(device(),VK_NULL_HANDLE,1,&info,nullptr,&result);
    }
    if (vert) vkDestroyShaderModule(device(),vert,nullptr);
    if (frag) vkDestroyShaderModule(device(),frag,nullptr);
    return result;
}

struct Target {
    Image image;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    bool create(VkRenderPass pass, std::uint32_t width, std::uint32_t height) {
        if (!TicoVulkan::CreateImage(image,width,height,TicoVulkan::SwapFormat(),
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
            return false;
        VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        info.renderPass = pass;
        info.attachmentCount = 1;
        info.pAttachments = &image.view;
        info.width = width;
        info.height = height;
        info.layers = 1;
        return vkCreateFramebuffer(device(),&info,nullptr,&framebuffer) == VK_SUCCESS;
    }

    void destroy() {
        if (framebuffer) vkDestroyFramebuffer(device(),framebuffer,nullptr);
        framebuffer = VK_NULL_HANDLE;
        TicoVulkan::DestroyImage(image);
    }
};

void beginTarget(VkCommandBuffer cmd, VkRenderPass pass, const Target& target) {
    VkClearValue black{};
    black.color.float32[3] = 1.F;
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = pass;
    info.framebuffer = target.framebuffer;
    info.renderArea.extent = {target.image.width,target.image.height};
    info.clearValueCount = 1;
    info.pClearValues = &black;
    vkCmdBeginRenderPass(cmd,&info,VK_SUBPASS_CONTENTS_INLINE);
}

void endTarget(VkCommandBuffer cmd, const Target& target) {
    vkCmdEndRenderPass(cmd);
    TicoVulkan::TransitionImage(cmd,target.image.image,1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

void viewport(VkCommandBuffer cmd, float width, float height) {
    const VkViewport view{0,0,width,height,0,1};
    vkCmdSetViewport(cmd,0,1,&view);
}
}

struct VulkanMenuEffects::Resources {
    VkRenderPass offscreen = VK_NULL_HANDLE;
    VkPipeline glass = VK_NULL_HANDLE, blur = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDeviceSize stride = 0;
    struct Frame {
        std::array<Target,3> targets;
        std::array<VkDescriptorSet,3> sets{};
        Buffer uniforms;
        std::uint32_t offset = 0;
    };
    std::array<Frame,TicoVulkan::kFramesInFlight> frames;
    ImDrawData* frame = nullptr;
    bool ready = false, blurred = false;

    ~Resources() {
        TicoVulkan::WaitIdle();
        for (auto& frame : frames) {
            for (auto& target : frame.targets) target.destroy();
            TicoVulkan::DestroyBuffer(frame.uniforms);
        }
        if (glass) vkDestroyPipeline(device(),glass,nullptr);
        if (blur) vkDestroyPipeline(device(),blur,nullptr);
        if (pool) vkDestroyDescriptorPool(device(),pool,nullptr);
        if (sampler) vkDestroySampler(device(),sampler,nullptr);
        if (layout) vkDestroyPipelineLayout(device(),layout,nullptr);
        if (descriptors) vkDestroyDescriptorSetLayout(device(),descriptors,nullptr);
        if (offscreen) vkDestroyRenderPass(device(),offscreen,nullptr);
    }

    bool initialize();
    bool prepareSlot(Frame& frame, std::uint32_t width, std::uint32_t height, std::size_t bytes, bool needsBlur);
};

bool VulkanMenuEffects::Resources::initialize() {
    VkAttachmentDescription color{};
    color.format = TicoVulkan::SwapFormat();
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount = pass.subpassCount = pass.dependencyCount = 1;
    pass.pAttachments = &color;
    pass.pSubpasses = &subpass;
    pass.pDependencies = &dependency;
    if (vkCreateRenderPass(device(),&pass,nullptr,&offscreen) != VK_SUCCESS) return false;

    constexpr VkDescriptorSetLayoutBinding bindings[] = {
        {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}
    };
    VkDescriptorSetLayoutCreateInfo describe{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    describe.bindingCount = 3;
    describe.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(device(),&describe,nullptr,&descriptors) != VK_SUCCESS) return false;
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(BlurUniforms)};
    VkPipelineLayoutCreateInfo pipelines{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelines.setLayoutCount = 1;
    pipelines.pSetLayouts = &descriptors;
    pipelines.pushConstantRangeCount = 1;
    pipelines.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device(),&pipelines,nullptr,&layout) != VK_SUCCESS) return false;

    constexpr std::uint32_t setCount = 3*TicoVulkan::kFramesInFlight;
    constexpr VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*setCount},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,setCount}
    };
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = setCount;
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device(),&poolInfo,nullptr,&pool) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout layouts[] = {descriptors,descriptors,descriptors};
    for (auto& frame : frames) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 3;
        allocate.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device(),&allocate,frame.sets.data()) != VK_SUCCESS) return false;
    }
    VkSamplerCreateInfo linear{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    linear.minFilter = linear.magFilter = VK_FILTER_LINEAR;
    linear.addressModeU = linear.addressModeV = linear.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device(),&linear,nullptr,&sampler) != VK_SUCCESS) return false;

    glass = createPipeline(TicoVulkan::SwapRenderPass(),layout,shaders::vulkanVertex,shaders::vulkanFragment,true);
    blur = createPipeline(offscreen,layout,shaders::vulkanFullscreen,shaders::vulkanBlur,false);
    const auto alignment = std::max<VkDeviceSize>(1,TicoVulkan::Ctx().props.limits.minUniformBufferOffsetAlignment);
    stride = (sizeof(GlassUniforms)+alignment-1)/alignment*alignment;
    return glass && blur;
}

bool VulkanMenuEffects::Resources::prepareSlot(Frame& frame, std::uint32_t width, std::uint32_t height,
                                               std::size_t bytes, bool needsBlur) {
    auto& targets = frame.targets;
    const auto count = needsBlur ? targets.size() : 1;
    for (std::size_t index = 0; index < count; ++index) {
        auto& target = targets[index];
        const auto w = index ? std::max(1u,width/2) : width;
        const auto h = index ? std::max(1u,height/2) : height;
        if (target.framebuffer && target.image.width == w && target.image.height == h) continue;
        target.destroy();
        if (!target.create(offscreen,w,h)) return false;
    }
    if (frame.uniforms.size < bytes) {
        TicoVulkan::DestroyBuffer(frame.uniforms);
        if (!TicoVulkan::CreateBuffer(frame.uniforms,bytes,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const auto source = index == 0 ? (needsBlur ? 2u : 0u) : index-1;
        const VkDescriptorImageInfo images[] = {
            {sampler,targets[source].image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {sampler,targets[0].image.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}
        };
        const VkDescriptorBufferInfo buffer{frame.uniforms.buffer,0,sizeof(GlassUniforms)};
        VkWriteDescriptorSet writes[] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.sets[index],0,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&images[0]},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.sets[index],1,0,1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,&images[1]},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,frame.sets[index],2,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,nullptr,&buffer}
        };
        vkUpdateDescriptorSets(device(),3,writes,0,nullptr);
    }
    frame.offset = 0;
    return true;
}

VulkanMenuEffects::VulkanMenuEffects() : gpu_(std::make_unique<Resources>()) {}
VulkanMenuEffects::~VulkanMenuEffects() = default;
bool VulkanMenuEffects::initialize() { return gpu_->ready = gpu_->initialize(); }
bool VulkanMenuEffects::isReady() const noexcept { return gpu_->ready; }
void VulkanMenuEffects::beginEffectFrame() { gpu_->frame = nullptr; }

void VulkanMenuEffects::prepare(VkCommandBuffer cmd, ImDrawData& frame) {
    if (!isReady() || !hasRequests() || !needsBackdrop(frame)) return;
    const auto width = static_cast<std::uint32_t>(frame.DisplaySize.x*frame.FramebufferScale.x);
    const auto height = static_cast<std::uint32_t>(frame.DisplaySize.y*frame.FramebufferScale.y);
    if (!width || !height) return;
    std::size_t commands = 0;
    for (const auto* list : frame.CmdLists) commands += list->CmdBuffer.Size;

    auto& g = *gpu_;
    auto& slot = g.frames[TicoVulkan::FrameIndex()];
    const auto radius = blurRadius()*std::min(frame.FramebufferScale.x,frame.FramebufferScale.y)/3.230769F;
    g.blurred = radius > 0.F && needsBlur(frame);
    if (!g.prepareSlot(slot,width,height,g.stride*commands,g.blurred)) {
        std::fprintf(stderr,"WoPlay Vulkan glass: cannot allocate backdrop\n");
        return;
    }

    ImDrawData background;
    backgroundDrawData(frame,background);
    beginTarget(cmd,g.offscreen,slot.targets[0]);
    ImGui_ImplVulkan_RenderDrawData(&background,cmd);
    endTarget(cmd,slot.targets[0]);

    if (g.blurred) {
        for (std::size_t index = 1; index <= 2; ++index) {
            const auto& input = slot.targets[index-1].image;
            const auto& output = slot.targets[index];
            BlurUniforms data;
            data.inverseResolution = {1.F/input.width,1.F/input.height};
            data.direction = index == 1 ? ImVec2(radius,0) : ImVec2(0,radius*.5F);
            beginTarget(cmd,g.offscreen,output);
            viewport(cmd,static_cast<float>(output.image.width),static_cast<float>(output.image.height));
            const VkRect2D scissor{{0,0},{output.image.width,output.image.height}};
            vkCmdSetScissor(cmd,0,1,&scissor);
            vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.blur);
            const std::uint32_t offset = 0;
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.layout,0,1,&slot.sets[index],1,&offset);
            vkCmdPushConstants(cmd,g.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(data),&data);
            vkCmdDraw(cmd,3,1,0,0);
            endTarget(cmd,output);
        }
    }
    g.frame = &frame;
}

void VulkanMenuEffects::drawGlass(const GlassRequest& request) {
    auto& g = *gpu_;
    const auto* renderer = static_cast<const ImGui_ImplVulkan_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    if (!g.frame || !renderer) return;
    auto& slot = g.frames[TicoVulkan::FrameIndex()];
    const auto& frame = *g.frame;
    auto data = glassUniforms(request,frame);
    const auto width = data.framebuffer.x, height = data.framebuffer.y;
    const auto sx = frame.FramebufferScale.x, sy = frame.FramebufferScale.y;
    const int left = std::max(0,static_cast<int>((request.clip.x-frame.DisplayPos.x)*sx));
    const int top = std::max(0,static_cast<int>((request.clip.y-frame.DisplayPos.y)*sy));
    const int right = std::min(static_cast<int>(width),static_cast<int>((request.clip.z-frame.DisplayPos.x)*sx));
    const int bottom = std::min(static_cast<int>(height),static_cast<int>((request.clip.w-frame.DisplayPos.y)*sy));
    if (right <= left || bottom <= top || slot.offset+sizeof(data) > slot.uniforms.size) return;
    const auto format = TicoVulkan::SwapFormat();
    data.framebuffer.z = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
    if (!g.blurred) data.deformation.z = 0;
    std::memcpy(static_cast<unsigned char*>(slot.uniforms.mapped)+slot.offset,&data,sizeof(data));

    const auto cmd = renderer->CommandBuffer;
    const VkRect2D scissor{{left,top},{static_cast<std::uint32_t>(right-left),static_cast<std::uint32_t>(bottom-top)}};
    viewport(cmd,width,height);
    vkCmdSetScissor(cmd,0,1,&scissor);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.glass);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,g.layout,0,1,&slot.sets[0],1,&slot.offset);
    vkCmdDraw(cmd,6,1,0,0);
    slot.offset += static_cast<std::uint32_t>(g.stride);
}
}
