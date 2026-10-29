// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/VulkanDevice.h"
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#endif

using namespace Natron::Gpu;
namespace
{
void require(bool condition, const char* description)
{
    if (!condition) {
        throw std::runtime_error(description);
    }
}

void testTransfer(const std::shared_ptr<VulkanDevice> &device, VkFormat format)
{
    VulkanImage image(device, 257, 129, format);
    std::vector<unsigned char> input(image.byteSize());

    for (size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<unsigned char>((i * 17 + i / 31) % 256);
    }

    image.upload(input.data(), input.size());
    require(image.download() == input, "Image transfer was not bit-exact.");
    // Padded rows, a nonzero image offset and a tile touching the last row and
    // column exercise the Viewer cache's subregion transfer contract.
    const size_t pixelSize = input.size() / 257 / 129;
    std::vector<unsigned char> tile(41 * 17 * pixelSize);

    for (size_t i = 0; i < tile.size(); ++i) {
        tile[i] = static_cast<unsigned char>(i * 13 + 83);
    }

    image.uploadRegion(222, 112, 35, 17, tile.data(), tile.size(), 41);

    for (size_t row = 0; row < 17; ++row) {
        std::memcpy(input.data() + ((row + 112) * 257 + 222) * pixelSize, tile.data() + row * 41 * pixelSize, 35 * pixelSize);
    }

    require(image.download() == input, "Padded tile transfer modified other pixels or dropped edge pixels.");
    // The row-span API must not read padding beyond the last row or write
    // destination padding. This catches a cropped view into a wider RAM image.
    const size_t rowBytes = 27 * pixelSize, activeBytes = 19 * pixelSize;
    std::vector<unsigned char> rows(6 * rowBytes + activeBytes);

    for (size_t i = 0; i < rows.size(); ++i) {
        rows[i] = static_cast<unsigned char>(i * 7 + 19);
    }

    image.uploadRegionRows(238, 122, 19, 7, rows.data(), rows.size(), rowBytes);

    for (size_t row = 0; row < 7; ++row) {
        std::memcpy(input.data() + ((row + 122) * 257 + 238)*pixelSize, rows.data() + row * rowBytes, activeBytes);
    }

    require(image.download() == input, "Cropped strided upload changed pixels outside its ROI.");
    std::vector<unsigned char> readback(rows.size() + 64, 0xc7), expected = readback;

    for (size_t row = 0; row < 7; ++row) {
        std::memcpy(expected.data() + 32 + row * rowBytes, rows.data() + row * rowBytes, activeBytes);
    }

    image.downloadRegionRows(238, 122, 19, 7, readback.data() + 32, rows.size(), rowBytes);
    require(readback == expected, "Strided readback overwrote padding or guards.");
    bool spanRejected = false;

    try {
        image.downloadRegionRows(238, 122, 19, 7, readback.data() + 32, rows.size() - 1, rowBytes);
    } catch (const std::invalid_argument &) {
        spanRejected = true;
    }

    require(spanRejected && readback == expected, "Short readback span changed the destination.");
    spanRejected = false;

    try {
        image.uploadRegionRows(238, 122, 19, 7, rows.data(), rows.size() - 1, rowBytes);
    } catch (const std::invalid_argument &) {
        spanRejected = true;
    }

    require(spanRejected && image.download() == input, "Short upload span modified the image.");
    bool rejected = false;

    try {
        image.upload(input.data(), input.size() - 1);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    require(rejected, "Incorrect upload size was accepted.");
}

float testCompute(const std::shared_ptr<VulkanDevice> &device, ImageTransform& transform, int seed)
{
    VulkanImage source(device, 257, 129), destination(device, 257, 129), chained(device, 257, 129);
    std::vector<float> input(source.byteSize() / sizeof(float));

    for (size_t i = 0; i < input.size(); ++i) {
        // Negative values, HDR and independent alpha, with an image extent not
        // divisible by the shader's workgroup size.
        input[i] = static_cast<float>(static_cast<int>(i % 4093) - 2046 + seed) / 257.0f;
    }

    const std::array<float, 4> gain{-1.25f, 0.5f, 2.0f, 0.75f}, offset{1.0f, -0.5f, 3.25f, 0.125f};
    source.upload(input.data(), source.byteSize());
    transform.apply(source, destination, gain, offset);
    transform.apply(destination, chained, gain, offset);
    auto bytes = chained.download();
    std::vector<float> output(input.size());
    std::memcpy(output.data(), bytes.data(), bytes.size());
    float maxError = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        const size_t c = i % 4;
        const float expected = (input[i] * gain[c] + offset[c]) * gain[c] + offset[c];
        const float error = std::abs(output[i] - expected);
        require(std::isfinite(output[i]) && error <= 0.00002f, "Compute result differs from CPU reference.");
        maxError = std::max(maxError, error);
    }

    bool rejected = false;

    try {
        transform.apply(source, source, gain, offset);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    require(rejected, "Aliased compute images were accepted.");
    return maxError;
}
}

int main(int argc, char** argv)
{
    try {
        int index = -1;

        if (argc == 3 && std::string(argv[1]) == "--gpu") {
            index = std::stoi(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: NatronVulkanCompute [--gpu index]");
        }

        InstanceOptions options;
        options.validation = true;
        auto instance = VulkanInstance::create(options);
        std::string name;
        float maxError = 0;
        {
            auto device = VulkanDevice::create(instance, index);
            name = device->properties().deviceName;

            for (auto format : {
                     VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_UNORM,
                     VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT
                 }) {
                testTransfer(device, format);
            }
            bool rejected = false;

            try {
                VulkanImage invalid(device, 0, 1);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }

            require(rejected, "Zero-sized image was accepted.");
            ImageTransform transform(device);
            maxError = testCompute(device, transform, 0);
            // Simultaneous host callers exercise allocator, descriptor and
            // submission synchronization on the same logical device.
            std::vector<std::future<float>> workers;

            for (int i = 1; i <= 4; ++i) {
                workers.emplace_back(std::async(std::launch::async, [ &, i] { return testCompute(device, transform, i); }));
            }

            for (auto& worker : workers) {
                maxError = std::max(maxError, worker.get());
            }

            device->waitIdle();
        }
        require(instance->validationEnabled() && instance->validationErrorCount() == 0, "Vulkan validation reported errors.");
        bool openGLLoaded = false;
#ifdef _WIN32
        openGLLoaded = GetModuleHandleW(L"opengl32.dll") != nullptr;
#endif
        require(!openGLLoaded, "OpenGL was loaded by the Vulkan compute test.");
        std::cout << "{\"passed\":true,\"device\":\"" << name
                  << "\",\"transferFormats\":4,\"regionTransfers\":4,\"computeChains\":5,\"maxAbsoluteError\":" << maxError
                  << ",\"validationErrors\":" << instance->validationErrorCount()
                  << ",\"openGLLoaded\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan compute test: " << error.what() << std::endl;
        return 1;
    }
}
