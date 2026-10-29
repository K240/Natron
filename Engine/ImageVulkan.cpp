// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/Image.h"
#include "Engine/ImageParams.h"
#include "Engine/VulkanImageStorage.h"
#include "Gpu/PerformanceTrace.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <limits>
#include <stdexcept>
NATRON_NAMESPACE_ENTER
ImagePtr
Image::convertToStorage(const ImagePtr& source, StorageModeEnum mode,
                        const std::shared_ptr<VulkanImageContext> &context)
{
    Gpu::PerformanceTrace trace(mode == eStorageModeVulkan ? "engine-to-vulkan" : "engine-to-ram");

    if (!source) {
        return source;
    }

    const auto sourceMode = source->getStorageMode();

    if (sourceMode == mode && (mode != eStorageModeVulkan || source->getVulkanImage()->context() == context)) {
        return source;
    }

    if (mode != eStorageModeRAM && mode != eStorageModeVulkan) {
        throw std::invalid_argument("Unsupported native image storage boundary.");
    }

    if ((mode == eStorageModeVulkan &&
         (!context || source->getBitDepth() != eImageBitDepthFloat || source->getComponentsCount() != 4))) {
        throw std::invalid_argument("Native rendering requires RGBA float images and a runtime.");
    }

    const RectI bounds = source->getBounds();
    const auto params = makeParams(source->getRoD(), bounds, source->getPixelAspectRatio(),
                                   source->getMipmapLevel(), source->getParams()->isRodProjectFormat(),
                                   source->getComponents(), source->getBitDepth(), source->getPremultiplication(),
                                   source->getFieldingOrder(), mode, 0);
    params->getStorageInfo().vulkanContext = mode == eStorageModeVulkan ? context : nullptr;
    ImagePtr result = source->usesBitMap() ? std::make_shared<Image>(source->getKey(), params, nullptr) :
                      std::make_shared<Image>(source->getKey(), params);
    result->allocateMemory();
    result->pasteFrom(*source, bounds, true);
    return result;
}

// Lock aliases once, and multiple images in a consistent address order. This
// also prevents reciprocal copies from deadlocking on different worker threads.
class ImageVulkanLocks
{
public:
    // Valid only while this image is already locked by ImageVulkanLocks.
    static RectI bounds(const Image& image)
    {
        return image._bounds;
    }
    static const unsigned char *pixel(const Image& image, int x, int y)
    {
        return image.pixelAt(x, y);
    }
    ImageVulkanLocks(Image& destination, std::initializer_list<const Image*> inputs)
    {
        std::map<const Image *, bool, std::less<const Image *>> images;

        for (const auto * input : inputs) if (input) {
                images[input] = false;
            }

        images[&destination] = true;
        _locks.reserve(images.size());

        try {
            for (const auto& entry : images) {
                auto* lock = &entry.first->_entryLock;

                if (entry.second) {
                    lock->lockForWrite();
                } else {
                    lock->lockForRead();
                }

                _locks.push_back(lock);
            }
        } catch (...) {
            release();
            throw;
        }
    }
    ~ImageVulkanLocks()
    {
        release();
    }
private:
    void release()
    {
        for (auto it = _locks.rbegin(); it != _locks.rend(); ++it) {
            (*it)->unlock();
        }

        _locks.clear();
    }
    std::vector<QReadWriteLock *> _locks;
};
ImageParamsPtr Image::makeVulkanParams(const std::shared_ptr<VulkanImageContext> &context,
                                       const RectD& rod, const RectI& bounds, double par, unsigned int mipmap)
{
    if (!context || bounds.isNull()) {
        throw std::invalid_argument("A Vulkan image requires a runtime context and nonempty bounds.");
    }

    auto params = makeParams(rod, bounds, par, mipmap, false, ImagePlaneDesc::getRGBAComponents(), eImageBitDepthFloat,
                             eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, eStorageModeVulkan, 0);
    params->getStorageInfo().vulkanContext = context;
    return params;
}
ImagePtr Image::createVulkan(const std::shared_ptr<VulkanImageContext> &context, const RectI& bounds)
{
    return std::make_shared<Image>(ImageKey(), makeVulkanParams(context, RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds));
}
namespace
{
void requireRGBAFloat(const Image& image)
{
    if (image.getBitDepth() != eImageBitDepthFloat || image.getComponents().getNumComponents() != 4) {
        throw std::invalid_argument("Native image operation requires RGBA float data.");
    }
}
}
void Image::halveVulkan(const RectI& requested, bool copyBitmap, Image* destination) const
{
    if (!destination || destination == this || getStorageMode() != eStorageModeVulkan ||
        destination->getStorageMode() != eStorageModeVulkan) {
        throw std::invalid_argument("Native mipmap generation requires distinct Vulkan images.");
    }

    ImageVulkanLocks locks(*destination, {this});
    requireRGBAFloat(*this);
    requireRGBAFloat(*destination);

    if (!_bounds.contains(requested) || (copyBitmap && (!_useBitmap || !destination->_useBitmap))) {
        throw std::invalid_argument("Native mipmap ROI or bitmap is invalid.");
    }

    const RectI target = requested.downscalePowerOfTwoSmallestEnclosing(1).intersect(destination->_bounds);

    if (target.isNull()) {
        return;
    }

    const int64_t offsetX = int64_t(destination->_bounds.x1) * 2 - _bounds.x1;
    const int64_t offsetY = int64_t(destination->_bounds.y1) * 2 - _bounds.y1;

    if (offsetX < std::numeric_limits<int32_t>::min() || offsetX > std::numeric_limits<int32_t>::max() ||
        offsetY < std::numeric_limits<int32_t>::min() || offsetY > std::numeric_limits<int32_t>::max()) {
        throw std::invalid_argument("Native mipmap grid offset overflows.");
    }

    destination->getVulkanImage()->halve(*getVulkanImage(), target.x1 - destination->_bounds.x1,
                                         target.y1 - destination->_bounds.y1, target.width(), target.height(), int32_t(offsetX), int32_t(offsetY));

    if (copyBitmap) {
        for (int y = target.y1; y < target.y2; ++y) for (int x = target.x1; x < target.x2; ++x) {
                bool complete = true;

                for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) {
                        const int64_t sx = int64_t(x) * 2 + dx, sy = int64_t(y) * 2 + dy;

                        if (sx >= _bounds.x1 && sx < _bounds.x2 && sy >= _bounds.y1 && sy < _bounds.y2) {
                            complete = complete && *_bitmap.getBitmapAt(int(sx), int(sy)) == 1;
                        }
                    }

                *destination->_bitmap.getBitmapAt(x, y) = complete ? 1 : 0;
            }
    }
}
void Image::upscaleVulkan(const RectI& requested, unsigned int levels, Image* destination) const
{
    requireRGBAFloat(*this);
    requireRGBAFloat(*destination);

    if (!levels || levels > getMipmapLevel()) {
        throw std::invalid_argument("Native upscale level does not match its image.");
    }

    const RectI sourceBounds = getBounds(), sourceRoi = requested.intersect(sourceBounds);
    const RectI target = sourceRoi.upscalePowerOfTwo(levels).intersect(destination->getBounds()).intersect(
                             getRoD().toPixelEnclosing(getMipmapLevel() - levels, getPixelAspectRatio()));

    if (sourceRoi.isNull() || target.isNull()) {
        return;
    }

    if (getStorageMode() != eStorageModeVulkan) {
        auto params = makeParams(getRoD(), sourceBounds, getPixelAspectRatio(), getMipmapLevel(), false,
                                 getComponents(), getBitDepth(), getPremultiplication(), getFieldingOrder(), eStorageModeVulkan);
        params->getStorageInfo().vulkanContext = destination->getVulkanImage()->context();
        auto native = std::make_shared<Image>(getKey(), params);
        native->allocateMemory();
        native->pasteFrom(*this, sourceRoi, false);
        native->upscaleVulkan(requested, levels, destination);
        return;
    }

    if (destination->getStorageMode() != eStorageModeVulkan) {
        auto params = makeParams(destination->getRoD(), destination->getBounds(), destination->getPixelAspectRatio(),
                                 destination->getMipmapLevel(), false, destination->getComponents(), destination->getBitDepth(),
                                 destination->getPremultiplication(), destination->getFieldingOrder(), eStorageModeVulkan);
        params->getStorageInfo().vulkanContext = getVulkanImage()->context();
        auto native = std::make_shared<Image>(getKey(), params);
        native->allocateMemory();
        upscaleVulkan(requested, levels, native.get());
        destination->pasteFrom(*native, target, false);
        return;
    }

    ImageVulkanLocks locks(*destination, {this});
    const int64_t scale = int64_t(1) << levels;
    const int64_t offsetX = int64_t(destination->_bounds.x1) - int64_t(_bounds.x1) * scale;
    const int64_t offsetY = int64_t(destination->_bounds.y1) - int64_t(_bounds.y1) * scale;

    if (offsetX < INT32_MIN || offsetX > INT32_MAX || offsetY < INT32_MIN || offsetY > INT32_MAX) {
        throw std::invalid_argument("Native upscale grid offset overflows.");
    }

    destination->getVulkanImage()->upscale(*getVulkanImage(), target.x1 - destination->_bounds.x1,
                                           target.y1 - destination->_bounds.y1, target.width(), target.height(), int32_t(offsetX), int32_t(offsetY), levels);
}
namespace
{
std::shared_ptr<VulkanImageStorage> alignedInput(const Image* source, const Image& destination, bool mask)
{
    if (!source) return {};

    const RectI target = ImageVulkanLocks::bounds(destination), bounds = ImageVulkanLocks::bounds(*source);

    const auto context = destination.getVulkanImage()->context();

    if (source->getStorageMode() == eStorageModeVulkan && bounds == target) {
        return source->getVulkanImage();
    }

    auto result = std::make_shared<VulkanImageStorage>(context, target.width(), target.height());
    const RectI roi = target.intersect(bounds);

    if (roi.isNull()) {
        return result;
    }

    if (source->getStorageMode() == eStorageModeVulkan) {
        result->copy(*source->getVulkanImage(), roi.x1 - bounds.x1, roi.y1 - bounds.y1, roi.width(), roi.height(),
                     roi.x1 - target.x1, roi.y1 - target.y1);
    } else {
        if (!mask) {
            requireRGBAFloat(*source);
        }

        if (source->getBitDepth() != eImageBitDepthFloat || source->getComponents().getNumComponents() < 1) {
            throw std::invalid_argument("Native mask upload requires float pixels.");
        }

        std::vector<float> packed(size_t(roi.width()) * roi.height() * 4, 0);

        for (int y = roi.y1; y < roi.y2; ++y) for (int x = roi.x1; x < roi.x2; ++x) {
                const auto* pixel = reinterpret_cast<const float *>(ImageVulkanLocks::pixel(*source, x, y));
                auto* output = packed.data() + (size_t(y - roi.y1) * roi.width() + x - roi.x1) * 4;

                if (mask) {
                    output[3] = pixel[source->getComponents().getNumComponents() - 1];
                } else {
                    std::memcpy(output, pixel, 4 * sizeof(float));
                }
            }

        result->uploadRegion(roi.x1 - target.x1, roi.y1 - target.y1, roi.width(), roi.height(), packed.data(), packed.size() * sizeof(float));
    }

    return result;
}
}
ImagePtr Image::snapshotVulkanToRam(const RectI& requested) const
{
    QReadLocker lock(&_entryLock);
    requireRGBAFloat(*this);

    if (getStorageMode() != eStorageModeVulkan || !_bounds.contains(requested)) {
        throw std::invalid_argument("Invalid Vulkan snapshot region.");
    }

    auto result = std::make_shared<Image>(getComponents(), getRoD(), _bounds, getMipmapLevel(), getPixelAspectRatio(),
                                          getBitDepth(), getPremultiplication(), getFieldingOrder(), _useBitmap);
    const size_t rowBytes = size_t(requested.width()) * 4 * sizeof(float);
    auto output = result->getWriteRights();
    const size_t stride = size_t(_bounds.width()) * 16;
    getVulkanImage()->downloadRegionRows(requested.x1 - _bounds.x1, requested.y1 - _bounds.y1,
                                         requested.width(), requested.height(), output.pixelAt(requested.x1, requested.y1),
                                         size_t(requested.height() - 1)*stride + rowBytes, stride);

    if (_useBitmap) {
        result->copyBitmapPortion(requested, *this);
    }

    return result;
}
bool Image::checkForNaNsVulkan(const RectI& requested, bool fix) const
{
    if (requested.isNull()) {
        return false;
    }

    if (!_bounds.contains(requested)) {
        throw std::invalid_argument("Vulkan NaN inspection ROI is outside the image.");
    }

    return getVulkanImage()->checkForNaNs(requested.x1 - _bounds.x1, requested.y1 - _bounds.y1,
                                          requested.width(), requested.height(), fix);
}
namespace
{
uint32_t gpuColorSpace(ViewerColorSpaceEnum space)
{
    switch (space) {
    case eViewerColorSpaceSRGB:
        return 1;

    case eViewerColorSpaceRec709:
        return 2;

    case eViewerColorSpaceBT1886:
        return 3;

    case eViewerColorSpaceLinear:
    default:
        return 0;
    }
}
}
void Image::convertToFormatVulkan(const RectI& requested, ViewerColorSpaceEnum sourceSpace, ViewerColorSpaceEnum destinationSpace,
                                  int channelForAlpha, bool useAlpha0, bool copyBitmap, bool requiresUnpremult, Image* destination) const
{
    if (!destination) {
        throw std::invalid_argument("Vulkan format conversion requires a CPU or Vulkan image.");
    }

    if (requested.isNull()) {
        return;
    }

    const bool sourceGpu = getStorageMode() == eStorageModeVulkan;
    const bool destinationGpu = destination->getStorageMode() == eStorageModeVulkan;

    if (sourceGpu && destinationGpu && getVulkanImage()->context() == destination->getVulkanImage()->context()) {
        ImageVulkanLocks locks(*destination, {this});
        requireRGBAFloat(*this);
        requireRGBAFloat(*destination);

        if (!_bounds.contains(requested) || !destination->_bounds.contains(requested)) {
            throw std::invalid_argument("Vulkan conversion ROI is outside its images.");
        }

        const auto input = alignedInput(this, *destination, false);
        destination->getVulkanImage()->convertColor(*input, requested.x1 - destination->_bounds.x1,
                requested.y1 - destination->_bounds.y1, requested.width(), requested.height(),
                gpuColorSpace(sourceSpace), gpuColorSpace(destinationSpace));

        // The existing CPU RGBA-to-RGBA path also preserves alpha and ignores
        // requiresUnpremult/useAlpha0, which apply only to component changes.
        if (copyBitmap && _useBitmap && destination->_useBitmap && this != destination) {
            destination->copyBitmapPortion(requested, *this);
        }

        return;
    }

    ImagePtr sourceSnapshot = sourceGpu ? snapshotVulkanToRam(requested) : ImagePtr();
    const Image* input = sourceSnapshot ? sourceSnapshot.get() : this;

    if (destinationGpu) {
        requireRGBAFloat(*destination);
        const RectI targetBounds = destination->getBounds();

        if (!input->getBounds().contains(requested) || !targetBounds.contains(requested)) {
            throw std::invalid_argument("CPU/Vulkan conversion ROI is outside its images.");
        }

        // CPU-only formats are converted at the graph boundary. The native
        // image receives just this ROI; its other pixels and bitmap stay intact.
        auto converted = std::make_shared<Image>(destination->getComponents(), destination->getRoD(), targetBounds,
                         destination->getMipmapLevel(), destination->getPixelAspectRatio(),
                         destination->getBitDepth(), destination->getPremultiplication(),
                         destination->getFieldingOrder(), destination->usesBitMap());
        input->convertToFormatCommon(requested, sourceSpace, destinationSpace, channelForAlpha,
                                     useAlpha0, copyBitmap && input->usesBitMap() && converted->usesBitMap(), requiresUnpremult, converted.get());
        destination->pasteFromVulkan(*converted, requested, copyBitmap);
    } else {
        input->convertToFormatCommon(requested, sourceSpace, destinationSpace, channelForAlpha,
                                     useAlpha0, copyBitmap && input->usesBitMap() && destination->usesBitMap(), requiresUnpremult, destination);
    }
}
void Image::pasteFromVulkan(const Image& source, const RectI& requested, bool copyBitmap)
{
    Gpu::PerformanceTrace trace("engine-boundary-copy", uint64_t(requested.area()) * 16);
    ImageVulkanLocks locks(*this, {&source});
    requireRGBAFloat(*this);
    requireRGBAFloat(source);
    const RectI bounds = _bounds, sourceBounds = source._bounds;
    const RectI roi = requested.intersect(bounds).intersect(sourceBounds);

    if (roi.isNull()) {
        return;
    }

    const size_t rowBytes = size_t(roi.width()) * 4 * sizeof(float);

    if (getStorageMode() == eStorageModeVulkan && source.getStorageMode() == eStorageModeVulkan) {
        getVulkanImage()->copy(*source.getVulkanImage(), roi.x1 - sourceBounds.x1, roi.y1 - sourceBounds.y1, roi.width(), roi.height(),
                               roi.x1 - bounds.x1, roi.y1 - bounds.y1);
    } else if (getStorageMode() == eStorageModeVulkan) {
        const size_t stride = size_t(sourceBounds.width()) * 16;
        getVulkanImage()->uploadRegionRows(roi.x1 - bounds.x1, roi.y1 - bounds.y1, roi.width(), roi.height(),
                                           source.pixelAt(roi.x1, roi.y1), size_t(roi.height() - 1)*stride + rowBytes, stride);
    } else {
        const size_t stride = size_t(bounds.width()) * 16;
        source.getVulkanImage()->downloadRegionRows(roi.x1 - sourceBounds.x1, roi.y1 - sourceBounds.y1, roi.width(), roi.height(),
                pixelAt(roi.x1, roi.y1), size_t(roi.height() - 1)*stride + rowBytes, stride);
    }

    if (copyBitmap && usesBitMap() && source.usesBitMap()) {
        copyBitmapPortion(roi, source);
    }
}
void Image::fillVulkan(const RectI& requested, const std::array<float, 4> &color)
{
    ImageVulkanLocks locks(*this, {});
    const RectI bounds = _bounds, roi = requested.intersect(_bounds);

    if (!roi.isNull()) {
        getVulkanImage()->fill(roi.x1 - bounds.x1, roi.y1 - bounds.y1, roi.width(), roi.height(), color);
    }
}
void Image::maskMixVulkan(const RectI& requested, const Image* mask, const Image* original, bool masked, bool inverted, float amount)
{
    if (!masked && amount == 1.f) {
        return;
    }

    ImageVulkanLocks locks(*this, {mask, original});
    const RectI bounds = _bounds, roi = requested.intersect(_bounds);

    if (roi.isNull()) {
        return;
    }

    auto input = alignedInput(original, *this, false), maskInput = alignedInput(masked ? mask : nullptr, *this, true);
    const auto output = getVulkanImage();
    output->mix(*output, input.get(), maskInput.get(), roi.x1 - bounds.x1, roi.y1 - bounds.y1, roi.width(), roi.height(), amount, masked, inverted);
}
void Image::restoreChannelsVulkan(const RectI& requested, const Image* original, uint32_t channels)
{
    ImageVulkanLocks locks(*this, {original});
    const RectI bounds = _bounds, roi = requested.intersect(_bounds);

    if (roi.isNull() || !channels) {
        return;
    }

    auto input = alignedInput(original, *this, false);
    const auto output = getVulkanImage();
    output->restore(*output, input.get(), roi.x1 - bounds.x1, roi.y1 - bounds.y1, roi.width(), roi.height(), channels);
}
NATRON_NAMESPACE_EXIT
