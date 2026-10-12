// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ViewerRenderer.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "ViewerShader.h"

namespace Natron
{
namespace Gpu
{
namespace
{
struct alignas(16) InputUniform {
    std::array<float, 4> bounds, clip, format, correction, pixelGrid;
    std::array<int32_t, 4> metadata;
};
struct alignas(16) SceneUniform {
    std::array<InputUniform, 2> inputs;
    std::array<float, 4> viewport, background, checkerA, checkerB, wipe;
    std::array<int32_t, 4> options;
};
static_assert(sizeof(InputUniform) == 96 && sizeof(SceneUniform) == 288, "Viewer std140 layout mismatch.");
bool finite(const std::array<float, 4> &values)
{
    return std::all_of(values.begin(), values.end(), [](float value) {
        return std::isfinite(value);
    });
}
bool validRect(const std::array<float, 4> &r, bool allowEmpty = false)
{
    return finite(r) && (allowEmpty ? r[0] <= r[2] && r[1] <= r[3] : r[0] < r[2] && r[1] < r[3]);
}
}
struct ViewerRenderer::Private {
    explicit Private(std::shared_ptr<VulkanDevice> device_) : device(std::move(device_)) {}
    ~Private()
    {
        const auto& fn = device->functions();

        if (buffer) {
            vmaDestroyBuffer(device->allocator(), buffer, allocation);
        }

        if (pool) {
            fn.vkDestroyDescriptorPool(device->handle(), pool, nullptr);
        }

        if (pipeline) {
            fn.vkDestroyPipeline(device->handle(), pipeline, nullptr);
        }

        if (pipelineLayout) {
            fn.vkDestroyPipelineLayout(device->handle(), pipelineLayout, nullptr);
        }

        if (layout) {
            fn.vkDestroyDescriptorSetLayout(device->handle(), layout, nullptr);
        }

        if (shader) {
            fn.vkDestroyShaderModule(device->handle(), shader, nullptr);
        }

        if (sampler) {
            fn.vkDestroySampler(device->handle(), sampler, nullptr);
        }
    }
    void initialize()
    {
        device->requireUsable();
        const auto& fn = device->functions();
        dummy = std::make_unique<VulkanImage>(device, 1, 1);
        const std::array<float, 4> empty{};
        dummy->upload(empty.data(), sizeof(empty));
        VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampling.magFilter = sampling.minFilter = VK_FILTER_NEAREST;
        sampling.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampling.addressModeU = sampling.addressModeV = sampling.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        device->check(fn.vkCreateSampler(device->handle(), &sampling, nullptr, &sampler), "create Viewer sampler");
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};

        for (uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        bindings[0].descriptorType = bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkDescriptorSetLayoutCreateInfo descriptorLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        descriptorLayout.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorLayout.pBindings = bindings.data();
        device->check(fn.vkCreateDescriptorSetLayout(device->handle(), &descriptorLayout, nullptr, &layout), "create Viewer descriptor layout");
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &layout;
        device->check(fn.vkCreatePipelineLayout(device->handle(), &pipelineInfo, nullptr, &pipelineLayout), "create Viewer pipeline layout");
        VkShaderModuleCreateInfo module
        {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
        };
        module.codeSize = sizeof(ViewerShader);
        module.pCode = reinterpret_cast<const uint32_t *>(ViewerShader);
        device->check(fn.vkCreateShaderModule(device->handle(), &module, nullptr, &shader), "create Viewer shader");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = shader;
        compute.stage.pName = "main";
        compute.layout = pipelineLayout;
        device->check(fn.vkCreateComputePipelines(device->handle(), VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline), "create Viewer pipeline");
        fn.vkDestroyShaderModule(device->handle(), shader, nullptr);
        shader = VK_NULL_HANDLE;
        std::array<VkDescriptorPoolSize, 3> sizes{{
                {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
            }};
        VkDescriptorPoolCreateInfo descriptorPool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorPool.maxSets = 1;
        descriptorPool.poolSizeCount = static_cast<uint32_t>(sizes.size());
        descriptorPool.pPoolSizes = sizes.data();
        device->check(fn.vkCreateDescriptorPool(device->handle(), &descriptorPool, nullptr, &pool), "create Viewer descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout;
        device->check(fn.vkAllocateDescriptorSets(device->handle(), &allocate, &descriptor), "allocate Viewer descriptor");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = sizeof(SceneUniform);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        VmaAllocationInfo info{};
        device->check(vmaCreateBuffer(device->allocator(), &bufferInfo, &memory, &buffer, &allocation, &info), "allocate Viewer scene uniform");
        mapped = info.pMappedData;
    }
    std::shared_ptr<VulkanDevice> device;
    std::unique_ptr<VulkanImage> dummy;
    VkSampler sampler = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void *mapped = nullptr;
    std::mutex mutex;
};
ViewerRenderer::ViewerRenderer(std::shared_ptr<VulkanDevice> device)
{
    if (!device) {
        throw std::invalid_argument("A Viewer renderer requires a device.");
    }

    _imp = std::make_unique<Private>(std::move(device));
    _imp->initialize();
}
ViewerRenderer::~ViewerRenderer() = default;
void ViewerRenderer::render(const std::array<ViewerInput, 2> &inputs,
                            const ViewerSettings& settings, VulkanImage& destination)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->device->requireUsable();

    if (destination.device() != _imp->device || destination.format() != VK_FORMAT_R32G32B32A32_SFLOAT ||
        !validRect(settings.viewport) || !settings.checkerSize || settings.checkerSize > INT32_MAX ||
        static_cast<int>(settings.operation) < 0 || static_cast<int>(settings.operation) > 8 ||
        !finite(settings.wipe) || !finite(settings.background) || !finite(settings.checkerA) || !finite(settings.checkerB) ||
        settings.wipe[3] < 0 || settings.wipe[3] > 1) {
        throw std::invalid_argument("Invalid Viewer destination or scene.");
    }

    SceneUniform scene{};
    std::array<VulkanImage *, 2> sources{};

    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& input = inputs[i];

        if (input.image && (input.image->device() != _imp->device || input.image->handle() == destination.handle() ||
                            !validRect(input.bounds) || !validRect(input.clip, true) || !validRect(input.format, true) ||
                            static_cast<int>(input.premult) < 0 || static_cast<int>(input.premult) > 2 ||
                            static_cast<int>(input.lut) < 0 || static_cast<int>(input.lut) > 3 ||
                            !std::isfinite(input.gain) || !std::isfinite(input.offset) || !std::isfinite(input.gamma) ||
                            !finite(input.pixelGrid) || input.pixelGrid[0] < 0 || input.pixelGrid[1] < 0 ||
                            ((input.pixelGrid[0] == 0) != (input.pixelGrid[1] == 0)))) {
            throw std::invalid_argument("Invalid Viewer source.");
        }

        sources[i] = input.image ? input.image : _imp->dummy.get();
        scene.inputs[i] = {input.bounds, input.clip, input.format, {input.gain, input.offset, input.gamma, 0}, input.pixelGrid,
            {
                static_cast<int32_t>(input.premult), input.image ? 1 : 0, static_cast<int32_t>(input.lut),
                (input.transform ? 1 : 0) | (input.linearFilter ? 2 : 0) | (input.topDown ? 4 : 0)
            }
        };
    }

    scene.viewport = settings.viewport;
    scene.background = settings.background;
    scene.checkerA = settings.checkerA;
    scene.checkerB = settings.checkerB;
    scene.wipe = settings.wipe;

    if (settings.preserveDestination && !destination.initialized()) {
        throw std::invalid_argument("Viewer overlay requires an initialized destination.");
    }

    scene.options = {static_cast<int32_t>(settings.operation), settings.checkerboard ? 1 : 0, static_cast<int32_t>(settings.checkerSize), settings.preserveDestination ? 1 : 0};
    std::memcpy(_imp->mapped, &scene, sizeof(scene));
    _imp->device->check(vmaFlushAllocation(_imp->device->allocator(), _imp->allocation, 0, VK_WHOLE_SIZE), "flush Viewer scene uniform");
    std::array<VkDescriptorImageInfo, 3> imageInfo{};

    for (size_t i = 0; i < 2; ++i) {
        imageInfo[i] = {_imp->sampler, sources[i]->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }

    imageInfo[2] = {VK_NULL_HANDLE, destination.view(), VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufferInfo{_imp->buffer, 0, sizeof(scene)};
    std::array<VkWriteDescriptorSet, 4> writes{};

    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _imp->descriptor;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
                                   i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

        if (i < 3) {
            writes[i].pImageInfo = &imageInfo[i];
        } else {
            writes[i].pBufferInfo = &bufferInfo;
        }
    }

    const auto& fn = _imp->device->functions();
    fn.vkUpdateDescriptorSets(_imp->device->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    _imp->device->execute([&](VkCommandBuffer command) {
        for (auto * source : sources) {
            source->transition(command, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        destination.transition(command, VK_IMAGE_LAYOUT_GENERAL);
        fn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, _imp->pipeline);
        fn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, _imp->pipelineLayout, 0, 1, &_imp->descriptor, 0, nullptr);
        fn.vkCmdDispatch(command, (destination.width() + 15) / 16, (destination.height() + 15) / 16, 1);
    });
}
} // namespace Gpu
} // namespace Natron
