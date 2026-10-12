// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_VULKANIMAGECONTEXTPOOL_H
#define NATRON_ENGINE_VULKANIMAGECONTEXTPOOL_H
#include <Python.h>
#include "Global/Macros.h"
#include <memory>
#include <string>
NATRON_NAMESPACE_ENTER
class VulkanImageContext;
// Share a logical device/runtime between Engine cache images and OFX contexts.
// The pool holds weak references; images and active renders own their lifetime.
// Selection/validation environment overrides match the public OFX suite.
// CPU-only mode never loads the Vulkan loader. Initialization failure is
// remembered for the process so CPU fallback does not repeatedly probe drivers.
bool vulkanImageRuntimeEnabled();
std::string vulkanImageRuntimeError();
void disableVulkanImageRuntime(const std::string& reason);
std::shared_ptr<VulkanImageContext> sharedVulkanImageContext(int deviceIndex = -1, bool validation = false);
NATRON_NAMESPACE_EXIT
#endif
