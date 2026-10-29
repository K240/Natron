// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/VulkanDevice.h"
#include "Gpu/ImageOperations.h"
#include "Gpu/ViewerRenderer.h"
#include "Gpu/ViewerDisplayConversion.h"
#include "Gpu/tests/DeviceFaultInjection.h"
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace Natron::Gpu;
namespace
{
void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class F> void expectLost(F&& operation)
{
    try {
        operation();
    } catch (const VulkanError& error) {
        require(error.deviceLost() && error.result() == VK_ERROR_DEVICE_LOST && !error.outOfMemory(),
                "Incorrect device-lost error classification");
        return;
    }

    throw std::runtime_error("A retired Vulkan device accepted an operation");
}
void checkPixels(VulkanImage& image)
{
    const auto bytes = image.download();
    const float expected[4] {.25f, .5f, 1.f, 1.f};

    for (size_t i = 0; i < bytes.size(); i += sizeof(expected)) {
        require(std::memcmp(bytes.data() + i, expected, sizeof(expected)) == 0,
                "Replacement device output disagrees with CPU reference");
    }
}
}
int main(int argc, char** argv)
{
    try {
        const int index = argc > 2 && std::strcmp(argv[1], "--gpu") == 0 ? std::stoi(argv[2]) : -1;
        auto instance = VulkanInstance::create({true});
        unsigned retiredRejections = 0;
        uint64_t lastGeneration = 0;
        std::string deviceName;

        for (auto point : {
                 DeviceFaultInjection::Submission, DeviceFaultInjection::Completion,
                 DeviceFaultInjection::Presentation
             }) {
            auto old = VulkanDevice::create(instance, index);
            deviceName = old->properties().deviceName;
            require(old->generation() > lastGeneration, "Device generation was reused");
            lastGeneration = old->generation();
            VulkanImage source(old, 16, 12), destination(old, 16, 12);
            ImageTransform transform(old);
            source.clear({.125f, .25f, .5f, 1.f});
            transform.apply(source, destination, {2.f, 2.f, 2.f, 1.f}, {});
            checkPixels(destination);
            DeviceFaultInjection::arm(old, point);
            expectLost([&] {
                if (point == DeviceFaultInjection::Presentation)
                {
                    // Injection returns before touching WSI. Actual presentation
                    // recovery is covered by the separate Qt/host integration test.
                    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                    old->present(present);
                } else
                {
                    source.clear({0.f, 0.f, 0.f, 1.f});
                }
            });
            require(old->lost(), "Loss was not retained by the shared device");
            auto rejected = [&](auto&& operation) {
                expectLost(operation);
                ++retiredRejections;
            };
            rejected([&] { source.download(); });
            rejected([&] { source.clear({}); });
            const std::array<float, 4> pixel{};
            rejected([&] { source.uploadRegion(0, 0, 1, 1, pixel.data(), sizeof(pixel)); });
            rejected([&] { transform.apply(source, destination, {1, 1, 1, 1}, {}); });
            rejected([&] { VulkanImage invalid(old, 1, 1); });
            rejected([&] { ImageTransform invalid(old); });
            rejected([&] { ImageOperations invalid(old); });
            rejected([&] { ViewerRenderer invalid(old); });
            rejected([&] { ViewerDisplayConversion invalid(old); });
            rejected([&] { old->waitIdle(); });
            std::atomic<unsigned> callbacks{0};
            std::vector<std::future<void>> workers;

            for (unsigned n = 0; n < 4; ++n) {
                workers.push_back(std::async(std::launch::async, [&] {
                    for (unsigned k = 0; k < 8; ++k)
                    {
                        expectLost([&] {
                            old->execute([&](VkCommandBuffer)
                            {
                                ++callbacks;
                            });
                        });
                    }
                }));
            }

            for (auto& worker : workers) {
                worker.get();
            }

            retiredRejections += 32;
            require(callbacks.load() == 0, "A retired command callback was recorded");
            // The old device and its images are deliberately retained during
            // creation and dispatch on the replacement; no handle is rebound.
            auto fresh = VulkanDevice::create(instance, index);
            require(!fresh->lost() && fresh->handle() != old->handle() &&
                    fresh->physicalDevice() == old->physicalDevice() && fresh->generation() > lastGeneration,
                    "Replacement did not create an independent logical device");
            lastGeneration = fresh->generation();
            VulkanImage freshSource(fresh, 16, 12), freshDestination(fresh, 16, 12);
            ImageTransform freshTransform(fresh);
            freshSource.clear({.125f, .25f, .5f, 1.f});
            freshTransform.apply(freshSource, freshDestination, {2.f, 2.f, 2.f, 1.f}, {});
            checkPixels(freshDestination);
        }
        {
            auto recording = VulkanDevice::create(instance, index);
            bool callback = false;
            expectLost([&] {
                recording->execute([&](VkCommandBuffer)
                {
                    callback = true;
                    auto detector = std::async(std::launch::async, [&] {
                        expectLost([&] { recording->check(VK_ERROR_DEVICE_LOST, "concurrent device-loss detection"); });
                    });
                    detector.get();
                });
            });
            require(callback && recording->lost(), "Concurrent loss during command recording was not rejected");
        }
        // All children and both devices from every iteration have now been
        // explicitly destroyed by RAII, including the simulated lost devices.
        require(instance->validationErrorCount() == 0, "Validation error during lost-device cleanup/recreation");
        std::cout << "{\"passed\":true,\"device\":\"" << deviceName
                  << "\",\"injected_loss_points\":3,\"replacements\":3,\"retired_operation_rejections\":"
                  << retiredRejections << ",\"compared_pixels\":1152,\"recording_loss_rejected\":true,\"physical_driver_reset\":false,\"validation_errors\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
