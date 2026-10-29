// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_VULKAN_IMAGE_SUITE_H
#define NATRON_OFX_VULKAN_IMAGE_SUITE_H
#include "ofxCore.h"
#include <stdint.h>
#define kNatronOfxVulkanImageSuite "NatronOfxVulkanImageSuite"
// Natron extension status: this logical device and all of its image/program
// handles are retired. Release them, create a new context, and rerender inputs.
// A new context never makes an old handle valid again. Cleanup remains allowed.
#define kNatronOfxStatErrDeviceLost 1001
#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t NatronVulkanContextHandle;
typedef uint64_t NatronVulkanImageHandle;
typedef struct NatronVulkanRegion {
    uint32_t x, y, width, height;
} NatronVulkanRegion;
typedef struct NatronVulkanDeviceInfo {
    char name[256];
    uint32_t vendorId, deviceId, apiVersion, validationEnabled, validationErrors;
} NatronVulkanDeviceInfo;
// Natron extension, not a standard OFX GPU render suite. Standard clip images
// remain CPU images. This suite owns separate, opaque RGBA32F Vulkan images.
// Operations complete before returning. Row zero is preserved, without a flip.
// All participating images use the same pixel grid; null ROI means the full
// image. Outside the ROI the destination is preserved (initially zero).
// Calls sharing a context are serialized; different contexts may run in parallel.
// Image handles belong to exactly one context and never become valid again after
// release. Releasing a context also releases its images. Plugins must finish
// their calls before releasing the context. No VkImage or GL texture is exposed.
typedef struct NatronOfxVulkanImageSuiteV1 {
    // -1 selects automatically; NATRON_VULKAN_DEVICE overrides automatic
    // selection. NATRON_VULKAN_VALIDATION=1 enables validation additionally.
    OfxStatus (*createContext)(int deviceIndex, int validation, NatronVulkanContextHandle* context);
    OfxStatus (*releaseContext)(NatronVulkanContextHandle context);
    OfxStatus (*getDeviceInfo)(NatronVulkanContextHandle context, NatronVulkanDeviceInfo* info);
    OfxStatus (*createImage)(NatronVulkanContextHandle context, uint32_t width, uint32_t height, NatronVulkanImageHandle* image);
    OfxStatus (*releaseImage)(NatronVulkanContextHandle context, NatronVulkanImageHandle image);
    // data points to the first logical row. Signed rowBytes permits bottom-up
    // buffers and padding; abs(rowBytes) must be at least width * 4 * sizeof(float).
    OfxStatus (*upload)(NatronVulkanContextHandle context, NatronVulkanImageHandle image, const void* data, int64_t rowBytes);
    OfxStatus (*download)(NatronVulkanContextHandle context, NatronVulkanImageHandle image, void* data, int64_t rowBytes);
    // Overlapping self-copies and aliased compute inputs/outputs are supported.
    OfxStatus (*copy)(NatronVulkanContextHandle context, NatronVulkanImageHandle source, NatronVulkanImageHandle destination,
                      const NatronVulkanRegion* sourceRegion, uint32_t destinationX, uint32_t destinationY);
    OfxStatus (*transform)(NatronVulkanContextHandle context, NatronVulkanImageHandle source, NatronVulkanImageHandle destination,
                           const NatronVulkanRegion* region, const float gain[4], const float offset[4]);
    // Original/mask handle 0 means absent. Mask is read from alpha, without
    // clamping. Absent masked input has weight zero (one when inverted).
    // output = processed * (mix * weight) + original * (1 - mix * weight).
    OfxStatus (*mix)(NatronVulkanContextHandle context, NatronVulkanImageHandle processed, NatronVulkanImageHandle original,
                     NatronVulkanImageHandle mask, NatronVulkanImageHandle destination, const NatronVulkanRegion* region,
                     float mix, int masked, int inverted);
    // Bits 0..3 restore RGBA from original, or zero when original is absent.
    OfxStatus (*restoreChannels)(NatronVulkanContextHandle context, NatronVulkanImageHandle processed,
                                 NatronVulkanImageHandle original, NatronVulkanImageHandle destination,
                                 const NatronVulkanRegion* region, uint32_t channels);
} NatronOfxVulkanImageSuiteV1;
#ifdef __cplusplus
}
#endif
#endif
