// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_VULKANDEVICE_H
#define NATRON_GPU_VULKANDEVICE_H
#include <volk.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <vector>
struct VmaAllocator_T;
struct VmaAllocation_T;
namespace Natron
{
namespace Gpu
{
void checkResult(VkResult result, const char* operation);
class VulkanError : public std::runtime_error
{
public:
    VulkanError(VkResult result, const char *operation);
    VkResult result() const noexcept
    {
        return _result;
    }
    bool deviceMemoryFailure() const noexcept;
    bool outOfMemory() const noexcept;
    bool deviceLost() const noexcept;
private:
    VkResult _result;
};
struct MemoryHeapBudget {
    bool deviceLocal = false;
    uint64_t blockBytes = 0, allocationBytes = 0, usageBytes = 0, budgetBytes = 0, limitBytes = 0;
};
struct MemoryBudgetSnapshot {
    bool driverBudget = false;
    uint64_t limitBytes = 0, localBlockBytes = 0, localPeakBlockBytes = 0;
    std::vector<MemoryHeapBudget> heaps;
};
// Completed image pixel copies across this logical device. Padding, uniforms,
// LUT buffers and GPU-to-GPU copies are excluded.
struct ImageTransferSnapshot {
    uint64_t uploads = 0, downloads = 0, uploadBytes = 0, downloadBytes = 0;
};
struct InstanceOptions {
    bool validation = false;
    std::vector<std::string> extensions;
    // Enable supported WSI extensions so the same device can later present.
    // Headless ICDs remain usable when they expose no surface extensions.
    bool availableSurfaceExtensions = false;
};
class VulkanInstance
{
public:
    static std::shared_ptr<VulkanInstance> create(const InstanceOptions& options = {});
    ~VulkanInstance();
    VulkanInstance(const VulkanInstance &) = delete;
    VulkanInstance &operator=(const VulkanInstance &) = delete;
    VkInstance handle() const
    {
        return _instance;
    }
    const volk::VolkInstanceTable &functions() const
    {
        return _functions;
    }
    std::vector<VkPhysicalDevice> physicalDevices() const;
    unsigned validationErrorCount() const
    {
        return _validationErrors.load();
    }
    bool validationEnabled() const
    {
        return _validation;
    }
    bool surfaceEnabled() const
    {
        return _surface;
    }
private:
    VulkanInstance() = default;
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT,
            VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *, void *);
    VkInstance _instance = VK_NULL_HANDLE;
    volk::VolkInstanceTable _functions{};
    VkDebugUtilsMessengerEXT _messenger = VK_NULL_HANDLE;
    bool _validation = false;
    bool _surface = false;
    std::atomic<unsigned> _validationErrors{0};
};
class VulkanDevice
{
public:
    static int selectPhysicalDeviceIndex(const std::shared_ptr<VulkanInstance> &instance,
                                         int physicalDeviceIndex = -1, VkSurfaceKHR surface = VK_NULL_HANDLE);
    static std::shared_ptr<VulkanDevice> create(std::shared_ptr<VulkanInstance> instance,
            int physicalDeviceIndex = -1, VkSurfaceKHR surface = VK_NULL_HANDLE, uint64_t memoryLimitBytes = 0);
    ~VulkanDevice();
    VulkanDevice(const VulkanDevice &) = delete;
    VulkanDevice &operator=(const VulkanDevice &) = delete;
    VkDevice handle() const
    {
        return _device;
    }
    VkPhysicalDevice physicalDevice() const
    {
        return _physicalDevice;
    }
    const VkPhysicalDeviceProperties &properties() const
    {
        return _properties;
    }
    const volk::VolkDeviceTable &functions() const
    {
        return _functions;
    }
    const std::shared_ptr<VulkanInstance> &instance() const
    {
        return _instance;
    }
    uint32_t queueFamily() const
    {
        return _queueFamily;
    }
    bool supportsPresentation(VkSurfaceKHR surface) const;
    bool supportsAnisotropy() const
    {
        return _anisotropy;
    }
    void execute(const std::function<void(VkCommandBuffer)> &record,
                 VkSemaphore waitSemaphore = VK_NULL_HANDLE, VkSemaphore signalSemaphore = VK_NULL_HANDLE);
    VkResult present(const VkPresentInfoKHR& info);
    void waitIdle();
    // Lost allocations remain owned for orderly destruction, but can never be
    // submitted, downloaded or reused by a replacement logical device.
    bool lost() const noexcept
    {
        return _lost.load();
    }
    uint64_t generation() const noexcept
    {
        return _generation;
    }
    void requireUsable() const;
    void check(VkResult result, const char* operation) const;
    MemoryBudgetSnapshot memoryBudget() const;
    ImageTransferSnapshot imageTransfers() const;
    VmaAllocator_T *allocator() const
    {
        return _allocator;
    }
private:
    VulkanDevice() = default;
    static void memoryAllocated(VmaAllocator_T *, uint32_t, VkDeviceMemory, VkDeviceSize, void *);
    static void memoryFreed(VmaAllocator_T *, uint32_t, VkDeviceMemory, VkDeviceSize, void *);
    std::shared_ptr<VulkanInstance> _instance;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties _properties{};
    VkDevice _device = VK_NULL_HANDLE;
    bool _presentationEnabled = false;
    bool _anisotropy = false;
    volk::VolkDeviceTable _functions{};
    VkQueue _queue = VK_NULL_HANDLE;
    uint32_t _queueFamily = 0;
    VmaAllocator_T *_allocator = nullptr;
    VkPhysicalDeviceMemoryProperties _memoryProperties{};
    std::array<VkDeviceSize, VK_MAX_MEMORY_HEAPS> _heapLimits{};
    uint64_t _memoryLimitBytes = 0;
    bool _memoryBudgetEnabled = false;
    std::atomic<uint64_t> _localBlockBytes{0}, _localPeakBlockBytes{0};
    std::atomic<uint64_t> _imageUploads{0}, _imageDownloads{0}, _imageUploadBytes{0}, _imageDownloadBytes{0};
    mutable std::atomic<uint32_t> _memoryFrame{0};
    VkCommandPool _commandPool = VK_NULL_HANDLE;
    VkSemaphore _timeline = VK_NULL_HANDLE;
    uint64_t _submissionValue = 0;
    std::mutex _queueMutex;
    mutable std::atomic<bool> _lost{false};
    uint64_t _generation = 0;
    std::atomic<uint32_t> _testLossPoint{0};
    friend class DeviceFaultInjection;
    friend class VulkanImage;
};
class VulkanImage
{
public:
    VulkanImage(std::shared_ptr<VulkanDevice> device, uint32_t width, uint32_t height,
                VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT);
    ~VulkanImage();
    VulkanImage(const VulkanImage &) = delete;
    VulkanImage &operator=(const VulkanImage &) = delete;
    VkImage handle() const
    {
        return _image;
    }
    VkImageView view() const
    {
        return _view;
    }
    VkFormat format() const
    {
        return _format;
    }
    uint32_t width() const
    {
        return _width;
    }
    uint32_t height() const
    {
        return _height;
    }
    size_t byteSize() const;
    bool initialized() const
    {
        return _layout != VK_IMAGE_LAYOUT_UNDEFINED;
    }
    const std::shared_ptr<VulkanDevice> &device() const
    {
        return _device;
    }
    void upload(const void* bytes, size_t byteCount);
    // Row zero is copied unchanged; bottom-up Viewer tiles remain bottom-up.
    void uploadRegion(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      const void *bytes, size_t byteCount, uint32_t rowLength = 0);
    // Copy only active row bytes, including a last row with no trailing padding.
    void uploadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                          const void *bytes, size_t byteCount, size_t rowBytes);
    void downloadRegionRows(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                            void *bytes, size_t byteCount, size_t rowBytes);
    void clear(const std::array<float, 4> &color);
    std::vector<unsigned char> download();
    void transition(VkCommandBuffer command, VkImageLayout layout);
private:
    std::shared_ptr<VulkanDevice> _device;
    VkImage _image = VK_NULL_HANDLE;
    VkImageView _view = VK_NULL_HANDLE;
    VmaAllocation_T *_allocation = nullptr;
    VkFormat _format;
    uint32_t _width;
    uint32_t _height;
    VkImageLayout _layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
class ImageTransform
{
public:
    explicit ImageTransform(std::shared_ptr<VulkanDevice> device);
    ~ImageTransform();
    ImageTransform(const ImageTransform &) = delete;
    ImageTransform &operator=(const ImageTransform &) = delete;
    void apply(VulkanImage& source, VulkanImage& destination,
               const std::array<float, 4> &gain, const std::array<float, 4> &offset);
private:
    std::shared_ptr<VulkanDevice> _device;
    VkDescriptorSetLayout _descriptorLayout = VK_NULL_HANDLE;
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;
    VkPipeline _pipeline = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _descriptorSet = VK_NULL_HANDLE;
    std::mutex _mutex;
};
} // namespace Gpu
} // namespace Natron
#endif
