// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ViewerRenderer.h"
#include "Gpu/ImageOperations.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
using namespace Natron::Gpu;
int main(int argc, char** argv)
{
    try {
        int gpu = -1;

        for (int i = 1; i + 1 < argc; ++i) if (std::string(argv[i]) == "--gpu") {
                gpu = std::stoi(argv[++i]);
            }

        auto instance = VulkanInstance::create({true});
        auto device = VulkanDevice::create(instance, {gpu});
        const std::string name = device->properties().deviceName;
        uint64_t frames = 0, channels = 0;
        float maximum = 0;
        {
            VulkanImage cropped(device, 45, 31), padded(device, 256, 256);
            std::vector<float> pixels(45 * 31 * 4);

            for (size_t i = 0; i < pixels.size() / 4; ++i) {
                pixels[4 * i] = float(i % 17) / 16;
                pixels[4 * i + 1] = float(i % 23) / 22;
                pixels[4 * i + 2] = float(i % 31) / 30;
                pixels[4 * i + 3] = .37f;
            }

            cropped.upload(pixels.data(), pixels.size() * sizeof(float));
            ImageOperations operations(device);
            operations.copy(cropped, padded, {0, 0, 45, 31}, 120, 116);
            ViewerRenderer renderer(device);
            VulkanImage cropFrame(device, 1400, 600), tileFrame(device, 1400, 600);

            for (const auto scale : {
                     std::array<float, 2> {1, 1}, {1.3f, 1}, {2.8f, 2}, {64, 64}
                 }) {
                for (bool linear : {
                         false, true
                     }) {
                    ViewerInput crop;
                    crop.image = &cropped;
                    crop.bounds = {-8 * scale[0], -12 * scale[1], 37 * scale[0], 19 * scale[1]};
                    crop.clip = crop.format = crop.bounds;
                    crop.pixelGrid = {scale[0], scale[1], -8, -12};
                    crop.transform = false;
                    crop.linearFilter = linear;
                    auto tile = crop;
                    tile.image = &padded;
                    tile.bounds = {-128 * scale[0], -128 * scale[1], 128 * scale[0], 128 * scale[1]};
                    tile.pixelGrid[2] = tile.pixelGrid[3] = -128;
                    ViewerSettings settings;
                    settings.viewport = {-118.5f * scale[0], -45.5f * scale[1], 161.5f * scale[0], 74.5f * scale[1]};
                    renderer.render({crop, {}}, settings, cropFrame);
                    renderer.render({tile, {}}, settings, tileFrame);
                    const auto a = cropFrame.download(), b = tileFrame.download();
                    std::vector<float> actual(a.size() / sizeof(float)), expected(b.size() / sizeof(float));
                    std::memcpy(actual.data(), a.data(), a.size());
                    std::memcpy(expected.data(), b.data(), b.size());

                    for (size_t i = 0; i < actual.size(); ++i) {
                        const float error = std::abs(actual[i] - expected[i]);
                        maximum = std::max(maximum, error);

                        if (error > 2e-5f) {
                            std::cerr << "Grid " << scale[0] << ',' << scale[1] << " linear=" << linear
                                      << " channel=" << i << " crop=" << actual[i] << " padded=" << expected[i] << std::endl;
                            throw std::runtime_error("Cropped and padded Viewer frames sample different pixels.");
                        }

                        ++channels;
                    }

                    ++frames;
                }
            }
            device->waitIdle();
        }

        if (!instance->validationEnabled() || instance->validationErrorCount()) {
            throw std::runtime_error("Viewer grid validation failed.");
        }

        std::cout << "{\"passed\":true,\"device\":\"" << name << "\",\"frames\":" << frames
                  << ",\"channelsCompared\":" << channels << ",\"maxAbsoluteError\":" << maximum
                  << ",\"validationErrors\":0}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan Viewer grid: " << error.what() << std::endl;
        return 1;
    }
}
