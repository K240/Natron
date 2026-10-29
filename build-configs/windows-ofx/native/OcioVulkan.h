// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_OCIOVULKAN_H
#define NATRON_OFX_OCIOVULKAN_H
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include <OpenColorIO/OpenColorIO.h>
#include <memory>
#include <array>
// No OpenGL or compiler-specific C++ objects cross the host/plugin boundary.
class OcioVulkanProcessor
{
public:
    OcioVulkanProcessor(const NatronOfxVulkanImageSuiteV1 *images, const NatronOfxVulkanShaderSuiteV2 *shaders);
    ~OcioVulkanProcessor();
    OcioVulkanProcessor(const OcioVulkanProcessor &) = delete;
    OcioVulkanProcessor &operator=(const OcioVulkanProcessor &) = delete;
    void apply(const OCIO_NAMESPACE::ConstProcessorRcPtr &processor, float *pixels, uint32_t width, uint32_t height,
               uint32_t components, int64_t rowBytes);
    // Optional acceleration of CPU pixels. A failure leaves the entire caller
    // buffer untouched, so its OCIO CPU processor may process it exactly once.
    bool tryApply(const OCIO_NAMESPACE::ConstProcessorRcPtr &processor, float *pixels, uint32_t width, uint32_t height,
                  uint32_t components, int64_t rowBytes, NatronVulkanDeviceInfo *info = nullptr) noexcept;
    bool applyClip(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor, const NatronOfxVulkanRenderSuiteV1& render,
                   OfxPropertySetHandle source, OfxPropertySetHandle destination, const OfxRectI& window,
                   bool premult, float mix, bool masked, bool inverted, const float *maskRGBA);
    // Decode is the CPU boundary. Crop before upload; convert, mipmap and
    // premultiply in that order, preserving the global pixel grid.
    void uploadReader(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor, const NatronOfxVulkanRenderSuiteV1& render,
                      const float *pixels, const OfxRectI& bounds, int64_t rowBytes, const OfxRectI& crop,
                      OfxPropertySetHandle destination, const OfxRectI& window, unsigned levels,
                      bool unpremult, bool premult);
    void clearClip(const NatronOfxVulkanRenderSuiteV1& render, OfxPropertySetHandle destination, const OfxRectI& window);
    // Borrow the original input, process a GPU temporary, then download once
    // into the encoder's CPU buffer. Downstream output remains the original.
    void downloadWriter(const OCIO_NAMESPACE::ConstProcessorRcPtr& processor, const NatronOfxVulkanRenderSuiteV1& render,
                        OfxPropertySetHandle source, const OfxRectI& window, float *pixels, int64_t rowBytes,
                        bool opaque, bool unpremult, bool premult);
    void copyClip(const NatronOfxVulkanRenderSuiteV1& render, OfxPropertySetHandle source,
                  OfxPropertySetHandle destination, const OfxRectI& window);
    NatronVulkanDeviceInfo deviceInfo() const;
    // Descriptor counts: 1D, 2D, 3D, uniforms, uniform type mask.
    std::array<uint32_t, 5> resourceInfo() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _imp;
};
#endif
