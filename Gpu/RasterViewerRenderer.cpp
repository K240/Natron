// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/RasterViewerRenderer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
namespace Natron
{
namespace Gpu
{
namespace
{
using Color = std::array<float, 4>;
bool inside(float x, float y, const Color& rect)
{
    return x >= rect[0] && y >= rect[1] && x < rect[2] && y < rect[3];
}
bool finite(const Color& color)
{
    return std::all_of(color.begin(), color.end(), [](float c) {
        return std::isfinite(c);
    });
}
Color mix(const Color& a, const Color& b, float amount)
{
    Color result;

    for (size_t c = 0; c < 4; ++c) {
        result[c] = a[c] * (1 - amount) + b[c] * amount;
    }

    return result;
}
Color texel(const RasterViewerInput& input, int64_t x, int64_t y, bool border)
{
    if (border && (x < 0 || y < 0 || x >= input.width || y >= input.height)) return {};

    x = std::clamp<int64_t>(x, 0, input.width - 1);

    y = std::clamp<int64_t>(y, 0, input.height - 1);

    const size_t at = (size_t(y) * input.width + size_t(x)) * 4;

    Color color;

    if (input.storage == ViewerStorage::BGRA8) {
        color = {input.pixels[at + 2] / 255.f, input.pixels[at + 1] / 255.f,
                 input.pixels[at] / 255.f, input.pixels[at + 3] / 255.f
                };
    } else {
        std::memcpy(color.data(), input.pixels + at * sizeof(float), sizeof(color));
    }

    return color;
}
float displayChannel(float value, ViewerLut lut, float gamma)
{
    if (lut == ViewerLut::SRGB) {
        value = value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1.f / 2.4f) - .055f;
    } else if (lut == ViewerLut::Rec709) {
        value = value < .018f ? 4.5f * value : 1.099f * std::pow(value, .45f) - .099f;
    } else if (lut == ViewerLut::BT1886) {
        value = std::pow(std::max(value, 0.f), 1.f / 2.4f);
    }

    if (gamma <= 0) {
        return value >= 1 ? 1.f : 0.f;
    }

    return gamma == 1 ? value : std::pow(std::max(value, 0.f), 1 / gamma);
}
Color sample(const RasterViewerInput& input, float x, float y)
{
    const auto& display = input.display;
    float px = (x - display.bounds[0]) / (display.bounds[2] - display.bounds[0]) * input.width;
    float py = (y - display.bounds[1]) / (display.bounds[3] - display.bounds[1]) * input.height;
    float ox = 0, oy = 0;
    const bool border = display.pixelGrid[0] > 0;

    if (border) {
        px = x / display.pixelGrid[0];
        py = y / display.pixelGrid[1];
        ox = display.pixelGrid[2];
        oy = display.pixelGrid[3];
    }

    if (display.topDown) {
        py = input.height + 2 * oy - py;
    }

    Color color;

    if (display.linearFilter) {
        px -= .5f;
        py -= .5f;
        const float fx = px - std::floor(px), fy = py - std::floor(py);
        const int64_t bx = int64_t(std::floor(px)) - int64_t(ox), by = int64_t(std::floor(py)) - int64_t(oy);
        color = mix(mix(texel(input, bx, by, border), texel(input, bx + 1, by, border), fx),
                    mix(texel(input, bx, by + 1, border), texel(input, bx + 1, by + 1, border), fx), fy);
    } else {
        color = texel(input, int64_t(std::floor(px)) - int64_t(ox), int64_t(std::floor(py)) - int64_t(oy), border);
    }

    if (!finite(color)) return {1, 0, 1, 1};

    if (display.transform) {
        for (size_t c = 0; c < 3; ++c) {
            color[c] = displayChannel(color[c] * display.gain + display.offset, display.lut, display.gamma);
        }
    }

    return finite(color) ? color : Color{1, 0, 1, 1};
}
Color over(const Color& color, const Color& base, ViewerPremult premult)
{
    if (premult == ViewerPremult::Opaque) {
        return color;
    }

    Color result;

    for (size_t c = 0; c < 4; ++c) {
        result[c] = (premult == ViewerPremult::Premultiplied ? color[c] : color[c] * color[3]) + base[c] * (1 - color[3]);
    }

    return result;
}
}
void renderRasterViewer(const std::array<RasterViewerInput, 2> &inputs, const ViewerSettings& settings,
                        uint32_t width, uint32_t height, std::vector<float> &destination)
{
    if (!width || !height || size_t(width) > std::numeric_limits<size_t>::max() / height / 4 ||
        settings.viewport[2] <= settings.viewport[0] || settings.viewport[3] <= settings.viewport[1] ||
        !finite(settings.viewport) || !finite(settings.wipe) || !finite(settings.background) ||
        !finite(settings.checkerA) || !finite(settings.checkerB) || !settings.checkerSize ||
        settings.checkerSize > INT32_MAX || int(settings.operation) < 0 || int(settings.operation) > 8 ||
        settings.wipe[3] < 0 || settings.wipe[3] > 1) {
        throw std::invalid_argument("Invalid raster Viewer scene.");
    }

    const size_t count = size_t(width) * height * 4;

    if (settings.preserveDestination && destination.size() != count) {
        throw std::invalid_argument("Raster overlay requires an initialized destination.");
    }

    destination.resize(count);

    for (const auto& input : inputs) {
        if (!input.pixels) {
            continue;
        }

        const auto& display = input.display;

        if (!input.width || !input.height || !finite(display.bounds) || !finite(display.clip) ||
            !finite(display.format) || !finite(display.pixelGrid) ||
            display.bounds[2] <= display.bounds[0] || display.bounds[3] <= display.bounds[1] ||
            display.clip[2] < display.clip[0] || display.clip[3] < display.clip[1] ||
            display.format[2] < display.format[0] || display.format[3] < display.format[1] ||
            int(display.premult) < 0 || int(display.premult) > 2 || int(display.lut) < 0 || int(display.lut) > 3 ||
            !std::isfinite(display.gain) || !std::isfinite(display.offset) || !std::isfinite(display.gamma) ||
            display.pixelGrid[0] < 0 || display.pixelGrid[1] < 0 ||
            ((display.pixelGrid[0] == 0) != (display.pixelGrid[1] == 0)) ||
            (input.storage != ViewerStorage::BGRA8 && input.storage != ViewerStorage::RGBA32F)) {
            throw std::invalid_argument("Invalid raster Viewer input.");
        }
    }

    const auto& a = inputs[0];
    const auto& b = inputs[1];
    const int op = int(settings.operation);

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float fx = (x + .5f) / width, fy = (y + .5f) / height;
            const float px = settings.viewport[0] * (1 - fx) + settings.viewport[2] * fx;
            const float py = settings.viewport[3] * (1 - fy) + settings.viewport[1] * fy;
            const bool va = a.pixels && inside(px, py, a.display.bounds) && inside(px, py, a.display.clip);
            const bool vb = b.pixels && inside(px, py, b.display.bounds) && inside(px, py, b.display.clip);
            const Color ca = va ? sample(a, px, py) : Color{}, cb = vb ? sample(b, px, py) : Color{};
            const bool stack = op >= 5;
            const bool right = (px - settings.wipe[0]) * std::cos(settings.wipe[2]) +
                               (py - settings.wipe[1]) * std::sin(settings.wipe[2]) >= 0;
            const bool compare = op != 0 && (stack || right);
            const size_t at = (size_t(y) * width + x) * 4;
            Color base = settings.background;

            if (settings.preserveDestination) {
                std::copy_n(destination.data() + at, 4, base.data());
            }

            if (settings.checkerboard && a.pixels && inside(px, py, a.display.format) && !compare) {
                base = ((x / settings.checkerSize + y / settings.checkerSize) & 1) ? settings.checkerB : settings.checkerA;
            }

            if (!compare) {
                if (va) {
                    base = over(ca, base, settings.checkerboard || settings.preserveDestination ? a.display.premult : ViewerPremult::Opaque);
                }
            } else {
                const int operation = stack ? op - 4 : op;
                const float amount = settings.wipe[3];

                if (operation == 2 && vb) {
                    base = mix(base, cb, amount);
                }

                if (va) {
                    base = over(ca, base, a.display.premult);
                }

                if (vb) {
                    if (operation == 1) {
                        base = mix(base, cb, amount);
                    } else if (operation == 3 || operation == 4)
                        for (size_t c = 0; c < 4; ++c) {
                            base[c] += cb[c] * amount * (operation == 3 ? -1 : 1);
                        }
                }
            }

            if (!finite(base)) base = {1, 0, 1, 1};

            std::copy(base.begin(), base.end(), destination.data() + at);
        }
    }
}
}
}
