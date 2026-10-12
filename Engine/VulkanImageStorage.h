// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_VULKANIMAGESTORAGE_H
#define NATRON_ENGINE_VULKANIMAGESTORAGE_H
#include <Python.h>
#include "Global/Macros.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
NATRON_NAMESPACE_ENTER
namespace Gpu
{
class VulkanDevice;
class VulkanImage;
class ViewerDisplayConversion;
struct ViewerDisplayCorrection;
struct ViewerDisplayTables;
}
class VulkanImageStorage;
class VulkanFragmentProgram;
class VulkanImageContext
{
public:
    explicit VulkanImageContext(std::shared_ptr<Gpu::VulkanDevice> device);
    ~VulkanImageContext();
    VulkanImageContext(const VulkanImageContext &) = delete;
    VulkanImageContext &operator=(const VulkanImageContext &) = delete;
    const std::shared_ptr<Gpu::VulkanDevice> &device() const;
    // Successful CPU pixel uploads/downloads. GPU copies and kernels do not
    // change these counters; callers can prove graph transfers stay native.
    std::array<uint64_t, 2> transferCounts() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _imp;
    friend class VulkanImageStorage;
    friend class VulkanFragmentProgram;
};
// CacheEntry owns this allocation; only runtime state refers to the device.
// No Vulkan handles or headers are exposed to the cache or Python bindings.
class VulkanImageStorage
{
public:
    VulkanImageStorage(std::shared_ptr<VulkanImageContext> context, uint32_t width, uint32_t height);
    ~VulkanImageStorage();
    VulkanImageStorage(const VulkanImageStorage &) = delete;
    VulkanImageStorage &operator=(const VulkanImageStorage &) = delete;
    size_t byteSize() const;
    uint32_t width() const;
    uint32_t height() const;
    const std::shared_ptr<VulkanImageContext> &context() const;
    void upload(const void* bytes, size_t count);
    void uploadRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* bytes, size_t count);
    void uploadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* bytes, size_t count, size_t rowBytes);
    void downloadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* bytes, size_t count, size_t rowBytes) const;
    std::vector<unsigned char> download() const;
    void copy(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t destinationX, uint32_t destinationY);
    // Snapshot a completed region into a display-owned allocation on the same
    // device. No CPU pixels or long-lived cache image locks cross this boundary.
    void copyTo(Gpu::VulkanImage& destination, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                uint32_t destinationX, uint32_t destinationY, bool forceOpaque = false) const;
    void displayTo(Gpu::ViewerDisplayConversion& conversion, Gpu::VulkanImage& destination,
                   uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   const Gpu::ViewerDisplayCorrection& correction, const Gpu::ViewerDisplayTables& tables) const;
    void fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height, const std::array<float, 4> &color);
    void halve(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
               int32_t sourceOffsetX, int32_t sourceOffsetY);
    void upscale(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                 int32_t sourceOffsetX, int32_t sourceOffsetY, uint32_t levels);
    void transform(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                   const std::array<float, 4> &gain, const std::array<float, 4> &offset);
    void mix(const VulkanImageStorage& processed, const VulkanImageStorage* original, const VulkanImageStorage* mask,
             uint32_t x, uint32_t y, uint32_t width, uint32_t height, float amount, bool masked, bool inverted);
    void restore(const VulkanImageStorage& processed, const VulkanImageStorage* original,
                 uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t channels);
    void convertColor(const VulkanImageStorage& source, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t sourceSpace, uint32_t destinationSpace);
    bool checkForNaNs(uint32_t x, uint32_t y, uint32_t width, uint32_t height, bool fix);
private:
    void checkContext(const VulkanImageStorage* other) const;
    struct Impl;
    std::unique_ptr<Impl> _imp;
    friend class VulkanFragmentProgram;
};
struct VulkanFragmentInput {
    const VulkanImageStorage *image = nullptr;
    uint32_t wrap = 1;
    uint32_t filter = 0;
};
class VulkanFragmentProgram
{
public:
    VulkanFragmentProgram(std::shared_ptr<VulkanImageContext> context, const std::string& source, uint32_t uniformBytes);
    VulkanFragmentProgram(std::shared_ptr<VulkanImageContext> context, const std::string& source, uint32_t uniformBytes,
                          const NatronVulkanShaderLayout *layout);
    ~VulkanFragmentProgram();
    const std::vector<uint32_t> &activeUniformOffsets() const;
    uint32_t activeSamplers() const;
    VulkanFragmentProgram(const VulkanFragmentProgram &) = delete;
    VulkanFragmentProgram &operator=(const VulkanFragmentProgram &) = delete;
    void render(VulkanImageStorage& destination, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                const std::array<VulkanFragmentInput, 4> &inputs, const void *uniforms, uint32_t uniformBytes);
private:
    struct Impl;
    std::unique_ptr<Impl> _imp;
};
NATRON_NAMESPACE_EXIT
#endif
