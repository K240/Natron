// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_TEST_DEVICEFAULTINJECTION_H
#define NATRON_GPU_TEST_DEVICEFAULTINJECTION_H
#include "Gpu/VulkanDevice.h"
namespace Natron
{
namespace Gpu
{
// Only test code and the profile-gated host test suite include this friend.
// The wait fault is returned after real work completes, keeping the desktop
// driver healthy while exercising the lost-device cleanup/result paths.
class DeviceFaultInjection
{
public:
    enum Point : uint32_t { Submission = 1, Completion = 2, Presentation = 3 };
    static void arm(const std::shared_ptr<VulkanDevice> &device, Point point)
    {
        if (!device || point < Submission || point > Presentation) {
            throw std::invalid_argument("Invalid Vulkan device fault point.");
        }

        std::lock_guard<std::mutex> lock(device->_queueMutex);
        device->requireUsable();
        uint32_t expected = 0;

        if (!device->_testLossPoint.compare_exchange_strong(expected, point)) {
            throw std::logic_error("A Vulkan device fault is already armed.");
        }
    }
};
}
}
#endif
