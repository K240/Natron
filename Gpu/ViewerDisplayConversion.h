// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_VIEWERDISPLAYCONVERSION_H
#define NATRON_GPU_VIEWERDISPLAYCONVERSION_H
#include "Gpu/ImageOperations.h"
namespace Natron
{
namespace Gpu
{
struct ViewerDisplayTables {
    // Natron's float high-word lookup preserves the byte LUT's round trips.
    std::array<uint32_t, 65536> color{};
    std::array<float, 1024> gamma{};
};
struct ViewerDisplayCorrection {
    float gain = 1, offset = 0, gamma = 1;
    bool linear = false, opaque = false;
    int32_t originX = 0, originY = 0;
};
// RGBA32F Engine pixels -> encoded RGBA8 display snapshot, without CPU pixels.
// Each row keeps the byte Viewer's bidirectional error diffusion; a stable
// start point prevents noise from changing when a cached image is redisplayed.
class ViewerDisplayConversion
{
public:
    explicit ViewerDisplayConversion(std::shared_ptr<VulkanDevice> device);
    ~ViewerDisplayConversion();
    ViewerDisplayConversion(const ViewerDisplayConversion &) = delete;
    ViewerDisplayConversion &operator=(const ViewerDisplayConversion &) = delete;
    void render(VulkanImage& source, VulkanImage& destination, ImageRegion region,
                const ViewerDisplayCorrection& correction, const ViewerDisplayTables& tables);
private:
    struct Impl;
    std::unique_ptr<Impl> _imp;
};
}
}
#endif
