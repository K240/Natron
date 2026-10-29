// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/OfxVulkanImageSuite.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/VulkanImageContextPool.h"
#include "Gpu/ImageOperations.h"
#include "Gpu/tests/DeviceFaultInjection.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
NATRON_NAMESPACE_ENTER
namespace
{
struct ImageRecord {
    ImagePtr owner;
    std::shared_ptr<VulkanImageStorage> storage;
    GenericAccessPtr access;
    RectI bounds, allowed;
    bool writable = true, owned = true;
};
struct Context {
    std::shared_ptr<Gpu::VulkanInstance> instance;
    std::shared_ptr<Gpu::VulkanDevice> device;
    std::shared_ptr<VulkanImageContext> storageContext;
    std::map<NatronVulkanImageHandle, ImageRecord> images;
    std::map<NatronVulkanProgramHandle, std::unique_ptr<VulkanFragmentProgram>> programs;
    std::mutex mutex;
};
struct RegisteredClip {
    ImageRecord record;
    OfxPropertySetHandle properties = nullptr;
    bool live = true;
    std::map<NatronVulkanContextHandle, std::pair<std::weak_ptr<Context>, NatronVulkanImageHandle>> loans;
    std::mutex mutex;
};
std::mutex registryMutex;
std::map<NatronVulkanContextHandle, std::shared_ptr<Context>> contexts;
std::map<uint64_t, std::shared_ptr<RegisteredClip>> registeredClips;
std::map<OfxPropertySetHandle, uint64_t> clipProperties;
std::atomic<uint64_t> nextHandle{1};
uint64_t newHandle()
{
    // A wrapped token must never resurrect a released handle.
    const uint64_t value = nextHandle.fetch_add(1);

    if (value == 0 || value == std::numeric_limits<uint64_t>::max()) {
        std::terminate();
    }

    return value;
}
std::shared_ptr<Context> lookup(NatronVulkanContextHandle handle)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    auto found = contexts.find(handle);

    if (found == contexts.end()) {
        throw OfxStatus(kOfxStatErrBadHandle);
    }

    return found->second;
}
VulkanImageStorage &image(Context& context, NatronVulkanImageHandle handle)
{
    auto found = context.images.find(handle);

    if (found == context.images.end()) {
        throw OfxStatus(kOfxStatErrBadHandle);
    }

    return *found->second.storage;
}
Gpu::ImageRegion region(const VulkanImageStorage& image, const NatronVulkanRegion* roi)
{
    const auto result = roi ? Gpu::ImageRegion{roi->x, roi->y, roi->width, roi->height} :
                        Gpu::ImageRegion{0, 0, image.width(), image.height()};

    if (!result.width || !result.height || result.x >= image.width() || result.y >= image.height() ||
        result.width > image.width() - result.x || result.height > image.height() - result.y) {
        throw std::invalid_argument("Invalid native image ROI.");
    }

    return result;
}
void validateRead(Context& context, NatronVulkanImageHandle handle, Gpu::ImageRegion roi)
{
    // Regions were checked against the image before converting to signed ints.
    const auto found = context.images.find(handle);

    if (found == context.images.end()) {
        throw OfxStatus(kOfxStatErrBadHandle);
    }

    const auto& record = found->second;
    const RectI pixels(roi.x, roi.y, roi.x + roi.width, roi.y + roi.height);

    if (!record.allowed.contains(pixels)) {
        throw std::invalid_argument("Read is outside the borrowed clip ROI.");
    }
}
void validateWrite(Context& context, NatronVulkanImageHandle handle, Gpu::ImageRegion roi)
{
    validateRead(context, handle, roi);

    if (!context.images.at(handle).writable) {
        throw std::invalid_argument("Write is outside the borrowed clip ROI or targets a read-only source.");
    }
}
void sameGrid(const VulkanImageStorage& destination, const VulkanImageStorage& source)
{
    if (destination.width() != source.width() || destination.height() != source.height()) {
        throw std::invalid_argument("Different image pixel grids.");
    }
}
template<class F> OfxStatus boundary(F&& call) noexcept
{
    try {
        call();
        return kOfxStatOK;
    } catch (OfxStatus status) {
        return status;
    } catch (const Gpu::VulkanError& error) {
        if (error.deviceLost()) {
            return kNatronOfxStatErrDeviceLost;
        }

        return error.outOfMemory() ? kOfxStatErrMemory : kOfxStatFailed;
    } catch (const std::bad_alloc &) {
        return kOfxStatErrMemory;
    } catch (const std::invalid_argument &) {
        return kOfxStatErrValue;
    } catch (...) {
        return kOfxStatFailed;
    }
}
template<class F> OfxStatus withContext(NatronVulkanContextHandle handle, F&& call, bool allowLost = false) noexcept
{
    return boundary([&] {
        auto context = lookup(handle);
        std::lock_guard<std::mutex> lock(context->mutex);

        if (!allowLost) context->device->requireUsable();
        call(*context);
    });
}
void checkRows(const VulkanImageStorage& image, const void* data, int64_t rowBytes)
{
    const uint64_t rowSize = uint64_t(image.width()) * 4 * sizeof(float);

    if (!data || rowBytes == std::numeric_limits<int64_t>::min()) {
        throw std::invalid_argument("Invalid image buffer.");
    }

    const uint64_t stride = static_cast<uint64_t>(rowBytes < 0 ? -rowBytes : rowBytes);
    const auto maxOffset = static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max());

    if (stride < rowSize || stride > maxOffset ||
        (image.height() > 1 && stride > (maxOffset - rowSize) / (image.height() - 1))) {
        throw std::invalid_argument("Invalid image row stride.");
    }
}
OfxStatus createContext(int index, int validation, NatronVulkanContextHandle* result)
{
    if (!result) {
        return kOfxStatErrValue;
    }

    *result = 0;
    return boundary([&] {
        if (index < -1 || (validation != 0 && validation != 1)) throw std::invalid_argument("Invalid Vulkan context options.");
        auto context = std::make_shared<Context>();
        context->storageContext = sharedVulkanImageContext(index, validation != 0);
        context->device = context->storageContext->device();
        context->instance = context->device->instance();
        const auto handle = newHandle();
        std::lock_guard<std::mutex> lock(registryMutex);
        contexts.emplace(handle, std::move(context));
        *result = handle;
    });
}
OfxStatus releaseContext(NatronVulkanContextHandle handle)
{
    return boundary([&] {
        std::shared_ptr<Context> released;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            auto found = contexts.find(handle);

            if (found == contexts.end()) throw OfxStatus(kOfxStatErrBadHandle);
            released = std::move(found->second);
            contexts.erase(found);
        }
        // Destroy GPU resources outside the global registry lock.
    });
}
OfxStatus getDeviceInfo(NatronVulkanContextHandle handle, NatronVulkanDeviceInfo* info)
{
    if (!info) {
        return kOfxStatErrValue;
    }

    return withContext(handle, [&](Context & context) {
        *info = {};
        const auto& properties = context.device->properties();
        std::memcpy(info->name, properties.deviceName, sizeof(info->name) - 1);
        info->vendorId = properties.vendorID;
        info->deviceId = properties.deviceID;
        info->apiVersion = properties.apiVersion;
        info->validationEnabled = context.instance->validationEnabled() ? 1 : 0;
        info->validationErrors = context.instance->validationErrorCount();
    });
}
OfxStatus createImage(NatronVulkanContextHandle handle, uint32_t width, uint32_t height, NatronVulkanImageHandle* result)
{
    if (!result) {
        return kOfxStatErrValue;
    }

    *result = 0;
    return withContext(handle, [&](Context & context) {
        if (!width || !height || width > INT32_MAX || height > INT32_MAX) {
            throw std::invalid_argument("Invalid native image dimensions.");
        }

        auto buffer = Image::createVulkan(context.storageContext, RectI(0, 0, width, height));
        const auto token = newHandle();
        ImageRecord record;
        record.storage = buffer->getVulkanImage();
        record.bounds = record.allowed = RectI(0, 0, width, height);
        record.owner = std::move(buffer);
        context.images.emplace(token, std::move(record));
        *result = token;
    });
}
OfxStatus releaseImage(NatronVulkanContextHandle handle, NatronVulkanImageHandle token)
{
    return withContext(handle, [&](Context & context) {
        const auto found = context.images.find(token);

        if (found == context.images.end()) {
            throw OfxStatus(kOfxStatErrBadHandle);
        }

        if (!found->second.owned) {
            throw std::invalid_argument("Borrowed images are released with their OFX clip.");
        }

        context.images.erase(found);
    }, true);
}
OfxStatus upload(NatronVulkanContextHandle handle, NatronVulkanImageHandle token, const void* data, int64_t rowBytes)
{
    return withContext(handle, [&](Context & context) {
        auto& buffer = image(context, token);
        validateWrite(context, token, region(buffer, nullptr));
        checkRows(buffer, data, rowBytes);
        const size_t rowSize = size_t(buffer.width()) * 4 * sizeof(float);
        std::vector<unsigned char> packed(buffer.byteSize());

        for (uint32_t y = 0; y < buffer.height(); ++y) {
            std::memcpy(packed.data() + y * rowSize, static_cast<const char*>(data) + static_cast<ptrdiff_t>(y) * rowBytes, rowSize);
        }

        buffer.upload(packed.data(), packed.size());
    });
}
OfxStatus download(NatronVulkanContextHandle handle, NatronVulkanImageHandle token, void* data, int64_t rowBytes)
{
    return withContext(handle, [&](Context & context) {
        auto& buffer = image(context, token);
        validateRead(context, token, region(buffer, nullptr));
        checkRows(buffer, data, rowBytes);
        const auto packed = buffer.download();
        const size_t rowSize = size_t(buffer.width()) * 4 * sizeof(float);

        for (uint32_t y = 0; y < buffer.height(); ++y) {
            std::memcpy(static_cast<char*>(data) + static_cast<ptrdiff_t>(y) * rowBytes, packed.data() + y * rowSize, rowSize);
        }
    });
}
OfxStatus copy(NatronVulkanContextHandle handle, NatronVulkanImageHandle source, NatronVulkanImageHandle destination,
               const NatronVulkanRegion* roi, uint32_t x, uint32_t y)
{
    return withContext(handle, [&](Context & context) {
        auto& src = image(context, source);
        auto& dst = image(context, destination);
        const auto area = region(src, roi);
        validateRead(context, source, area);

        if (x >= dst.width() || y >= dst.height() || area.width > dst.width() - x || area.height > dst.height() - y) {
            throw std::invalid_argument("Invalid copy destination ROI.");
        }

        validateWrite(context, destination, {x, y, area.width, area.height});
        dst.copy(src, area.x, area.y, area.width, area.height, x, y);
    });
}
OfxStatus transform(NatronVulkanContextHandle handle, NatronVulkanImageHandle source, NatronVulkanImageHandle destination,
                    const NatronVulkanRegion* roi, const float *gain, const float *offset)
{
    if (!gain || !offset) {
        return kOfxStatErrValue;
    }

    return withContext(handle, [&](Context & context) {
        auto& src = image(context, source);
        std::array<float, 4> g, o;
        std::memcpy(g.data(), gain, sizeof(g));
        std::memcpy(o.data(), offset, sizeof(o));
        const auto area = region(src, roi);
        validateRead(context, source, area);
        validateWrite(context, destination, area);
        image(context, destination).transform(src, area.x, area.y, area.width, area.height, g, o);
    });
}
OfxStatus mix(NatronVulkanContextHandle handle, NatronVulkanImageHandle processed, NatronVulkanImageHandle original,
              NatronVulkanImageHandle mask, NatronVulkanImageHandle destination, const NatronVulkanRegion* roi,
              float amount, int masked, int inverted)
{
    if ((masked != 0 && masked != 1) || (inverted != 0 && inverted != 1)) {
        return kOfxStatErrValue;
    }

    return withContext(handle, [&](Context & context) {
        auto& src = image(context, processed);
        auto& dst = image(context, destination);
        auto* orig = original ? &image(context, original) : nullptr;
        auto* masking = mask ? &image(context, mask) : nullptr;
        sameGrid(dst, src);

        if (orig) {
            sameGrid(dst, *orig);
        }

        if (masking) {
            sameGrid(dst, *masking);
        }

        const auto area = region(src, roi);
        validateRead(context, processed, area);

        if (original) {
            validateRead(context, original, area);
        }

        if (mask) {
            validateRead(context, mask, area);
        }

        validateWrite(context, destination, area);

        if (&src == &dst && !masked && amount == 1.f) {
            return;
        }

        dst.mix(src, orig, masking, area.x, area.y, area.width, area.height, amount, masked != 0, inverted != 0);
    });
}
OfxStatus restore(NatronVulkanContextHandle handle, NatronVulkanImageHandle processed, NatronVulkanImageHandle original,
                  NatronVulkanImageHandle destination, const NatronVulkanRegion* roi, uint32_t channels)
{
    return withContext(handle, [&](Context & context) {
        auto& src = image(context, processed);

        if (channels > 15) {
            throw std::invalid_argument("Invalid channel mask.");
        }

        auto& dst = image(context, destination);
        auto* orig = original ? &image(context, original) : nullptr;
        sameGrid(dst, src);

        if (orig) {
            sameGrid(dst, *orig);
        }

        const auto area = region(src, roi);
        validateRead(context, processed, area);

        if (original) {
            validateRead(context, original, area);
        }

        validateWrite(context, destination, area);

        if (&src == &dst && channels == 0) {
            return;
        }

        dst.restore(src, orig, area.x, area.y, area.width, area.height, channels);
    });
}
OfxStatus createProgramV2(NatronVulkanContextHandle handle, const char* source, uint32_t bytes,
                          const NatronVulkanShaderLayout *layout, NatronVulkanProgramHandle* result, char *error, uint32_t capacity)
{
    if (capacity && !error) {
        return kOfxStatErrValue;
    }

    if (capacity) {
        error[0] = '\0';
    }

    if (!result) {
        return kOfxStatErrValue;
    }

    *result = 0;
    return withContext(handle, [&](Context & context) {
        try {
            if (!source) {
                throw std::invalid_argument("A fragment program requires source.");
            }

            auto program = std::make_unique<VulkanFragmentProgram>(context.storageContext, source, bytes, layout);
            auto token = newHandle();
            context.programs.emplace(token, std::move(program));
            *result = token;
        } catch (const std::exception& exception) {
            if (capacity) {
                const size_t length = std::min<size_t>(capacity - 1, std::strlen(exception.what()));
                std::memcpy(error, exception.what(), length);
                error[length] = '\0';
            }

            throw;
        }
    });
}
OfxStatus createProgram(NatronVulkanContextHandle handle, const char* source, uint32_t bytes,
                        NatronVulkanProgramHandle* result, char *error, uint32_t capacity)
{
    return createProgramV2(handle, source, bytes, nullptr, result, error, capacity);
}
OfxStatus createProgramExtended(NatronVulkanContextHandle handle, const char* source, uint32_t bytes,
                                const NatronVulkanShaderLayout *layout, NatronVulkanProgramHandle* result, char *error, uint32_t capacity)
{
    if (!layout) {
        if (result) {
            *result = 0;
        }

        if (error && capacity) {
            error[0] = '\0';
        }

        return kOfxStatErrValue;
    }

    return createProgramV2(handle, source, bytes, layout, result, error, capacity);
}
OfxStatus releaseProgram(NatronVulkanContextHandle handle, NatronVulkanProgramHandle program)
{
    return withContext(handle, [&](Context & context) {
        if (!context.programs.erase(program)) {
            throw OfxStatus(kOfxStatErrBadHandle);
        }
    }, true);
}
OfxStatus renderProgram(NatronVulkanContextHandle handle, NatronVulkanProgramHandle token,
                        NatronVulkanImageHandle destination, const NatronVulkanRegion* roi,
                        const NatronVulkanShaderInput* inputs, const void *uniforms, uint32_t bytes)
{
    return withContext(handle, [&](Context & context) {
        auto found = context.programs.find(token);

        if (found == context.programs.end()) {
            throw OfxStatus(kOfxStatErrBadHandle);
        }

        auto& output = image(context, destination);
        auto area = region(output, roi);
        validateWrite(context, destination, area);
        std::array<VulkanFragmentInput, 4> nativeInputs{};

        if (inputs) for (unsigned i = 0; i < 4; ++i) {
                if (inputs[i].wrap > 3 || inputs[i].filter > 3) {
                    throw std::invalid_argument("Invalid fragment sampler options.");
                }

                if (inputs[i].image) {
                    auto& input = image(context, inputs[i].image);
                    validateRead(context, inputs[i].image, region(input, nullptr));
                }

                nativeInputs[i] = {inputs[i].image ? &image(context, inputs[i].image) : nullptr, inputs[i].wrap, inputs[i].filter};
            }

        found->second->render(output, area.x, area.y, area.width, area.height, nativeInputs, uniforms, bytes);
    });
}
}
namespace
{
OfxStatus programResources(NatronVulkanContextHandle handle, NatronVulkanProgramHandle token,
                           uint32_t *samplerMask, uint32_t *count, uint32_t *offsets, uint32_t capacity)
{
    if (!samplerMask || !count || (capacity && !offsets)) {
        return kOfxStatErrValue;
    }

    *samplerMask = *count = 0;
    return withContext(handle, [&](Context & context) {
        auto found = context.programs.find(token);

        if (found == context.programs.end()) {
            throw OfxStatus(kOfxStatErrBadHandle);
        }

        const auto& active = found->second->activeUniformOffsets();
        *samplerMask = found->second->activeSamplers();
        *count = static_cast<uint32_t>(active.size());

        if (capacity) {
            if (capacity < active.size()) {
                throw std::invalid_argument("Insufficient shader resource capacity.");
            }

            std::copy(active.begin(), active.end(), offsets);
        }
    });
}
}
namespace
{
void setRectangle(int32_t output[4], const RectI& bounds)
{
    output[0] = bounds.x1;
    output[1] = bounds.y1;
    output[2] = bounds.x2;
    output[3] = bounds.y2;
}
OfxStatus acquireClipImage(NatronVulkanContextHandle handle, OfxPropertySetHandle properties, NatronVulkanImageView* view)
{
    if (!view) {
        return kOfxStatErrValue;
    }

    *view = {};
    return boundary([&] {
        std::shared_ptr<RegisteredClip> clip;
        {
            std::lock_guard<std::mutex> lock(registryMutex);
            const auto found = clipProperties.find(properties);

            if (found == clipProperties.end()) throw OfxStatus(kOfxStatErrBadHandle);
            clip = registeredClips.at(found->second);
        }
        auto context = lookup(handle);
        // Clip -> context is the only nested registry object lock order.
        // Neither lock is acquired while holding the global registry mutex.
        std::lock_guard<std::mutex> clipLock(clip->mutex);

        if (!clip->live) throw OfxStatus(kOfxStatErrBadHandle);
        std::lock_guard<std::mutex> contextLock(context->mutex);
        context->device->requireUsable();
        clip->record.storage->context()->device()->requireUsable();
        if (context->storageContext != clip->record.storage->context())
        {
            throw std::invalid_argument("Native clip and plugin context use different runtimes.");
        }
        auto existing = clip->loans.find(handle);
        NatronVulkanImageHandle token;

        if (existing != clip->loans.end())
        {
            token = existing->second.second;
        } else
        {
            auto record = clip->record;
            record.owned = false;

            if (!record.writable && record.allowed != RectI(0, 0, record.bounds.width(), record.bounds.height())) {
                const auto area = record.allowed;
                const RectI visible(record.bounds.x1 + area.x1, record.bounds.y1 + area.y1,
                                    record.bounds.x1 + area.x2, record.bounds.y1 + area.y2);
                auto cropped = Image::createVulkan(context->storageContext, visible);
                cropped->getVulkanImage()->copy(*record.storage, area.x1, area.y1, area.width(), area.height(), 0, 0);
                record.storage = cropped->getVulkanImage();
                record.owner = std::move(cropped);
                record.bounds = visible;
                record.allowed = RectI(0, 0, visible.width(), visible.height());
            }

            token = newHandle();
            context->images.emplace(token, std::move(record));

            try {
                clip->loans.emplace(handle, std::make_pair(std::weak_ptr<Context>(context), token));
            } catch (...) {
                context->images.erase(token);
                throw;
            }
        }
        const auto& record = context->images.at(token);
        view->image = token;
        setRectangle(view->bounds, record.bounds);
        setRectangle(view->visibleBounds, RectI(record.bounds.x1 + record.allowed.x1, record.bounds.y1 + record.allowed.y1,
                                                record.bounds.x1 + record.allowed.x2, record.bounds.y1 + record.allowed.y2));
        view->writable = record.writable ? 1 : 0;
    });
}
OfxStatus transferCounts(NatronVulkanContextHandle handle, uint64_t* uploads, uint64_t* downloads)
{
    if (!uploads || !downloads) {
        return kOfxStatErrValue;
    }

    *uploads = *downloads = 0;
    return withContext(handle, [&](Context & context) {
        const auto counts = context.storageContext->transferCounts();
        *uploads = counts[0];
        *downloads = counts[1];
    });
}
}
uint64_t registerOfxVulkanClip(OfxPropertySetHandle properties, const ImagePtr& image,
                               const GenericAccessPtr& access, const RectI& bounds, const RectI& visible, bool writable)
{
    if (!properties || !image || !access || image->getStorageMode() != eStorageModeVulkan ||
        image->getBitDepth() != eImageBitDepthFloat || image->getComponents().getNumComponents() != 4 ||
        visible.isNull() || !bounds.contains(visible)) {
        throw std::invalid_argument("Invalid native OFX clip registration.");
    }

    auto clip = std::make_shared<RegisteredClip>();
    clip->properties = properties;
    clip->record.owner = image;
    clip->record.storage = image->getVulkanImage();
    clip->record.access = access;
    clip->record.bounds = bounds;
    clip->record.allowed = RectI(visible.x1 - bounds.x1, visible.y1 - bounds.y1, visible.x2 - bounds.x1, visible.y2 - bounds.y1);
    clip->record.writable = writable;
    clip->record.owned = false;
    const auto token = newHandle();
    std::lock_guard<std::mutex> lock(registryMutex);

    if (!clipProperties.emplace(properties, token).second) {
        throw std::invalid_argument("OFX clip is already registered.");
    }

    try {
        registeredClips.emplace(token, std::move(clip));
    } catch (...) {
        clipProperties.erase(properties);
        throw;
    }

    return token;
}
void unregisterOfxVulkanClip(uint64_t token)
{
    if (!token) {
        return;
    }

    std::shared_ptr<RegisteredClip> clip;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        const auto found = registeredClips.find(token);

        if (found == registeredClips.end()) {
            return;
        }

        clip = std::move(found->second);
        registeredClips.erase(found);
        clipProperties.erase(clip->properties);
    }
    std::lock_guard<std::mutex> clipLock(clip->mutex);
    clip->live = false;

    for (const auto& loan : clip->loans) {
        if (auto context = loan.second.first.lock()) {
            std::lock_guard<std::mutex> contextLock(context->mutex);
            context->images.erase(loan.second.second);
        }
    }
}
namespace
{
OfxStatus getDeviceGeneration(NatronVulkanContextHandle handle, uint64_t* generation)
{
    if (!std::getenv("NATRON_TEST_PROFILE")) {
        return kOfxStatErrUnsupported;
    }

    if (!generation) {
        return kOfxStatErrValue;
    }

    *generation = 0;
    return withContext(handle, [&](Context & context) {
        *generation = context.device->generation();
    }, true);
}
OfxStatus armDeviceLoss(NatronVulkanContextHandle handle, uint32_t point)
{
    if (!std::getenv("NATRON_TEST_PROFILE")) {
        return kOfxStatErrUnsupported;
    }

    return withContext(handle, [&](Context & context) {
        Gpu::DeviceFaultInjection::arm(context.device, static_cast<Gpu::DeviceFaultInjection::Point>(point));
    });
}
OfxStatus getTestStatistics(NatronVulkanContextHandle handle, NatronVulkanTestStatistics* statistics)
{
    if (!std::getenv("NATRON_TEST_PROFILE")) {
        return kOfxStatErrUnsupported;
    }

    if (!statistics) {
        return kOfxStatErrValue;
    }

    *statistics = {};
    return withContext(handle, [&](Context & context) {
        const auto transfers = context.device->imageTransfers();
        const auto memory = context.device->memoryBudget();
        statistics->generation = context.device->generation();
        statistics->imageUploads = transfers.uploads;
        statistics->imageDownloads = transfers.downloads;
        statistics->imageUploadBytes = transfers.uploadBytes;
        statistics->imageDownloadBytes = transfers.downloadBytes;
        statistics->managedLocalBlockBytes = memory.localBlockBytes;
        statistics->managedLocalPeakBlockBytes = memory.localPeakBlockBytes;
        statistics->driverBudgetAvailable = memory.driverBudget;
        statistics->validationErrors = context.device->instance()->validationErrorCount();

        for (const auto& heap : memory.heaps) {
            if (heap.deviceLocal) {
                statistics->managedLocalAllocationBytes += heap.allocationBytes;

                if (memory.driverBudget) {
                    statistics->driverLocalUsageBytes += heap.usageBytes;
                    statistics->driverLocalBudgetBytes += heap.budgetBytes;
                }
            }
        }
    });
}
OfxStatus clearTestNodeCache()
{
    if (!std::getenv("NATRON_TEST_PROFILE")) {
        return kOfxStatErrUnsupported;
    }

    try {
        appPTR->clearNodeCache();
        return kOfxStatOK;
    } catch (...) {
        return kOfxStatFailed;
    }
}
}
const NatronOfxVulkanTestSuiteV1 &ofxVulkanTestSuite()
{
    static const NatronOfxVulkanTestSuiteV1 suite = {getDeviceGeneration, armDeviceLoss};
    return suite;
}
const NatronOfxVulkanTestSuiteV2 &ofxVulkanTestSuiteV2()
{
    static const NatronOfxVulkanTestSuiteV2 suite = {getDeviceGeneration, armDeviceLoss, getTestStatistics, clearTestNodeCache};
    return suite;
}
const NatronOfxVulkanRenderSuiteV1 &ofxVulkanRenderSuite()
{
    static const NatronOfxVulkanRenderSuiteV1 suite = {acquireClipImage, transferCounts};
    return suite;
}
const NatronOfxVulkanShaderSuiteV1 &ofxVulkanShaderSuite()
{
    static const NatronOfxVulkanShaderSuiteV1 suite = {createProgram, releaseProgram, renderProgram, programResources};
    return suite;
}
const NatronOfxVulkanShaderSuiteV2 &ofxVulkanShaderSuiteV2()
{
    static const NatronOfxVulkanShaderSuiteV2 suite = {createProgramExtended, releaseProgram, renderProgram, programResources};
    return suite;
}
const NatronOfxVulkanImageSuiteV1 &ofxVulkanImageSuite()
{
    static const NatronOfxVulkanImageSuiteV1 suite = {
        createContext, releaseContext, getDeviceInfo, createImage, releaseImage,
        upload, download, copy, transform, mix, restore
    };
    return suite;
}
NATRON_NAMESPACE_EXIT
