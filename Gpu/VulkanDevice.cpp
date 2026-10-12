// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/VulkanDevice.h"
#include "Gpu/PerformanceTrace.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include "TransformShader.h"

namespace Natron
{
namespace Gpu
{
namespace
{
std::mutex loaderMutex;
bool loaderInitialized = false;
std::atomic<uint64_t> nextDeviceGeneration{1};
bool hasExtension(const std::vector<VkExtensionProperties> &extensions, const char* name)
{
    return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties & entry) {
        return std::strcmp(entry.extensionName, name) == 0;
    });
}
struct StagingBuffer {
    std::shared_ptr<VulkanDevice> device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void *mapped = nullptr;
    StagingBuffer(std::shared_ptr<VulkanDevice> device_, size_t size) : device(std::move(device_))
    {
        PerformanceTrace trace("staging-allocate", size);
        device->requireUsable();
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo allocationInfo{};
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        VmaAllocationInfo result{};
        device->check(vmaCreateBuffer(device->allocator(), &info, &allocationInfo, &buffer, &allocation, &result), "allocate staging buffer");
        mapped = result.pMappedData;
    }
    ~StagingBuffer()
    {
        if (buffer) {
            vmaDestroyBuffer(device->allocator(), buffer, allocation);
        }
    }
};
}

VulkanError::VulkanError(VkResult result, const char *operation)
    : std::runtime_error(std::string(operation) + " failed with VkResult " + std::to_string(result)), _result(result) {}
bool VulkanError::deviceMemoryFailure() const noexcept
{
    return _result == VK_ERROR_OUT_OF_DEVICE_MEMORY || _result == VK_ERROR_OUT_OF_POOL_MEMORY ||
           _result == VK_ERROR_FRAGMENTED_POOL || _result == VK_ERROR_FRAGMENTATION;
}
bool VulkanError::outOfMemory() const noexcept
{
    return deviceMemoryFailure() || _result == VK_ERROR_OUT_OF_HOST_MEMORY;
}
bool VulkanError::deviceLost() const noexcept
{
    return _result == VK_ERROR_DEVICE_LOST;
}
void VulkanDevice::requireUsable() const
{
    if (lost()) {
        throw VulkanError(VK_ERROR_DEVICE_LOST, "use retired Vulkan device");
    }
}
void VulkanDevice::check(VkResult result, const char* operation) const
{
    if (result == VK_ERROR_DEVICE_LOST) {
        _lost.store(true);
    }

    checkResult(result, operation);
}
void checkResult(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        throw VulkanError(result, operation);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanInstance::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* message, void *userData)
{
    const bool validation = types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    if (validation && error) {
        ++static_cast<VulkanInstance *>(userData)->_validationErrors;
    }

    std::cerr << (validation ? "Vulkan validation " : "Vulkan runtime ")
              << (error ? "error: " : "warning: ") << message->pMessage << std::endl;
    return VK_FALSE;
}

std::shared_ptr<VulkanInstance> VulkanInstance::create(const InstanceOptions& options)
{
    std::lock_guard<std::mutex> lock(loaderMutex);

    if (!loaderInitialized) {
        checkResult(volk::volkInitialize(), "initialize Vulkan loader");
        loaderInitialized = true;
    }

    if (volk::volkGetInstanceVersion() < VK_API_VERSION_1_3) {
        throw std::runtime_error("Vulkan 1.3 or newer is required.");
    }

    auto result = std::shared_ptr<VulkanInstance>(new VulkanInstance);
    uint32_t count = 0;
    checkResult(volk::vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "enumerate instance extensions");
    std::vector<VkExtensionProperties> available(count);
    checkResult(volk::vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()), "enumerate instance extensions");
    std::vector<std::string> requested = options.extensions;

    if (hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        requested.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }

    if (options.availableSurfaceExtensions && hasExtension(available, VK_KHR_SURFACE_EXTENSION_NAME)) {
        requested.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);

        for (const char * name : {
                 "VK_KHR_win32_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
                 "VK_KHR_wayland_surface", "VK_EXT_metal_surface"
             }) {
            if (hasExtension(available, name)) {
                requested.emplace_back(name);
            }
        }
    }

    std::vector<const char *> layers;

    if (options.validation) {
        uint32_t layerCount = 0;
        checkResult(volk::vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "enumerate validation layers");
        std::vector<VkLayerProperties> entries(layerCount);
        checkResult(volk::vkEnumerateInstanceLayerProperties(&layerCount, entries.data()), "enumerate validation layers");

        if (!std::any_of(entries.begin(), entries.end(), [](const VkLayerProperties & entry) {
        return std::strcmp(entry.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        })) {
            throw std::runtime_error("The requested Vulkan validation layer is unavailable.");
        }
        layers.push_back("VK_LAYER_KHRONOS_validation");
        requested.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        uint32_t extensionCount = 0;
        checkResult(volk::vkEnumerateInstanceExtensionProperties(layers.front(), &extensionCount, nullptr), "enumerate validation extensions");
        std::vector<VkExtensionProperties> validationExtensions(extensionCount);
        checkResult(volk::vkEnumerateInstanceExtensionProperties(layers.front(), &extensionCount, validationExtensions.data()), "enumerate validation extensions");
        available.insert(available.end(), validationExtensions.begin(), validationExtensions.end());
        requested.emplace_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        result->_validation = true;
    }

    std::sort(requested.begin(), requested.end());
    requested.erase(std::unique(requested.begin(), requested.end()), requested.end());
    std::vector<const char *> extensions;

    for (const auto& name : requested) {
        if (!hasExtension(available, name.c_str())) {
            throw std::runtime_error("Required instance extension is unavailable: " + name);
        }

        extensions.push_back(name.c_str());
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Natron";
    app.applicationVersion = VK_MAKE_VERSION(2, 6, 0);
    app.pEngineName = "Natron";
    app.engineVersion = app.applicationVersion;
    app.apiVersion = VK_API_VERSION_1_3;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = debugCallback;
    debug.pUserData = result.get();
    const VkValidationFeatureEnableEXT synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation.enabledValidationFeatureCount = 1;
    validation.pEnabledValidationFeatures = &synchronization;
    debug.pNext = options.validation ? &validation : nullptr;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.enabledLayerCount = static_cast<uint32_t>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.pNext = options.validation ? &debug : nullptr;

    if (hasExtension(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    checkResult(volk::vkCreateInstance(&info, nullptr, &result->_instance), "create Vulkan instance");
    result->_surface = std::find(requested.begin(), requested.end(), VK_KHR_SURFACE_EXTENSION_NAME) != requested.end();
    volk::volkLoadInstanceTable(&result->_functions, result->_instance);

    if (options.validation) {
        debug.pNext = nullptr;
        checkResult(result->_functions.vkCreateDebugUtilsMessengerEXT(result->_instance, &debug, nullptr, &result->_messenger), "create debug messenger");
    }

    return result;
}

VulkanInstance::~VulkanInstance()
{
    if (_messenger) {
        _functions.vkDestroyDebugUtilsMessengerEXT(_instance, _messenger, nullptr);
    }

    if (_instance) {
        _functions.vkDestroyInstance(_instance, nullptr);
    }
}

std::vector<VkPhysicalDevice> VulkanInstance::physicalDevices() const
{
    uint32_t count = 0;
    checkResult(_functions.vkEnumeratePhysicalDevices(_instance, &count, nullptr), "enumerate physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    checkResult(_functions.vkEnumeratePhysicalDevices(_instance, &count, devices.data()), "enumerate physical devices");
    return devices;
}

namespace
{
struct DeviceSelection {
    int index = -1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties properties{};
};
DeviceSelection selectDevice(const std::shared_ptr<VulkanInstance> &instance, int index, VkSurfaceKHR surface)
{
    if (!instance || index < -1) {
        throw std::invalid_argument("Invalid Vulkan instance or device index.");
    }

    if (surface && !instance->surfaceEnabled()) {
        throw std::invalid_argument("Vulkan instance has no surface support.");
    }

    DeviceSelection result;
    const auto& fn = instance->functions();
    const auto devices = instance->physicalDevices();

    if (devices.empty()) {
        throw std::runtime_error("No Vulkan physical device was found.");
    }

    std::vector<size_t> candidates;

    if (index >= 0) {
        if (static_cast<size_t>(index) >= devices.size()) {
            throw std::invalid_argument("The selected Vulkan device does not exist.");
        }

        candidates.push_back(static_cast<size_t>(index));
    } else {
        for (size_t i = 0; i < devices.size(); ++i) {
            candidates.push_back(i);
        }

        std::stable_sort(candidates.begin(), candidates.end(), [&](size_t a, size_t b) {
            VkPhysicalDeviceProperties pa{}, pb{};
            fn.vkGetPhysicalDeviceProperties(devices[a], &pa);
            fn.vkGetPhysicalDeviceProperties(devices[b], &pb);
            return (pa.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) > (pb.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        });
    }

    for (size_t candidate : candidates) {
        VkPhysicalDeviceProperties properties{};
        fn.vkGetPhysicalDeviceProperties(devices[candidate], &properties);
        VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.pNext = &v13;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &v12;
        fn.vkGetPhysicalDeviceFeatures2(devices[candidate], &features);

        if (properties.apiVersion < VK_API_VERSION_1_3 || !v12.timelineSemaphore || !v13.synchronization2 || !v13.dynamicRendering) {
            continue;
        }

        uint32_t queueCount = 0;
        fn.vkGetPhysicalDeviceQueueFamilyProperties(devices[candidate], &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        fn.vkGetPhysicalDeviceQueueFamilyProperties(devices[candidate], &queueCount, queues.data());

        for (uint32_t i = 0; i < queueCount; ++i) {
            VkBool32 present = VK_TRUE;

            if (surface) {
                checkResult(fn.vkGetPhysicalDeviceSurfaceSupportKHR(devices[candidate], i, surface, &present), "query present queue");
            }

            const auto flags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;

            if (queues[i].queueCount && (queues[i].queueFlags & flags) == flags && present) {
                result.index = int(candidate);
                result.device = devices[candidate];
                result.queueFamily = i;
                result.properties = properties;
                break;
            }
        }

        if (result.device) {
            break;
        }
    }

    if (!result.device) {
        throw std::runtime_error("No selected device supports Vulkan 1.3, the required features, and presentation.");
    }

    return result;
}
}
int VulkanDevice::selectPhysicalDeviceIndex(const std::shared_ptr<VulkanInstance> &instance, int index, VkSurfaceKHR surface)
{
    return selectDevice(instance, index, surface).index;
}
std::shared_ptr<VulkanDevice> VulkanDevice::create(std::shared_ptr<VulkanInstance> instance, int index, VkSurfaceKHR surface, uint64_t memoryLimitBytes)
{
    if (!memoryLimitBytes) {
        if (const char *setting = std::getenv("NATRON_VULKAN_MEMORY_LIMIT_MB")) {
            const std::string value(setting);

            if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) {
                throw std::runtime_error("NATRON_VULKAN_MEMORY_LIMIT_MB must be a nonnegative integer.");
            }
            uint64_t megabytes = 0;
            const auto maximum = std::numeric_limits<uint64_t>::max() / (1024 * 1024);

            for (char digit : value) {
                const auto next = uint64_t(digit - '0');

                if (megabytes > (maximum - next) / 10) {
                    throw std::runtime_error("NATRON_VULKAN_MEMORY_LIMIT_MB is too large.");
                }

                megabytes = megabytes * 10 + next;
            }

            memoryLimitBytes = megabytes * (1024 * 1024);
        }
    }

    const auto selected = selectDevice(instance, index, surface);
    auto result = std::shared_ptr<VulkanDevice>(new VulkanDevice);
    result->_instance = std::move(instance);
    result->_physicalDevice = selected.device;
    result->_queueFamily = selected.queueFamily;
    result->_properties = selected.properties;
    const auto& fn = result->_instance->functions();
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = result->_queueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.synchronization2 = VK_TRUE;
    v13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    v12.pNext = &v13;
    v12.timelineSemaphore = VK_TRUE;
    uint32_t count = 0;
    checkResult(fn.vkEnumerateDeviceExtensionProperties(result->_physicalDevice, nullptr, &count, nullptr), "enumerate device extensions");
    std::vector<VkExtensionProperties> available(count);
    checkResult(fn.vkEnumerateDeviceExtensionProperties(result->_physicalDevice, nullptr, &count, available.data()), "enumerate device extensions");
    std::vector<const char *> extensions;

    result->_memoryBudgetEnabled = hasExtension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

    if (result->_memoryBudgetEnabled) {
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }

    const bool swapchain = result->_instance->surfaceEnabled() && hasExtension(available, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    if (surface && !swapchain) {
        throw std::runtime_error("Vulkan swapchain support is unavailable.");
    }

    if (swapchain) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }

    if (hasExtension(available, "VK_KHR_portability_subset")) {
        extensions.push_back("VK_KHR_portability_subset");
    }

    VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures supported{}, enabled{};
    fn.vkGetPhysicalDeviceFeatures(result->_physicalDevice, &supported);
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    result->_anisotropy = enabled.samplerAnisotropy != VK_FALSE;
    device.pEnabledFeatures = &enabled;
    device.pNext = &v12;
    device.queueCreateInfoCount = 1;
    device.pQueueCreateInfos = &queue;
    device.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    device.ppEnabledExtensionNames = extensions.data();
    checkResult(fn.vkCreateDevice(result->_physicalDevice, &device, nullptr, &result->_device), "create Vulkan device");
    result->_generation = nextDeviceGeneration.fetch_add(1);

    if (!result->_generation) {
        std::terminate();
    }

    result->_presentationEnabled = swapchain;
    {
        std::lock_guard<std::mutex> lock(loaderMutex);
        // Volk's table loader uses a global instance-specific resolver. Another
        // headless/presentation instance may have replaced it since creation.
        volk::vkGetDeviceProcAddr = fn.vkGetDeviceProcAddr;
        volk::volkLoadDeviceTable(&result->_functions, result->_device);
    }
    result->_functions.vkGetDeviceQueue(result->_device, result->_queueFamily, 0, &result->_queue);
    VmaVulkanFunctions allocatorFunctions{};
    allocatorFunctions.vkGetInstanceProcAddr = volk::vkGetInstanceProcAddr;
    allocatorFunctions.vkGetDeviceProcAddr = fn.vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo allocator{};
    allocator.instance = result->_instance->handle();
    allocator.physicalDevice = result->_physicalDevice;
    allocator.device = result->_device;
    allocator.vulkanApiVersion = VK_API_VERSION_1_3;
    allocator.pVulkanFunctions = &allocatorFunctions;

    if (result->_memoryBudgetEnabled) {
        allocator.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }

    result->_memoryLimitBytes = memoryLimitBytes;
    fn.vkGetPhysicalDeviceMemoryProperties(result->_physicalDevice, &result->_memoryProperties);
    result->_heapLimits.fill(VK_WHOLE_SIZE);

    if (memoryLimitBytes) {
        uint32_t localHeaps = 0;

        for (uint32_t i = 0; i < result->_memoryProperties.memoryHeapCount; ++i) {
            localHeaps += (result->_memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        }

        uint64_t remaining = memoryLimitBytes;

        for (uint32_t i = 0; i < result->_memoryProperties.memoryHeapCount; ++i) {
            const auto& heap = result->_memoryProperties.memoryHeaps[i];

            if (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                result->_heapLimits[i] = std::min<uint64_t>(heap.size, remaining / localHeaps);
                remaining -= result->_heapLimits[i];
                --localHeaps;
            }
        }

        allocator.pHeapSizeLimit = result->_heapLimits.data();
    }

    const VmaDeviceMemoryCallbacks memoryCallbacks{memoryAllocated, memoryFreed, result.get()};
    allocator.pDeviceMemoryCallbacks = &memoryCallbacks;
    result->check(vmaCreateAllocator(&allocator, &result->_allocator), "create Vulkan allocator");
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = result->_queueFamily;
    result->check(result->_functions.vkCreateCommandPool(result->_device, &pool, nullptr, &result->_commandPool), "create command pool");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore.pNext = &type;
    result->check(result->_functions.vkCreateSemaphore(result->_device, &semaphore, nullptr, &result->_timeline), "create timeline semaphore");
    return result;
}

void VulkanDevice::memoryAllocated(VmaAllocator_T *, uint32_t type, VkDeviceMemory, VkDeviceSize size, void *data)
{
    auto& device = *static_cast<VulkanDevice *>(data);
    const auto heap = device._memoryProperties.memoryTypes[type].heapIndex;

    if (device._memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        const uint64_t current = device._localBlockBytes.fetch_add(size) + size;
        uint64_t peak = device._localPeakBlockBytes.load();

        while (peak < current && !device._localPeakBlockBytes.compare_exchange_weak(peak, current)) {}
    }
}
void VulkanDevice::memoryFreed(VmaAllocator_T *, uint32_t type, VkDeviceMemory, VkDeviceSize size, void *data)
{
    auto& device = *static_cast<VulkanDevice *>(data);
    const auto heap = device._memoryProperties.memoryTypes[type].heapIndex;

    if (device._memoryProperties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
        device._localBlockBytes.fetch_sub(size);
    }
}
ImageTransferSnapshot VulkanDevice::imageTransfers() const
{
    return {_imageUploads.load(), _imageDownloads.load(), _imageUploadBytes.load(), _imageDownloadBytes.load()};
}
MemoryBudgetSnapshot VulkanDevice::memoryBudget() const
{
    vmaSetCurrentFrameIndex(_allocator, _memoryFrame.fetch_add(1));
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] {};
    vmaGetHeapBudgets(_allocator, budgets);
    MemoryBudgetSnapshot result;
    result.driverBudget = _memoryBudgetEnabled;
    result.limitBytes = _memoryLimitBytes;
    result.localBlockBytes = _localBlockBytes.load();
    result.localPeakBlockBytes = _localPeakBlockBytes.load();

    for (uint32_t i = 0; i < _memoryProperties.memoryHeapCount; ++i) {
        const auto& budget = budgets[i];
        result.heaps.push_back({(_memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0,
                                budget.statistics.blockBytes, budget.statistics.allocationBytes,
                                budget.usage, budget.budget, _heapLimits[i]});
    }

    return result;
}

VulkanDevice::~VulkanDevice()
{
    if (_device) {
        _functions.vkDeviceWaitIdle(_device);

        if (_timeline) {
            _functions.vkDestroySemaphore(_device, _timeline, nullptr);
        }

        if (_commandPool) {
            _functions.vkDestroyCommandPool(_device, _commandPool, nullptr);
        }

        if (_allocator) {
            vmaDestroyAllocator(_allocator);
        }

        _functions.vkDestroyDevice(_device, nullptr);
    }
}

bool VulkanDevice::supportsPresentation(VkSurfaceKHR surface) const
{
    requireUsable();

    if (!_presentationEnabled || !surface || !_instance->functions().vkGetPhysicalDeviceSurfaceSupportKHR) {
        return false;
    }

    VkBool32 supported = VK_FALSE;
    check(_instance->functions().vkGetPhysicalDeviceSurfaceSupportKHR(_physicalDevice, _queueFamily, surface, &supported), "query present queue");
    return supported == VK_TRUE;
}

void VulkanDevice::execute(const std::function<void(VkCommandBuffer)> &record, VkSemaphore wait, VkSemaphore signal)
{
    PerformanceTrace trace("submit-wait");
    std::lock_guard<std::mutex> lock(_queueMutex);
    requireUsable();
    vmaSetCurrentFrameIndex(_allocator, _memoryFrame.fetch_add(1));
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = _commandPool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    check(_functions.vkAllocateCommandBuffers(_device, &allocate, &command), "allocate command buffer");
    bool submitted = false;

    try {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(_functions.vkBeginCommandBuffer(command, &begin), "begin command buffer");
        record(command);
        check(_functions.vkEndCommandBuffer(command), "end command buffer");
        VkCommandBufferSubmitInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        commands.commandBuffer = command;
        VkSemaphoreSubmitInfo waiting{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waiting.semaphore = wait;
        waiting.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        std::array<VkSemaphoreSubmitInfo, 2> signals{};
        signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[0].semaphore = _timeline;
        signals[0].value = ++_submissionValue;
        signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[1].semaphore = signal;
        signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        info.waitSemaphoreInfoCount = wait ? 1 : 0;
        info.pWaitSemaphoreInfos = &waiting;
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &commands;
        info.signalSemaphoreInfoCount = signal ? 2 : 1;
        info.pSignalSemaphoreInfos = signals.data();
        // Another context can discover loss while this callback records.
        // Never submit its commands after that retirement was observed.
        requireUsable();
        uint32_t lossPoint = 1;
        const bool testLoss = _testLossPoint.compare_exchange_strong(lossPoint, 0);
        check(testLoss ? VK_ERROR_DEVICE_LOST : _functions.vkQueueSubmit2(_queue, 1, &info, VK_NULL_HANDLE), "submit Vulkan commands");
        submitted = true;
        VkSemaphoreWaitInfo cpuWait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        cpuWait.semaphoreCount = 1;
        cpuWait.pSemaphores = &_timeline;
        cpuWait.pValues = &_submissionValue;
        // Synchronous transfers retain all resources until completion. The process
        // watchdog bounds smoke tests; production scheduling will be asynchronous.
        const auto waited = _functions.vkWaitSemaphores(_device, &cpuWait, UINT64_MAX);
        lossPoint = 2;
        const bool testWaitLoss = waited == VK_SUCCESS && _testLossPoint.compare_exchange_strong(lossPoint, 0);
        check(testWaitLoss ? VK_ERROR_DEVICE_LOST : waited, "wait for Vulkan commands");
        requireUsable();
    } catch (...) {
        if (submitted) {
            if (_functions.vkDeviceWaitIdle(_device) == VK_ERROR_DEVICE_LOST) {
                _lost.store(true);
            }
        }

        _functions.vkFreeCommandBuffers(_device, _commandPool, 1, &command);
        throw;
    }

    _functions.vkFreeCommandBuffers(_device, _commandPool, 1, &command);
}

VkResult VulkanDevice::present(const VkPresentInfoKHR& info)
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    requireUsable();
    uint32_t lossPoint = 3;
    const bool testLoss = _testLossPoint.compare_exchange_strong(lossPoint, 0);
    const auto status = testLoss ? VK_ERROR_DEVICE_LOST : _functions.vkQueuePresentKHR(_queue, &info);

    if (status != VK_SUBOPTIMAL_KHR && status != VK_ERROR_OUT_OF_DATE_KHR) {
        check(status, "present Vulkan frame");
    }

    check(_functions.vkQueueWaitIdle(_queue), "wait for presentation");
    requireUsable();
    return status;
}

void VulkanDevice::waitIdle()
{
    std::lock_guard<std::mutex> lock(_queueMutex);
    requireUsable();
    check(_functions.vkDeviceWaitIdle(_device), "wait for Vulkan device");
}

VulkanImage::VulkanImage(std::shared_ptr<VulkanDevice> device, uint32_t width, uint32_t height, VkFormat format)
    : _device(std::move(device)), _format(format), _width(width), _height(height)
{
    if (!_device) {
        throw std::invalid_argument("A Vulkan image requires a device.");
    }

    _device->requireUsable();

    if (!width || !height || width > _device->properties().limits.maxImageDimension2D || height > _device->properties().limits.maxImageDimension2D) {
        throw std::invalid_argument("Invalid Vulkan image dimensions.");
    }

    byteSize();
    VkFormatProperties properties{};
    _device->instance()->functions().vkGetPhysicalDeviceFormatProperties(_device->physicalDevice(), format, &properties);
    const VkFormatFeatureFlags required = (format == VK_FORMAT_R32G32B32A32_SFLOAT ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0) |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

    if ((properties.optimalTilingFeatures & required) != required) {
        throw std::runtime_error("Unsupported Vulkan image format.");
    }

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    if (format == VK_FORMAT_R32G32B32A32_SFLOAT) {
        info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    allocation.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    allocation.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    _device->check(vmaCreateImage(_device->allocator(), &info, &allocation, &_image, &_allocation, nullptr), "allocate Vulkan image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = _image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    try {
        _device->check(_device->functions().vkCreateImageView(_device->handle(), &view, nullptr, &_view), "create image view");
    } catch (...) {
        vmaDestroyImage(_device->allocator(), _image, _allocation);
        throw;
    }
}

VulkanImage::~VulkanImage()
{
    _device->functions().vkDestroyImageView(_device->handle(), _view, nullptr);
    vmaDestroyImage(_device->allocator(), _image, _allocation);
}

size_t VulkanImage::byteSize() const
{
    size_t pixelSize = 0;

    switch (_format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        pixelSize = 4;
        break;

    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        pixelSize = 8;
        break;

    case VK_FORMAT_R32G32B32A32_SFLOAT:
        pixelSize = 16;
        break;

    default:
        throw std::invalid_argument("Unsupported Vulkan image format.");
    }

    if (_height == 0 || static_cast<size_t>(_width) > std::numeric_limits<size_t>::max() / _height / pixelSize) {
        throw std::overflow_error("Vulkan image is too large.");
    }

    return static_cast<size_t>(_width) * _height * pixelSize;
}

void VulkanImage::transition(VkCommandBuffer command, VkImageLayout layout)
{
    _device->requireUsable();
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = _layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = _layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_ACCESS_2_NONE : VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.oldLayout = _layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = _image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &barrier;
    _device->functions().vkCmdPipelineBarrier2(command, &info);
    _layout = layout;
}

void VulkanImage::upload(const void* bytes, size_t byteCount)
{
    uploadRegion(0, 0, _width, _height, bytes, byteCount);
}

void VulkanImage::uploadRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                               const void *bytes, size_t byteCount, uint32_t rowLength)
{
    _device->requireUsable();
    const size_t stride = rowLength ? rowLength : width;
    const size_t pixelSize = byteSize() / _width / _height;

    if (!bytes || !width || !height || x > _width || y > _height || width > _width - x || height > _height - y ||
        stride < width || stride > std::numeric_limits<size_t>::max() / height / pixelSize || byteCount != stride * height * pixelSize) {
        throw std::invalid_argument("Invalid Vulkan image upload region or buffer size.");
    }

    uploadRegionRows(x, y, width, height, bytes, byteCount, stride * pixelSize);
}

void VulkanImage::uploadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                   const void *bytes, size_t byteCount, size_t rowBytes)
{
    _device->requireUsable();
    const size_t pixelSize = byteSize() / _width / _height;
    const size_t packedRow = size_t(width) * pixelSize;

    if (!bytes || !width || !height || x > _width || y > _height || width > _width - x || height > _height - y ||
        rowBytes < packedRow || rowBytes > size_t(PTRDIFF_MAX) / height ||
        byteCount < size_t(height - 1) * rowBytes + packedRow) {
        throw std::invalid_argument("Invalid Vulkan upload row span.");
    }

    const size_t packedSize = packedRow * height;
    PerformanceTrace trace("image-upload", packedSize);
    StagingBuffer staging(_device, packedSize);

    if (rowBytes == packedRow) {
        std::memcpy(staging.mapped, bytes, packedSize);
    } else {
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(static_cast<unsigned char*>(staging.mapped) + size_t(row)*packedRow,
                        static_cast<const unsigned char *>(bytes) + size_t(row)*rowBytes, packedRow);
        }
    }

    _device->check(vmaFlushAllocation(_device->allocator(), staging.allocation, 0, VK_WHOLE_SIZE), "flush upload allocation");
    _device->execute([&](VkCommandBuffer command) {
        transition(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
        copy.imageExtent = {width, height, 1};
        _device->functions().vkCmdCopyBufferToImage(command, staging.buffer, _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    });
    _device->_imageUploadBytes.fetch_add(uint64_t(width) * height * pixelSize);
    _device->_imageUploads.fetch_add(1);
}

void VulkanImage::clear(const std::array<float, 4> &color)
{
    _device->execute([&](VkCommandBuffer command) {
        transition(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue value{};
        std::copy(color.begin(), color.end(), value.float32);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        _device->functions().vkCmdClearColorImage(command, _image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    });
}

std::vector<unsigned char> VulkanImage::download()
{
    std::vector<unsigned char> bytes(byteSize());
    downloadRegionRows(0, 0, _width, _height, bytes.data(), bytes.size(), bytes.size() / _height);
    return bytes;
}

void VulkanImage::downloadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                                     void *bytes, size_t byteCount, size_t rowBytes)
{
    _device->requireUsable();
    const size_t pixelSize = byteSize() / _width / _height;
    const size_t packedRow = size_t(width) * pixelSize;

    if (!bytes || !initialized() || !width || !height || x > _width || y > _height || width > _width - x || height > _height - y ||
        rowBytes < packedRow || rowBytes > size_t(PTRDIFF_MAX) / height ||
        byteCount < size_t(height - 1) * rowBytes + packedRow) {
        throw std::invalid_argument("Invalid Vulkan readback row span.");
    }

    const size_t packedSize = packedRow * height;
    PerformanceTrace trace("image-download", packedSize);
    StagingBuffer staging(_device, packedSize);
    _device->execute([&](VkCommandBuffer command) {
        transition(command, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageOffset = {static_cast<int32_t>(x), static_cast<int32_t>(y), 0};
        copy.imageExtent = {width, height, 1};
        _device->functions().vkCmdCopyImageToBuffer(command, _image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
        VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        info.memoryBarrierCount = 1;
        info.pMemoryBarriers = &barrier;
        _device->functions().vkCmdPipelineBarrier2(command, &info);
    });
    _device->check(vmaInvalidateAllocation(_device->allocator(), staging.allocation, 0, VK_WHOLE_SIZE), "invalidate readback allocation");
    PerformanceTrace copyTrace("readback-cpu-copy", packedSize);
    _device->requireUsable();

    if (rowBytes == packedRow) {
        std::memcpy(bytes, staging.mapped, packedSize);
    } else {
        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(static_cast<unsigned char*>(bytes) + size_t(row)*rowBytes,
                        static_cast<const unsigned char *>(staging.mapped) + size_t(row)*packedRow, packedRow);
        }
    }

    _device->requireUsable();
    _device->_imageDownloadBytes.fetch_add(packedSize);
    _device->_imageDownloads.fetch_add(1);
}

ImageTransform::ImageTransform(std::shared_ptr<VulkanDevice> device) : _device(std::move(device))
{
    if (!_device) {
        throw std::invalid_argument("An image transform requires a device.");
    }

    _device->requireUsable();

    const auto& fn = _device->functions();
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};

    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = static_cast<uint32_t>(bindings.size());
    layout.pBindings = bindings.data();
    _device->check(fn.vkCreateDescriptorSetLayout(_device->handle(), &layout, nullptr, &_descriptorLayout), "create descriptor layout");
    VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8 * sizeof(float)};
    VkPipelineLayoutCreateInfo pipelineLayout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayout.setLayoutCount = 1;
    pipelineLayout.pSetLayouts = &_descriptorLayout;
    pipelineLayout.pushConstantRangeCount = 1;
    pipelineLayout.pPushConstantRanges = &constants;
    VkShaderModule shader = VK_NULL_HANDLE;

    try {
        _device->check(fn.vkCreatePipelineLayout(_device->handle(), &pipelineLayout, nullptr, &_pipelineLayout), "create pipeline layout");
        VkShaderModuleCreateInfo shaderInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        shaderInfo.codeSize = sizeof(TransformShader);
        shaderInfo.pCode = reinterpret_cast<const uint32_t *>(TransformShader);
        _device->check(fn.vkCreateShaderModule(_device->handle(), &shaderInfo, nullptr, &shader), "create transform shader");
        VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline.stage.module = shader;
        pipeline.stage.pName = "main";
        pipeline.layout = _pipelineLayout;
        _device->check(fn.vkCreateComputePipelines(_device->handle(), VK_NULL_HANDLE, 1, &pipeline, nullptr, &_pipeline), "create transform pipeline");
        fn.vkDestroyShaderModule(_device->handle(), shader, nullptr);
        shader = VK_NULL_HANDLE;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &size;
        _device->check(fn.vkCreateDescriptorPool(_device->handle(), &pool, nullptr, &_descriptorPool), "create descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocate.descriptorPool = _descriptorPool;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &_descriptorLayout;
        _device->check(fn.vkAllocateDescriptorSets(_device->handle(), &allocate, &_descriptorSet), "allocate image descriptor set");
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

ImageTransform::~ImageTransform()
{
    const auto& fn = _device->functions();
    fn.vkDestroyDescriptorPool(_device->handle(), _descriptorPool, nullptr);
    fn.vkDestroyPipeline(_device->handle(), _pipeline, nullptr);
    fn.vkDestroyPipelineLayout(_device->handle(), _pipelineLayout, nullptr);
    fn.vkDestroyDescriptorSetLayout(_device->handle(), _descriptorLayout, nullptr);
}

void ImageTransform::apply(VulkanImage& source, VulkanImage& destination,
                           const std::array<float, 4> &gain, const std::array<float, 4> &offset)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _device->requireUsable();

    if (source.device() != _device || destination.device() != _device || source.width() != destination.width() ||
        source.height() != destination.height() || source.format() != VK_FORMAT_R32G32B32A32_SFLOAT ||
        destination.format() != VK_FORMAT_R32G32B32A32_SFLOAT || source.handle() == destination.handle()) {
        throw std::invalid_argument("Image transform requires distinct, same-sized RGBA32F images on the same device.");
    }

    const auto& fn = _device->functions();
    std::array<VkDescriptorImageInfo, 2> images{};
    images[0].imageView = source.view();
    images[1].imageView = destination.view();
    std::array<VkWriteDescriptorSet, 2> writes{};

    for (uint32_t i = 0; i < images.size(); ++i) {
        images[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = _descriptorSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[i].pImageInfo = &images[i];
    }

    fn.vkUpdateDescriptorSets(_device->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    struct Constants {
        std::array<float, 4> gain;
        std::array<float, 4> offset;
    } constants{gain, offset};
    _device->execute([&](VkCommandBuffer command) {
        source.transition(command, VK_IMAGE_LAYOUT_GENERAL);
        destination.transition(command, VK_IMAGE_LAYOUT_GENERAL);
        fn.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, _pipeline);
        fn.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, _pipelineLayout, 0, 1, &_descriptorSet, 0, nullptr);
        fn.vkCmdPushConstants(command, _pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        fn.vkCmdDispatch(command, (source.width() + 15) / 16, (source.height() + 15) / 16, 1);
    });
}
} // namespace Gpu
} // namespace Natron
