// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_COMPOSITING_H
#define NATRON_GPU_COMPOSITING_H
#include "Gpu/CardScene.h"
namespace Natron
{
namespace Gpu
{
    namespace Cards
    {
        void validateCompositing(const Compositing& value);
        float shadowOpacity(double shadowed, double reference, double density);
        void makeReflectionPreview(std::vector<float>& pixels);
        // Scene-linear RGBA, with an optional signed additive reflection RGB
        // delta (its alpha is ignored). Preserve plate/foreground coverage.
        std::array<float, 4> compositePlatePixel(const float* foreground, const float* shadow,
            const float* plate, const float* reflection = nullptr);
        void compositePlate(std::vector<float>& foreground, const std::vector<float>& shadow,
            const Texture* plate, int width, int height, const std::vector<float>& reflection = {});
    }
}
}
#endif
