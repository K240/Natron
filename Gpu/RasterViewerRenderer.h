// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_RASTERVIEWERRENDERER_H
#define NATRON_GPU_RASTERVIEWERRENDERER_H
#include "Gpu/ViewerFrame.h"
#include "Gpu/ViewerRenderer.h"
namespace Natron
{
namespace Gpu
{
struct RasterViewerInput {
    ViewerInput display;
    const unsigned char *pixels = nullptr;
    uint32_t width = 0, height = 0;
    ViewerStorage storage = ViewerStorage::BGRA8;
};
// CPU display path for machines without a usable Vulkan device. Rows in the
// destination are top-down; cached source tiles retain the Engine's pixel grid.
void renderRasterViewer(const std::array<RasterViewerInput, 2> &inputs,
                        const ViewerSettings& settings, uint32_t width, uint32_t height,
                        std::vector<float> &destination);
}
}
#endif
