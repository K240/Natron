// SPDX-License-Identifier: GPL-2.0-or-later
#include "OcioVulkanAdapter.h"
#include "OcioVulkan.h"
#include <atomic>
#include <map>
#include <mutex>
#include <vector>
namespace
{
std::mutex cacheMutex;
std::map<OFX::ImageEffect *, std::shared_ptr<OcioVulkanProcessor>> cache;
std::atomic<uint64_t> renders{0};
std::atomic<uint64_t> clipRenders{0}, directClipRenders{0}, maskUploads{0};
std::atomic<uint64_t> readerRenders[31] {};
std::atomic<uint64_t> writerRenders{0};
NatronVulkanDeviceInfo lastInfo{};
const NatronOfxVulkanImageSuiteV1 *images()
{
    return static_cast<const NatronOfxVulkanImageSuiteV1 *>(OFX::fetchSuite(kNatronOfxVulkanImageSuite, 1, true));
}
const NatronOfxVulkanShaderSuiteV2 *shaders()
{
    return static_cast<const NatronOfxVulkanShaderSuiteV2 *>(OFX::fetchSuite(kNatronOfxVulkanShaderSuite, 2, true));
}
const NatronOfxVulkanRenderSuiteV1 *renderSuite()
{
    return static_cast<const NatronOfxVulkanRenderSuiteV1 *>(OFX::fetchSuite(kNatronOfxVulkanRenderSuite, 1, true));
}
std::shared_ptr<OcioVulkanProcessor> processorState(OFX::ImageEffect& instance)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto found = cache.find(&instance);

    if (found != cache.end()) {
        return found->second;
    }

    auto created = std::make_shared<OcioVulkanProcessor>(images(), shaders());
    return cache.emplace(&instance, created).first->second;
}
}
bool nativeOCIOAvailable()
{
    return images() && shaders();
}
bool nativeOCIOClipsAvailable()
{
    return nativeOCIOAvailable() && renderSuite();
}
void describeNativeIO(OFX::ImageEffectDescriptor& descriptor, OFX::PageParamDescriptor* page)
{
    auto param = descriptor.defineBooleanParam("enableGPU");
    param->setLabel("Vulkan processing");
    param->setHint("Process Float RGBA colors, alpha and proxy resolution on the GPU.");
    param->setDefault(true);
    param->setAnimates(false);
    param->setIsSecretAndDisabled(!nativeOCIOClipsAvailable());

    if (page) {
        page->addChild(*param);
    }
}
void updateNativeIOSupport(OFX::ImageEffect& instance)
{
    setNativeOCIOClipSupport(instance, instance.paramExists("enableGPU") && instance.fetchBooleanParam("enableGPU")->getValue());
}
bool uploadNativeReader(OFX::ImageEffect& instance, const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                        const float *pixels, const OfxRectI& bounds, int64_t rowBytes, const OfxRectI& crop,
                        OFX::Image& destination, const OfxRectI& window, unsigned levels, bool unpremult, bool premult)
{
    if (destination.getPixelData()) {
        return false;
    }

    if (!nativeOCIOClipsAvailable() || destination.getPixelDepth() != OFX::eBitDepthFloat ||
        destination.getPixelComponentCount() != 4 || levels > 30) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    try {
        auto state = processorState(instance);
        state->uploadReader(processor, *renderSuite(), pixels, bounds, rowBytes, crop,
                            destination.getPropertySet().propSetHandle(), window, levels, unpremult, premult);
        const auto info = state->deviceInfo();
        ++readerRenders[levels];
        std::lock_guard<std::mutex> lock(cacheMutex);
        lastInfo = info;
        return true;
    } catch (const std::exception& error) {
        instance.setPersistentMessage(OFX::Message::eMessageError, "", std::string("Reader Vulkan error: ") + error.what());
        throw;
    }
}
void clearNativeIO(OFX::ImageEffect& instance, OFX::Image& destination, const OfxRectI& window)
{
    if (!nativeOCIOClipsAvailable()) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    processorState(instance)->clearClip(*renderSuite(), destination.getPropertySet().propSetHandle(), window);
}
bool downloadNativeWriter(OFX::ImageEffect& instance, const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                          const OFX::Image& source, const OfxRectI& window, float *pixels, int64_t rowBytes,
                          bool opaque, bool unpremult, bool premult)
{
    if (source.getPixelData()) {
        return false;
    }

    if (!nativeOCIOClipsAvailable() || source.getPixelDepth() != OFX::eBitDepthFloat || source.getPixelComponentCount() != 4) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    try {
        auto state = processorState(instance);
        state->downloadWriter(processor, *renderSuite(), source.getPropertySet().propSetHandle(), window,
                              pixels, rowBytes, opaque, unpremult, premult);
        const auto info = state->deviceInfo();
        ++writerRenders;
        std::lock_guard<std::mutex> lock(cacheMutex);
        lastInfo = info;
        return true;
    } catch (const std::exception& error) {
        instance.setPersistentMessage(OFX::Message::eMessageError, "", std::string("Writer Vulkan error: ") + error.what());
        throw;
    }
}
void copyNativeIO(OFX::ImageEffect& instance, const OFX::Image& source, OFX::Image& destination, const OfxRectI& window)
{
    if (!nativeOCIOClipsAvailable() || source.getPixelData() || destination.getPixelData() ||
        source.getPixelDepth() != OFX::eBitDepthFloat || destination.getPixelDepth() != OFX::eBitDepthFloat ||
        source.getPixelComponentCount() != 4 || destination.getPixelComponentCount() != 4) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    processorState(instance)->copyClip(*renderSuite(), source.getPropertySet().propSetHandle(),
                                       destination.getPropertySet().propSetHandle(), window);
}
void setNativeOCIOClipSupport(OFX::ImageEffect& instance, bool enabled)
{
    instance.getPropertySet().propSetInt(kNatronOfxImageEffectPropVulkanRenderSupported,
                                         enabled && nativeOCIOClipsAvailable() ? 1 : 0, 0, false);
}
bool renderNativeOCIOClips(OFX::ImageEffect& instance, const OCIO_NAMESPACE::ConstProcessorRcPtr& processor,
                           const OFX::Image& source, OFX::Image& destination, const OfxRectI& window,
                           bool premult, float mix, bool masked, bool inverted, const OFX::Image* mask)
{
    if (destination.getPixelData()) {
        return false;
    }

    if (!nativeOCIOClipsAvailable() || source.getPixelData() || source.getPixelDepth() != OFX::eBitDepthFloat ||
        destination.getPixelDepth() != OFX::eBitDepthFloat || source.getPixelComponentCount() != 4 || destination.getPixelComponentCount() != 4) {
        OFX::throwSuiteStatusException(kOfxStatErrFormat);
    }

    try {
        if (window.x2 <= window.x1 || window.y2 <= window.y1) {
            OFX::throwSuiteStatusException(kOfxStatErrValue);
        }

        std::vector<float> packedMask;

        if (masked && mask) {
            if (!mask->getPixelData() || mask->getPixelDepth() != OFX::eBitDepthFloat || mask->getPixelComponentCount() != 1) {
                OFX::throwSuiteStatusException(kOfxStatErrFormat);
            }

            const uint32_t width = window.x2 - window.x1, height = window.y2 - window.y1;
            packedMask.resize(size_t(width) * height * 4, 0.f);

            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                    const auto pixel = static_cast<const float *>(mask->getPixelAddress(window.x1 + x, window.y1 + y));

                    if (pixel) {
                        packedMask[(size_t(y) * width + x) * 4 + 3] = *pixel;
                    }
                }
        }

        auto state = processorState(instance);
        const bool direct = state->applyClip(processor, *renderSuite(), source.getPropertySet().propSetHandle(), destination.getPropertySet().propSetHandle(),
                                             window, premult, mix, masked, inverted, packedMask.empty() ? nullptr : packedMask.data());
        const auto info = state->deviceInfo();
        ++renders;
        ++clipRenders;

        if (direct) {
            ++directClipRenders;
        }

        if (!packedMask.empty()) {
            ++maskUploads;
        }

        std::lock_guard<std::mutex> lock(cacheMutex);
        lastInfo = info;
        return true;
    } catch (const std::exception& error) {
        instance.setPersistentMessage(OFX::Message::eMessageError, "", std::string("OCIO Vulkan clip error: ") + error.what());
        throw;
    }
}
void releaseNativeOCIO(OFX::ImageEffect *instance)
{
    std::lock_guard<std::mutex> lock(cacheMutex);
    cache.erase(instance);
}
bool applyNativeOCIO(OFX::ImageEffect &instance, const OCIO_NAMESPACE::ConstProcessorRcPtr &processor,
                     float *pixels, uint32_t width, uint32_t height, uint32_t components, int64_t rowBytes)
{
    if (!nativeOCIOAvailable() || !processor || processor->isNoOp() ||
        (instance.paramExists("enableGPU") && !instance.fetchBooleanParam("enableGPU")->getValue())) {
        return false;
    }

    std::shared_ptr<OcioVulkanProcessor> state;
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto found = cache.find(&instance);

        if (found != cache.end()) {
            state = found->second;
        }
    }

    if (!state) {
        try {
            auto created = std::make_shared<OcioVulkanProcessor>(images(), shaders());
            std::lock_guard<std::mutex> lock(cacheMutex);
            state = cache.emplace(&instance, created).first->second;
        } catch (const std::exception &) {
            // This path has CPU pixels and an existing OCIO CPU processor.
            // Initialization can fail after the suite availability check.
            // Keep the optional acceleration failure inside this OFX callback.
            return false;
        }
    }

    NatronVulkanDeviceInfo info{};

    if (!state->tryApply(processor, pixels, width, height, components, rowBytes, &info)) {
        return false;
    }

    ++renders;
    std::lock_guard<std::mutex> lock(cacheMutex);
    lastInfo = info;
    return true;
}
extern "C" uint64_t OfxOCIOVulkanRenderCount()
{
    return renders.load();
}
extern "C" uint64_t OfxOCIOVulkanClipRenderCount()
{
    return clipRenders.load();
}
extern "C" uint64_t OfxOCIOVulkanMaskUploadCount()
{
    return maskUploads.load();
}
extern "C" uint64_t OfxOCIOVulkanDirectClipRenderCount()
{
    return directClipRenders.load();
}
extern "C" uint64_t OfxReaderVulkanRenderCount(unsigned level)
{
    return level < 31 ? readerRenders[level].load() : 0;
}
extern "C" uint64_t OfxWriterVulkanRenderCount()
{
    return writerRenders.load();
}
extern "C" OfxStatus OfxOCIOVulkanTransferCounts(uint64_t* uploads, uint64_t* downloads)
{
    if (!uploads || !downloads || !renderSuite() || !images()) {
        return kOfxStatErrValue;
    }

    NatronVulkanContextHandle context = 0;
    const OfxStatus status = images()->createContext(-1, 0, &context);

    if (status != kOfxStatOK) {
        return status;
    }

    const OfxStatus result = renderSuite()->getTransferCounts(context, uploads, downloads);
    images()->releaseContext(context);
    return result;
}
extern "C" OfxStatus OfxOCIOVulkanDeviceInfo(NatronVulkanDeviceInfo *info)
{
    if (!info) {
        return kOfxStatErrValue;
    }

    std::lock_guard<std::mutex> lock(cacheMutex);
    *info = lastInfo;
    return lastInfo.apiVersion ? kOfxStatOK : kOfxStatFailed;
}
