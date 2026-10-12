// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/Image.h"
#include "Engine/VulkanImageStorage.h"
#include "Engine/VulkanImageContextPool.h"
#include "Gpu/VulkanDevice.h"
#include <QCoreApplication>
#include <cmath>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace Natron;
namespace
{
void require(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
struct RecordingCache : CacheAPI {
    mutable size_t bytes = 0;
    mutable unsigned allocations = 0, releases = 0;
    QString getCachePath() const override
    {
        return {};
    }
    bool isTileCache() const override
    {
        return false;
    }
    size_t getTileSizeBytes() const override
    {
        return 0;
    }
    void notifyEntrySizeChanged(size_t before, size_t after) const override
    {
        require(bytes >= before, "Native cache resize accounting underflow.");
        bytes = bytes - before + after;
    }
    void notifyEntryAllocated(double, size_t size, StorageModeEnum mode) const override
    {
        require(mode == eStorageModeVulkan, "Native cache allocation used the wrong storage mode.");
        bytes += size;
        ++allocations;
    }
    void notifyEntryDestroyed(double, size_t size, StorageModeEnum mode) const override
    {
        require(mode == eStorageModeVulkan && bytes >= size, "Native cache release accounting failed.");
        bytes -= size;
        ++releases;
    }
    void notifyMemoryDeallocated() const override {}
    void backingFileClosed() const override
    {
        throw std::runtime_error("Vulkan allocation opened a backing file.");
    }
    void notifyEntryStorageChanged(StorageModeEnum, StorageModeEnum, double, size_t) const override
    {
        throw std::runtime_error("Vulkan allocation changed disk storage.");
    }
    void removeAllEntriesWithDifferentNodeHashForHolderPrivate(const std::string &, U64, bool) override {}
    TileCacheFilePtr allocTile(size_t*) override
    {
        throw std::runtime_error("Vulkan allocation requested a disk tile.");
    }
    TileCacheFilePtr getTileCacheFile(const std::string &, size_t) override
    {
        return {};
    }
    void freeTile(const TileCacheFilePtr &, size_t) override
    {
        throw std::runtime_error("Vulkan allocation released a disk tile.");
    }
};
const RectI bounds(-11, -7, 26, 16), small(-9, -5, 20, 14), maskBounds(-15, -11, 17, 11), roi(-6, -3, 23, 13);
ImagePtr ram(const RectI& box, bool alpha = false)
{
    return std::make_shared<Image>(alpha ? ImagePlaneDesc::getAlphaComponents() : ImagePlaneDesc::getRGBAComponents(),
                                   RectD(box.x1, box.y1, box.x2, box.y2), box, 0, 1., eImageBitDepthFloat,
                                   eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
}
void pattern(Image& image, int seed)
{
    const auto box = image.getBounds();
    const int count = image.getComponents().getNumComponents();
    auto access = image.getWriteRights();

    for (int y = box.y1; y < box.y2; ++y) for (int x = box.x1; x < box.x2; ++x) {
            auto* pixel = reinterpret_cast<float *>(access.pixelAt(x, y));

            for (int c = 0; c < count; ++c) {
                pixel[c] = float(((x - box.x1) * 17 + (y - box.y1) * 23 + c * 11 + seed * 29) % 131) / 97.f - 0.25f;
            }
        }
}
float compare(const Image& actual, const Image& expected)
{
    static unsigned comparison = 0;
    const unsigned current = ++comparison;
    require(actual.getBounds() == expected.getBounds(), "Native comparison has different bounds.");
    const auto box = expected.getBounds();
    auto actualPixels = actual.getReadRights(), expectedPixels = expected.getReadRights();
    float maximum = 0;

    for (int y = box.y1; y < box.y2; ++y) for (int x = box.x1; x < box.x2; ++x) {
            auto* a = reinterpret_cast<const float *>(actualPixels.pixelAt(x, y));
            auto* e = reinterpret_cast<const float *>(expectedPixels.pixelAt(x, y));

            for (int c = 0; c < 4; ++c) {
                if (std::isnan(e[c])) {
                    require(std::isnan(a[c]), "Fill changed a NaN outside ROI.");
                    continue;
                }

                if (std::isinf(e[c])) {
                    require(a[c] == e[c], "Native processing changed an infinity or its sign.");
                    continue;
                }

                if (!std::isfinite(a[c]) || std::abs(a[c] - e[c]) >= 0.00001f) {
                    throw std::runtime_error("Native comparison " + std::to_string(current) + " at " + std::to_string(x) + "," +
                                             std::to_string(y) + " channel " + std::to_string(c) + ": actual=" + std::to_string(a[c]) + " expected=" + std::to_string(e[c]));
                }

                maximum = std::max(maximum, std::abs(a[c] - e[c]));
            }
        }

    return maximum;
}
float readCompare(const Image& gpu, const Image& expected)
{
    auto result = ram(gpu.getBounds());
    result->pasteFrom(gpu, gpu.getBounds(), false);
    return compare(*result, expected);
}
unsigned testMipmap(const std::shared_ptr<VulkanImageContext> &context, float &maximum)
{
    const std::array<RectI, 9> shapes{RectI(1, 3, 14, 12), RectI(-7, -5, 6, 4),
                                      RectI(-3, -1, 10, 8), RectI(3, 1, 4, 14), RectI(-3, -7, -2, 6),
                                      RectI(1, 3, 14, 4), RectI(-7, -3, 6, -2), RectI(3, 5, 4, 6), RectI(-3, -5, -2, -4)};
    unsigned cases = 0;

    for (const RectI& shape : shapes) for (unsigned from : {
                                                   0u, 2u

                                               }) for (bool partial : {
                                                               false, true
                                                           })

                for (unsigned level = 1; level <= 5; ++level) for (bool bitmap : {
                            false, true
                        }) {
                        const RectD rod = shape.toCanonical_noClipping(from, 1.);
                        auto source = std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, shape, from, 1.,
                                                              eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
                        pattern(*source, 37);
                        source->markForRendered(shape);
                        source->clearBitmap(RectI(shape.x1, shape.y1, shape.x1 + 1, shape.y1 + 1));
                        auto params = Image::makeVulkanParams(context, rod, shape, 1., from);
                        auto nativeSource = std::make_shared<Image>(ImageKey(), params, nullptr);
                        nativeSource->allocateMemory();
                        nativeSource->pasteFrom(*source, shape, true);
                        RectI requested = shape;

                        if (partial && shape.width() > 3) {
                            ++requested.x1;
                            --requested.x2;
                        }

                        if (partial && shape.height() > 3) {
                            ++requested.y1;
                            --requested.y2;
                        }

                        const RectI target = requested.downscalePowerOfTwoSmallestEnclosing(level);
                        const RectI padded(target.x1 - 1, target.y1 - 1, target.x2 + 1, target.y2 + 1);
                        auto expected = std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, padded, from + level, 1.,
                                                                eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
                        auto targetParams = Image::makeVulkanParams(context, rod, padded, 1., from + level);
                        auto actual = std::make_shared<Image>(ImageKey(), targetParams, nullptr);
                        actual->allocateMemory();
                        expected->fill(padded, 91, 91, 91, 91);
                        actual->fill(padded, 91, 91, 91, 91);
                        expected->markForRendered(padded);
                        actual->markForRendered(padded);
                        source->downscaleMipmap(rod, requested, from, from + level, bitmap, expected.get());
                        const auto transfers = context->transferCounts();
                        nativeSource->downscaleMipmap(rod, requested, from, from + level, bitmap, actual.get());
                        require(context->transferCounts() == transfers, "Native mipmap generation transferred CPU pixels.");
                        maximum = std::max(maximum, readCompare(*actual, *expected));
                        const auto actualAccess = actual->getReadRights(), expectedAccess = expected->getReadRights();

                        for (int y = padded.y1; y < padded.y2; ++y) for (int x = padded.x1; x < padded.x2; ++x) {
                                require(*actualAccess.bitmapAt(x, y) == *expectedAccess.bitmapAt(x, y), "Native mipmap changed bitmap completeness or padding.");
                            }

                        ++cases;
                    }
    return cases;
}
unsigned testUpscale(const std::shared_ptr<VulkanImageContext> &context, float &maximum)
{
    const std::array<RectI, 9> shapes{RectI(1, 3, 14, 12), RectI(-7, -5, 6, 4), RectI(-3, -1, 10, 8),
                                      RectI(3, 1, 4, 14), RectI(-3, -7, -2, 6), RectI(1, 3, 14, 4),
                                      RectI(-7, -3, 6, -2), RectI(3, 5, 4, 6), RectI(-3, -5, -2, -4)};
    unsigned cases = 0;

    for (const RectI& shape : shapes) for (unsigned to : {
                                                   0u, 2u

                                               }) for (bool partial : {
                                                               false, true
                                                           })

                for (unsigned level = 1; level <= 5; ++level) for (unsigned boundary = 0; boundary < 3; ++boundary) {
                        const RectD rod = shape.toCanonical_noClipping(to + level, 1.);
                        auto source = std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, shape, to + level, 1.,
                                                              eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
                        pattern(*source, 59);
                        auto nativeSource = Image::convertToStorage(source, eStorageModeVulkan, context);
                        RectI requested = shape;

                        if (partial && shape.width() > 3) {
                            ++requested.x1;
                            --requested.x2;
                        }

                        if (partial && shape.height() > 3) {
                            ++requested.y1;
                            --requested.y2;
                        }

                        const RectI target = requested.upscalePowerOfTwo(level);
                        const RectI padded(target.x1 - 1, target.y1 - 1, target.x2 + 1, target.y2 + 1);
                        auto expected = std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, padded, to, 1.,
                                                                eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
                        expected->fill(padded, 91, 91, 91, 91);
                        {
                            auto input = source->getReadRights();
                            auto output = expected->getWriteRights();
                            const int scale = 1 << level;

                            for (int y = target.y1; y < target.y2; ++y) for (int x = target.x1; x < target.x2; ++x) {
                                    const int sx = int(std::floor(double(x) / scale)), sy = int(std::floor(double(y) / scale));
                                    std::memcpy(output.pixelAt(x, y), input.pixelAt(sx, sy), 4 * sizeof(float));
                                }
                        }
                        auto cpu = std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, padded, to, 1.,
                                                           eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
                        cpu->fill(padded, 91, 91, 91, 91);
                        source->upscaleMipmap(requested, to + level, to, cpu.get());
                        require(compare(*cpu, *expected) == 0, "CPU nearest upscale changed pixels or ROI padding.");
                        ImagePtr actual;

                        if (boundary == 2) {
                            actual = std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), rod, padded, to, 1.,
                                                             eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone);
                        } else {
                            actual = std::make_shared<Image>(ImageKey(), Image::makeVulkanParams(context, rod, padded, 1., to));
                            actual->allocateMemory();
                        }

                        actual->fill(padded, 91, 91, 91, 91);
                        const auto before = context->transferCounts();
                        (boundary == 1 ? source : nativeSource)->upscaleMipmap(requested, to + level, to, actual.get());
                        const auto after = context->transferCounts();
                        require(after[0] - before[0] == (boundary == 1 ? 1u : 0u) &&
                                after[1] - before[1] == (boundary == 2 ? 1u : 0u), "Upscale made unexpected CPU pixel transfers.");
                        maximum = std::max(maximum, boundary == 2 ? compare(*actual, *expected) : readCompare(*actual, *expected));
                        ++cases;
                    }

    return cases;
}
unsigned test(const std::shared_ptr<VulkanImageContext> &context, float &maximum)
{
    unsigned cases = 0;
    auto source = ram(small);
    pattern(*source, 2);
    auto expected = ram(bounds);
    expected->fillBoundsZero();
    expected->pasteFrom(*source, RectI(-100, -100, 100, 100), false);
    auto gpu = Image::createVulkan(context, bounds);
    gpu->pasteFrom(*source, RectI(-100, -100, 100, 100), false);
    maximum = std::max(maximum, readCompare(*gpu, *expected));
    ++cases;
    auto copy = Image::createVulkan(context, maskBounds), copyExpected = ram(maskBounds);
    copy->fill(maskBounds, -3, 5, 1.25f, 0.25f);
    copyExpected->fill(maskBounds, -3, 5, 1.25f, 0.25f);
    copy->pasteFrom(*gpu, roi, false);
    copyExpected->pasteFrom(*expected, roi, false);
    maximum = std::max(maximum, readCompare(*copy, *copyExpected));
    ++cases;
    auto partial = ram(maskBounds), partialExpected = ram(maskBounds);
    partial->fill(maskBounds, 3, -2, 1, 0.5f);
    partialExpected->fill(maskBounds, 3, -2, 1, 0.5f);
    partial->pasteFrom(*gpu, roi, false);
    partialExpected->pasteFrom(*expected, roi, false);
    maximum = std::max(maximum, compare(*partial, *partialExpected));
    ++cases;
    auto nan = ram(bounds);
    nan->fill(bounds, std::numeric_limits<float>::quiet_NaN(), 2, 3, 4);
    gpu->pasteFrom(*nan, bounds, false);
    gpu->fillZero(roi);
    nan->fillZero(roi);
    maximum = std::max(maximum, readCompare(*gpu, *nan));
    ++cases;
    gpu->fill(RectI(-100, -100, 100, 100), -1.5f, 4.2f, 0.25f, 1.25f);
    nan->fill(bounds, -1.5f, 4.2f, 0.25f, 1.25f);
    maximum = std::max(maximum, readCompare(*gpu, *nan));
    ++cases;

    auto processed = ram(bounds), original = ram(small), masking = ram(maskBounds, true);
    pattern(*processed, 1);
    pattern(*original, 3);
    pattern(*masking, 5);
    auto originalGpu = Image::createVulkan(context, small);
    originalGpu->pasteFrom(*original, small, false);
    auto maskRGBA = ram(maskBounds);
    maskRGBA->fillBoundsZero();

    {
        auto output = maskRGBA->getWriteRights();
        auto input = masking->getReadRights();

        for (int y = maskBounds.y1; y < maskBounds.y2; ++y) for (int x = maskBounds.x1; x < maskBounds.x2; ++x) {
                reinterpret_cast<float *>(output.pixelAt(x, y))[3] = *reinterpret_cast<const float*>(input.pixelAt(x, y));
            }
    }

    auto maskGpu = Image::createVulkan(context, maskBounds);
    maskGpu->pasteFrom(*maskRGBA, maskBounds, false);

    for (int originalMode = 0; originalMode < 3; ++originalMode) for (int maskMode = 0; maskMode < 3; ++maskMode) for (int flags = 0; flags < 4; ++flags) {
                gpu->pasteFrom(*processed, bounds, false);
                expected->pasteFrom(*processed, bounds, false);
                const float amount = flags == 0 ? 0 : flags == 1 ? 0.35f : flags == 2 ? 1.4f : -0.25f;
                gpu->applyMaskMix(roi, maskMode == 0 ? nullptr : maskMode == 1 ? masking.get() : maskGpu.get(),
                                  originalMode == 0 ? nullptr : originalMode == 1 ? original.get() : originalGpu.get(), (flags & 1) != 0, (flags & 2) != 0, amount);
                expected->applyMaskMix(roi, maskMode == 0 ? nullptr : masking.get(), originalMode == 0 ? nullptr : original.get(), (flags & 1) != 0, (flags & 2) != 0, amount);
                maximum = std::max(maximum, readCompare(*gpu, *expected));
                ++cases;
            }

    for (int originalMode = 0; originalMode < 3; ++originalMode) for (uint32_t channels = 0; channels < 16; ++channels) {
            gpu->pasteFrom(*processed, bounds, false);
            expected->pasteFrom(*processed, bounds, false);
            const std::bitset<4> process(~channels & 15);
            gpu->copyUnProcessedChannels(roi, eImagePremultiplicationPremultiplied, eImagePremultiplicationPremultiplied, process,
                                         originalMode == 0 ? ImagePtr() : originalMode == 1 ? original : originalGpu, true);
            expected->copyUnProcessedChannels(roi, eImagePremultiplicationPremultiplied, eImagePremultiplicationPremultiplied, process,
                                              originalMode == 0 ? ImagePtr() : original, true);
            maximum = std::max(maximum, readCompare(*gpu, *expected));
            ++cases;
        }

    // Original aliases the destination. The reference uses a snapshot to avoid
    // the CPU implementation's recursive locking of the same image.
    gpu->pasteFrom(*processed, bounds, false);
    expected->pasteFrom(*processed, bounds, false);
    auto snapshot = ram(bounds);
    snapshot->pasteFrom(*processed, bounds, false);
    gpu->applyMaskMix(roi, masking.get(), gpu.get(), true, true, 0.35f);
    expected->applyMaskMix(roi, masking.get(), snapshot.get(), true, true, 0.35f);
    maximum = std::max(maximum, readCompare(*gpu, *expected));
    ++cases;

    const RectI expanded(-16, -12, 35, 25);
    auto expandedExpected = ram(expanded);
    expandedExpected->fillBoundsZero();
    expandedExpected->pasteFrom(*expected, bounds, false);
    ImagePtr resizedCopy;
    require(gpu->copyAndResizeIfNeeded(expanded, true, true, &resizedCopy), "Vulkan copy resize did not expand bounds.");
    require(resizedCopy->getStorageMode() == eStorageModeVulkan && resizedCopy->getBounds() == expanded, "Vulkan copy resize lost storage/bounds.");
    maximum = std::max(maximum, readCompare(*resizedCopy, *expandedExpected));
    ++cases;
    require(gpu->ensureBounds(expanded, true, true), "Vulkan in-place resize did not expand bounds.");
    maximum = std::max(maximum, readCompare(*gpu, *expandedExpected));
    ++cases;

    auto a = Image::createVulkan(context, bounds), b = Image::createVulkan(context, bounds);
    a->pasteFrom(*processed, bounds, false);
    b->fillBoundsZero();
    auto forward = std::async(std::launch::async, [&] { for (int i = 0; i < 32; ++i) a->pasteFrom(*b, bounds, false); });
    auto reverse = std::async(std::launch::async, [&] { for (int i = 0; i < 32; ++i) b->pasteFrom(*a, bounds, false); });
    forward.get();
    reverse.get();
    auto aa = ram(bounds), bb = ram(bounds);
    aa->pasteFrom(*a, bounds, false);
    bb->pasteFrom(*b, bounds, false);
    maximum = std::max(maximum, compare(*aa, *bb));
    ++cases;

    RecordingCache cache;
    auto params = Image::makeVulkanParams(context, RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds);
    std::weak_ptr<VulkanImageStorage> allocation;
    {
        Image entry(ImageKey(), params, &cache);
        entry.allocateMemory();
        entry.allocateMemory();
        require(cache.allocations == 1 && cache.bytes == entry.size() && entry.getVulkanImage()->byteSize() == size_t(bounds.area()) * 16,
                "Vulkan cache entry allocation/accounting is incorrect.");
        require(entry.usesBitMap() && !entry.isStoredOnDisk(), "Vulkan cache entry bitmap/disk state is incorrect.");
        allocation = entry.getVulkanImage();
        entry.fillBoundsZero();
        entry.markForRendered(bounds);
        require(entry.ensureBounds(expanded, true, true), "Cached Vulkan image did not resize.");
        require(cache.bytes == entry.size() && allocation.expired(), "Native cache resize retained old VRAM or miscounted bytes.");
        allocation = entry.getVulkanImage();
        {
            auto bitmap = entry.getReadRights();

            for (int y = expanded.y1; y < expanded.y2; ++y) for (int x = expanded.x1; x < expanded.x2; ++x) {
                    require(*bitmap.bitmapAt(x, y) == 1, "Vulkan image resize lost bitmap flags.");
                }
        }
    }
    require(cache.bytes == 0 && cache.releases == 1 && allocation.expired(), "Vulkan cache entry retained VRAM after destruction.");
    ++cases;
    auto otherContext = std::make_shared<VulkanImageContext>(context->device());
    auto otherParams = Image::makeVulkanParams(otherContext, RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds);
    require(!(params->getStorageInfo() == otherParams->getStorageInfo()), "Cache parameters ignored Vulkan context identity.");
    ++cases;

    for (unsigned channel = 0; channel < 4; ++channel) for (unsigned inside = 0; inside < 2; ++inside) {
            auto expectedNan = ram(bounds);
            pattern(*expectedNan, 19);
            {
                auto pixels = expectedNan->getWriteRights();
                auto* outside = reinterpret_cast<float *>(pixels.pixelAt(bounds.x2 - 1, bounds.y2 - 1));
                outside[channel] = std::numeric_limits<float>::quiet_NaN();
                auto* sample = reinterpret_cast<float *>(pixels.pixelAt(roi.x1, roi.y1));
                sample[channel] = inside ? std::numeric_limits<float>::quiet_NaN() : -0.f;
                sample[(channel + 1) % 4] = std::numeric_limits<float>::infinity();
                sample[(channel + 2) % 4] = -std::numeric_limits<float>::infinity();
            }
            auto nanParams = Image::makeVulkanParams(context, RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds);
            auto nativeNan = std::make_shared<Image>(ImageKey(), nanParams, nullptr);
            nativeNan->allocateMemory();
            nativeNan->pasteFrom(*expectedNan, bounds, false);
            nativeNan->markForRendered(RectI(bounds.x1, bounds.y1, 0, bounds.y2));
            std::list<RectI> beforeBitmap, afterBitmap;
            nativeNan->getRestToRender(bounds, beforeBitmap);
            const auto beforePixels = nativeNan->getVulkanImage()->download();
            auto beforeTransfers = context->transferCounts();
            require(nativeNan->getVulkanImage()->checkForNaNs(roi.x1 - bounds.x1, roi.y1 - bounds.y1,
                    roi.width(), roi.height(), false) == (inside != 0), "Native NaN inspection returned the wrong flag.");
            require(context->transferCounts() == beforeTransfers, "NaN inspection downloaded pixel data.");
            require(nativeNan->getVulkanImage()->download() == beforePixels, "Read-only NaN inspection changed pixel bits.");
            beforeTransfers = context->transferCounts();
            const bool expectedFlag = expectedNan->checkForNaNsAndFix(roi);
            require(nativeNan->checkForNaNsAndFix(roi) == expectedFlag && expectedFlag == (inside != 0), "NaN repair differs from CPU.");
            require(!nativeNan->checkForNaNsAndFix(roi) && !nativeNan->checkForNaNsAndFix(RectI()), "NaN flag persisted after repair/empty ROI.");
            require(context->transferCounts() == beforeTransfers, "NaN repair staged pixels through CPU.");
            maximum = std::max(maximum, readCompare(*nativeNan, *expectedNan));
            {
                const auto repaired = nativeNan->getVulkanImage()->download();
                auto reference = expectedNan->getReadRights();
                const size_t rowBytes = size_t(bounds.width()) * 4 * sizeof(float);

                for (int y = bounds.y1; y < bounds.y2; ++y) {
                    require(std::memcmp(repaired.data() + size_t(y - bounds.y1) * rowBytes,
                                        reference.pixelAt(bounds.x1, y), rowBytes) == 0,
                            "NaN repair changed finite bits, signed zero, infinities or NaN payloads outside ROI.");
                }
            }
            nativeNan->getRestToRender(bounds, afterBitmap);
            require(beforeBitmap == afterBitmap, "NaN repair changed rendered bitmap validity.");
            ++cases;
        }

    return cases;
}
unsigned testSharedRuntime(int index, float &maximum)
{
    std::vector<std::future<std::shared_ptr<VulkanImageContext>>> workers;

    for (unsigned worker = 0; worker < 8; ++worker) {
        workers.emplace_back(std::async(std::launch::async, [index] { return sharedVulkanImageContext(index, true); }));
    }

    std::vector<std::shared_ptr<VulkanImageContext>> owners;

    for (auto& worker : workers) {
        owners.push_back(worker.get());
    }

    for (const auto& owner : owners) {
        require(owner == owners.front() && owner->device() == owners.front()->device(), "Concurrent OFX/Engine runtimes created duplicate logical devices.");
    }

    auto first = Image::makeVulkanParams(owners[0], RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds);
    auto second = Image::makeVulkanParams(owners[1], RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds);
    require(first->getStorageInfo() == second->getStorageInfo(), "Shared runtime cache parameters have different context identity.");
    auto source = Image::createVulkan(owners[0], bounds), destination = Image::createVulkan(owners[1], bounds);
    source->fill(bounds, .8f, -.4f, 2.f, 1.f);
    const auto transfers = owners[0]->transferCounts();
    destination->pasteFrom(*source, bounds, false);
    require(owners[0]->transferCounts() == transfers, "Shared-runtime images copied through CPU.");
    auto expected = ram(bounds);
    expected->fill(bounds, .8f, -.4f, 2.f, 1.f);
    maximum = std::max(maximum, readCompare(*destination, *expected));
    require(owners[0]->device()->instance()->validationEnabled() && owners[0]->device()->instance()->validationErrorCount() == 0,
            "Shared runtime produced Vulkan validation errors.");
    std::weak_ptr<VulkanImageContext> lifetime = owners[0];
    first.reset();
    second.reset();
    owners.clear();
    source.reset();
    require(!lifetime.expired(), "Shared runtime expired while its cached image was alive.");
    destination.reset();
    require(lifetime.expired(), "Shared runtime pool retained a logical device after its last owner expired.");
    return 3;
}
}
int main(int argc, char** argv)
{
    try {
        int index = -1;

        if (argc == 3 && std::string(argv[1]) == "--gpu") {
            index = std::stoi(argv[2]);
        } else if (argc != 1) {
            throw std::invalid_argument("Usage: NatronVulkanImageStorage [--gpu index]");
        }

        QCoreApplication application(argc, argv);
        Gpu::InstanceOptions options;
        options.validation = true;
        auto instance = Gpu::VulkanInstance::create(options);
        unsigned cases = 0, mipmapCases = 0, upscaleCases = 0;
        float maximum = 0;
        std::string name;
        {
            auto device = Gpu::VulkanDevice::create(instance, index);
            name = device->properties().deviceName;
            auto context = std::make_shared<VulkanImageContext>(device);
            cases = test(context, maximum);
            mipmapCases = testMipmap(context, maximum);
            upscaleCases = testUpscale(context, maximum);
        }
        cases += testSharedRuntime(index, maximum);
        require(instance->validationEnabled() && instance->validationErrorCount() == 0, "Native Engine image storage produced Vulkan validation errors.");
        std::cout << "{\"ok\":true,\"passed\":true,\"device\":\"" << name << "\",\"referenceCases\":" << cases
                  << ",\"cacheLifetime\":true,\"contextIdentity\":true,\"reciprocalCopies\":64,\"maxAbsoluteError\":" << maximum
                  << ",\"nanCases\":8,\"nanPixelTransfers\":0,\"nanDiagnosticBytes\":4,\"nanBitExact\":true,\"sharedRuntimeCases\":3,\"runtimeWorkers\":8"
                  << ",\"mipmapCases\":" << mipmapCases << ",\"mipmapPixelTransfers\":0,\"mipmapMaxLevel\":5,\"mipmapBitmaps\":true"
                  << ",\"upscaleCases\":" << upscaleCases << ",\"nativeUpscaleCases\":180,\"cpuUploadUpscaleCases\":180,\"cpuDownloadUpscaleCases\":180,\"nativeUpscalePixelTransfers\":0,\"upscalePadding\":true,\"validationErrors\":0}" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
