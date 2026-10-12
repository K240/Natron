// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/Qt/VulkanCanvas.h"
#include <QEvent>
#include <QGuiApplication>
#include <QPlatformSurfaceEvent>
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace Natron
{
namespace Gpu
{
std::shared_ptr<QtVulkanInstance> QtVulkanInstance::create(bool validation)
{
    auto result = std::make_shared<QtVulkanInstance>();
    result->_qt = std::make_unique<QVulkanInstance>();
    InstanceOptions options;
    options.validation = validation;
    options.extensions.emplace_back(VK_KHR_SURFACE_EXTENSION_NAME);

    // The active platform's surface extension is required, even if Qt's early
    // extension snapshot is incomplete while another GPU context starts.
    // VulkanInstance checks support before creating the instance.
    const auto platform = QGuiApplication::platformName();

    if (platform == QString::fromUtf8("windows")) {
        options.extensions.emplace_back("VK_KHR_win32_surface");
    } else if (platform == QString::fromUtf8("xcb")) {
        options.extensions.emplace_back("VK_KHR_xcb_surface");
    } else if (platform.startsWith(QString::fromUtf8("wayland"))) {
        options.extensions.emplace_back("VK_KHR_wayland_surface");
    } else if (platform == QString::fromUtf8("cocoa")) {
        options.extensions.emplace_back("VK_EXT_metal_surface");
    } else {
        const auto supported = result->_qt->supportedExtensions();

        for (const char *name : {
                 "VK_KHR_win32_surface", "VK_KHR_xcb_surface", "VK_KHR_xlib_surface",
                 "VK_KHR_wayland_surface", "VK_EXT_metal_surface"
             }) {
            if (supported.contains(QByteArray(name))) {
                options.extensions.emplace_back(name);
            }
        }
    }

    return adopt(VulkanInstance::create(options));
}
std::shared_ptr<QtVulkanInstance> QtVulkanInstance::adopt(std::shared_ptr<VulkanInstance> instance)
{
    if (!instance) {
        throw std::invalid_argument("Cannot adopt a null Vulkan instance.");
    }

    auto result = std::make_shared<QtVulkanInstance>();
    result->_qt = std::make_unique<QVulkanInstance>();
    result->_native = std::move(instance);
    result->_qt->setFlags(QVulkanInstance::NoDebugOutputRedirect);
    result->_qt->setVkInstance(result->_native->handle());

    if (!result->_qt->create()) {
        throw std::runtime_error("Qt could not adopt the Vulkan instance.");
    }

    return result;
}

VulkanCanvas::VulkanCanvas(std::shared_ptr<QtVulkanInstance> instance, int index,
                           std::shared_ptr<VulkanDevice> sharedDevice) : _instance(std::move(instance))
{
    if (!_instance) {
        throw std::invalid_argument("A Vulkan canvas requires a Qt instance.");
    }

    setSurfaceType(QSurface::VulkanSurface);
    setVulkanInstance(_instance->qt());

    try {
        create();
        _surface = QVulkanInstance::surfaceForWindow(this);

        if (!_surface) {
            throw std::runtime_error("Qt could not create the Vulkan window surface.");
        }

        _device = sharedDevice ? std::move(sharedDevice) : VulkanDevice::create(_instance->native(), index, _surface);

        if (_device->instance() != _instance->native() || !_device->supportsPresentation(_surface)) {
            throw std::runtime_error("The shared Vulkan device cannot present to this window.");
        }
    } catch (...) {
        // Member destructors precede QWindow's base destructor during failed
        // construction. Release its surface before the adapter can disappear.
        destroy();
        setVulkanInstance(nullptr);
        throw;
    }
}

VulkanCanvas::~VulkanCanvas()
{
    releaseSwapchain();
    // Destroy the Qt-owned native surface while the adopted instance is alive.
    destroy();
    setVulkanInstance(nullptr);
}

void VulkanCanvas::setImage(std::shared_ptr<VulkanImage> image)
{
    if (image && image->device() != _device) {
        throw std::invalid_argument("Viewer image belongs to another device.");
    }

    _image = std::move(image);
    _frameRenderer = {};
    _renderedImage.reset();
    requestUpdate();
}

void VulkanCanvas::setFrameRenderer(FrameRenderer renderer)
{
    _frameRenderer = std::move(renderer);
    _image.reset();
    _renderedImage.reset();
    requestUpdate();
}

void VulkanCanvas::setCaptureEnabled(bool enabled)
{
    if (_captureEnabled != enabled) {
        _captureEnabled = enabled;
        _resizePending = true;
        _lastFrame = QImage();
        requestUpdate();
    }
}

QImage VulkanCanvas::grabFrame()
{
    const bool previouslyEnabled = _captureEnabled;
    setCaptureEnabled(true);
    render();
    QImage result = _lastFrame;
    setCaptureEnabled(previouslyEnabled);
    return result;
}

bool VulkanCanvas::event(QEvent* event)
{
    if (event->type() == QEvent::UpdateRequest) {
        render();
        return true;
    }

    if (event->type() == QEvent::PlatformSurface) {
        auto* surface = static_cast<QPlatformSurfaceEvent *>(event);

        if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
            releaseSwapchain();
            _surface = VK_NULL_HANDLE;
        } else {
            _resizePending = true;
        }
    }

    return QWindow::event(event);
}

void VulkanCanvas::exposeEvent(QExposeEvent*)
{
    if (isExposed()) {
        requestUpdate();
    }
}

void VulkanCanvas::resizeEvent(QResizeEvent*)
{
    _resizePending = true;

    if (isExposed()) {
        requestUpdate();
    }
}

void VulkanCanvas::releaseSwapchain() noexcept
{
    if (!_device) {
        return;
    }

    const auto& fn = _device->functions();

    try {
        _device->waitIdle();
    } catch (...) { }

    for (auto semaphore : _ready) {
        fn.vkDestroySemaphore(_device->handle(), semaphore, nullptr);
    }

    _ready.clear();

    if (_acquire) {
        fn.vkDestroySemaphore(_device->handle(), _acquire, nullptr);
    }

    if (_swapchain) {
        fn.vkDestroySwapchainKHR(_device->handle(), _swapchain, nullptr);
    }

    _acquire = VK_NULL_HANDLE;
    _swapchain = VK_NULL_HANDLE;
    _images.clear();
    _presented.clear();
    _captureImage.reset();
    _renderedImage.reset();
    _resizePending = true;
}

void VulkanCanvas::createSwapchain()
{
    releaseSwapchain();
    _surface = QVulkanInstance::surfaceForWindow(this);

    if (!_surface) {
        throw std::runtime_error("The Vulkan window has no surface.");
    }

    const auto& instanceFn = _instance->native()->functions();
    const auto& fn = _device->functions();
    VkSurfaceCapabilitiesKHR capabilities{};
    _device->check(instanceFn.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_device->physicalDevice(), _surface, &capabilities), "query surface capabilities");

    if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        throw std::runtime_error("The surface does not support Vulkan blit presentation.");
    }

    _extent = capabilities.currentExtent;

    if (_extent.width == UINT32_MAX) {
        const auto pixels = size() * devicePixelRatio();
        _extent.width = std::clamp(static_cast<uint32_t>(std::max(1, pixels.width())), capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        _extent.height = std::clamp(static_cast<uint32_t>(std::max(1, pixels.height())), capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }

    if (!_extent.width || !_extent.height) {
        return;
    }

    uint32_t count = 0;
    _device->check(instanceFn.vkGetPhysicalDeviceSurfaceFormatsKHR(_device->physicalDevice(), _surface, &count, nullptr), "query surface formats");
    std::vector<VkSurfaceFormatKHR> formats(count);
    _device->check(instanceFn.vkGetPhysicalDeviceSurfaceFormatsKHR(_device->physicalDevice(), _surface, &count, formats.data()), "query surface formats");
    auto chosen = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR & entry) {
        return entry.format == VK_FORMAT_B8G8R8A8_UNORM && entry.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });

    if (chosen == formats.end()) {
        chosen = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR & entry) {
            return entry.format == VK_FORMAT_R8G8B8A8_UNORM && entry.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
    }

    if (chosen == formats.end()) {
        throw std::runtime_error("No supported UNORM Vulkan presentation format.");
    }

    VkFormatProperties destinationProperties{};
    instanceFn.vkGetPhysicalDeviceFormatProperties(_device->physicalDevice(), chosen->format, &destinationProperties);

    if (!(destinationProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        throw std::runtime_error("Presentation format does not support blitting.");
    }

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = _surface;
    info.minImageCount = capabilities.minImageCount + 1;

    if (capabilities.maxImageCount) {
        info.minImageCount = std::min(info.minImageCount, capabilities.maxImageCount);
    }

    info.imageFormat = chosen->format;
    info.imageColorSpace = chosen->colorSpace;
    info.imageExtent = _extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    if (_captureEnabled) {
        if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
            throw std::runtime_error("The Vulkan surface does not support frame readback.");
        }

        info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        _captureImage = std::make_shared<VulkanImage>(_device, _extent.width, _extent.height, VK_FORMAT_R8G8B8A8_UNORM);
    }

    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = capabilities.currentTransform;

    for (auto alpha : {
             VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
             VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
         }) {
        if (capabilities.supportedCompositeAlpha & alpha) {
            info.compositeAlpha = alpha;
            break;
        }
    }
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    _device->check(fn.vkCreateSwapchainKHR(_device->handle(), &info, nullptr, &_swapchain), "create Qt swapchain");
    _device->check(fn.vkGetSwapchainImagesKHR(_device->handle(), _swapchain, &count, nullptr), "query swapchain images");
    _images.resize(count);
    _device->check(fn.vkGetSwapchainImagesKHR(_device->handle(), _swapchain, &count, _images.data()), "query swapchain images");
    _presented.assign(count, false);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    _device->check(fn.vkCreateSemaphore(_device->handle(), &semaphore, nullptr, &_acquire), "create acquisition semaphore");

    for (uint32_t i = 0; i < count; ++i) {
        VkSemaphore ready = VK_NULL_HANDLE;
        _device->check(fn.vkCreateSemaphore(_device->handle(), &semaphore, nullptr, &ready), "create presentation semaphore");
        _ready.push_back(ready);
    }

    _resizePending = false;
    ++_swapchainGeneration;
}

void VulkanCanvas::imageBarrier(VkCommandBuffer command, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? VK_ACCESS_2_TRANSFER_WRITE_BIT :
                            newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_NONE;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    info.imageMemoryBarrierCount = 1;
    info.pImageMemoryBarriers = &barrier;
    _device->functions().vkCmdPipelineBarrier2(command, &info);
}

void VulkanCanvas::render()
{
    if (!isExposed() || !_device || !_error.empty()) {
        return;
    }

    try {
        _device->requireUsable();

        if (_resizePending || !_swapchain) {
            createSwapchain();
        }

        if (!_swapchain) {
            return;
        }

        // Render before acquiring a swapchain image. A caller exception must
        // not leave the acquire semaphore signaled without a consumer.
        auto displayedImage = _image;

        if (_frameRenderer) {
            if (!_renderedImage) {
                _renderedImage = std::make_shared<VulkanImage>(_device, _extent.width, _extent.height);
            }

            _frameRenderer(*_renderedImage);
            displayedImage = _renderedImage;
        }

        const auto& fn = _device->functions();
        uint32_t index = 0;
        const auto acquire = fn.vkAcquireNextImageKHR(_device->handle(), _swapchain, 1000000000ULL, _acquire, VK_NULL_HANDLE, &index);

        if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
            _resizePending = true;
            requestUpdate();
            return;
        }

        if (acquire == VK_TIMEOUT || acquire == VK_NOT_READY) {
            requestUpdate();
            return;
        }

        if (acquire != VK_SUBOPTIMAL_KHR) {
            _device->check(acquire, "acquire Viewer frame");
        }

        _device->execute([&](VkCommandBuffer command) {
            imageBarrier(command, _images[index], _presented[index] ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

            if (displayedImage) {
                VkFormatProperties properties{};
                _instance->native()->functions().vkGetPhysicalDeviceFormatProperties(_device->physicalDevice(), displayedImage->format(), &properties);

                if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
                    throw std::runtime_error("Viewer source format does not support blitting.");
                }

                displayedImage->transition(command, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(displayedImage->width()), static_cast<int32_t>(displayedImage->height()), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(_extent.width), static_cast<int32_t>(_extent.height), 1};
                fn.vkCmdBlitImage(command, displayedImage->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _images[index],
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            } else {
                VkClearColorValue clear{{0.1f, 0.1f, 0.1f, 1.0f}};
                VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                fn.vkCmdClearColorImage(command, _images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            }

            if (_captureImage) {
                imageBarrier(command, _images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                _captureImage->transition(command, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageBlit blit{};
                blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = blit.dstOffsets[1] = {static_cast<int32_t>(_extent.width), static_cast<int32_t>(_extent.height), 1};
                fn.vkCmdBlitImage(command, _images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, _captureImage->handle(),
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            }

            imageBarrier(command, _images[index], _captureImage ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }, _acquire, _ready[index]);
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &_ready[index];
        info.swapchainCount = 1;
        info.pSwapchains = &_swapchain;
        info.pImageIndices = &index;
        _instance->qt()->presentAboutToBeQueued(this);
        const auto status = _device->present(info);
        _instance->qt()->presentQueued(this);

        if (_captureImage) {
            const auto bytes = _captureImage->download();
            _lastFrame = QImage(bytes.data(), static_cast<int>(_extent.width), static_cast<int>(_extent.height),
                                static_cast<int>(_extent.width * 4), QImage::Format_RGBA8888).copy();
        }

        _presented[index] = true;
        ++_presentedFrames;

        if (_presentedHandler && (status == VK_SUCCESS || status == VK_SUBOPTIMAL_KHR)) {
            _presentedHandler(_presentedFrames);
        }

        if (status == VK_ERROR_OUT_OF_DATE_KHR || status == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR) {
            _resizePending = true;
            requestUpdate();
        }
    } catch (const std::exception& error) {
        _error = error.what();
        releaseSwapchain();
        bool handled = false;

        if (const auto * nativeError = dynamic_cast<const VulkanError * >(&error)) {
            if (_failureHandler) {
                try {
                    handled = _failureHandler(*nativeError);
                } catch (...) { }
            }
        }

        if (!handled) {
            std::cerr << "Vulkan Viewer: " << _error << std::endl;
        }
    }
}
} // namespace Gpu
} // namespace Natron
