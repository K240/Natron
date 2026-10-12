// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_VIEWERRENDERER_H
#define NATRON_GPU_VIEWERRENDERER_H
#include "Gpu/VulkanDevice.h"

namespace Natron
{
namespace Gpu
{
enum class ViewerLut { SRGB = 0, Linear, Rec709, BT1886 };
enum class ViewerPremult { Opaque = 0, Premultiplied, Straight };
enum class ViewerOperator { None = 0, WipeUnder, WipeOver, WipeMinus, WipeOnion,
                            StackUnder, StackOver, StackMinus, StackOnion
                          };
struct ViewerInput {
    VulkanImage *image = nullptr;
    // Canonical rectangles: left, bottom, right, top. Image row zero is bottom.
    std::array<float, 4> bounds{};
    std::array<float, 4> clip{};
    std::array<float, 4> format{};
    // Optional canonical pixel size X/Y and integer image origin X/Y. Using
    // the same grid keeps padded cache tiles and cropped snapshots aligned.
    std::array<float, 4> pixelGrid{};
    float gain = 1, offset = 0, gamma = 1;
    ViewerLut lut = ViewerLut::Linear;
    ViewerPremult premult = ViewerPremult::Opaque;
    bool transform = true;
    bool linearFilter = false;
    bool topDown = false; // Raster overlays use top-to-bottom rows.
};
struct ViewerSettings {
    std::array<float, 4> viewport{0, 0, 1, 1};
    std::array<float, 4> background{0, 0, 0, 1};
    std::array<float, 4> checkerA{0.25f, 0.25f, 0.25f, 1};
    std::array<float, 4> checkerB{0.5f, 0.5f, 0.5f, 1};
    // Canonical center X/Y, angle in radians, B mix.
    std::array<float, 4> wipe{0.5f, 0.5f, 0, 1};
    ViewerOperator operation = ViewerOperator::None;
    bool checkerboard = false;
    // Composite a partial image or raster overlay over an initialized target.
    bool preserveDestination = false;
    uint32_t checkerSize = 32; // Physical viewport pixels.
};
class ViewerRenderer
{
public:
    explicit ViewerRenderer(std::shared_ptr<VulkanDevice> device);
    ~ViewerRenderer();
    ViewerRenderer(const ViewerRenderer &) = delete;
    ViewerRenderer &operator=(const ViewerRenderer &) = delete;
    void render(const std::array<ViewerInput, 2> &inputs,
                const ViewerSettings& settings, VulkanImage& destination);
private:
    struct Private;
    std::unique_ptr<Private> _imp;
};
} // namespace Gpu
} // namespace Natron
#endif
