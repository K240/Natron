// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ViewerDisplayConversion.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#endif
using namespace Natron::Gpu;
namespace
{
uint32_t quantize(float value, uint32_t maximum)
{
    if (std::isnan(value) || value <= 0) {
        return 0;
    }

    if (value >= 1) {
        return maximum;
    }

    return uint32_t(value * maximum + .5f);
}
float encode(float value, int lut)
{
    if (lut == 1) {
        return value < .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
    }

    if (lut == 2) {
        return value < .0181f ? 4.5f * value : 1.0993f * std::pow(value, .45f) - .0993f;
    }

    return std::pow(std::max(0.f, value), 1.f / 2.4f);
}
float gammaValue(float value, float gamma, const ViewerDisplayTables& tables)
{
    if (gamma <= 0) {
        return value < 1 ? 0 : value == 1 ? 1 : std::numeric_limits<float>::infinity();
    }

    if (gamma == 1) {
        return value;
    }

    if (value < 0) {
        return 0;
    }

    if (value > 1) {
        return 1;
    }

    const float pos = value * 1023;
    const int i = int(pos);
    const float alpha = std::clamp(pos - i, 0.f, 1.f);
    return tables.gamma[i] * (1 - alpha) + (i < 1023 ? tables.gamma[i + 1] : 0) * alpha;
}
template<class F> void reject(F call)
{
    try {
        call();
    } catch (const std::invalid_argument &) {
        return;
    }

    throw std::runtime_error("Invalid Viewer display input was accepted.");
}
}
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
        uint64_t cases = 0, channels = 0;
        int maximumError = 0;
        {
            ViewerDisplayConversion conversion(device);
            VulkanImage source(device, 37, 19);
            std::vector<float> pixels(37 * 19 * 4);

            for (size_t i = 0; i < pixels.size() / 4; ++i) {
                pixels[4 * i] = float(i % 73) / 48 - .2f;
                pixels[4 * i + 1] = float(i % 47) / 39;
                pixels[4 * i + 2] = float(i % 31) / 40;
                pixels[4 * i + 3] = float(i % 19) / 15 - .1f;
            }

            pixels[3] = std::numeric_limits<float>::quiet_NaN();
            pixels[7] = std::numeric_limits<float>::infinity();
            pixels[11] = -std::numeric_limits<float>::infinity();
            source.upload(pixels.data(), pixels.size() * sizeof(float));
            const auto original = source.download();
            auto tables = std::make_unique<ViewerDisplayTables>();

            for (int lut = 0; lut < 4; ++lut) {
                for (uint32_t i = 0; i < tables->color.size(); ++i) {
                    const uint32_t bits = i << 16;
                    float value;
                    std::memcpy(&value, &bits, sizeof(value));
                    tables->color[i] = quantize(encode(value, lut), 65280);
                }

                for (float gamma : {
                         -1.f, 0.f, .45f, 1.f, 2.2f, 4.f
                     }) {
                    for (size_t i = 0; i < tables->gamma.size(); ++i) {
                        tables->gamma[i] = gamma <= 0 ? float(i == 1023) : float(std::pow(double(i) / 1023, 1. / gamma));
                    }

                    for (const auto gainOffset : {
                             std::array<float, 2> {1, 0}, {1.7f, -.2f}, {-.5f, .75f}
                         }) {
                        for (bool opaque : {
                                 false, true
                             }) {
                            for (const ImageRegion region : {
                                     ImageRegion{0, 0, 37, 19}, {4, 3, 23, 13}, {0, 0, 1, 5}
                                 }) {
                                VulkanImage output(device, region.width, region.height, VK_FORMAT_R8G8B8A8_UNORM);
                                ViewerDisplayCorrection correction{gainOffset[0], gainOffset[1], gamma, lut == 0, opaque, -17, -9};
                                conversion.render(source, output, region, correction, *tables);
                                const auto result = output.download();

                                for (uint32_t y = 0; y < region.height; ++y) for (uint32_t x = 0; x < region.width; ++x) {
                                        const size_t at = ((region.y + y) * 37 + region.x + x) * 4;

                                        for (size_t c = 0; c < 4; ++c) {
                                            int expected;

                                            if (c == 3) {
                                                expected = opaque ? 255 : quantize(pixels[at + c], 255);
                                            } else {
                                                const float value = gammaValue(pixels[at + c] * correction.gain + correction.offset, gamma, *tables);
                                                uint32_t bits;
                                                std::memcpy(&bits, &value, sizeof(bits));
                                                expected = lut == 0 ? quantize(value, 255) : (tables->color[bits >> 16] + 128) >> 8;
                                            }

                                            const int error = std::abs(int(result[(y * region.width + x) * 4 + c]) - expected);

                                            if (error > (c == 3 || lut == 0 ? 0 : 1)) {
                                                throw std::runtime_error("GPU byte display differs from CPU LUT/gamma/alpha reference.");
                                            }

                                            maximumError = std::max(maximumError, error);
                                            ++channels;
                                        }
                                    }

                                conversion.render(source, output, region, correction, *tables);

                                if (output.download() != result) {
                                    throw std::runtime_error("Cached Viewer display dithering changed.");
                                }

                                ++cases;
                            }
                        }
                    }
                }
            }

            if (source.download() != original) {
                throw std::runtime_error("Viewer conversion modified Engine pixels.");
            }

            VulkanImage output(device, 37, 19, VK_FORMAT_R8G8B8A8_UNORM), wrongFormat(device, 37, 19), empty(device, 37, 19);
            const ViewerDisplayCorrection correction{};
            reject([&] { conversion.render(source, wrongFormat, {0, 0, 37, 19}, correction, *tables); });
            reject([&] { conversion.render(empty, output, {0, 0, 37, 19}, correction, *tables); });
            reject([&] { conversion.render(source, output, {1, 0, 37, 19}, correction, *tables); });
            reject([&] { conversion.render(source, output, {0, 0, 0, 19}, correction, *tables); });
            tables->color[0] = 65281;
            reject([&] { conversion.render(source, output, {0, 0, 37, 19}, correction, *tables); });
            tables->color[0] = 0;
            tables->gamma[0] = std::numeric_limits<float>::quiet_NaN();
            reject([&] { conversion.render(source, output, {0, 0, 37, 19}, correction, *tables); });
            device->waitIdle();
        }

        if (!instance->validationEnabled() || instance->validationErrorCount()) {
            throw std::runtime_error("Viewer display Vulkan validation failed.");
        }

#ifdef _WIN32

        if (GetModuleHandleW(L"opengl32.dll")) {
            throw std::runtime_error("Viewer display conversion loaded OpenGL.");
        }

#endif
        std::cout << "{\"passed\":true,\"device\":\"" << name << "\",\"cases\":" << cases
                  << ",\"channelsCompared\":" << channels << ",\"maximumByteError\":" << maximumError
                  << ",\"invalidCases\":6,\"stableDithering\":true,\"sourceUnchanged\":true,\"validationErrors\":0,\"openGLLoaded\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan Viewer display: " << error.what() << std::endl;
        return 1;
    }
}
