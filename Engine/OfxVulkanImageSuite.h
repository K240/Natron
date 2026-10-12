// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_OFXVULKANIMAGESUITE_H
#define NATRON_ENGINE_OFXVULKANIMAGESUITE_H
#include "Global/Macros.h"
#include "Global/OpenFX/NatronVulkanImageSuite.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "Global/OpenFX/NatronVulkanTestSuite.h"
#include "Engine/EngineFwd.h"
NATRON_NAMESPACE_ENTER
const NatronOfxVulkanImageSuiteV1 &ofxVulkanImageSuite();
const NatronOfxVulkanShaderSuiteV1 &ofxVulkanShaderSuite();
const NatronOfxVulkanShaderSuiteV2 &ofxVulkanShaderSuiteV2();
const NatronOfxVulkanRenderSuiteV1 &ofxVulkanRenderSuite();
const NatronOfxVulkanTestSuiteV1 &ofxVulkanTestSuite();
const NatronOfxVulkanTestSuiteV2 &ofxVulkanTestSuiteV2();
// Called after acquiring the native OFX wrapper's entry access guard.
uint64_t registerOfxVulkanClip(OfxPropertySetHandle properties, const ImagePtr& image,
                               const GenericAccessPtr& access, const RectI& bounds, const RectI& visible, bool writable);
void unregisterOfxVulkanClip(uint64_t registration);
NATRON_NAMESPACE_EXIT
#endif
