// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/RasterViewerRenderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace Natron::Gpu;
int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::invalid_argument("Usage: RasterGpuCompare deviceIndex");
        }

        InstanceOptions options;
        options.validation = true;
        auto instance = VulkanInstance::create(options);
        size_t cases = 0, channels = 0;
        float maximum = 0;
        std::string name;
        {
            auto device = VulkanDevice::create(instance, std::stoi(argv[1]));
            name = device->properties().deviceName;
            ViewerRenderer renderer(device);
            VulkanImage target(device, 13, 7);

            for (ViewerStorage storage : {
                     ViewerStorage::BGRA8, ViewerStorage::RGBA32F
                 }) {
                const size_t stride = storage == ViewerStorage::BGRA8 ? 4 : 16;
                std::array<std::vector<unsigned char>, 2> pixels;
                std::array<std::unique_ptr<ViewerFrame>, 2> frames;
                std::array<RasterViewerInput, 2> inputs;

                for (size_t input = 0; input < 2; ++input) {
                    pixels[input].resize(5 * 3 * stride);

                    for (size_t i = 0; i < 15; ++i) {
                        std::array<float, 4> color{float((i + input) % 9) / 7 - .15f,
                                                   float((i * 3 + input) % 8) / 5, float((i * 2 + input) % 7) / 6, .2f + float(i % 4)*.2f};

                        if (storage == ViewerStorage::RGBA32F) {
                            std::memcpy(pixels[input].data() + i * 16, color.data(), 16);
                        } else {
                            for (size_t c = 0; c < 4; ++c) {
                                pixels[input][i * 4 + c] = static_cast<unsigned char>((i * 31 + c * 57 + input * 43) % 256);
                            }
                        }
                    }

                    frames[input] = std::make_unique<ViewerFrame>(device, ViewerPixelBounds{-2, 4, 3, 7}, storage);
                    frames[input]->uploadTile({-2, 4, 3, 7}, pixels[input].data(), pixels[input].size());
                    inputs[input].pixels = pixels[input].data();
                    inputs[input].width = 5;
                    inputs[input].height = 3;
                    inputs[input].storage = storage;
                    auto& display = inputs[input].display;
                    display.image = frames[input]->image().get();
                    display.bounds = display.clip = display.format = {-2.6f, 2, 3.9f, 3.5f};
                    display.pixelGrid = {1.3f, .5f, -2, 4};
                    display.transform = storage == ViewerStorage::RGBA32F;
                    display.gain = .73f;
                    display.offset = .04f;
                    display.gamma = 1.2f;

                    if (input) display.clip = {-1.3f, 2.25f, 2.6f, 3.5f};
                }

                for (int lut = 0; lut < 4; ++lut) for (int op = 0; op < 9; ++op)
                        for (int premult = 0; premult < 3; ++premult) for (bool linear : {
                                    false, true
                                })

                                for (bool topDown : {
                                         false, true

                                     }) for (bool preserve : {
                                                     false, true

                                                 }) for (int view = 0; view < 3; ++view) {
                                            for (auto& input : inputs) {
                                                input.display.lut = static_cast<ViewerLut>(lut);
                                                input.display.premult = static_cast<ViewerPremult>(premult);
                                                input.display.linearFilter = linear;
                                                input.display.topDown = topDown;
                                            }

                                            ViewerSettings scene;
                                            scene.viewport = view == 0 ? std::array<float, 4> {-3, 1, 4, 4} :
                                                             view == 1 ? std::array<float, 4> {-2.6f, 2, 3.9f, 3.5f} :
                                                             std::array<float, 4> {-.9f, 2.2f, 2.3f, 3.3f};
                                            scene.operation = static_cast<ViewerOperator>(op);
                                            scene.background = {.05f, .1f, .2f, 1};
                                            scene.wipe = {.25f, 2.75f, .37f, .6f};
                                            scene.checkerboard = view != 2;
                                            scene.checkerSize = 3;
                                            scene.preserveDestination = preserve;
                                            std::vector<float> expected;

                                            if (preserve) {
                                                target.clear({.7f, .2f, .4f, .8f});
                                                expected.resize(13 * 7 * 4);

                                                for (size_t i = 0; i < expected.size(); i += 4) {
                                                    const std::array<float, 4> background{.7f, .2f, .4f, .8f};
                                                    std::copy(background.begin(), background.end(), expected.begin() + i);
                                                }
                                            }

                                            renderer.render({inputs[0].display, inputs[1].display}, scene, target);
                                            const auto bytes = target.download();
                                            std::vector<float> actual(bytes.size() / 4);
                                            std::memcpy(actual.data(), bytes.data(), bytes.size());
                                            renderRasterViewer(inputs, scene, 13, 7, expected);

                                            for (size_t i = 0; i < actual.size(); ++i) {
                                                const float error = std::abs(actual[i] - expected[i]);

                                                if (!std::isfinite(actual[i]) || !std::isfinite(expected[i]) ||
                                                    error > 3e-5f * std::max(1.f, std::abs(expected[i]))) {
                                                    std::cerr << "case " << cases << " channel " << i << " GPU " << actual[i] << " raster " << expected[i] << '\n';
                                                    throw std::runtime_error("Raster/GPU display mismatch");
                                                }

                                                maximum = std::max(maximum, error);
                                                ++channels;
                                            }

                                            ++cases;
                                        }
            }
        }

        if (instance->validationErrorCount()) {
            throw std::runtime_error("Raster/GPU comparison reported validation errors");
        }

#ifdef _WIN32

        if (GetModuleHandleW(L"opengl32.dll")) {
            throw std::runtime_error("Raster/GPU comparison loaded OpenGL");
        }

#endif
        std::cout << "{\"passed\":true,\"gpu\":" << argv[1] << ",\"device\":\"" << name << "\",\"cases\":" << cases
                  << ",\"channels\":" << channels << ",\"maximum_error\":" << maximum << ",\"validation_errors\":0,\"opengl_loaded\":false}\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
