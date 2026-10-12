// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/FragmentProgram.h"
#include "Gpu/PerformanceTrace.h"
#include "Gpu/LutTexture.h"
#include <shaderc/shaderc.hpp>
#include <spirv/unified1/spirv.hpp>
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
namespace Natron
{
namespace Gpu
{
namespace
{
std::vector<uint32_t> compile(const std::string& source, shaderc_shader_kind stage)
{
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    options.SetTargetSpirv(shaderc_spirv_version_1_6);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    // Do not use relaxed/fast arithmetic: OFX images carry HDR/negative values.
    auto result = compiler.CompileGlslToSpv(source, stage, "Natron shader", options);

    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        throw std::invalid_argument(result.GetErrorMessage());
    }

    return {result.cbegin(), result.cend()};
}
VkSamplerAddressMode addressMode(TextureWrap wrap)
{
    switch (wrap) {
    case TextureWrap::Repeat:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;

    case TextureWrap::Clamp:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

    case TextureWrap::Mirror:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;

    case TextureWrap::Black:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }

    throw std::invalid_argument("Invalid fragment texture wrap.");
}
}
struct MipTexture {
    std::shared_ptr<VulkanDevice> device;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    explicit MipTexture(std::shared_ptr<VulkanDevice> value) : device(std::move(value)) {}
    ~MipTexture()
    {
        device->functions().vkDestroyImageView(device->handle(), view, nullptr);

        if (image) {
            vmaDestroyImage(device->allocator(), image, allocation);
        }
    }
    void build(VulkanImage& source)
    {
        uint32_t levels = 1;

        for (uint32_t size = std::max(source.width(), source.height()); size > 1; size >>= 1) {
            ++levels;
        }

        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = source.format();
        info.extent = {source.width(), source.height(), 1};
        info.mipLevels = levels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        memory.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        memory.flags |= VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        device->check(vmaCreateImage(device->allocator(), &info, &memory, &image, &allocation, nullptr), "allocate shader mipmaps");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = info.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        device->check(device->functions().vkCreateImageView(device->handle(), &viewInfo, nullptr, &view), "create shader mipmap view");
        device->execute([&](VkCommandBuffer command) {
            const auto& fn = device->functions();
            auto barrier = [&](uint32_t level, uint32_t count, VkImageLayout oldLayout, VkImageLayout newLayout) {
                VkImageMemoryBarrier2 item{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                item.srcStageMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                item.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                item.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                item.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                item.oldLayout = oldLayout;
                item.newLayout = newLayout;
                item.srcQueueFamilyIndex = item.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                item.image = image;
                item.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
                VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dependency.imageMemoryBarrierCount = 1;
                dependency.pImageMemoryBarriers = &item;
                fn.vkCmdPipelineBarrier2(command, &dependency);
            };
            source.transition(command, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(0, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.extent = info.extent;
            fn.vkCmdCopyImage(command, source.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            int32_t width = source.width(), height = source.height();

            for (uint32_t level = 1; level < levels; ++level) {
                barrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
                blit.srcOffsets[1] = {width, height, 1};
                width = std::max(1, width / 2);
                height = std::max(1, height / 2);
                blit.dstOffsets[1] = {width, height, 1};
                fn.vkCmdBlitImage(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
                barrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }

            barrier(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    }
};
struct FragmentResources {
    std::vector<uint32_t> offsets;
    uint32_t samplers = 0;
};
FragmentResources reflect(const std::vector<uint32_t> &words, const FragmentLayout &layout)
{
    // Read the interface from the optimized module, after dead-code elimination.
    // Explicit member byte offsets survive member removal/reordering. GLSL's
    // uniform struct member is the first constant index of OpAccessChain.
    std::map<uint32_t, uint32_t> bindings, sets, pointers, variables, constants;
    std::map<uint32_t, uint32_t> sampledImages, imageDimensions, storageClasses;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> offsets;
    auto each = [&](auto&& visit) {
        for (size_t i = 5; i < words.size();) {
            const uint32_t count = words[i] >> 16;

            if (!count || count > words.size() - i) {
                throw std::runtime_error("Malformed compiler SPIR-V.");
            }

            visit(static_cast<spv::Op>(words[i] & 0xffff), words.data() + i, count);
            i += count;
        }
    };
    each([&](spv::Op op, const uint32_t* w, uint32_t count) {
        if (op == spv::OpDecorate && count >= 4) {
            if (w[2] == spv::DecorationBinding) {
                bindings[w[1]] = w[3];
            }

            if (w[2] == spv::DecorationDescriptorSet) {
                sets[w[1]] = w[3];
            }
        } else if (op == spv::OpMemberDecorate && count >= 5 && w[3] == spv::DecorationOffset) offsets[ {w[1], w[2]}] = w[4];
        else if (op == spv::OpTypePointer && count >= 4) {
            pointers[w[1]] = w[3];
        } else if (op == spv::OpVariable && count >= 4) {
            variables[w[2]] = w[1];
            storageClasses[w[2]] = w[3];
        } else if (op == spv::OpTypeSampledImage && count == 3) {
            sampledImages[w[1]] = w[2];
        } else if (op == spv::OpTypeImage && count >= 9) {
            // Only non-arrayed, non-multisampled float sampled images.
            if (w[4] || w[5] || w[6] || w[7] != 1) {
                throw std::invalid_argument("Unsupported fragment sampler type.");
            }

            imageDimensions[w[1]] = w[3];
        } else if (op == spv::OpConstant && count == 4) {
            constants[w[2]] = w[3];
        }
    });
    std::map<uint32_t, uint32_t> expectedDimensions;

    for (auto binding : layout.inputBindings) {
        expectedDimensions[binding] = spv::Dim2D;
    }

    for (const auto &lut : layout.luts) {
        expectedDimensions[lut.binding] = lut.dimensions - 1;
    }

    for (const auto &entry : bindings) {
        const auto variable = entry.first;

        if (sets[variable] != 0) {
            throw std::invalid_argument("Fragment descriptors must use set 0.");
        }

        if (entry.second == layout.uniformBinding) {
            if (storageClasses[variable] != spv::StorageClassUniform) {
                throw std::invalid_argument("Fragment uniform binding has the wrong type.");
            }
        } else {
            const auto expected = expectedDimensions.find(entry.second);
            const auto sampled = sampledImages.find(pointers[variables[variable]]);

            if (storageClasses[variable] != spv::StorageClassUniformConstant || expected == expectedDimensions.end() ||
                sampled == sampledImages.end() || !imageDimensions.count(sampled->second) ||
                imageDimensions[sampled->second] != expected->second) {
                throw std::invalid_argument("Fragment texture binding or dimension does not match its layout.");
            }
        }
    }

    FragmentResources result;
    std::set<uint32_t> active;
    each([&](spv::Op op, const uint32_t* w, uint32_t count) {
        if (op == spv::OpLoad && count >= 4 && bindings.count(w[3]) && sets[w[3]] == 0) {
            const auto found = std::find(layout.inputBindings.begin(), layout.inputBindings.end(), bindings[w[3]]);

            if (found != layout.inputBindings.end()) {
                result.samplers |= 1u << std::distance(layout.inputBindings.begin(), found);
            }
        }

        if ((op == spv::OpAccessChain || op == spv::OpInBoundsAccessChain) && count >= 5 &&
            bindings.count(w[3]) && bindings[w[3]] == layout.uniformBinding && sets[w[3]] == 0 && constants.count(w[4])) {
            const auto found = offsets.find({pointers[variables[w[3]]], constants[w[4]]});

            if (found != offsets.end()) {
                active.insert(found->second);
            }
        }
    });
    result.offsets.assign(active.begin(), active.end());
    return result;
}
struct FragmentProgram::Impl {
    FragmentResources resources;
    std::shared_ptr<VulkanDevice> device;
    uint32_t uniformBytes;
    uint32_t uniformBinding = 4;
    std::vector<uint32_t> inputBindings;
    std::vector<std::unique_ptr<LutTexture>> luts;
    ImageOperations copies;
    std::unique_ptr<VulkanImage> zero;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuffer uniformBuffer = VK_NULL_HANDLE;
    VmaAllocation uniformAllocation = nullptr;
    void *mapped = nullptr;
    std::array<VkSampler, 16> samplers{};
    std::mutex mutex;
    Impl(std::shared_ptr<VulkanDevice> value, uint32_t bytes) : device(std::move(value)), uniformBytes(bytes), copies(device) {}
    ~Impl()
    {
        const auto& fn = device->functions();

        for (auto sampler : samplers) {
            fn.vkDestroySampler(device->handle(), sampler, nullptr);
        }

        if (uniformBuffer) {
            vmaDestroyBuffer(device->allocator(), uniformBuffer, uniformAllocation);
        }

        fn.vkDestroyDescriptorPool(device->handle(), pool, nullptr);
        fn.vkDestroyPipeline(device->handle(), pipeline, nullptr);
        fn.vkDestroyPipelineLayout(device->handle(), layout, nullptr);
        fn.vkDestroyDescriptorSetLayout(device->handle(), descriptors, nullptr);
    }
};
FragmentProgram::FragmentProgram(std::shared_ptr<VulkanDevice> device, const std::string& source, uint32_t bytes)
    : FragmentProgram(std::move(device), source, bytes, FragmentLayout()) {}
FragmentProgram::FragmentProgram(std::shared_ptr<VulkanDevice> device, const std::string& source, uint32_t bytes, const FragmentLayout &layout)
{
    if (!device || !bytes || bytes % 16 || bytes > device->properties().limits.maxUniformBufferRange ||
        source.empty() || source.size() > 4 * 1024 * 1024) {
        throw std::invalid_argument("Invalid fragment program or uniform size.");
    }

    device->requireUsable();

    const auto &limits = device->properties().limits;
    const size_t samplerCount = layout.inputBindings.size() + layout.luts.size();

    if (layout.inputBindings.size() > 4 || samplerCount > limits.maxPerStageDescriptorSamplers ||
        samplerCount > limits.maxPerStageDescriptorSampledImages || samplerCount > limits.maxDescriptorSetSamplers ||
        samplerCount > limits.maxDescriptorSetSampledImages || samplerCount + 1 > limits.maxPerStageResources) {
        throw std::invalid_argument("Fragment descriptor count exceeds device limits.");
    }

    std::set<uint32_t> occupied{layout.uniformBinding};

    for (auto binding : layout.inputBindings) {
        if (!occupied.insert(binding).second) {
            throw std::invalid_argument("Duplicate fragment descriptor binding.");
        }
    }

    for (const auto &lut : layout.luts) {
        if (!occupied.insert(lut.binding).second || lut.dimensions < 1 || lut.dimensions > 3) {
            throw std::invalid_argument("Duplicate or invalid fragment LUT binding.");
        }
    }

    const auto vertexCode = compile(
                                "#version 450\nvoid main(){vec2 p=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);"
                                "gl_Position=vec4(p*2.0-1.0,0.5,1.0);}", shaderc_vertex_shader);
    const auto fragmentCode = compile(source, shaderc_fragment_shader);
    _imp = std::make_unique<Impl>(std::move(device), bytes);
    _imp->resources = reflect(fragmentCode, layout);
    _imp->uniformBinding = layout.uniformBinding;
    _imp->inputBindings = layout.inputBindings;

    for (const auto &lut : layout.luts) {
        _imp->luts.push_back(std::make_unique<LutTexture>(_imp->device, lut));
    }

    const auto& fn = _imp->device->functions();
    const VkDevice handle = _imp->device->handle();
    std::vector<VkDescriptorSetLayoutBinding> bindings;

    for (auto binding : layout.inputBindings) bindings.push_back({binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});

    for (const auto &lut : layout.luts) bindings.push_back({lut.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    bindings.push_back({layout.uniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});

    VkDescriptorSetLayoutCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    descriptorInfo.pBindings = bindings.data();
    _imp->device->check(fn.vkCreateDescriptorSetLayout(handle, &descriptorInfo, nullptr, &_imp->descriptors), "create fragment descriptors");
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &_imp->descriptors;
    _imp->device->check(fn.vkCreatePipelineLayout(handle, &layoutInfo, nullptr, &_imp->layout), "create fragment layout");
    std::array<VkShaderModule, 2> modules{};

    try {
        const std::array<const std::vector<uint32_t>*, 2> codes{&vertexCode, &fragmentCode};
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};

        for (unsigned i = 0; i < modules.size(); ++i) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = codes[i]->size() * sizeof(uint32_t);
            info.pCode = codes[i]->data();
            _imp->device->check(fn.vkCreateShaderModule(handle, &info, nullptr, &modules[i]), "create fragment shader module");
            stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module = modules[i];
            stages[i].pName = "main";
        }

        VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.f;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
        const VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &format;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pipeline.pNext = &rendering;
        pipeline.stageCount = static_cast<uint32_t>(stages.size());
        pipeline.pStages = stages.data();
        pipeline.pVertexInputState = &vertex;
        pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport;
        pipeline.pRasterizationState = &raster;
        pipeline.pMultisampleState = &samples;
        pipeline.pColorBlendState = &blend;
        pipeline.pDynamicState = &dynamic;
        pipeline.layout = _imp->layout;
        _imp->device->check(fn.vkCreateGraphicsPipelines(handle, VK_NULL_HANDLE, 1, &pipeline, nullptr, &_imp->pipeline), "create fragment pipeline");
    } catch (...) {
        for (auto module : modules) {
            fn.vkDestroyShaderModule(handle, module, nullptr);
        }

        throw;
    }

    for (auto module : modules) {
        fn.vkDestroyShaderModule(handle, module, nullptr);
    }

    std::vector<VkDescriptorPoolSize> sizes{{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};

    if (samplerCount) sizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(samplerCount)});
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    _imp->device->check(fn.vkCreateDescriptorPool(handle, &poolInfo, nullptr, &_imp->pool), "create fragment pool");
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = _imp->pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &_imp->descriptors;
    _imp->device->check(fn.vkAllocateDescriptorSets(handle, &allocate, &_imp->set), "allocate fragment descriptors");
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    VmaAllocationInfo memory{};
    _imp->device->check(vmaCreateBuffer(_imp->device->allocator(), &bufferInfo, &allocation, &_imp->uniformBuffer,
                                        &_imp->uniformAllocation, &memory), "allocate fragment uniforms");
    _imp->mapped = memory.pMappedData;

    for (unsigned i = 0; i < _imp->samplers.size(); ++i) {
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.minFilter = sampler.magFilter = i % 4 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler.maxLod = i % 4 >= 2 ? VK_LOD_CLAMP_NONE : 0.f;
        sampler.anisotropyEnable = i % 4 == 3 && _imp->device->supportsAnisotropy();
        sampler.maxAnisotropy = sampler.anisotropyEnable ? _imp->device->properties().limits.maxSamplerAnisotropy : 1.f;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = addressMode(static_cast<TextureWrap>(i / 4));
        sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        _imp->device->check(fn.vkCreateSampler(handle, &sampler, nullptr, &_imp->samplers[i]), "create fragment sampler");
    }

    _imp->zero = std::make_unique<VulkanImage>(_imp->device, 1, 1);
    _imp->zero->clear({0, 0, 0, 0});
}
FragmentProgram::~FragmentProgram() = default;
const std::vector<uint32_t> &FragmentProgram::activeUniformOffsets() const
{
    return _imp->resources.offsets;
}
uint32_t FragmentProgram::activeSamplers() const
{
    return _imp->resources.samplers;
}
void FragmentProgram::render(VulkanImage& destination, ImageRegion roi, const std::array<FragmentInput, 4> &inputs,
                             const void *uniforms, size_t bytes)
{
    PerformanceTrace trace("fragment-render", destination.byteSize());
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->device->requireUsable();

    if (destination.device() != _imp->device || destination.format() != VK_FORMAT_R32G32B32A32_SFLOAT ||
        !roi.width || !roi.height || roi.x >= destination.width() || roi.y >= destination.height() ||
        roi.width > destination.width() - roi.x || roi.height > destination.height() - roi.y ||
        !uniforms || bytes != _imp->uniformBytes) {
        throw std::invalid_argument("Invalid fragment render destination, ROI or uniforms.");
    }

    bool aliased = false;

    for (unsigned i = 0; i < inputs.size(); ++i) {
        const auto &input = inputs[i];

        if (i >= _imp->inputBindings.size() && input.image) {
            throw std::invalid_argument("Fragment input has no descriptor binding.");
        }

        addressMode(input.wrap);

        if (input.image && (input.image->device() != _imp->device || !input.image->initialized() ||
                            input.image->format() != VK_FORMAT_R32G32B32A32_SFLOAT)) {
            throw std::invalid_argument("Fragment input requires initialized RGBA32F on the same device.");
        }

        aliased |= input.image == &destination;

        if (input.filter > 3) {
            throw std::invalid_argument("Invalid fragment texture filter.");
        }

        if (input.filter) {
            VkFormatProperties properties{};
            _imp->device->instance()->functions().vkGetPhysicalDeviceFormatProperties(_imp->device->physicalDevice(),
                    VK_FORMAT_R32G32B32A32_SFLOAT, &properties);

            if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
                throw std::invalid_argument("Device cannot linearly filter RGBA32F.");
            }

            if (input.filter >= 2 && (properties.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) !=
                (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
                throw std::invalid_argument("Device cannot build RGBA32F mipmaps.");
            }
        }
    }

    std::unique_ptr<VulkanImage> scratch;

    if (aliased) {
        scratch = std::make_unique<VulkanImage>(_imp->device, destination.width(), destination.height());
    }

    VulkanImage& output = scratch ? *scratch : destination;

    if (!output.initialized()) output.clear({0, 0, 0, 0});
    std::memcpy(_imp->mapped, uniforms, bytes);
    _imp->device->check(vmaFlushAllocation(_imp->device->allocator(), _imp->uniformAllocation, 0, bytes), "flush fragment uniforms");
    const auto& fn = _imp->device->functions();
    std::array<VulkanImage *, 4> images{};
    std::array<std::unique_ptr<MipTexture>, 4> mipmaps;
    const size_t inputCount = _imp->inputBindings.size();
    std::vector<VkDescriptorImageInfo> imageInfo(inputCount + _imp->luts.size());
    std::vector<VkWriteDescriptorSet> writes(imageInfo.size() + 1);

    for (unsigned i = 0; i < inputCount; ++i) {
        images[i] = inputs[i].image ? inputs[i].image : _imp->zero.get();

        if (inputs[i].image && inputs[i].filter >= 2) {
            mipmaps[i] = std::make_unique<MipTexture>(_imp->device);
            mipmaps[i]->build(*images[i]);
        }

        imageInfo[i] = {_imp->samplers[static_cast<unsigned>(inputs[i].wrap) * 4 + inputs[i].filter],
                        mipmaps[i] ? mipmaps[i]->view : images[i]->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                       };
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _imp->set;
        writes[i].dstBinding = _imp->inputBindings[i];
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfo[i];
    }

    for (size_t j = 0; j < _imp->luts.size(); ++j) {
        const size_t i = inputCount + j;
        const auto &lut = *_imp->luts[j];
        imageInfo[i] = {_imp->samplers[4 + lut.filter()], lut.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _imp->set;
        writes[i].dstBinding = lut.binding();
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfo[i];
    }

    VkDescriptorBufferInfo bufferInfo{_imp->uniformBuffer, 0, bytes};
    auto &uniformWrite = writes.back();
    uniformWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    uniformWrite.dstSet = _imp->set;
    uniformWrite.dstBinding = _imp->uniformBinding;
    uniformWrite.descriptorCount = 1;
    uniformWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uniformWrite.pBufferInfo = &bufferInfo;
    fn.vkUpdateDescriptorSets(_imp->device->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    _imp->device->execute([&](VkCommandBuffer command) {
        for (size_t i = 0; i < inputCount; ++i) {
            images[i]->transition(command, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        output.transition(command, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = output.view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{static_cast<int32_t>(roi.x), static_cast<int32_t>(roi.y)}, {roi.width, roi.height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        fn.vkCmdBeginRendering(command, &rendering);
        VkViewport viewport{0, 0, static_cast<float>(output.width()), static_cast<float>(output.height()), 0, 1};
        fn.vkCmdSetViewport(command, 0, 1, &viewport);
        fn.vkCmdSetScissor(command, 0, 1, &rendering.renderArea);
        fn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _imp->pipeline);
        fn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, _imp->layout, 0, 1, &_imp->set, 0, nullptr);
        fn.vkCmdDraw(command, 3, 1, 0, 0);
        fn.vkCmdEndRendering(command);
    });

    if (scratch) {
        _imp->copies.copy(*scratch, destination, roi, roi.x, roi.y);
    }
}
}
}
