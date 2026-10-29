// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_IMAGEOPERATIONS_H
#define NATRON_GPU_IMAGEOPERATIONS_H
#include "Gpu/VulkanDevice.h"
namespace Natron
{
namespace Gpu
{
struct ImageRegion {
    uint32_t x, y, width, height;
};
// Callers serialize access to each image, including uploads and downloads.
// All compute images are RGBA32F on one pixel grid. Pixels outside the ROI
// are preserved (zero on a newly allocated destination).
class ImageOperations
{
public:
    explicit ImageOperations(std::shared_ptr<VulkanDevice> device);
    ~ImageOperations();
    ImageOperations(const ImageOperations &) = delete;
    ImageOperations &operator=(const ImageOperations &) = delete;
    void copy(VulkanImage& source, VulkanImage& destination, ImageRegion sourceRegion,
              uint32_t destinationX, uint32_t destinationY);
    void fill(VulkanImage& destination, ImageRegion region, const std::array<float, 4> &color);
    void forceOpaque(VulkanImage& image, ImageRegion region);
    // Global pixel grids: sourceLocal = 2 * destinationLocal + sourceOffset.
    // Average the available 2x2 footprint, including odd and negative edges.
    void halve(VulkanImage& source, VulkanImage& destination, ImageRegion region,
               int32_t sourceOffsetX, int32_t sourceOffsetY);
    void upscale(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                 int32_t sourceOffsetX, int32_t sourceOffsetY, uint32_t levels);
    void transform(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                   const std::array<float, 4> &gain, const std::array<float, 4> &offset);
    // 0 linear, 1 sRGB, 2 Rec.709 (Natron/Rec.2020 coefficients), 3 BT.1886.
    // Alpha is copied and matching spaces preserve the original values.
    void convertColor(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                      uint32_t sourceSpace, uint32_t destinationSpace);
    // Inspect/replace NaNs in all four channels; infinities are preserved.
    // Only a four-byte diagnostic flag is read back, never pixel data.
    bool checkForNaNs(VulkanImage& image, ImageRegion region, bool fix);
    void mix(VulkanImage& processed, VulkanImage* original, VulkanImage* mask,
             VulkanImage& destination, ImageRegion region, float amount, bool masked, bool inverted);
    // Bits 0..3 select RGBA channels to restore. Missing original pixels are zero.
    void restore(VulkanImage& processed, VulkanImage* original, VulkanImage& destination,
                 ImageRegion region, uint32_t channels);
private:
    struct Constants;
    struct Diagnostics;
    bool dispatch(VulkanImage& processed, VulkanImage* original, VulkanImage* mask,
                  VulkanImage& destination, const Constants& constants);
    void copyImpl(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                  uint32_t x, uint32_t y);
    std::shared_ptr<VulkanDevice> _device;
    std::unique_ptr<VulkanImage> _zero;
    std::unique_ptr<Diagnostics> _diagnostics;
    VkDescriptorSetLayout _descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _descriptorSet = VK_NULL_HANDLE;
    std::mutex _mutex;
};
}
}
#endif
