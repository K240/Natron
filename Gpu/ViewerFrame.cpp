// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/ViewerFrame.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Natron
{
namespace Gpu
{
ViewerFrame::ViewerFrame(std::shared_ptr<VulkanDevice> device, ViewerPixelBounds bounds, ViewerStorage storage)
    : _bounds(bounds), _storage(storage)
{
    const int64_t width = static_cast<int64_t>(bounds[2]) - bounds[0];
    const int64_t height = static_cast<int64_t>(bounds[3]) - bounds[1];

    if (width <= 0 || height <= 0 || width > UINT32_MAX || height > UINT32_MAX ||
        (storage != ViewerStorage::BGRA8 && storage != ViewerStorage::RGBA32F)) {
        throw std::invalid_argument("Invalid Viewer frame bounds or storage.");
    }

    if (!device) {
        const size_t pixelSize = storage == ViewerStorage::BGRA8 ? 4 : 16;

        if (size_t(width) > std::numeric_limits<size_t>::max() / height / pixelSize) {
            throw std::overflow_error("Raster Viewer frame is too large.");
        }

        _pixels.resize(size_t(width) * height * pixelSize, 0);
        return;
    }

    const auto format = storage == ViewerStorage::BGRA8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32G32B32A32_SFLOAT;
    _image = std::make_shared<VulkanImage>(std::move(device), static_cast<uint32_t>(width), static_cast<uint32_t>(height), format);
    _image->clear({0, 0, 0, 0}); // Missing cache tiles must not expose uninitialized VRAM.
}
void ViewerFrame::uploadTile(const ViewerPixelBounds& tile, const void* bytes, size_t byteCount)
{
    if (!bytes || tile[0] < _bounds[0] || tile[1] < _bounds[1] || tile[2] > _bounds[2] || tile[3] > _bounds[3] ||
        tile[0] >= tile[2] || tile[1] >= tile[3]) {
        throw std::invalid_argument("Viewer tile is outside the allocated frame.");
    }

    const auto width = static_cast<uint32_t>(static_cast<int64_t>(tile[2]) - tile[0]);
    const auto height = static_cast<uint32_t>(static_cast<int64_t>(tile[3]) - tile[1]);
    const size_t pixelSize = _storage == ViewerStorage::BGRA8 ? 4 : 16;

    if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / height / pixelSize ||
        byteCount != static_cast<size_t>(width) * height * pixelSize) {
        throw std::invalid_argument("Viewer tile byte count does not match its bounds.");
    }

    const auto x = static_cast<uint32_t>(static_cast<int64_t>(tile[0]) - _bounds[0]);
    const auto y = static_cast<uint32_t>(static_cast<int64_t>(tile[1]) - _bounds[1]);

    if (!_image) {
        const size_t rowSize = size_t(width) * pixelSize;
        const size_t stride = size_t(int64_t(_bounds[2]) - _bounds[0]) * pixelSize;
        const auto* source = static_cast<const unsigned char *>(bytes);

        for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(_pixels.data() + size_t(y + row) * stride + size_t(x) * pixelSize,
                        source + size_t(row) * rowSize, rowSize);
        }

        return;
    }

    if (_storage == ViewerStorage::BGRA8) {
        // Vulkan's guaranteed RGBA8 format avoids requiring optional BGRA8
        // storage support. Swap channels only; keep Engine display values intact.
        const auto* source = static_cast<const unsigned char *>(bytes);
        std::vector<unsigned char> rgba(byteCount);

        for (size_t i = 0; i < byteCount; i += 4) {
            rgba[i] = source[i + 2];
            rgba[i + 1] = source[i + 1];
            rgba[i + 2] = source[i];
            rgba[i + 3] = source[i + 3];
        }

        _image->uploadRegion(x, y, width, height, rgba.data(), rgba.size());
    } else {
        _image->uploadRegion(x, y, width, height, bytes, byteCount);
    }
}
} // namespace Gpu
} // namespace Natron
