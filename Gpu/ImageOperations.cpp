// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ImageOperations.h"
#include "ImageOperationsShader.h"
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vk_mem_alloc.h>
namespace Natron
{
namespace Gpu
{
namespace
{
void validateRegion(const VulkanImage& image, ImageRegion region)
{
    if (!region.width || !region.height || region.x >= image.width() || region.y >= image.height() ||
        region.width > image.width() - region.x || region.height > image.height() - region.y) {
        throw std::invalid_argument("Image operation ROI is outside the image.");
    }
}
}
struct ImageOperations::Constants {
    std::array<uint32_t, 4> region;
    std::array<float, 4> gain{};
    std::array<float, 4> offset{};
    uint32_t operation = 0, flags = 0, channels = 0;
    float amount = 0;
    std::array<int32_t, 4> sampleOffset{};
};
struct ImageOperations::Diagnostics {
    std::shared_ptr<VulkanDevice> device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void *mapped = nullptr;
    explicit Diagnostics(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner))
    {
        device->requireUsable();
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = sizeof(uint32_t);
        info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        VmaAllocationInfo result{};
        device->check(vmaCreateBuffer(device->allocator(), &info, &allocationInfo, &buffer, &allocation, &result), "allocate image diagnostic flag");
        mapped = result.pMappedData;
    }
    ~Diagnostics()
    {
        vmaDestroyBuffer(device->allocator(), buffer, allocation);
    }
};
ImageOperations::ImageOperations(std::shared_ptr<VulkanDevice> device) : _device(std::move(device))
{
    if (!_device) {
        throw std::invalid_argument("Image operations require a device.");
    }

    _device->requireUsable();

    const auto& fn = _device->functions();
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};

    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {i, i == 4 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr
                      };
    }

    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    _device->check(fn.vkCreateDescriptorSetLayout(_device->handle(), &layout, nullptr, &_descriptorLayout), "create image operation descriptors");
    VkShaderModule shader = VK_NULL_HANDLE;

    try {
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants)};
        VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineLayout.setLayoutCount = 1;
        pipelineLayout.pSetLayouts = &_descriptorLayout;
        pipelineLayout.pushConstantRangeCount = 1;
        pipelineLayout.pPushConstantRanges = &range;
        _device->check(fn.vkCreatePipelineLayout(_device->handle(), &pipelineLayout, nullptr, &_pipelineLayout), "create image operation layout");
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(ImageOperationsShader);
        shaderInfo.pCode = reinterpret_cast<const uint32_t *>(ImageOperationsShader);
        _device->check(fn.vkCreateShaderModule(_device->handle(), &shaderInfo, nullptr, &shader), "create image operation shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module = shader;
        pipeline.stage.pName = "main";
        pipeline.layout = _pipelineLayout;
        _device->check(fn.vkCreateComputePipelines(_device->handle(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &_pipeline), "create image operation pipeline");
        fn.vkDestroyShaderModule(_device->handle(), shader, nullptr);
        shader = VK_NULL_HANDLE;
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 2;
        pool.pPoolSizes = sizes;
        _device->check(fn.vkCreateDescriptorPool(_device->handle(), &pool, nullptr, &_descriptorPool), "create image operation pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = _descriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &_descriptorLayout;
        _device->check(fn.vkAllocateDescriptorSets(_device->handle(), &allocate, &_descriptorSet), "allocate image operation descriptors");
        _diagnostics = std::make_unique<Diagnostics>(_device);
        VkDescriptorBufferInfo bufferInfo{_diagnostics->buffer, 0, sizeof(uint32_t)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = _descriptorSet;
        write.dstBinding = 4;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        fn.vkUpdateDescriptorSets(_device->handle(), 1, &write, 0, nullptr);
        _zero = std::make_unique<VulkanImage>(_device, 1, 1);
        _zero->clear({0, 0, 0, 0});
    } catch (...) {
        if (shader) {
            fn.vkDestroyShaderModule(_device->handle(), shader, nullptr);
        }

        if (_descriptorPool) {
            fn.vkDestroyDescriptorPool(_device->handle(), _descriptorPool, nullptr);
        }

        if (_pipeline) {
            fn.vkDestroyPipeline(_device->handle(), _pipeline, nullptr);
        }

        if (_pipelineLayout) {
            fn.vkDestroyPipelineLayout(_device->handle(), _pipelineLayout, nullptr);
        }

        fn.vkDestroyDescriptorSetLayout(_device->handle(), _descriptorLayout, nullptr);
        throw;
    }
}
ImageOperations::~ImageOperations()
{
    const auto& fn = _device->functions();
    fn.vkDestroyDescriptorPool(_device->handle(), _descriptorPool, nullptr);
    fn.vkDestroyPipeline(_device->handle(), _pipeline, nullptr);
    fn.vkDestroyPipelineLayout(_device->handle(), _pipelineLayout, nullptr);
    fn.vkDestroyDescriptorSetLayout(_device->handle(), _descriptorLayout, nullptr);
}
void ImageOperations::copyImpl(VulkanImage& source, VulkanImage& destination, ImageRegion region, uint32_t x, uint32_t y)
{
    if (source.device() != _device || destination.device() != _device || source.format() != destination.format() || !source.initialized()) {
        throw std::invalid_argument("Image copy requires initialized, equally formatted images on one device.");
    }

    validateRegion(source, region);
    validateRegion(destination, {x, y, region.width, region.height});

    if (&source == &destination) {
        VulkanImage temporary(_device, region.width, region.height, source.format());
        copyImpl(source, temporary, region, 0, 0);
        copyImpl(temporary, destination, {0, 0, region.width, region.height}, x, y);
        return;
    }

    if (!destination.initialized()) destination.clear({0, 0, 0, 0});
    _device->execute([&](VkCommandBuffer command) {
        source.transition(command, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        destination.transition(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.srcOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y), 0};
        copy.dstOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
        copy.extent = {region.width, region.height, 1};
        _device->functions().vkCmdCopyImage(command, source.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                            destination.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    });
}
void ImageOperations::copy(VulkanImage& source, VulkanImage& destination, ImageRegion region, uint32_t x, uint32_t y)
{
    std::lock_guard<std::mutex> lock(_mutex);
    copyImpl(source, destination, region, x, y);
}
void ImageOperations::forceOpaque(VulkanImage& image, ImageRegion region)
{
    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 8;
    dispatch(image, nullptr, nullptr, image, constants);
}
void ImageOperations::transform(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                                const std::array<float, 4> &gain, const std::array<float, 4> &offset)
{
    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.gain = gain;
    constants.offset = offset;

    for (size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(gain[i]) || !std::isfinite(offset[i])) {
            throw std::invalid_argument("Non-finite image transform.");
        }
    }

    dispatch(source, nullptr, nullptr, destination, constants);
}

void ImageOperations::fill(VulkanImage& destination, ImageRegion region, const std::array<float, 4> &color)
{
    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 3;
    constants.offset = color;
    dispatch(destination, nullptr, nullptr, destination, constants);
}
void ImageOperations::halve(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                            int32_t sourceOffsetX, int32_t sourceOffsetY)
{
    validateRegion(destination, region);
    const int64_t x1 = int64_t(region.x) * 2 + sourceOffsetX;
    const int64_t y1 = int64_t(region.y) * 2 + sourceOffsetY;
    const int64_t x2 = int64_t(region.x + region.width - 1) * 2 + sourceOffsetX;
    const int64_t y2 = int64_t(region.y + region.height - 1) * 2 + sourceOffsetY;

    if (x1 < -1 || y1 < -1 || x2 >= source.width() || y2 >= source.height() ||
        x2 >= std::numeric_limits<int32_t>::max() || y2 >= std::numeric_limits<int32_t>::max()) {
        throw std::invalid_argument("Mipmap ROI has an empty or overflowing source footprint.");
    }

    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 6;
    constants.sampleOffset = {sourceOffsetX, sourceOffsetY, 0, 0};
    dispatch(source, nullptr, nullptr, destination, constants);
}
void ImageOperations::convertColor(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                                   uint32_t sourceSpace, uint32_t destinationSpace)
{
    if (sourceSpace > 3 || destinationSpace > 3) {
        throw std::invalid_argument("Unknown image conversion color space.");
    }

    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 4;
    constants.flags = sourceSpace;
    constants.channels = destinationSpace;
    dispatch(source, nullptr, nullptr, destination, constants);
}
void ImageOperations::upscale(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                              int32_t sourceOffsetX, int32_t sourceOffsetY, uint32_t levels)
{
    validateRegion(destination, region);

    if (!levels || levels > 30) {
        throw std::invalid_argument("Invalid native upscale level.");
    }

    const int64_t scale = int64_t(1) << levels;
    const int64_t x1 = int64_t(region.x) + sourceOffsetX, y1 = int64_t(region.y) + sourceOffsetY;
    const int64_t x2 = int64_t(region.x + region.width - 1) + sourceOffsetX;
    const int64_t y2 = int64_t(region.y + region.height - 1) + sourceOffsetY;

    if (x1 < 0 || y1 < 0 || x2 / scale >= source.width() || y2 / scale >= source.height() ||
        x2 > INT32_MAX || y2 > INT32_MAX) {
        throw std::invalid_argument("Native upscale source footprint is outside the image.");
    }

    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 7;
    constants.sampleOffset = {sourceOffsetX, sourceOffsetY, static_cast<int32_t>(scale), 0};
    dispatch(source, nullptr, nullptr, destination, constants);
}
bool ImageOperations::checkForNaNs(VulkanImage& image, ImageRegion region, bool fix)
{
    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 5;
    constants.flags = fix ? 1u : 0u;
    return dispatch(image, nullptr, nullptr, image, constants);
}
void ImageOperations::mix(VulkanImage& processed, VulkanImage* original, VulkanImage* mask,
                          VulkanImage& destination, ImageRegion region, float amount, bool masked, bool inverted)
{
    if (!std::isfinite(amount)) {
        throw std::invalid_argument("Non-finite image mix.");
    }

    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 1;
    constants.flags = (original ? 1u : 0u) | (masked ? 2u : 0u) | (mask ? 4u : 0u) | (inverted ? 8u : 0u);
    constants.amount = amount;
    dispatch(processed, original, mask, destination, constants);
}
void ImageOperations::restore(VulkanImage& processed, VulkanImage* original, VulkanImage& destination,
                              ImageRegion region, uint32_t channels)
{
    if (channels > 15) {
        throw std::invalid_argument("Invalid image channel mask.");
    }

    Constants constants{{region.x, region.y, region.width, region.height}};
    constants.operation = 2;
    constants.flags = original ? 1u : 0u;
    constants.channels = channels;
    dispatch(processed, original, nullptr, destination, constants);
}
bool ImageOperations::dispatch(VulkanImage& processed, VulkanImage* original, VulkanImage* mask,
                               VulkanImage& destination, const Constants& constants)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _device->requireUsable();
    ImageRegion region{constants.region[0], constants.region[1], constants.region[2], constants.region[3]};
    validateRegion(destination, region);

    for (auto * input : {
             &processed, original, mask, &destination
         }) {
        if (!input) {
            continue;
        }

        if (input->device() != _device || input->format() != VK_FORMAT_R32G32B32A32_SFLOAT ||
            (constants.operation < 6 && (input->width() != destination.width() || input->height() != destination.height())) ||
            (input != &destination && !input->initialized())) {
            throw std::invalid_argument("Compute requires initialized RGBA32F inputs on one device and pixel grid.");
        }
    }

    // An aliased input must be initialized too; destinations alone may be new.
    if (constants.operation != 3 && ((!processed.initialized()) || (original && !original->initialized()) || (mask && !mask->initialized()))) {
        throw std::invalid_argument("Uninitialized compute input.");
    }

    std::unique_ptr<VulkanImage> temporary;
    VulkanImage* output = &destination;

    if (constants.operation != 3 && !(constants.operation == 5 && constants.flags == 0) &&
        (&processed == output || original == output || mask == output)) {
        temporary = std::make_unique<VulkanImage>(_device, destination.width(), destination.height());
        output = temporary.get();
    }

    if (!output->initialized()) output->clear({0, 0, 0, 0});
    std::array<VulkanImage *, 4> images{&processed, original ? original : _zero.get(), mask ? mask : _zero.get(), output};
    std::array<VkDescriptorImageInfo, 4> imageInfos{};
    std::array<VkWriteDescriptorSet, 4> writes{};

    for (uint32_t i = 0; i < images.size(); ++i) {
        imageInfos[i] = {VK_NULL_HANDLE, images[i]->view(), VK_IMAGE_LAYOUT_GENERAL};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &imageInfos[i];
    }

    const auto& fn = _device->functions();
    fn.vkUpdateDescriptorSets(_device->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    _device->execute([&](VkCommandBuffer command) {
        if (constants.operation == 5) {
            VkBufferMemoryBarrier2 beforeFill{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            beforeFill.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            beforeFill.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            beforeFill.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            beforeFill.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            beforeFill.srcQueueFamilyIndex = beforeFill.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            beforeFill.buffer = _diagnostics->buffer;
            beforeFill.size = sizeof(uint32_t);
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &beforeFill;
            fn.vkCmdPipelineBarrier2(command, &dependency);
            fn.vkCmdFillBuffer(command, _diagnostics->buffer, 0, sizeof(uint32_t), 0);
            VkBufferMemoryBarrier2 toCompute = beforeFill;
            toCompute.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toCompute.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toCompute.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            toCompute.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            dependency.pBufferMemoryBarriers = &toCompute;
            fn.vkCmdPipelineBarrier2(command, &dependency);
        }

        for (auto * image : images) {
            image->transition(command, VK_IMAGE_LAYOUT_GENERAL);
        }

        fn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        fn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &_descriptorSet, 0, nullptr);
        fn.vkCmdPushConstants(command, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        fn.vkCmdDispatch(command, (region.width + 15) / 16, (region.height + 15) / 16, 1);

        if (constants.operation == 5) {
            VkBufferMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
            toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toHost.buffer = _diagnostics->buffer;
            toHost.size = sizeof(uint32_t);
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = &toHost;
            fn.vkCmdPipelineBarrier2(command, &dependency);
        }
    });

    if (temporary) {
        copyImpl(*temporary, destination, region, region.x, region.y);
    }

    if (constants.operation == 5) {
        _device->check(vmaInvalidateAllocation(_device->allocator(), _diagnostics->allocation, 0, VK_WHOLE_SIZE), "invalidate image diagnostic flag");
        uint32_t flag;
        _device->requireUsable();
        std::memcpy(&flag, _diagnostics->mapped, sizeof(flag));
        _device->requireUsable();
        return flag != 0;
    }

    return false;
}
}
}
