// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/Image.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Dot.h"
#include "Engine/OfxClipInstance.h"
#include "Engine/OfxVulkanImageSuite.h"
#include "Engine/VulkanImageContextPool.h"
#include "Gpu/VulkanDevice.h"
#include "Gpu/ViewerFrame.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <limits>
using namespace Natron;
namespace
{
unsigned cases = 0;
void check(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }

    ++cases;
}
void status(OfxStatus actual, OfxStatus expected, const char* message)
{
    check(actual == expected, message);
}
ImagePtr ram(const RectI& bounds)
{
    return std::make_shared<Image>(ImagePlaneDesc::getRGBAComponents(), RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds,
                                   0, 1., eImageBitDepthFloat, eImagePremultiplicationPremultiplied, eImageFieldingOrderNone, true);
}
std::unique_ptr<OfxImage> clip(const ImagePtr& image, bool source, const RectI& roi)
{
    return std::make_unique<OfxImage>(OfxClipInstance::RenderActionDataPtr(), image, source, roi,
                                      Transform::Matrix3x3Ptr(), kOfxImageComponentRGBA, 4, 1.);
}
struct Context {
    NatronVulkanContextHandle handle = 0;
    Context()
    {
        status(ofxVulkanImageSuite().createContext(-1, 1, &handle), kOfxStatOK, "Could not create clip runtime.");
    }
    ~Context()
    {
        if (handle) {
            ofxVulkanImageSuite().releaseContext(handle);
        }
    }
};
void test()
{
    const auto& images = ofxVulkanImageSuite();
    const auto& render = ofxVulkanRenderSuite();
    const auto& shaders = ofxVulkanShaderSuite();
    auto runtime = sharedVulkanImageContext(-1, true);
    const auto devices = runtime->device()->instance()->physicalDevices();
    const auto selected = std::find(devices.begin(), devices.end(), runtime->device()->physicalDevice());
    check(selected != devices.end(), "Selected physical device is absent from its instance.");
    check(sharedVulkanImageContext(int(selected - devices.begin()), true) == runtime,
          "Environment and explicit selection created different logical devices.");
    const bool hadSelection = qEnvironmentVariableIsSet("NATRON_VULKAN_DEVICE");
    const QByteArray savedSelection = qgetenv("NATRON_VULKAN_DEVICE");
    qunsetenv("NATRON_VULKAN_DEVICE");
    auto automatic = sharedVulkanImageContext(-1, true);

    if (hadSelection) {
        qputenv("NATRON_VULKAN_DEVICE", savedSelection);
    }

    check(automatic->device()->instance() == runtime->device()->instance(), "Physical devices did not share the Vulkan instance.");
    const auto autoSelected = std::find(devices.begin(), devices.end(), automatic->device()->physicalDevice());
    check(autoSelected != devices.end() && sharedVulkanImageContext(int(autoSelected - devices.begin()), true) == automatic,
          "Automatic and explicit selection created different logical devices.");
    {
        VulkanImageStorage source(runtime, 7, 5);
        source.fill(0, 0, 7, 5, {.25f, -.5f, 2.f, .37f});
        Gpu::ViewerFrame snapshot(runtime->device(), {-3, -2, 3, 2}, Gpu::ViewerStorage::RGBA32F);
        const auto before = runtime->transferCounts();
        source.copyTo(*snapshot.image(), 2, 1, 4, 3, 1, 1);
        source.fill(0, 0, 7, 5, {9, 8, 7, 6});
        check(runtime->transferCounts() == before, "Viewer GPU snapshot used CPU pixel transfers.");
        const auto bytes = snapshot.image()->download();
        const float copied[4] {.25f, -.5f, 2.f, .37f}, empty[4] {};

        for (int y = 0; y < 4; ++y) for (int x = 0; x < 6; ++x) {
                const bool inside = x >= 1 && x < 5 && y >= 1 && y < 4;
                check(std::memcmp(bytes.data() + (y * 6 + x) * 16, inside ? copied : empty, 16) == 0,
                      "Viewer snapshot changed with source writes or exposed padding.");
            }

        source.fill(0, 0, 7, 5, {9, 8, 7, std::numeric_limits<float>::quiet_NaN()});
        source.copyTo(*snapshot.image(), 2, 1, 4, 3, 1, 1, true);
        const auto opaqueBytes = snapshot.image()->download();
        const float opaque[4] {9, 8, 7, 1};
        check(std::memcmp(opaqueBytes.data() + (1 * 6 + 1) * 16, opaque, 16) == 0,
              "Opaque Viewer snapshot did not preserve RGB and force alpha.");
        check(runtime->transferCounts() == before, "Opaque Viewer snapshot used CPU pixel transfers.");
    }
    Context first, second;
    const RectI bounds(-5, -3, 2, 2), visible(-3, -2, 1, 1);
    // Reproduce the interval between publishing a native cache entry and
    // allocating its image, as seen by a concurrent abortable Viewer render.
    {
        auto pending = std::make_shared<Image>(ImageKey(), Image::makeVulkanParams(runtime,
                                               RectD(bounds.x1, bounds.y1, bounds.x2, bounds.y2), bounds), nullptr);
        check(!pending->isAllocated(), "Pending cache fixture was already allocated.");
        Dot effect{NodePtr()};
        EffectInstance::InputImagesMap inputs;
        inputs[0].push_back(pending);
        ImagePtr found;
        const auto before = runtime->transferCounts();
        effect.getImageFromCacheAndConvertIfNeeded(true, eStorageModeVulkan, eStorageModeRAM,
                pending->getKey(), 0, nullptr, nullptr, bounds,
                eImageBitDepthFloat, ImagePlaneDesc::getRGBAComponents(),
                inputs, RenderStatsPtr(), &found);
        check(found == pending && pending->isAllocated(), "Cache lookup did not synchronize native allocation.");
        check(found->getVulkanImage()->context() == runtime, "Pending cache allocation changed runtime.");
        check(runtime->transferCounts() == before, "Pending native cache allocation transferred CPU pixels.");
        std::list<RectI> missing;
        found->getRestToRender(bounds, missing);
        check(!missing.empty(), "Pending native cache entry was marked complete.");
    }
    auto cpuSource = ram(bounds), cpuDestination = ram(bounds);
    cpuSource->fill(bounds, .25f, -.5f, 2.f, 1.f);
    cpuDestination->fill(bounds, -2.f, -2.f, -2.f, -2.f);
    cpuSource->markForRendered(bounds);
    cpuDestination->markForRendered(RectI(-5, -3, -3, -2));
    auto source = Image::convertToStorage(cpuSource, eStorageModeVulkan, runtime);
    auto destination = Image::convertToStorage(cpuDestination, eStorageModeVulkan, runtime);
    check(source->getVulkanImage()->context() == runtime && source->usesBitMap(), "CPU boundary lost native runtime/bitmap.");
    check(Image::convertToStorage(source, eStorageModeVulkan, runtime) == source, "Native storage conversion copied a resident image.");
    const auto before = runtime->transferCounts();
    NatronVulkanImageHandle loan = 0;
    NatronVulkanProgramHandle program = 0;
    const float uniforms[4] {.5f, -1.f, 4.f, 2.f};
    const char *fragment = "#version 450\nlayout(location=0) out vec4 result;\nvoid main(){result=vec4(.5,-1.,4.,2.);}";
    status(shaders.createProgram(first.handle, fragment, sizeof(uniforms), &program, nullptr, 0), kOfxStatOK, "Native persistent shader creation failed.");
    OfxPropertySetHandle staleProperties = nullptr;
    {
        auto srcClip = clip(source, true, visible), dstClip = clip(destination, false, visible);
        staleProperties = srcClip->getPropHandle();
        check(srcClip->getPointerProperty(kOfxImagePropData) == nullptr && dstClip->getPointerProperty(kOfxImagePropData) == nullptr &&
              srcClip->getIntProperty(kOfxImagePropRowBytes) == 0, "Native clips exposed CPU data.");
        NatronVulkanImageView src{}, dst{}, repeated{}, other{};
        status(render.acquireImage(first.handle, srcClip->getPropHandle(), &src), kOfxStatOK, "Source borrowing failed.");
        status(render.acquireImage(first.handle, dstClip->getPropHandle(), &dst), kOfxStatOK, "Output borrowing failed.");
        status(render.acquireImage(first.handle, srcClip->getPropHandle(), &repeated), kOfxStatOK, "Repeated source borrowing failed.");
        check(src.image == repeated.image && !src.writable && dst.writable, "Native loan identity or permissions changed.");
        check(src.bounds[0] == visible.x1 && src.bounds[1] == visible.y1 && src.bounds[2] == visible.x2 &&
              dst.bounds[0] == bounds.x1 && dst.visibleBounds[0] == visible.x1, "Global clip bounds/ROI were not preserved.");
        status(render.acquireImage(second.handle, srcClip->getPropHandle(), &other), kOfxStatOK, "Second namespace borrowing failed.");
        check(other.image != src.image, "Context namespaces shared an image handle.");
        status(images.copy(second.handle, src.image, other.image, nullptr, 0, 0), kOfxStatErrBadHandle, "Foreign context accepted a loan token.");
        const float gain[4] {2, 2, 2, 2}, offset[4] {};
        const NatronVulkanRegion allowed{2, 1, 4, 3}, outside{0, 0, 1, 1};
        float pixels[7 * 5 * 4] {};
        status(images.releaseImage(first.handle, src.image), kOfxStatErrValue, "Borrower released host source image.");
        status(images.releaseImage(first.handle, dst.image), kOfxStatErrValue, "Borrower released host output image.");
        status(images.upload(first.handle, src.image, pixels, 7 * 16), kOfxStatErrValue, "Read-only upload succeeded.");
        status(images.copy(first.handle, src.image, src.image, nullptr, 0, 0), kOfxStatErrValue, "Read-only copy succeeded.");
        status(images.transform(first.handle, src.image, src.image, nullptr, gain, offset), kOfxStatErrValue, "Read-only compute succeeded.");
        status(images.mix(first.handle, src.image, src.image, 0, src.image, nullptr, 1, 0, 0), kOfxStatErrValue, "Read-only mix no-op bypassed permissions.");
        status(images.restoreChannels(first.handle, src.image, 0, src.image, nullptr, 0), kOfxStatErrValue, "Read-only restore no-op bypassed permissions.");
        status(shaders.render(first.handle, program, src.image, nullptr, nullptr, uniforms, sizeof(uniforms)), kOfxStatErrValue, "Read-only shader succeeded.");
        status(images.upload(first.handle, dst.image, pixels, 7 * 16), kOfxStatErrValue, "Full output upload escaped the clip ROI.");
        status(images.download(first.handle, dst.image, pixels, 7 * 16), kOfxStatErrValue, "Full output read escaped the clip ROI.");
        status(images.copy(first.handle, src.image, dst.image, nullptr, 0, 0), kOfxStatErrValue, "Output copy escaped clip ROI.");
        status(images.transform(first.handle, dst.image, dst.image, &outside, gain, offset), kOfxStatErrValue, "Output compute escaped clip ROI.");
        status(shaders.render(first.handle, program, dst.image, &outside, nullptr, uniforms, sizeof(uniforms)), kOfxStatErrValue, "Output shader escaped clip ROI.");
        status(images.copy(first.handle, src.image, dst.image, nullptr, 2, 1), kOfxStatOK, "ROI GPU copy failed.");
        status(images.transform(first.handle, dst.image, dst.image, &allowed, gain, offset), kOfxStatOK, "ROI aliased compute failed.");
        status(shaders.render(first.handle, program, dst.image, &allowed, nullptr, uniforms, sizeof(uniforms)), kOfxStatOK, "ROI fragment shader failed.");
        loan = src.image;
        srcClip.reset();
        status(images.download(first.handle, loan, pixels, 7 * 16), kOfxStatErrBadHandle, "Released clip loan remained valid.");
        status(images.copy(second.handle, other.image, other.image, nullptr, 0, 0), kOfxStatErrBadHandle, "Released clip loan survived in another context.");
        status(render.acquireImage(first.handle, staleProperties, &repeated), kOfxStatErrBadHandle, "Released standard OFX image could be borrowed.");
        status(render.acquireImage(first.handle, dstClip->getPropHandle(), nullptr), kOfxStatErrValue, "Null image-view destination was accepted.");
    }
    check(runtime->transferCounts() == before, "Borrowing/compute used CPU pixel transfers.");
    auto cpuResult = Image::convertToStorage(destination, eStorageModeRAM, nullptr);
    auto original = Image::convertToStorage(source, eStorageModeRAM, nullptr);
    {
        const auto resultAccess = cpuResult->getReadRights(), sourceAccess = original->getReadRights();
        const float input[4] {.25f, -.5f, 2.f, 1.f}, processed[4] {.5f, -1.f, 4.f, 2.f}, untouched[4] {-2.f, -2.f, -2.f, -2.f};

        for (int y = bounds.y1; y < bounds.y2; ++y) for (int x = bounds.x1; x < bounds.x2; ++x) {
                const bool inside = x >= visible.x1 && x < visible.x2 && y >= visible.y1 && y < visible.y2;
                check(std::memcmp(resultAccess.pixelAt(x, y), inside ? processed : untouched, sizeof(input)) == 0, "Output ROI pixels differ.");
                check(std::memcmp(sourceAccess.pixelAt(x, y), input, sizeof(input)) == 0, "Read-only input pixels changed.");
            }
    }
    std::list<RectI> beforeMissing, afterMissing;
    cpuDestination->getRestToRender(bounds, beforeMissing);
    cpuResult->getRestToRender(bounds, afterMissing);
    check(beforeMissing == afterMissing, "Borrowed GPU writes changed the host bitmap.");
    // A shader belongs to the plugin context, not to a transient clip image.
    {
        auto dstClip = clip(destination, false, bounds);
        NatronVulkanImageView dst{};
        status(render.acquireImage(first.handle, dstClip->getPropHandle(), &dst), kOfxStatOK, "Second render borrowing failed.");
        status(shaders.render(first.handle, program, dst.image, nullptr, nullptr, uniforms, sizeof(uniforms)), kOfxStatOK, "Cached shader died with the previous clip.");
        status(images.releaseContext(first.handle), kOfxStatOK, "Context release before clip release failed.");
        first.handle = 0;
    }
    check(runtime->device()->instance()->validationErrorCount() == 0, "Vulkan clip validation errors.");
    std::cout << "{\"ok\":true,\"passed\":true,\"device\":\"" << runtime->device()->properties().deviceName
              << "\",\"referenceCases\":" << cases << ",\"nativePixelTransfers\":0,\"validationErrors\":0,\"borrowLifetime\":true,\"roiPermissions\":true,\"synchronizedCacheAllocation\":true,\"normalizedDeviceSelection\":true,\"sharedInstance\":true,\"viewerSnapshot\":true}" << std::endl;
}
}
int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        qputenv("NATRON_VULKAN_VALIDATION", "1");
        AppManager manager;
        test();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
