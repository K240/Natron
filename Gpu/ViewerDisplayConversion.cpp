// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ViewerDisplayConversion.h"
#include "ViewerDisplayShader.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
namespace Natron
{
namespace Gpu
{
struct ViewerDisplayConversion::Impl {
    explicit Impl(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}
    ~Impl()
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
    }
    void initialize()
    {
        device->requireUsable();
        const auto& fn = device->functions();
        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{{
                {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}
            }};
        VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutInfo.bindingCount = uint32_t(bindings.size());
        layoutInfo.pBindings = bindings.data();
        device->check(fn.vkCreateDescriptorSetLayout(device->handle(), &layoutInfo, nullptr, &layout), "create Viewer conversion descriptors");
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &layout;
        pipelineInfo.pushConstantRangeCount = 1;
        pipelineInfo.pPushConstantRanges = &range;
        device->check(fn.vkCreatePipelineLayout(device->handle(), &pipelineInfo, nullptr, &pipelineLayout), "create Viewer conversion layout");
        VkShaderModuleCreateInfo module
        {
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO
        };
        module.codeSize = sizeof(ViewerDisplayShader);
        module.pCode = reinterpret_cast<const uint32_t *>(ViewerDisplayShader);
        device->check(fn.vkCreateShaderModule(device->handle(), &module, nullptr, &shader), "create Viewer conversion shader");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = shader;
        compute.stage.pName = "main";
        compute.layout = pipelineLayout;
        device->check(fn.vkCreateComputePipelines(device->handle(), VK_NULL_HANDLE, 1, &compute, nullptr, &pipeline), "create Viewer conversion pipeline");
        fn.vkDestroyShaderModule(device->handle(), shader, nullptr);
        shader = VK_NULL_HANDLE;
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = sizes;
        device->check(fn.vkCreateDescriptorPool(device->handle(), &poolInfo, nullptr, &pool), "create Viewer conversion pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = pool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &layout;
        device->check(fn.vkAllocateDescriptorSets(device->handle(), &allocate, &descriptor), "allocate Viewer conversion descriptors");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = sizeof(ViewerDisplayTables);
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo memory{};
        memory.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        memory.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        VmaAllocationInfo info{};
        device->check(vmaCreateBuffer(device->allocator(), &bufferInfo, &memory, &buffer, &allocation, &info), "allocate Viewer conversion tables");
        mapped = info.pMappedData;
    }
    std::shared_ptr<VulkanDevice> device;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void *mapped = nullptr;
    std::unique_ptr<ViewerDisplayTables> previous;
    std::mutex mutex;
};
ViewerDisplayConversion::ViewerDisplayConversion(std::shared_ptr<VulkanDevice> device)
{
    if (!device) {
        throw std::invalid_argument("Viewer conversion requires a device.");
    }

    _imp = std::make_unique<Impl>(std::move(device));
    _imp->initialize();
}
ViewerDisplayConversion::~ViewerDisplayConversion() = default;
void ViewerDisplayConversion::render(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                                     const ViewerDisplayCorrection& correction, const ViewerDisplayTables& tables)
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    _imp->device->requireUsable();

    if (source.device() != _imp->device || destination.device() != _imp->device || !source.initialized() ||
        source.format() != VK_FORMAT_R32G32B32A32_SFLOAT || destination.format() != VK_FORMAT_R8G8B8A8_UNORM ||
        !region.width || !region.height || region.x >= source.width() || region.y >= source.height() ||
        region.width > source.width() - region.x || region.height > source.height() - region.y ||
        destination.width() != region.width || destination.height() != region.height ||
        !std::isfinite(correction.gain) || !std::isfinite(correction.offset) || !std::isfinite(correction.gamma)) {
        throw std::invalid_argument("Invalid Viewer conversion source, destination or correction.");
    }

    if (!_imp->previous || _imp->previous->color != tables.color || _imp->previous->gamma != tables.gamma) {
        if (std::any_of(tables.color.begin(), tables.color.end(), [](uint32_t v) {
        return v > 65280;
    }) ||
    std::any_of(tables.gamma.begin(), tables.gamma.end(), [](float v) {
        return !std::isfinite(v) || v < 0 || v > 1;
        })) {
            throw std::invalid_argument("Invalid Viewer display lookup tables.");
        }
        std::memcpy(_imp->mapped, &tables, sizeof(tables));
        _imp->device->check(vmaFlushAllocation(_imp->device->allocator(), _imp->allocation, 0, sizeof(tables)), "flush Viewer conversion tables");
        _imp->previous = std::make_unique<ViewerDisplayTables>(tables);
    }

    struct Constants {
        std::array<uint32_t, 4> region;
        std::array<float, 4> correction;
        std::array<int32_t, 4> options;
    } constants{{region.x, region.y, region.width, region.height},
        {correction.gain, correction.offset, correction.gamma, 0},
        {correction.linear, correction.opaque, correction.originX, correction.originY}};
    static_assert(sizeof(Constants) == 48 && sizeof(ViewerDisplayTables) == 266240, "Viewer conversion shader layout mismatch.");
    std::array<VkDescriptorImageInfo, 2> images{{
            {VK_NULL_HANDLE, source.view(), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, destination.view(), VK_IMAGE_LAYOUT_GENERAL}
        }};
    VkDescriptorBufferInfo buffer{_imp->buffer, 0, sizeof(tables)};
    std::array<VkWriteDescriptorSet, 3> writes{};

    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _imp->descriptor;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

        if (i == 2) {
            writes[i].pBufferInfo = &buffer;
        } else {
            writes[i].pImageInfo = &images[i];
        }
    }

    const auto& fn = _imp->device->functions();
    fn.vkUpdateDescriptorSets(_imp->device->handle(), uint32_t(writes.size()), writes.data(), 0, nullptr);
    _imp->device->execute([&](VkCommandBuffer command) {
        source.transition(command, VK_IMAGE_LAYOUT_GENERAL);
        destination.transition(command, VK_IMAGE_LAYOUT_GENERAL);
        fn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, _imp->pipeline);
        fn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, _imp->pipelineLayout, 0, 1, &_imp->descriptor, 0, nullptr);
        fn.vkCmdPushConstants(command, _imp->pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        fn.vkCmdDispatch(command, (region.height + 63) / 64, 1, 1);
    });
}
}
}
