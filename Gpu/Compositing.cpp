// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/Compositing.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Natron
{
namespace Gpu
{
    namespace Cards
    {
        void validateCompositing(const Compositing& v)
        {
            if (int(v.role) < 0 || int(v.role) > 4 || !std::isfinite(v.shadowDensity) || v.shadowDensity < 0
                || v.shadowDensity > 10 || !std::isfinite(v.shadowTint.x) || !std::isfinite(v.shadowTint.y)
                || !std::isfinite(v.shadowTint.z) || v.shadowTint.x < 0 || v.shadowTint.y < 0
                || v.shadowTint.z < 0 || !std::isfinite(v.reflectionStrength) || v.reflectionStrength < 0
                || v.reflectionStrength > 10 || !std::isfinite(v.reflectionRoughness)
                || v.reflectionRoughness < 0 || v.reflectionRoughness > 1)
                throw std::invalid_argument("Invalid 3D compositing role or catcher controls");
        }
        float shadowOpacity(double shadowed, double reference, double density)
        {
            if (!std::isfinite(shadowed) || !std::isfinite(reference) || reference <= 1e-8)
                return 0;
            return float(std::clamp((1 - std::max(0., shadowed) / reference) * density, 0., 1.));
        }
        void makeReflectionPreview(std::vector<float>& pixels)
        {
            if (pixels.size() % 4)
                throw std::invalid_argument("Reflection preview requires RGBA storage");
            for (size_t i = 0; i < pixels.size(); i += 4)
            {
                for (int c = 0; c < 3; ++c)
                {
                    const float v = pixels[i + c];
                    pixels[i + c] = .5f + .5f * v / (1 + std::abs(v));
                }
                pixels[i + 3] = 1;
            }
        }
        std::array<float, 4> compositePlatePixel(
            const float* f, const float* s, const float* plate, const float* reflection)
        {
            const float zero[4] {};
            if (!s)
                s = zero;
            const float a = std::clamp(s[3], 0.f, 1.f), transmission = 1 - std::clamp(f[3], 0.f, 1.f);
            std::array<float, 4> out;
            for (int c = 0; c < 3; ++c)
                out[c] = f[c]
                    + transmission
                        * ((plate ? plate[c] * (1 - a) + s[c] * plate[3] : s[c])
                            + (reflection ? reflection[c] * (plate ? plate[3] : 1.f) : 0.f));
            out[3] = f[3] + transmission * (plate ? plate[3] : a);
            return out;
        }
        void compositePlate(std::vector<float>& foreground, const std::vector<float>& shadow,
            const Texture* plate, int width, int height, const std::vector<float>& reflection)
        {
            if (width <= 0 || height <= 0 || foreground.size() != size_t(width) * height * 4
                || (!shadow.empty() && shadow.size() != foreground.size())
                || (!reflection.empty() && reflection.size() != foreground.size()))
                throw std::invalid_argument("Invalid 3D plate/shadow image dimensions");
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    const size_t i = (size_t(y) * width + x) * 4;
                    std::array<float, 4> bg {};
                    if (plate)
                        bg = plate->sample((x + .5) / width, (y + .5) / height);
                    const auto p = compositePlatePixel(foreground.data() + i,
                        shadow.empty() ? nullptr : shadow.data() + i, plate ? bg.data() : nullptr,
                        reflection.empty() ? nullptr : reflection.data() + i);
                    std::copy(p.begin(), p.end(), foreground.data() + i);
                }
        }
    }
}
}
