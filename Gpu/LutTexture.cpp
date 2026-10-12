// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/LutTexture.h"
#include <vk_mem_alloc.h>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace Natron
{
namespace Gpu
{
LutTexture::LutTexture(std::shared_ptr<VulkanDevice> device, const FragmentLut &data)
    : _device(std::move(device)), _binding(data.binding), _filter(data.filter)
{
    if (!_device || !data.values || !data.width || !data.height || !data.depth ||
        data.dimensions < 1 || data.dimensions > 3 || data.filter > 1 ||
        (data.components != 1 && data.components != 3 && data.components != 4) ||
        (data.dimensions == 1 && (data.height != 1 || data.depth != 1)) ||
        (data.dimensions == 2 && data.depth != 1)) {
        throw std::invalid_argument("Invalid fragment LUT dimensions, channels or filter.");
    }

    _device->requireUsable();

    const VkImageType type = data.dimensions == 1 ? VK_IMAGE_TYPE_1D : data.dimensions == 2 ? VK_IMAGE_TYPE_2D : VK_IMAGE_TYPE_3D;
    const VkFormat format = data.components == 1 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32G32B32A32_SFLOAT;
    const auto &fn = _device->functions();
    const auto &instance = _device->instance()->functions();
    VkImageFormatProperties limits{};
    VkResult supported = instance.vkGetPhysicalDeviceImageFormatProperties(_device->physicalDevice(), format, type,
                         VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &limits);
    VkFormatProperties features{};
    instance.vkGetPhysicalDeviceFormatProperties(_device->physicalDevice(), format, &features);

    if (supported != VK_SUCCESS || data.width > limits.maxExtent.width || data.height > limits.maxExtent.height ||
        data.depth > limits.maxExtent.depth || (data.filter && !(features.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))) {
        throw std::invalid_argument("Device cannot sample the requested fragment LUT.");
    }

    size_t pixels = data.width;

    for (uint32_t dimension : {
             data.height, data.depth
         }) {
        if (pixels > std::numeric_limits<size_t>::max() / dimension) {
            throw std::invalid_argument("Fragment LUT size overflow.");
        }

        pixels *= dimension;
    }
    const size_t channels = data.components == 1 ? 1 : 4;

    if (pixels > std::numeric_limits<size_t>::max() / (channels * sizeof(float)) ||
        pixels > std::numeric_limits<size_t>::max() / data.components || data.valueCount != pixels * data.components) {
        throw std::invalid_argument("Invalid fragment LUT value count.");
    }

    const size_t bytes = pixels * channels * sizeof(float);
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation stagingAllocation = nullptr;

    try {
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imageInfo.imageType = type;
        imageInfo.format = format;
        imageInfo.extent = {data.width, data.height, data.depth};
        imageInfo.mipLevels = imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        allocation.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        _device->check(vmaCreateImage(_device->allocator(), &imageInfo, &allocation, &_image, &_allocation, nullptr), "allocate fragment LUT");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = _image;
        viewInfo.viewType = data.dimensions == 1 ? VK_IMAGE_VIEW_TYPE_1D : data.dimensions == 2 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_3D;
        viewInfo.format = format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        _device->check(fn.vkCreateImageView(_device->handle(), &viewInfo, nullptr, &_view), "create fragment LUT view");
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = bytes;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocation.requiredFlags = 0;
        allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        VmaAllocationInfo memory{};
        _device->check(vmaCreateBuffer(_device->allocator(), &bufferInfo, &allocation, &staging, &stagingAllocation, &memory), "allocate LUT upload buffer");

        if (data.components == 3) {
            float *target = static_cast<float *>(memory.pMappedData);

            for (size_t i = 0; i < pixels; ++i) {
                std::memcpy(target + i * 4, data.values + i * 3, 3 * sizeof(float));
                target[i * 4 + 3] = 1.f;
            }
        } else {
            std::memcpy(memory.pMappedData, data.values, bytes);
        }

        _device->check(vmaFlushAllocation(_device->allocator(), stagingAllocation, 0, bytes), "flush LUT upload");
        _device->execute([&](VkCommandBuffer command) {
            VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = _image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &barrier;
            fn.vkCmdPipelineBarrier2(command, &dependency);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = imageInfo.extent;
            fn.vkCmdCopyBufferToImage(command, staging, _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fn.vkCmdPipelineBarrier2(command, &dependency);
        });
        vmaDestroyBuffer(_device->allocator(), staging, stagingAllocation);
    } catch (...) {
        if (staging) {
            vmaDestroyBuffer(_device->allocator(), staging, stagingAllocation);
        }

        fn.vkDestroyImageView(_device->handle(), _view, nullptr);

        if (_image) {
            vmaDestroyImage(_device->allocator(), _image, _allocation);
        }

        throw;
    }
}
LutTexture::~LutTexture()
{
    _device->functions().vkDestroyImageView(_device->handle(), _view, nullptr);

    if (_image) {
        vmaDestroyImage(_device->allocator(), _image, _allocation);
    }
}
}
}
