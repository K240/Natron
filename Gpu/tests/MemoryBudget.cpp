// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/VulkanDevice.h"
#include <algorithm>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace Natron::Gpu;
namespace
{
void require(bool condition, const char *message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
void bounded(const MemoryBudgetSnapshot& budget)
{
    require(budget.limitBytes == 32 * 1024 * 1024, "Incorrect managed memory limit");
    require(budget.localBlockBytes <= budget.limitBytes && budget.localPeakBlockBytes <= budget.limitBytes,
            "Allocator exceeded the managed device-local heap limit");
    uint64_t blocks = 0, limits = 0;

    for (const auto& heap : budget.heaps) {
        require(heap.allocationBytes <= heap.blockBytes, "Invalid heap statistics");

        if (heap.deviceLocal) {
            blocks += heap.blockBytes;
            limits += heap.limitBytes;
        }
    }

    require(blocks == budget.localBlockBytes && limits <= budget.limitBytes, "Allocation callback accounting disagrees with VMA");
}
void transferAccounting(const std::shared_ptr<VulkanDevice> &device)
{
    VulkanImage image(device, 16, 11);
    image.clear({0.f, .125f, .25f, 1.f});
    const auto before = device->imageTransfers();
    constexpr uint32_t width = 7, height = 5, stride = 12;
    std::vector<float> input(stride * height * 4, -123.f);
    const float color[4] {.125f, .25f, .5f, 1.f}, background[4] {0.f, .125f, .25f, 1.f};

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            std::copy(color, color + 4, input.data() + (y * stride + x) * 4);
        }
    }

    bool rejected = false;

    try {
        image.uploadRegion(2, 3, width, height, input.data(), input.size() * sizeof(float) - 4, stride);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    require(rejected, "Invalid padded transfer was accepted");
    image.uploadRegion(2, 3, width, height, input.data(), input.size() * sizeof(float), stride);
    const auto pixels = image.download();

    for (uint32_t y = 0; y < image.height(); ++y) {
        for (uint32_t x = 0; x < image.width(); ++x) {
            const auto* expected = x >= 2 && x < 2 + width && y >= 3 && y < 3 + height ? color : background;
            require(std::memcmp(pixels.data() + (y * image.width() + x) * 16, expected, 16) == 0,
                    "Padded region transfer corrupted pixels or pixels outside its ROI");
        }
    }

    const auto after = device->imageTransfers();
    require(after.uploads - before.uploads == 1 && after.downloads - before.downloads == 1,
            "Invalid transfers or GPU clears counted as completed CPU image copies");
    require(after.uploadBytes - before.uploadBytes == uint64_t(width) * height * 16 &&
            after.downloadBytes - before.downloadBytes == image.byteSize(), "Image transfer accounting included row padding or lost texels");
}
}
int main(int argc, char **argv)
{
    try {
        const int index = argc > 2 && std::strcmp(argv[1], "--gpu") == 0 ? std::stoi(argv[2]) : argc > 1 ? std::stoi(argv[1]) : -1;
        auto instance = VulkanInstance::create({true});
        auto device = VulkanDevice::create(instance, index, VK_NULL_HANDLE, 32 * 1024 * 1024);
        transferAccounting(device);
        unsigned failures = 0, allocations = 0;

        for (unsigned iteration = 0; iteration < 3; ++iteration) {
            std::vector<std::unique_ptr<VulkanImage>> images;
            bool exhausted = false;

            for (unsigned i = 0; i < 128; ++i) {
                try {
                    images.push_back(std::make_unique<VulkanImage>(device, 256, 256));
                    ++allocations;
                } catch (const VulkanError& error) {
                    require(error.result() == VK_ERROR_OUT_OF_DEVICE_MEMORY && error.deviceMemoryFailure(), "Wrong allocation failure classification");
                    exhausted = true;
                    ++failures;
                    break;
                }
            }

            require(exhausted && !images.empty(), "Limit failed to reject oversized allocation batch");
            bounded(device->memoryBudget());
            images.clear();
            auto recovered = std::make_unique<VulkanImage>(device, 64, 48);
            recovered->clear({.125f, .25f, .5f, 1.f});
            const auto bytes = recovered->download();
            const float expected[4] {.125f, .25f, .5f, 1.f};

            for (size_t i = 0; i < bytes.size(); i += sizeof(expected)) {
                require(std::memcmp(bytes.data() + i, expected, sizeof(expected)) == 0, "Post-OOM image contents corrupted");
            }

            bounded(device->memoryBudget());
        }

        std::vector<std::future<void>> workers;

        for (unsigned i = 0; i < 4; ++i) {
            workers.push_back(std::async(std::launch::async, [device] {
                for (unsigned n = 0; n < 12; ++n)
                {
                    VulkanImage image(device, 32, 32);
                    image.clear({.25f, .5f, 1.f, 1.f});
                    const auto bytes = image.download();
                    const float expected[4] {.25f, .5f, 1.f, 1.f};
                    require(std::memcmp(bytes.data(), expected, sizeof(expected)) == 0, "Concurrent post-OOM transfer failed");
                }
            }));
        }

        for (auto& worker : workers) {
            worker.get();
        }

        const auto budget = device->memoryBudget();
        const auto transfers = device->imageTransfers();
        require(transfers.uploads == 1 && transfers.downloads == 52 && transfers.uploadBytes == 560 &&
                transfers.downloadBytes == 936704, "Concurrent completed image transfer counts disagree with copied pixels");
        bounded(budget);
        require(instance->validationErrorCount() == 0, "Validation error during allocation/recovery");
        std::cout << "{\"passed\":true,\"device\":\"" << device->properties().deviceName
                  << "\",\"limit_bytes\":" << budget.limitBytes << ",\"peak_block_bytes\":" << budget.localPeakBlockBytes
                  << ",\"allocations_before_failures\":" << allocations << ",\"allocation_failures\":" << failures
                  << ",\"post_oom_retries\":3,\"concurrent_transfers\":48,\"transfer_accounting\":true,\"driver_budget\":" << (budget.driverBudget ? "true" : "false") << ",\"validation_errors\":0}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
