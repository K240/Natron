// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ImageOperations.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
using namespace Natron::Gpu;
namespace
{
void require(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
template<class F> void rejects(F&& operation)
{
    bool rejected = false;

    try {
        operation();
    } catch (const std::invalid_argument &) {
        rejected = true;
    }

    require(rejected, "Invalid image operation was accepted.");
}
constexpr uint32_t W = 37, H = 23;
const ImageRegion ROI{3, 2, 29, 17};
bool inside(uint32_t x, uint32_t y)
{
    return x >= ROI.x && y >= ROI.y && x < ROI.x + ROI.width && y < ROI.y + ROI.height;
}
std::vector<float> pattern(int seed)
{
    std::vector<float> values(W * H * 4);

    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = float((i * 17 + seed * 31) % 241) / 113.0f - 0.5f;
    }

    return values;
}
float compare(VulkanImage& image, const std::vector<float> &expected)
{
    auto bytes = image.download();
    float maximum = 0;

    for (size_t i = 0; i < expected.size(); ++i) {
        float actual;
        std::memcpy(&actual, bytes.data() + i * sizeof(float), sizeof(float));
        float error = std::abs(actual - expected[i]);
        require(std::isfinite(actual) && error <= 0.00001f, "GPU image differs from the CPU reference, including outside ROI.");
        maximum = std::max(maximum, error);
    }

    return maximum;
}
unsigned testCopy(const std::shared_ptr<VulkanDevice> &device, ImageOperations& operations)
{
    unsigned cases = 0;

    for (auto format : {
             VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT
         }) {
        VulkanImage source(device, W, H, format), destination(device, W, H, format);
        const size_t pixelSize = source.byteSize() / W / H;
        std::vector<unsigned char> bytes(source.byteSize());

        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<unsigned char>(i * 29 + 7);
        }

        source.upload(bytes.data(), bytes.size());
        operations.copy(source, destination, ROI, 5, 4);
        std::vector<unsigned char> expected(bytes.size(), 0);

        for (uint32_t y = 0; y < ROI.height; ++y) {
            std::memcpy(expected.data() + ((y + 4) * W + 5) * pixelSize,
                        bytes.data() + ((y + ROI.y) * W + ROI.x) * pixelSize, ROI.width * pixelSize);
        }

        require(destination.download() == expected, "Region copy or zero initialization failed.");
        ++cases;
        operations.copy(source, source, ROI, 5, 4);
        expected = bytes;

        for (uint32_t y = 0; y < ROI.height; ++y) {
            std::memcpy(expected.data() + ((y + 4) * W + 5) * pixelSize,
                        bytes.data() + ((y + ROI.y) * W + ROI.x) * pixelSize, ROI.width * pixelSize);
        }

        require(source.download() == expected, "Overlapping self-copy did not preserve its source.");
        ++cases;
    }
    return cases;
}
unsigned testMixEndpoints(const std::shared_ptr<VulkanDevice> &device, ImageOperations& operations)
{
    VulkanImage processed(device, 1, 1), original(device, 1, 1), mask(device, 1, 1), output(device, 1, 1);
    // Preserve the selected endpoint exactly, including NaNs, infinities and signed zero.
    const std::array<uint32_t, 4> p{0x7fc12345u, 0x80000000u, 0x7f800000u, 0xff800000u};
    const std::array<uint32_t, 4> o{0x80000000u, 0x7fc45678u, 0xff800000u, 0x3f800000u};
    unsigned cases = 0;

    for (int mode = 0; mode < 3; ++mode) for (int endpoint = 0; endpoint < 2; ++endpoint) {
            processed.upload(p.data(), sizeof(p));
            original.upload(o.data(), sizeof(o));
            const bool masked = mode != 0, inverted = mode == 2;
            mask.clear({0, 0, 0, float(inverted ? 1 - endpoint : endpoint)});
            auto& destination = mode == 0 ? output : mode == 1 ? processed : original;
            operations.mix(processed, &original, &mask, destination, {0, 0, 1, 1}, masked ? 1.f : float(endpoint), masked, inverted);
            const auto bytes = destination.download();
            const auto& expected = endpoint ? p : o;
            require(bytes.size() == sizeof(expected) && std::memcmp(bytes.data(), expected.data(), sizeof(expected)) == 0,
                    "Mask/mix endpoint contaminated the selected pixels with the other input.");
            ++cases;
        }

    return cases;
}
float testCompute(const std::shared_ptr<VulkanDevice> &device, ImageOperations& operations, int seed, unsigned& cases)
{
    VulkanImage processed(device, W, H), original(device, W, H), mask(device, W, H), output(device, W, H);
    const auto p = pattern(seed), o = pattern(seed + 2), m = pattern(seed + 4), background = pattern(seed + 6);
    auto upload = [](VulkanImage & image, const std::vector<float> &values) {
        image.upload(values.data(), values.size() * sizeof(float));
    };
    const std::array<float, 4> gain{-1.5f, 0.25f, 2.1f, 0.8f}, offset{0.2f, -0.3f, 1.25f, 0.1f};
    float maximum = 0;

    // Distinct output plus each of the three input/output alias possibilities.
    for (int alias = 0; alias < 4; ++alias) {
        for (int hasOriginal = 0; hasOriginal < 2; ++hasOriginal) {
            for (int hasMask = 0; hasMask < 2; ++hasMask) {
                for (int flags = 0; flags < 4; ++flags) {
                    upload(processed, p);
                    upload(original, o);
                    upload(mask, m);
                    upload(output, background);
                    auto* destination = alias == 1 ? &processed : alias == 2 ? &original : alias == 3 ? &mask : &output;
                    auto expected = alias == 1 ? p : alias == 2 ? o : alias == 3 ? m : background;
                    const float amount = flags == 0 ? 0.0f : flags == 1 ? 0.35f : flags == 2 ? 1.4f : -0.25f;
                    const bool masked = (flags & 1) != 0, inverted = (flags & 2) != 0;

                    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
                            if (!inside(x, y)) {
                                continue;
                            }

                            size_t pixel = (y * W + x) * 4;
                            float weight = masked ? (hasMask ? m[pixel + 3] : 0) : 1;

                            if (masked && inverted) {
                                weight = 1 - weight;
                            }

                            const float alpha = amount * weight;

                            for (size_t c = 0; c < 4; ++c) {
                                expected[pixel + c] = p[pixel + c] * alpha + (hasOriginal ? o[pixel + c] : 0) * (1 - alpha);
                            }
                        }

                    operations.mix(processed, hasOriginal ? &original : nullptr, hasMask ? &mask : nullptr, *destination, ROI, amount, masked, inverted);
                    maximum = std::max(maximum, compare(*destination, expected));
                    ++cases;
                }
            }
        }
    }

    for (int alias = 0; alias < 3; ++alias) for (int present = 0; present < 2; ++present) for (uint32_t channels = 0; channels < 16; ++channels) {
                upload(processed, p);
                upload(original, o);
                upload(output, background);
                auto* destination = alias == 1 ? &processed : alias == 2 ? &original : &output;
                auto expected = alias == 1 ? p : alias == 2 ? o : background;

                for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) {
                        if (!inside(x, y)) {
                            continue;
                        }

                        for (uint32_t c = 0; c < 4; ++c) {
                            size_t i = (y * W + x) * 4 + c;
                            expected[i] = (channels & (1u << c)) ? (present ? o[i] : 0) : p[i];
                        }
                    }

                operations.restore(processed, present ? &original : nullptr, *destination, ROI, channels);
                maximum = std::max(maximum, compare(*destination, expected));
                ++cases;
            }

    for (int alias = 0; alias < 2; ++alias) {
        upload(processed, p);
        upload(output, background);
        auto& destination = alias ? processed : output;
        auto expected = alias ? p : background;

        for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) if (inside(x, y)) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        size_t i = (y * W + x) * 4 + c;
                        expected[i] = p[i] * gain[c] + offset[c];
                    }
                }

        operations.transform(processed, destination, ROI, gain, offset);
        maximum = std::max(maximum, compare(destination, expected));
        ++cases;
    }

    // Partial compute on a new output initializes untouched pixels to zero.
    VulkanImage fresh(device, W, H);
    auto expected = std::vector<float>(p.size(), 0);
    upload(processed, p);

    for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x) if (inside(x, y)) {
                for (uint32_t c = 0; c < 4; ++c) {
                    size_t i = (y * W + x) * 4 + c;
                    expected[i] = p[i] * gain[c] + offset[c];
                }
            }

    operations.transform(processed, fresh, ROI, gain, offset);
    maximum = std::max(maximum, compare(fresh, expected));
    ++cases;
    return maximum;
}
}
int main(int argc, char** argv)
{
    try {
        int index = -1;

        if (argc == 3 && std::string(argv[1]) == "--gpu") {
            index = std::stoi(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: NatronVulkanImageOperations [--gpu index]");
        }

        InstanceOptions options;
        options.validation = true;
        auto instance = VulkanInstance::create(options);
        unsigned cases = 0;
        float maximum = 0;
        std::string name;
        {
            auto device = VulkanDevice::create(instance, index);
            name = device->properties().deviceName;
            ImageOperations operations(device);
            cases += testCopy(device, operations);
            cases += testMixEndpoints(device, operations);
            maximum = testCompute(device, operations, 1, cases);
            std::vector<std::future<std::pair<float, unsigned>>> workers;

            for (int i = 2; i < 5; ++i) workers.emplace_back(std::async(std::launch::async, [ &, i] {
                unsigned count = 0;
                float error = testCompute(device, operations, i, count);
                return std::make_pair(error, count);
            }));

            for (auto& worker : workers) {
                auto result = worker.get();
                maximum = std::max(maximum, result.first);
                cases += result.second;
            }

            VulkanImage a(device, W, H), b(device, W, H), wrongFormat(device, W, H, VK_FORMAT_R8G8B8A8_UNORM);
            rejects([&] { operations.copy(a, b, ROI, 0, 0); });
            a.clear({0, 0, 0, 0});
            rejects([&] { operations.copy(a, b, {0, 0, 0, 1}, 0, 0); });
            rejects([&] { operations.copy(a, b, {UINT32_MAX, 0, 2, 2}, 0, 0); });
            rejects([&] { operations.copy(a, b, ROI, UINT32_MAX, 0); });
            rejects([&] { operations.copy(a, wrongFormat, ROI, 0, 0); });
            rejects([&] { operations.restore(a, nullptr, b, ROI, 16); });
            rejects([&] { operations.mix(a, nullptr, nullptr, b, ROI, std::numeric_limits<float>::quiet_NaN(), false, false); });
            rejects([&] { operations.transform(a, b, ROI, {1, 1, 1, 1}, {0, 0, 0, std::numeric_limits<float>::infinity()}); });
            auto secondDevice = VulkanDevice::create(instance, index);
            VulkanImage foreign(secondDevice, W, H);
            rejects([&] { operations.copy(a, foreign, ROI, 0, 0); });
            rejects([&] { operations.restore(a, &foreign, b, ROI, 1); });
            rejects([&] { operations.halve(a, b, {0, 0, 1, 1}, -2, 0); });
            rejects([&] { operations.halve(a, b, {0, 0, 1, 1}, 0, static_cast<int32_t>(H)); });
            rejects([&] { operations.halve(a, b, {0, 0, 0, 1}, 0, 0); });
            rejects([&] { operations.halve(a, wrongFormat, {0, 0, 1, 1}, 0, 0); });
            rejects([&] { operations.halve(a, foreign, {0, 0, 1, 1}, 0, 0); });
            rejects([&] { operations.halve(a, b, {0, 0, 1, 1}, INT32_MAX, INT32_MIN); });
            rejects([&] { operations.upscale(a, b, {0, 0, 1, 1}, 0, 0, 0); });
            rejects([&] { operations.upscale(a, b, {0, 0, 1, 1}, 0, 0, 31); });
            rejects([&] { operations.upscale(a, b, {0, 0, 1, 1}, -1, 0, 1); });
            rejects([&] { operations.upscale(a, b, {0, 0, 1, 1}, static_cast<int32_t>(W * 2), 0, 1); });
            rejects([&] { operations.upscale(a, wrongFormat, {0, 0, 1, 1}, 0, 0, 1); });
            rejects([&] { operations.upscale(a, foreign, {0, 0, 1, 1}, 0, 0, 1); });
            device->waitIdle();
        }
        require(instance->validationEnabled() && instance->validationErrorCount() == 0, "Vulkan validation reported image operation errors.");
#ifdef _WIN32
        require(GetModuleHandleW(L"opengl32.dll") == nullptr, "Native image operations loaded OpenGL.");
#endif
        std::cout << "{\"passed\":true,\"device\":\"" << name << "\",\"referenceCases\":" << cases
                  << ",\"invalidCases\":22,\"hostWorkers\":4,\"maxAbsoluteError\":" << maximum
                  << ",\"validationErrors\":0,\"openGLLoaded\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
