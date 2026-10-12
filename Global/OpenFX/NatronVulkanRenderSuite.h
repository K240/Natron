// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_VULKAN_RENDER_SUITE_H
#define NATRON_OFX_VULKAN_RENDER_SUITE_H
#include "NatronVulkanImageSuite.h"
#define kNatronOfxVulkanRenderSuite "NatronOfxVulkanRenderSuite"
#define kNatronOfxImageEffectPropVulkanRenderSupported "NatronOfxImageEffectPropVulkanRenderSupported"
// Descriptor/instance integer (0 or 1). Required effects have no CPU render implementation.
#define kNatronOfxImageEffectPropVulkanRenderRequired "NatronOfxImageEffectPropVulkanRenderRequired"
#define kNatronOfxImageEffectPropVulkanRenderingEnabled "NatronOfxImageEffectPropVulkanRenderingEnabled"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct NatronVulkanImageView {
    NatronVulkanImageHandle image;
    // Global pixel coordinates: allocation bounds and exposed clip bounds.
    // Image/Shader Suite ROIs are relative to bounds[0..1].
    int32_t bounds[4], visibleBounds[4];
    uint32_t writable;
} NatronVulkanImageView;
// Native render clips have null CPU data and zero rowBytes. A plugin acquires
// their opaque image in its existing Image Suite context, which must share the
// Engine runtime/device. Source views are read-only, cropped with GPU copies.
// Output writes are confined to the exposed clip ROI. Handles expire when the
// standard OFX clip image is released; releaseImage cannot release a loan.
// All suite calls using a clip must finish before clipReleaseImage/context
// release. Retained shader programs and owned temporary images keep their usual
// lifetime. Clip handles and image handles are never interchangeable.
typedef struct NatronOfxVulkanRenderSuiteV1 {
    OfxStatus (*acquireImage)(NatronVulkanContextHandle context, OfxPropertySetHandle clipImage, NatronVulkanImageView* view);
    // Successful CPU pixel transfers for the shared runtime, across contexts.
    OfxStatus (*getTransferCounts)(NatronVulkanContextHandle context, uint64_t* uploads, uint64_t* downloads);
} NatronOfxVulkanRenderSuiteV1;
#ifdef __cplusplus
}
#endif
#endif
