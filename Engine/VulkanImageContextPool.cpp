// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/VulkanImageContextPool.h"
#include "Engine/VulkanImageStorage.h"
#include "Gpu/VulkanDevice.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
NATRON_NAMESPACE_ENTER
namespace
{
std::atomic<bool> runtimeFailed{false};
std::mutex runtimeFailureMutex;
std::string runtimeFailure;
}
bool vulkanImageRuntimeEnabled()
{
    const char *disabled = std::getenv("NATRON_VULKAN_DISABLED");
    return !runtimeFailed.load() && !(disabled && std::strcmp(disabled, "1") == 0);
}
std::string vulkanImageRuntimeError()
{
    std::lock_guard<std::mutex> lock(runtimeFailureMutex);
    return runtimeFailure.empty() ? "Vulkan processing is disabled (NATRON_VULKAN_DISABLED=1)." : runtimeFailure;
}
void disableVulkanImageRuntime(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(runtimeFailureMutex);

    if (!runtimeFailed.load()) {
        runtimeFailure = reason;
        runtimeFailed.store(true);
    }
}
std::shared_ptr<VulkanImageContext> sharedVulkanImageContext(int index, bool validation)
{
    if (!vulkanImageRuntimeEnabled()) {
        throw std::runtime_error(vulkanImageRuntimeError());
    }

    if (index < -1) {
        throw std::invalid_argument("Invalid Vulkan device selection.");
    }

    if (index == -1) {
        if (const char * selected = std::getenv("NATRON_VULKAN_DEVICE")) {
            size_t consumed = 0;

            try {
                index = std::stoi(selected, &consumed);
            } catch (const std::exception &) {
                throw std::invalid_argument("Invalid Vulkan device selection.");
            }

            if (index < 0 || consumed != std::strlen(selected)) {
                throw std::invalid_argument("Invalid Vulkan device selection.");
            }
        }
    }

    const char *forced = std::getenv("NATRON_VULKAN_VALIDATION");
    const char *testValidation = std::getenv("NATRON_TEST_VULKAN_VALIDATION");
    const bool profileValidation = std::getenv("NATRON_TEST_PROFILE") &&
                                   !(testValidation && std::strcmp(testValidation, "0") == 0);
    // Test profiles validate by default. The explicit profile-only override
    // permits production-cost benchmarks; an explicit validation request wins.
    validation = validation || (forced && std::strcmp(forced, "1") == 0) || profileValidation;
    static std::mutex mutex;
    static std::map<bool, std::weak_ptr<Gpu::VulkanInstance>> instances;
    static std::map<std::pair<int, bool>, std::weak_ptr<VulkanImageContext>> contexts;
    std::lock_guard<std::mutex> lock(mutex);

    // A caller may have waited while another thread failed initialization.
    if (!vulkanImageRuntimeEnabled()) {
        throw std::runtime_error(vulkanImageRuntimeError());
    }

    try {
        auto instance = instances[validation].lock();

        if (!instance) {
            Gpu::InstanceOptions options;
            options.validation = validation;
            options.availableSurfaceExtensions = true;
            instance = Gpu::VulkanInstance::create(options);
            instances[validation] = instance;
        }

        // Auto and explicit selections of the same physical device must use one
        // logical device. Otherwise native clips cannot cross those contexts.
        index = Gpu::VulkanDevice::selectPhysicalDeviceIndex(instance, index);
        auto& entry = contexts[ {index, validation}];

        if (auto existing = entry.lock()) {
            if (!existing->device()->lost()) {
                return existing;
            }

            // Other callers may still own old clips/Viewer surfaces. Keep their
            // resources on the retired device; publish one new context below.
        }

        auto device = Gpu::VulkanDevice::create(instance, index);
        auto created = std::make_shared<VulkanImageContext>(std::move(device));
        entry = created;
        return created;
    } catch (const std::runtime_error &error) {
        // OFX suites can initialize before a Viewer or an Engine render. Every
        // entry point must remember the failure, not only those two callers.
        disableVulkanImageRuntime(error.what());
        throw;
    }
}
NATRON_NAMESPACE_EXIT
