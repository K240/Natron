// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_QT_VULKANCANVAS_H
#define NATRON_GPU_QT_VULKANCANVAS_H
#include "Gpu/VulkanDevice.h"
#include <QVulkanInstance>
#include <QWindow>
#include <QImage>

namespace Natron
{
namespace Gpu
{
// Qt adopts the engine's instance; it does not create or own a separate device.
class QtVulkanInstance
{
public:
    static std::shared_ptr<QtVulkanInstance> create(bool validation);
    static std::shared_ptr<QtVulkanInstance> adopt(std::shared_ptr<VulkanInstance> instance);
    const std::shared_ptr<VulkanInstance> &native() const
    {
        return _native;
    }
    QVulkanInstance *qt() const
    {
        return _qt.get();
    }
private:
    std::shared_ptr<VulkanInstance> _native;
    std::unique_ptr<QVulkanInstance> _qt;
};

// Native Vulkan surface suitable for QWidget::createWindowContainer(). A frame
// renderer receives a device-owned RGBA32F target at the physical window size.
class VulkanCanvas : public QWindow
{
public:
    VulkanCanvas(std::shared_ptr<QtVulkanInstance> instance, int physicalDeviceIndex = -1,
                 std::shared_ptr<VulkanDevice> sharedDevice = {});
    ~VulkanCanvas() override;
    const std::shared_ptr<VulkanDevice> &device() const
    {
        return _device;
    }
    void setImage(std::shared_ptr<VulkanImage> image);
    using FrameRenderer = std::function<void(VulkanImage &)>;
    void setFrameRenderer(FrameRenderer renderer);
    // Runs after a successful present submission and queue wait, without an
    // implicit readback. This is not a physical scanout/remote-display fence.
    using PresentedHandler = std::function<void(uint64_t)>;
    void setPresentedHandler(PresentedHandler handler)
    {
        _presentedHandler = std::move(handler);
    }
    QSize swapchainExtent() const
    {
        return QSize(int(_extent.width), int(_extent.height));
    }
    bool captureEnabled() const
    {
        return _captureEnabled;
    }
    // A handler may queue a display fallback. It must not delete this window
    // synchronously from its own render/event stack.
    using FailureHandler = std::function<bool(const VulkanError &)>;
    void setFailureHandler(FailureHandler handler)
    {
        _failureHandler = std::move(handler);
    }
    void setCaptureEnabled(bool enabled);
    // Explicit readback only; regular production frames stay on the GPU.
    QImage grabFrame();
    const QImage &frameImage() const
    {
        return _lastFrame;
    }
    uint64_t presentedFrames() const
    {
        return _presentedFrames;
    }
    uint64_t swapchainGeneration() const
    {
        return _swapchainGeneration;
    }
    const std::string &error() const
    {
        return _error;
    }
protected:
    bool event(QEvent* event) override;
    void exposeEvent(QExposeEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
private:
    void render();
    void createSwapchain();
    void releaseSwapchain() noexcept;
    void imageBarrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout,
                      VkImageLayout newLayout);
    std::shared_ptr<QtVulkanInstance> _instance;
    std::shared_ptr<VulkanDevice> _device;
    std::shared_ptr<VulkanImage> _image;
    FrameRenderer _frameRenderer;
    PresentedHandler _presentedHandler;
    FailureHandler _failureHandler;
    std::shared_ptr<VulkanImage> _renderedImage;
    std::shared_ptr<VulkanImage> _captureImage;
    QImage _lastFrame;
    bool _captureEnabled = false;
    VkSurfaceKHR _surface = VK_NULL_HANDLE;
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkSemaphore _acquire = VK_NULL_HANDLE;
    VkExtent2D _extent{};
    std::vector<VkImage> _images;
    std::vector<VkSemaphore> _ready;
    std::vector<bool> _presented;
    bool _resizePending = true;
    uint64_t _presentedFrames = 0;
    uint64_t _swapchainGeneration = 0;
    std::string _error;
};
} // namespace Gpu
} // namespace Natron
#endif
