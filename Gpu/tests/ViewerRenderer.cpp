// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ViewerRenderer.h"
#include "Gpu/ViewerFrame.h"
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
using Color = std::array<float, 4>;
using Rect = std::array<float, 4>;
struct Checks {
    size_t frames = 0, channels = 0, tileUploads = 0;
    float maxError = 0;
    void pixel(const std::vector<float> &pixels, size_t index, const Color& expected, const char* name)
    {
        for (size_t c = 0; c < 4; ++c) {
            const auto actual = pixels.at(index * 4 + c);
            const auto error = std::abs(actual - expected[c]);

            if (!std::isfinite(actual) || error > 0.00002f * std::max(1.0f, std::abs(expected[c]))) {
                std::cerr << name << " pixel " << index << " channel " << c << ": " << actual << " expected " << expected[c] << std::endl;
                throw std::runtime_error("Viewer pixels differ from CPU expectations.");
            }

            ++channels;
            maxError = std::max(maxError, error);
        }
    }
    std::vector<float> draw(ViewerRenderer& renderer, const std::array<ViewerInput, 2> &inputs,
                            const ViewerSettings& settings, VulkanImage& target)
    {
        renderer.render(inputs, settings, target);
        const auto bytes = target.download();
        std::vector<float> result(bytes.size() / sizeof(float));
        std::memcpy(result.data(), bytes.data(), bytes.size());
        ++frames;
        return result;
    }
};
ViewerInput input(VulkanImage& image, Rect bounds = {0, 0, 2, 2})
{
    ViewerInput result;
    result.image = &image;
    result.bounds = result.clip = result.format = bounds;
    return result;
}
void upload(VulkanImage& image, const std::vector<Color> &colors)
{
    static_assert(sizeof(Color) == sizeof(float) * 4, "Color padding is unsupported.");
    image.upload(colors.data(), colors.size() * sizeof(Color));
}
void geometry(const std::shared_ptr<VulkanDevice> &device, ViewerRenderer& renderer, Checks& checks)
{
    VulkanImage source(device, 2, 2), target(device, 2, 2);
    const std::vector<Color> bottomUp{{1, 0, 0, 0.25f}, {0, 1, 0, 0.5f}, {0, 0, 1, 0.75f}, {1, 1, 1, 1}};
    upload(source, bottomUp);
    std::array<ViewerInput, 2> inputs{input(source, {10, -8, 14, 0}), {}};
    ViewerSettings settings;
    // Nonzero origin, PAR 2 and mipmap scale 2 are expressed in canonical
    // bounds. Texture resolution must never be mistaken for canonical size.
    settings.viewport = inputs[0].bounds;
    auto pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 4; ++i) {
        checks.pixel(pixels, i, bottomUp[i ^ 2], "Bottom-up canonical frame");
    }

    settings.viewport = {10, -4, 12, 0};
    VulkanImage zoom(device, 3, 5);
    pixels = checks.draw(renderer, inputs, settings, zoom);

    for (size_t i = 0; i < 15; ++i) {
        checks.pixel(pixels, i, bottomUp[2], "Panned/zoomed frame");
    }

    settings.viewport = inputs[0].bounds;
    inputs[0].clip = {10, -8, 12, 0};
    pixels = checks.draw(renderer, inputs, settings, target);
    checks.pixel(pixels, 0, bottomUp[2], "ROI left top");
    checks.pixel(pixels, 2, bottomUp[0], "ROI left bottom");
    checks.pixel(pixels, 1, settings.background, "ROI right top");
    checks.pixel(pixels, 3, settings.background, "ROI right bottom");
    inputs[0].clip = {10, -8, 10, 0}; // Empty ROI is a valid invisible input.
    pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 4; ++i) {
        checks.pixel(pixels, i, settings.background, "Empty ROI");
    }

    inputs[0].clip = inputs[0].bounds;
    inputs[0].linearFilter = true;
    VulkanImage filtered(device, 5, 5);
    pixels = checks.draw(renderer, inputs, settings, filtered);

    for (size_t y = 0; y < 5; ++y) {
        for (size_t x = 0; x < 5; ++x) {
            // The four texels encode an analytic bilinear surface, so this
            // expectation is independent of shader sampling/interpolation code.
            const float s = std::clamp((x + 0.5f) * 2 / 5 - 0.5f, 0.0f, 1.0f);
            const float t = std::clamp((4 - y + 0.5f) * 2 / 5 - 0.5f, 0.0f, 1.0f);
            checks.pixel(pixels, y * 5 + x, {(1 - s) * (1 - t) + s * t, s, t, 0.25f + 0.25f * s + 0.5f * t}, "Bilinear surface");
        }
    }

    inputs[0].clip = {10, -8, 10, 0};
    settings.checkerboard = true;
    settings.checkerSize = 2;
    inputs[0].format = {10, -8, 12, 0};
    pixels = checks.draw(renderer, inputs, settings, filtered);

    for (size_t y = 0; y < 5; ++y) {
        for (size_t x = 0; x < 5; ++x) {
            const Color expected = x < 2 ? ((x / 2 + y / 2) % 2 ? settings.checkerB : settings.checkerA) : settings.background;
            checks.pixel(pixels, y * 5 + x, expected, "Checkerboard format clipping");
        }
    }
}
double displayValue(double value, ViewerLut lut, double gamma)
{
    switch (lut) {
    case ViewerLut::SRGB:
        value = value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1 / 2.4) - 0.055;
        break;

    case ViewerLut::Rec709:
        value = value < 0.018 ? value * 4.5 : 1.099 * std::pow(value, 0.45) - 0.099;
        break;

    case ViewerLut::BT1886:
        value = std::pow(std::max(0.0, value), 1 / 2.4);
        break;

    case ViewerLut::Linear:
        break;
    }

    if (gamma <= 0) {
        return value >= 1 ? 1 : 0;
    }

    return gamma == 1 ? value : std::pow(std::max(0.0, value), 1 / gamma);
}
void color(const std::shared_ptr<VulkanDevice> &device, ViewerRenderer& renderer, Checks& checks)
{
    const std::vector<float> values{-0.1f, 0, 0.002f, 0.004f, 0.017f, 0.02f, 0.18f, 0.5f, 1, 2, 8};
    VulkanImage source(device, static_cast<uint32_t>(values.size()), 1), target(device, static_cast<uint32_t>(values.size()), 1);
    std::vector<Color> colors;

    for (float value : values) {
        colors.push_back({value, value, value, 0.37f});
    }

    upload(source, colors);
    std::array<ViewerInput, 2> inputs{input(source), {}};
    ViewerSettings settings;
    settings.viewport = inputs[0].bounds;

    for (auto lut : {
             ViewerLut::SRGB, ViewerLut::Linear, ViewerLut::Rec709, ViewerLut::BT1886
         }) {
        for (float gamma : {
                 1.0f, 0.5f, 2.0f, 0.0f, -1.0f
             }) {
            for (const auto& correction : {
                     std::array<float, 2> {1, 0}, std::array<float, 2> {1.3f, 0.05f}, std::array<float, 2> {-0.7f, -0.01f}
                 }) {
                inputs[0].lut = lut;
                inputs[0].gamma = gamma;
                inputs[0].gain = correction[0];
                inputs[0].offset = correction[1];
                const auto pixels = checks.draw(renderer, inputs, settings, target);

                for (size_t i = 0; i < values.size(); ++i) {
                    const auto value = static_cast<float>(displayValue(static_cast<double>(values[i]) * correction[0] + correction[1], lut, gamma));
                    checks.pixel(pixels, i, {value, value, value, 0.37f}, "Float display transform");
                }
            }
        }
    }
    VulkanImage byteSource(device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM), byteTarget(device, 1, 1);
    const std::array<unsigned char, 4> bytes{20, 80, 160, 128};
    byteSource.upload(bytes.data(), bytes.size());
    inputs[0] = input(byteSource);
    inputs[0].transform = false; // Already transformed by the Engine's byte path.
    inputs[0].gain = 100;
    inputs[0].offset = -10;
    inputs[0].gamma = 2;
    inputs[0].lut = ViewerLut::SRGB;
    checks.pixel(checks.draw(renderer, inputs, settings, byteTarget), 0, {20 / 255.0f, 80 / 255.0f, 160 / 255.0f, 128 / 255.0f}, "Byte transform bypass");

    for (float invalid : {
             std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()
         }) {
        colors[0][1] = invalid;
        upload(source, colors);
        inputs[0] = input(source);
        checks.pixel(checks.draw(renderer, inputs, settings, target), 0, {1, 0, 1, 1}, "Nonfinite display diagnostic");
    }
    colors[0] = {std::numeric_limits<float>::max(), 0, 0, 1};
    upload(source, colors);
    inputs[0] = input(source);
    inputs[0].gain = 2;
    checks.pixel(checks.draw(renderer, inputs, settings, target), 0, {1, 0, 1, 1}, "Display transform overflow diagnostic");
}
void compositing(const std::shared_ptr<VulkanDevice> &device, ViewerRenderer& renderer, Checks& checks)
{
    VulkanImage a(device, 1, 1), b(device, 1, 1), target(device, 4, 2);
    upload(a, {{0.4f, 0.2f, 0.1f, 0.5f}});
    upload(b, {{0.1f, 0.6f, 0.9f, 0.25f}});
    std::array<ViewerInput, 2> inputs{input(a), input(b)};
    inputs[0].premult = ViewerPremult::Premultiplied;
    ViewerSettings settings;
    settings.viewport = {0, 0, 2, 2};
    settings.background = {0.2f, 0.2f, 0.2f, 1};
    settings.wipe = {1, 1, 0, 0.25f};
    const Color rawA{0.4f, 0.2f, 0.1f, 0.5f};
    // Closed-form results of the former Viewer blend equations for this fixture:
    // A over background = (.5,.3,.2,1); B amount = .25.
    const std::array<Color, 4> combined{{{0.4f, 0.375f, 0.375f, 0.8125f},
            {0.4875f, 0.35f, 0.2875f, 0.90625f}, {0.475f, 0.15f, -0.025f, 0.9375f}, {0.525f, 0.45f, 0.425f, 1.0625f}
        }};

    for (int op = 0; op <= 8; ++op) {
        settings.operation = static_cast<ViewerOperator>(op);
        const auto pixels = checks.draw(renderer, inputs, settings, target);

        for (size_t i = 0; i < 8; ++i) {
            const Color expected = op == 0 || (op < 5 && i % 4 < 2) ? rawA : combined[(op - 1) % 4];
            checks.pixel(pixels, i, expected, "Wipe/stack blend");
        }
    }

    // Rotate the wipe: with angle pi/2, comparison occupies the top half.
    settings.operation = ViewerOperator::WipeMinus;
    settings.wipe[2] = static_cast<float>(std::acos(-1.0) / 2);
    auto pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 8; ++i) {
        checks.pixel(pixels, i, i < 4 ? combined[2] : rawA, "Rotated wipe");
    }

    settings.operation = ViewerOperator::None;
    settings.checkerboard = true;
    settings.checkerA = settings.checkerB = settings.background;

    for (auto mode : {
             ViewerPremult::Opaque, ViewerPremult::Premultiplied, ViewerPremult::Straight
         }) {
        inputs[0].premult = mode;
        pixels = checks.draw(renderer, inputs, settings, target);
        const Color expected = mode == ViewerPremult::Opaque ? rawA :
                               mode == ViewerPremult::Premultiplied ? Color{0.5f, 0.3f, 0.2f, 1} :
                               Color{0.3f, 0.2f, 0.15f, 0.75f};

        for (size_t i = 0; i < 8; ++i) {
            checks.pixel(pixels, i, expected, "Alpha convention");
        }
    }
    settings.operation = ViewerOperator::StackUnder;
    inputs[1].image = nullptr;
    pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 8; ++i) {
        checks.pixel(pixels, i, {0.3f, 0.2f, 0.15f, 0.75f}, "Missing B");
    }

    inputs[1].image = &b;
    inputs[0].image = nullptr;
    pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 8; ++i) {
        checks.pixel(pixels, i, {0.175f, 0.3f, 0.375f, 0.8125f}, "Missing A");
    }

    inputs[1].image = nullptr;
    pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 8; ++i) {
        checks.pixel(pixels, i, settings.background, "Missing both inputs");
    }

    inputs = {input(a), input(a)};
    settings.operation = ViewerOperator::StackMinus;
    settings.wipe[3] = 1;
    pixels = checks.draw(renderer, inputs, settings, target);

    for (size_t i = 0; i < 8; ++i) {
        checks.pixel(pixels, i, {0, 0, 0, 0}, "Same-source difference");
    }
}
template<class F> void reject(F&& operation)
{
    try {
        operation();
    } catch (const std::invalid_argument &) {
        return;
    }

    throw std::runtime_error("Invalid Viewer scene was accepted.");
}
void tileFrames(const std::shared_ptr<VulkanDevice> &device, Checks& checks)
{
    const ViewerPixelBounds allocation{-7, 11, 6, 20};

    for (auto storage : {
             ViewerStorage::BGRA8, ViewerStorage::RGBA32F
         }) {
        ViewerFrame frame(device, allocation, storage);
        const size_t pixelSize = storage == ViewerStorage::BGRA8 ? 4 : 16;
        std::vector<unsigned char> expected(frame.image()->byteSize(), 0);

        if (frame.image()->download() != expected) {
            throw std::runtime_error("Missing tiles expose uninitialized image data.");
        }

        // Reverse-order tiles plus a one-pixel repaint. Arbitrary bytes also
        // cover preservation of float NaN payloads and signed zero.
        for (const auto& tile : {
                 ViewerPixelBounds{-1, 11, 6, 20}, ViewerPixelBounds{-7, 11, -1, 20}, ViewerPixelBounds{-7, 11, -6, 12}
             }) {
            const size_t w = tile[2] - tile[0], h = tile[3] - tile[1];
            std::vector<unsigned char> bytes(w * h * pixelSize);

            for (size_t i = 0; i < bytes.size(); ++i) {
                bytes[i] = static_cast<unsigned char>(i * 37 + checks.tileUploads * 71);
            }

            frame.uploadTile(tile, bytes.data(), bytes.size());
            ++checks.tileUploads;

            for (size_t y = 0; y < h; ++y) {
                for (size_t x = 0; x < w; ++x) {
                    const auto source = (y * w + x) * pixelSize;
                    const auto dest = ((y + tile[1] - allocation[1]) * 13 + x + tile[0] - allocation[0]) * pixelSize;

                    for (size_t c = 0; c < pixelSize; ++c) {
                        const size_t sc = storage == ViewerStorage::BGRA8 && (c == 0 || c == 2) ? 2 - c : c;
                        expected[dest + c] = bytes[source + sc];
                    }
                }
            }

            if (frame.image()->download() != expected) {
                throw std::runtime_error("Viewer cache tile upload was not bit-exact.");
            }
        }
        unsigned char one = 0;
        reject([&] { frame.uploadTile({-8, 11, -7, 12}, &one, 1); });
        reject([&] { frame.uploadTile({-7, 11, -6, 12}, &one, 1); });
        reject([&] { frame.uploadTile(allocation, nullptr, expected.size()); });
    }
    reject([&] { ViewerFrame invalid(device, {-7, 11, -7, 20}, ViewerStorage::BGRA8); });
}
void rasterAndPartialOverlays(const std::shared_ptr<VulkanDevice> &device, ViewerRenderer& renderer, Checks& checks)
{
    VulkanImage target(device, 2, 2), overlay(device, 2, 2, VK_FORMAT_R8G8B8A8_UNORM);
    // Top row: half-alpha premultiplied red, transparent. Bottom: opaque
    // green, quarter-alpha premultiplied blue. Alpha uses exactly representable UNORMs.
    const std::array<unsigned char, 16> bytes{128, 0, 0, 128, 0, 0, 0, 0, 0, 255, 0, 255, 0, 0, 64, 64};
    overlay.upload(bytes.data(), bytes.size());
    const Color background{0.2f, 0.3f, 0.4f, 1};
    target.clear(background);
    std::array<ViewerInput, 2> inputs{input(overlay), {}};
    inputs[0].transform = false;
    inputs[0].topDown = true;
    inputs[0].premult = ViewerPremult::Premultiplied;
    ViewerSettings settings;
    settings.viewport = inputs[0].bounds;
    settings.preserveDestination = true;
    auto pixels = checks.draw(renderer, inputs, settings, target);
    const float red = 128.f / 255, blue = 64.f / 255;
    checks.pixel(pixels, 0, {red + background[0] * (1 - red), background[1] * (1 - red), background[2] * (1 - red), 1}, "Top-down raster alpha");
    checks.pixel(pixels, 1, background, "Transparent raster preserves background");
    checks.pixel(pixels, 2, {0, 1, 0, 1}, "Top-down raster bottom row");
    const Color bottomRight{background[0] *(1 - blue), background[1]*(1 - blue), blue + background[2]*(1 - blue), 1};
    checks.pixel(pixels, 3, bottomRight, "Raster bottom-right alpha");
    VulkanImage partial(device, 1, 1);
    upload(partial, {{2, 3, 4, 1}});
    inputs = {input(partial, {0, 0, 1, 1}), {}};
    pixels = checks.draw(renderer, inputs, settings, target);
    checks.pixel(pixels, 0, {red + background[0] * (1 - red), background[1] * (1 - red), background[2] * (1 - red), 1}, "Partial preserves top row");
    checks.pixel(pixels, 1, background, "Partial preserves transparent region");
    checks.pixel(pixels, 2, {2, 3, 4, 1}, "Partial retains HDR data");
    checks.pixel(pixels, 3, bottomRight, "Partial preserves right region");
    VulkanImage uninitialized(device, 2, 2);
    reject([&] { renderer.render(inputs, settings, uninitialized); });
}

void blendOverflow(const std::shared_ptr<VulkanDevice> &device, ViewerRenderer& renderer, Checks& checks)
{
    VulkanImage first(device, 1, 1), second(device, 1, 1), target(device, 1, 1);
    upload(first, {{3e38f, 0, 0, 1}});
    upload(second, {{3e38f, 0, 0, 1}});
    std::array<ViewerInput, 2> inputs{input(first, {0, 0, 1, 1}), input(second, {0, 0, 1, 1})};
    ViewerSettings settings;
    settings.operation = ViewerOperator::StackOnion;
    checks.pixel(checks.draw(renderer, inputs, settings, target), 0, {1, 0, 1, 1}, "HDR addition overflow marker");
    upload(second, {{-3e38f, 0, 0, 1}});
    settings.operation = ViewerOperator::StackMinus;
    checks.pixel(checks.draw(renderer, inputs, settings, target), 0, {1, 0, 1, 1}, "HDR subtraction overflow marker");
}

void contracts(const std::shared_ptr<VulkanDevice> &device, ViewerRenderer& renderer)
{
    VulkanImage source(device, 1, 1), target(device, 1, 1), byteTarget(device, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
    upload(source, {{1, 1, 1, 1}});
    std::array<ViewerInput, 2> inputs{input(source), {}};
    ViewerSettings settings;
    reject([&] { renderer.render(inputs, settings, source); });
    reject([&] { renderer.render(inputs, settings, byteTarget); });
    settings.wipe[2] = std::numeric_limits<float>::quiet_NaN();
    reject([&] { renderer.render(inputs, settings, target); });
    settings = ViewerSettings{};
    settings.checkerSize = 0;
    reject([&] { renderer.render(inputs, settings, target); });
    settings = ViewerSettings{};
    settings.viewport[2] = settings.viewport[0];
    reject([&] { renderer.render(inputs, settings, target); });
    settings = ViewerSettings{};
    inputs[0].lut = static_cast<ViewerLut>(9);
    reject([&] { renderer.render(inputs, settings, target); });
    inputs[0] = input(source);
    inputs[0].clip[0] = std::numeric_limits<float>::infinity();
    reject([&] { renderer.render(inputs, settings, target); });
}
}
int main(int argc, char** argv)
{
    try {
        int index = -1;

        if (argc == 3 && std::string(argv[1]) == "--gpu") {
            index = std::stoi(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: NatronVulkanRenderer [--gpu index]");
        }

        InstanceOptions options;
        options.validation = true;
        auto instance = VulkanInstance::create(options);
        Checks checks;
        std::string name;
        {
            auto device = VulkanDevice::create(instance, index);
            name = device->properties().deviceName;
            ViewerRenderer renderer(device);
            tileFrames(device, checks);
            geometry(device, renderer, checks);
            color(device, renderer, checks);
            compositing(device, renderer, checks);
            rasterAndPartialOverlays(device, renderer, checks);
            blendOverflow(device, renderer, checks);
            contracts(device, renderer);
            device->waitIdle();
        }

        if (!instance->validationEnabled() || instance->validationErrorCount()) {
            throw std::runtime_error("Viewer validation reported errors.");
        }

#ifdef _WIN32

        if (GetModuleHandleW(L"opengl32.dll")) {
            throw std::runtime_error("OpenGL was loaded by the Vulkan renderer test.");
        }

#endif
        std::cout << "{\"passed\":true,\"device\":\"" << name << "\",\"frames\":" << checks.frames
                  << ",\"channelsCompared\":" << checks.channels << ",\"tileUploads\":" << checks.tileUploads << ",\"maxAbsoluteError\":" << checks.maxError
                  << ",\"validationErrors\":0,\"openGLLoaded\":false}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Vulkan renderer test: " << error.what() << std::endl;
        return 1;
    }
}
