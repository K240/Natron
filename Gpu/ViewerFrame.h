// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_VIEWERFRAME_H
#define NATRON_GPU_VIEWERFRAME_H
#include "Gpu/VulkanDevice.h"

namespace Natron
{
namespace Gpu
{
// Pixel bounds at the Engine's current mipmap level: left, bottom, right, top.
using ViewerPixelBounds = std::array<int32_t, 4>;
enum class ViewerStorage { BGRA8, RGBA32F };
class ViewerFrame
{
public:
    ViewerFrame(std::shared_ptr<VulkanDevice> device, ViewerPixelBounds bounds, ViewerStorage storage);
    const std::shared_ptr<VulkanImage> &image() const
    {
        return _image;
    }
    const ViewerPixelBounds &bounds() const
    {
        return _bounds;
    }
    ViewerStorage storage() const
    {
        return _storage;
    }
    // Available only for a CPU frame, constructed with a null device.
    const std::vector<unsigned char> &pixels() const
    {
        return _pixels;
    }
    void uploadTile(const ViewerPixelBounds& tile, const void* bytes, size_t byteCount);
private:
    ViewerPixelBounds _bounds;
    ViewerStorage _storage;
    std::shared_ptr<VulkanImage> _image;
    std::vector<unsigned char> _pixels;
};
} // namespace Gpu
} // namespace Natron
#endif
