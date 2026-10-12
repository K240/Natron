// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX C ABI consumer: no Natron, Qt, Vulkan or OpenGL linkage.
#include "Global/OpenFX/NatronVulkanImageSuite.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include "ofxParam.h"
#include "ofxNatron.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
namespace
{
const OfxPropertySuiteV1 *properties = nullptr;
const OfxImageEffectSuiteV1 *effects = nullptr;
const NatronOfxVulkanImageSuiteV1 *gpu = nullptr;
const NatronOfxVulkanShaderSuiteV1 *shaderSuite = nullptr;
const NatronOfxVulkanRenderSuiteV1 *renderSuite = nullptr;
const OfxParameterSuiteV1 *parameters = nullptr;
bool rejectedVersion = false;
std::atomic<unsigned long long> renders{0}, selfTests{0};
std::atomic<unsigned long long> nativeRenders{0}, nativePixelTransfers{0};
std::array<std::atomic<unsigned long long>, 6> nativeScaleRenders{};
std::atomic<float> maximumError{0};
std::mutex infoMutex;
NatronVulkanDeviceInfo lastInfo{};
void require(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}
void check(bool condition)
{
    if (!condition) {
        throw OfxStatus(kOfxStatFailed);
    }
}
void recordInfo(NatronVulkanContextHandle context)
{
    NatronVulkanDeviceInfo info{};
    require(gpu->getDeviceInfo(context, &info));
    check(info.validationErrors == 0);
    std::lock_guard<std::mutex> lock(infoMutex);
    lastInfo = info;
}
struct Context {
    NatronVulkanContextHandle handle = 0;
    Context()
    {
        require(gpu->createContext(-1, 0, &handle));
    }
    ~Context()
    {
        if (handle) {
            gpu->releaseContext(handle);
        }
    }
};
struct Buffer {
    NatronVulkanContextHandle context;
    NatronVulkanImageHandle handle = 0;
    Buffer(NatronVulkanContextHandle ctx, uint32_t w, uint32_t h) : context(ctx)
    {
        require(gpu->createImage(ctx, w, h, &handle));
    }
    ~Buffer()
    {
        if (handle) {
            gpu->releaseImage(context, handle);
        }
    }
};
struct Instance {
    Context context;
    OfxImageClipHandle source = nullptr, output = nullptr;
    OfxParamHandle nativeEnabled = nullptr;
    std::mutex mutex;
};
struct ClipImage {
    OfxPropertySetHandle handle = nullptr;
    char *data = nullptr;
    std::array<int, 4> bounds{};
    int rowBytes = 0;
    ClipImage(OfxImageClipHandle clip, double time, const OfxRectD& roi, bool native = false)
    {
        require(effects->clipGetImage(clip, time, &roi, &handle));

        try {
            const char *depth = nullptr, *components = nullptr;
            require(properties->propGetString(handle, kOfxImageEffectPropPixelDepth, 0, &depth));
            require(properties->propGetString(handle, kOfxImageEffectPropComponents, 0, &components));
            check(depth && components && std::strcmp(depth, kOfxBitDepthFloat) == 0 && std::strcmp(components, kOfxImageComponentRGBA) == 0);
            require(properties->propGetPointer(handle, kOfxImagePropData, 0, reinterpret_cast<void**>(&data)));
            require(properties->propGetIntN(handle, kOfxImagePropBounds, 4, bounds.data()));
            require(properties->propGetInt(handle, kOfxImagePropRowBytes, 0, &rowBytes));
            check((native ? !data && rowBytes == 0 : data != nullptr) && bounds[2] > bounds[0] && bounds[3] > bounds[1]);
        } catch (...) {
            effects->clipReleaseImage(handle);
            handle = nullptr;
            throw;
        }
    }
    ~ClipImage()
    {
        if (handle) {
            effects->clipReleaseImage(handle);
        }
    }
    char *pixel(int x, int y) const
    {
        if (x < bounds[0] || y < bounds[1] || x >= bounds[2] || y >= bounds[3]) {
            return nullptr;
        }

        return data + static_cast<ptrdiff_t>(y - bounds[1]) * rowBytes + static_cast<ptrdiff_t>(x - bounds[0]) * 4 * sizeof(float);
    }
};
void render(Instance& state, OfxPropertySetHandle args)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    double time = 0;
    std::array<int, 4> window{};
    require(properties->propGetDouble(args, kOfxPropTime, 0, &time));
    require(properties->propGetIntN(args, kOfxImageEffectPropRenderWindow, 4, window.data()));
    double scale[2] {1, 1};
    require(properties->propGetDoubleN(args, kOfxImageEffectPropRenderScale, 2, scale));
    // This test node declares square pixels. Convert the render ROI to canonical
    // coordinates so scaled renders fetch the correct standard OFX CPU images.
    OfxRectD canonical{window[0] / scale[0], window[1] / scale[1], window[2] / scale[0], window[3] / scale[1]};
    int native = 0;
    require(properties->propGetInt(args, kNatronOfxImageEffectPropVulkanRenderingEnabled, 0, &native));
    ClipImage output(state.output, time, canonical, native != 0), source(state.source, time, canonical, native != 0);
    window[0] = std::max(window[0], output.bounds[0]);
    window[1] = std::max(window[1], output.bounds[1]);
    window[2] = std::min(window[2], output.bounds[2]);
    window[3] = std::min(window[3], output.bounds[3]);

    if (window[2] <= window[0] || window[3] <= window[1]) {
        return;
    }

    const uint32_t w = static_cast<uint32_t>(window[2] - window[0]), h = static_cast<uint32_t>(window[3] - window[1]);

    if (native) {
        const auto ctx = state.context.handle;
        uint64_t uploads = 0, downloads = 0, afterUploads = 0, afterDownloads = 0;
        require(renderSuite->getTransferCounts(ctx, &uploads, &downloads));
        NatronVulkanImageView inputView{}, outputView{}, repeated{};
        require(renderSuite->acquireImage(ctx, source.handle, &inputView));
        require(renderSuite->acquireImage(ctx, output.handle, &outputView));
        require(renderSuite->acquireImage(ctx, source.handle, &repeated));
        check(inputView.image == repeated.image && !inputView.writable && outputView.writable);
        check(gpu->releaseImage(ctx, inputView.image) == kOfxStatErrValue);
        const float identity[4] {1, 1, 1, 1}, zero[4] {};
        check(gpu->transform(ctx, inputView.image, inputView.image, nullptr, identity, zero) == kOfxStatErrValue);
        Buffer input(ctx, w, h), processed(ctx, w, h), copied(ctx, w, h);
        const int x1 = std::max(window[0], inputView.visibleBounds[0]), y1 = std::max(window[1], inputView.visibleBounds[1]);
        const int x2 = std::min(window[2], inputView.visibleBounds[2]), y2 = std::min(window[3], inputView.visibleBounds[3]);

        if (x2 > x1 && y2 > y1) {
            NatronVulkanRegion sourceArea{uint32_t(x1 - inputView.bounds[0]), uint32_t(y1 - inputView.bounds[1]), uint32_t(x2 - x1), uint32_t(y2 - y1)};
            require(gpu->copy(ctx, inputView.image, input.handle, &sourceArea, uint32_t(x1 - window[0]), uint32_t(y1 - window[1])));
        }

        const float gain[4] {1.25f, .5f, -.75f, 1}, offset[4] {.1f, -.2f, .3f, 0};
        require(gpu->transform(ctx, input.handle, processed.handle, nullptr, gain, offset));
        require(gpu->restoreChannels(ctx, processed.handle, input.handle, processed.handle, nullptr, 5));
        require(gpu->mix(ctx, processed.handle, input.handle, input.handle, processed.handle, nullptr, .35f, 1, 1));
        require(gpu->copy(ctx, processed.handle, copied.handle, nullptr, 0, 0));
        require(gpu->copy(ctx, copied.handle, outputView.image, nullptr,
                          uint32_t(window[0] - outputView.bounds[0]), uint32_t(window[1] - outputView.bounds[1])));
        require(renderSuite->getTransferCounts(ctx, &afterUploads, &afterDownloads));
        nativePixelTransfers += afterUploads - uploads + afterDownloads - downloads;
        check(afterUploads == uploads && afterDownloads == downloads);
        recordInfo(ctx);
        double outputScale[2] {}, inputScale[2] {};
        require(properties->propGetDoubleN(output.handle, kOfxImageEffectPropRenderScale, 2, outputScale));
        require(properties->propGetDoubleN(source.handle, kOfxImageEffectPropRenderScale, 2, inputScale));
        check(outputScale[0] == scale[0] && outputScale[1] == scale[1] &&
              inputScale[0] == scale[0] && inputScale[1] == scale[1]);

        for (unsigned level = 0; level < nativeScaleRenders.size(); ++level) {
            if (scale[0] == std::ldexp(1., -int(level)) && scale[1] == scale[0]) {
                ++nativeScaleRenders[level];
            }
        }

        ++nativeRenders;
        ++renders;
        return;
    }

    std::vector<float> packed(size_t(w) * h * 4, 0);

    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            if (const char * pixel = source.pixel(window[0] + static_cast<int>(x), window[1] + static_cast<int>(y))) {
                std::memcpy(packed.data() + (size_t(y) * w + x) * 4, pixel, 4 * sizeof(float));
            }
        }

    const auto ctx = state.context.handle;
    Buffer input(ctx, w, h), processed(ctx, w, h), copied(ctx, w, h);
    require(gpu->upload(ctx, input.handle, packed.data(), int64_t(w) * 4 * sizeof(float)));
    const float gain[4] {1.25f, 0.5f, -0.75f, 1}, offset[4] {0.1f, -0.2f, 0.3f, 0};
    require(gpu->transform(ctx, input.handle, processed.handle, nullptr, gain, offset));
    require(gpu->restoreChannels(ctx, processed.handle, input.handle, processed.handle, nullptr, 5));
    require(gpu->mix(ctx, processed.handle, input.handle, input.handle, processed.handle, nullptr, 0.35f, 1, 1));
    require(gpu->copy(ctx, processed.handle, copied.handle, nullptr, 0, 0));
    require(gpu->download(ctx, copied.handle, output.pixel(window[0], window[1]), output.rowBytes));
    float error = 0;

    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
            float actual[4];
            std::memcpy(actual, output.pixel(window[0] + static_cast<int>(x), window[1] + static_cast<int>(y)), sizeof(actual));
            const float *inputPixel = packed.data() + (size_t(y) * w + x) * 4;
            const float alpha = 0.35f * (1 - inputPixel[3]);

            for (uint32_t c = 0; c < 4; ++c) {
                const float expected = c == 1 ? (inputPixel[c] * 0.5f - 0.2f) * alpha + inputPixel[c] * (1 - alpha) : inputPixel[c];
                const float difference = std::abs(actual[c] - expected);
                check(std::isfinite(actual[c]) && difference <= 0.00001f);
                error = std::max(error, difference);
            }
        }

    float previous = maximumError.load();

    while (previous < error && !maximumError.compare_exchange_weak(previous, error)) {}

    recordInfo(ctx);
    ++renders;
}
void setHost(OfxHost* host)
{
    properties = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    gpu = static_cast<const NatronOfxVulkanImageSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 1));
    shaderSuite = static_cast<const NatronOfxVulkanShaderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanShaderSuite, 1));
    renderSuite = static_cast<const NatronOfxVulkanRenderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    rejectedVersion = host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 2) == nullptr &&
                      host->fetchSuite(host->host, kNatronOfxVulkanShaderSuite, 3) == nullptr &&
                      host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 2) == nullptr;
}
OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle)
{
    try {
        if (std::strcmp(action, kOfxActionLoad) == 0) {
            return properties && effects && gpu && renderSuite && parameters && rejectedVersion ? kOfxStatOK : kOfxStatErrMissingHostFeature;
        }

        auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
        OfxPropertySetHandle props = nullptr;

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            require(effects->getPropertySet(effect, &props));
            require(properties->propSetString(props, kOfxPropLabel, 0, "Vulkan Image Suite Smoke"));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1));
            require(properties->propSetInt(props, kNatronOfxImageEffectPropVulkanRenderSupported, 0, 1));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 1));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0));
            require(properties->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            OfxParamSetHandle params = nullptr;
            require(effects->getParamSet(effect, &params));
            require(parameters->paramDefine(params, kOfxParamTypeBoolean, "nativeEnabled", &props));
            require(properties->propSetInt(props, kOfxParamPropDefault, 0, 1));
            require(properties->propSetInt(props, kOfxParamPropAnimates, 0, 0));

            for (const char * name : {
                     "Source", "Output"
                 }) {
                require(effects->clipDefine(effect, name, &props));
                require(properties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));
            }
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            auto state = std::make_unique<Instance>();
            require(effects->clipGetHandle(effect, "Source", &state->source, nullptr));
            require(effects->clipGetHandle(effect, "Output", &state->output, nullptr));
            OfxParamSetHandle params = nullptr;
            require(effects->getParamSet(effect, &params));
            require(parameters->paramGetHandle(params, "nativeEnabled", &state->nativeEnabled, nullptr));
            int enabled = 0;
            require(parameters->paramGetValue(state->nativeEnabled, &enabled));
            require(effects->getPropertySet(effect, &props));
            require(properties->propSetInt(props, kNatronOfxImageEffectPropVulkanRenderSupported, 0, enabled));
            require(properties->propSetPointer(props, kOfxPropInstanceData, 0, state.get()));
            state.release();
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionInstanceChanged) == 0) {
            require(effects->getPropertySet(effect, &props));
            void *data = nullptr;
            require(properties->propGetPointer(props, kOfxPropInstanceData, 0, &data));

            if (auto state = static_cast<Instance * >(data)) {
                int enabled = 0;
                require(parameters->paramGetValue(state->nativeEnabled, &enabled));
                require(properties->propSetInt(props, kNatronOfxImageEffectPropVulkanRenderSupported, 0, enabled));
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0 || std::strcmp(action, kOfxImageEffectActionRender) == 0) {
            require(effects->getPropertySet(effect, &props));
            Instance* state = nullptr;
            require(properties->propGetPointer(props, kOfxPropInstanceData, 0, reinterpret_cast<void**>(&state)));
            check(state != nullptr);

            if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
                require(properties->propSetPointer(props, kOfxPropInstanceData, 0, nullptr));
                delete state;
            } else {
                render(*state, args);
            }

            return kOfxStatOK;
        }

        return kOfxStatReplyDefault;
    } catch (OfxStatus status) {
        return status;
    } catch (const std::bad_alloc &) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatFailed;
    }
}
OfxStatus fullScaleEntry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle result)
{
    if (std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0 ||
        std::strcmp(action, kOfxImageEffectActionRender) == 0) {
        double scale[2] = {};

        if (properties->propGetDoubleN(args, kOfxImageEffectPropRenderScale, 2, scale) != kOfxStatOK ||
            scale[0] != 1. || scale[1] != 1.) {
            return kOfxStatFailed;
        }
    }

    const auto status = mainEntry(action, handle, args, result);

    if (status == kOfxStatOK && std::strcmp(action, kOfxActionDescribe) == 0) {
        OfxPropertySetHandle props = nullptr;

        if (effects->getPropertySet(static_cast<OfxImageEffectHandle>(const_cast<void * >(handle)), &props) != kOfxStatOK ||
            properties->propSetString(props, kOfxPropLabel, 0, "Vulkan Full Scale Suite Smoke") != kOfxStatOK) {
            return kOfxStatFailed;
        }
    }

    return status;
}
OfxStatus fullScaleMaskEntry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle result)
{
    const auto status = fullScaleEntry(action, handle, args, result);

    if (status == kOfxStatOK && std::strcmp(action, kOfxActionDescribe) == 0) {
        OfxPropertySetHandle props = nullptr;

        if (effects->getPropertySet(static_cast<OfxImageEffectHandle>(const_cast<void * >(handle)), &props) != kOfxStatOK ||
            properties->propSetString(props, kOfxPropLabel, 0, "Vulkan Full Scale Mask Suite Smoke") != kOfxStatOK ||
            properties->propSetInt(props, kNatronOfxImageEffectPropHostMasking, 0, 1) != kOfxStatOK ||
            properties->propSetInt(props, kNatronOfxImageEffectPropHostMixing, 0, 1) != kOfxStatOK) {
            return kOfxStatFailed;
        }
    }

    return status;
}
OfxPlugin plugins[] {
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanImageSuite", 1, 0, setHost, mainEntry},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanFullScaleSuite", 1, 0, setHost, fullScaleEntry},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanFullScaleMaskSuite", 1, 0, setHost, fullScaleMaskEntry}
};
void testBuffers(NatronVulkanContextHandle context, int seed)
{
    constexpr uint32_t w = 7, h = 5, stride = 40;
    Buffer input(context, w, h), output(context, w, h);
    std::vector<float> source(stride * h, 123), result(stride * h, -321);

    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w * 4; ++x) {
            source[y * stride + x] = float(y * 31 + x + seed) / 37 - 0.5f;
        }

    require(gpu->upload(context, input.handle, source.data() + stride * (h - 1), -int64_t(stride * sizeof(float))));
    const float gain[4] {1, 2, -1, 0.5f}, offset[4] {0, 0.2f, -0.1f, 0};
    NatronVulkanRegion roi{1, 1, 4, 3};
    require(gpu->copy(context, input.handle, output.handle, nullptr, 0, 0));
    require(gpu->transform(context, output.handle, output.handle, &roi, gain, offset));
    require(gpu->download(context, output.handle, result.data() + stride * (h - 1), -int64_t(stride * sizeof(float))));

    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < stride; ++x) {
            float expected = x < w * 4 ? source[y * stride + x] : -321;
            uint32_t logicalY = h - y - 1;

            if (x < w * 4 && x / 4 >= roi.x && x / 4 < roi.x + roi.width && logicalY >= roi.y && logicalY < roi.y + roi.height) {
                expected = expected * gain[x % 4] + offset[x % 4];
            }

            check(std::abs(result[y * stride + x] - expected) < 0.00001f);
        }

    std::fill(result.begin(), result.end(), -321);
    require(gpu->download(context, input.handle, result.data(), stride * sizeof(float)));

    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < stride; ++x) {
            check(result[y * stride + x] == (x < w * 4 ? source[(h - y - 1) * stride + x] : -321));
        }

    check(gpu->upload(context, input.handle, source.data(), 1) == kOfxStatErrValue);
    check(gpu->download(context, input.handle, nullptr, stride * sizeof(float)) == kOfxStatErrValue);
    check(gpu->upload(context, input.handle, source.data(), std::numeric_limits<int64_t>::min()) == kOfxStatErrValue);
    check(gpu->upload(context, input.handle, source.data(), std::numeric_limits<int64_t>::max()) == kOfxStatErrValue);
    NatronVulkanRegion bad{UINT32_MAX, 0, 2, 1};
    check(gpu->copy(context, input.handle, output.handle, &bad, 0, 0) == kOfxStatErrValue);
    check(gpu->restoreChannels(context, input.handle, 0, output.handle, nullptr, 16) == kOfxStatErrValue);
    const auto stale = output.handle;
    require(gpu->releaseImage(context, stale));
    output.handle = 0;
    check(gpu->releaseImage(context, stale) == kOfxStatErrBadHandle);
    check(gpu->copy(context, input.handle, stale, nullptr, 0, 0) == kOfxStatErrBadHandle);
}
}
#ifdef _WIN32
#define SMOKE_EXPORT __declspec(dllexport)
#else
#define SMOKE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" SMOKE_EXPORT int OfxGetNumberOfPlugins()
{
    return 3;
}
extern "C" SMOKE_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    return index >= 0 && index < 3 ? &plugins[index] : nullptr;
}
extern "C" SMOKE_EXPORT unsigned long long OfxVulkanSmokeRenderCount()
{
    return renders.load();
}
extern "C" SMOKE_EXPORT unsigned long long OfxVulkanSmokeNativeRenderCount()
{
    return nativeRenders.load();
}
extern "C" SMOKE_EXPORT unsigned long long OfxVulkanSmokeNativeScaleRenderCount(unsigned level)
{
    return level < nativeScaleRenders.size() ? nativeScaleRenders[level].load() : 0;
}
extern "C" SMOKE_EXPORT unsigned long long OfxVulkanSmokeNativePixelTransfers()
{
    return nativePixelTransfers.load();
}
extern "C" SMOKE_EXPORT float OfxVulkanSmokeMaximumError()
{
    return maximumError.load();
}
extern "C" SMOKE_EXPORT int OfxVulkanSmokeTransferCounts(uint64_t* uploads, uint64_t* downloads)
{
    try {
        Context context;
        require(renderSuite->getTransferCounts(context.handle, uploads, downloads));
        return 1;
    } catch (...) {
        return 0;
    }
}
extern "C" SMOKE_EXPORT int OfxVulkanSmokeDeviceInfo(NatronVulkanDeviceInfo* info)
{
    if (!info) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(infoMutex);
    *info = lastInfo;
    return info->name[0] != 0;
}
extern "C" SMOKE_EXPORT int OfxVulkanSmokeSelfTest()
{
    try {
        check(gpu && shaderSuite && rejectedVersion);
        Context first, second;
        NatronVulkanProgramHandle program = 99;
        std::array<char, 8> diagnostic{};
        check(shaderSuite->createProgram(first.handle, "#version 450\ninvalid shader", 16, &program, diagnostic.data(), diagnostic.size()) == kOfxStatErrValue);
        check(program == 0 && diagnostic[0] && diagnostic.back() == '\0');
        const char *source = R"(#version 450
layout(set=0,binding=0) uniform sampler2D input0;
layout(set=0,binding=4,std140) uniform Params { vec4 gain; };
layout(location=0) out vec4 color;
void main(){color=texelFetch(input0,ivec2(gl_FragCoord.xy),0)*gain;})";
        require(shaderSuite->createProgram(first.handle, source, 16, &program, nullptr, 0));
        check(program != 0);
        uint32_t samplerMask = 0, uniformCount = 0, offset = 99;
        require(shaderSuite->getResources(first.handle, program, &samplerMask, &uniformCount, nullptr, 0));
        check(samplerMask == 1 && uniformCount == 1);
        require(shaderSuite->getResources(first.handle, program, &samplerMask, &uniformCount, &offset, 1));
        check(offset == 0);
        check(shaderSuite->getResources(first.handle, program, &samplerMask, nullptr, nullptr, 0) == kOfxStatErrValue);
        check(shaderSuite->getResources(first.handle, program, &samplerMask, &uniformCount, nullptr, 1) == kOfxStatErrValue);
        Buffer shaderImage(first.handle, 7, 5);
        std::vector<float> shaderValues(7 * 5 * 4);

        for (size_t i = 0; i < shaderValues.size(); ++i) {
            shaderValues[i] = float(i) / 137 - .5f;
        }

        require(gpu->upload(first.handle, shaderImage.handle, shaderValues.data(), 7 * 16));
        std::array<NatronVulkanShaderInput, 4> shaderInputs{};
        shaderInputs[0].image = shaderImage.handle;
        std::array<float, 4> gain{2, -1, .5f, 1.5f};
        NatronVulkanRegion shaderRoi{1, 1, 5, 3};
        require(shaderSuite->render(first.handle, program, shaderImage.handle, &shaderRoi, shaderInputs.data(), gain.data(), 16));
        std::vector<float> shaderResult(shaderValues.size());
        require(gpu->download(first.handle, shaderImage.handle, shaderResult.data(), 7 * 16));

        for (unsigned y = 0; y < 5; ++y) for (unsigned x = 0; x < 7; ++x) for (unsigned c = 0; c < 4; ++c) {
                    size_t i = (y * 7 + x) * 4 + c;
                    float expected = shaderValues[i] * (x >= 1 && x < 6 && y >= 1 && y < 4 ? gain[c] : 1.f);
                    check(std::abs(shaderResult[i] - expected) < 0.00001f);
                }

        check(shaderSuite->render(second.handle, program, shaderImage.handle, nullptr, nullptr, gain.data(), 16) == kOfxStatErrBadHandle);
        check(shaderSuite->render(first.handle, program, shaderImage.handle, nullptr, nullptr, gain.data(), 15) == kOfxStatErrValue);
        shaderInputs[0].filter = 4;
        check(shaderSuite->render(first.handle, program, shaderImage.handle, nullptr, shaderInputs.data(), gain.data(), 16) == kOfxStatErrValue);
        require(shaderSuite->releaseProgram(first.handle, program));
        check(shaderSuite->releaseProgram(first.handle, program) == kOfxStatErrBadHandle);
        check(shaderSuite->render(first.handle, program, shaderImage.handle, nullptr, nullptr, gain.data(), 16) == kOfxStatErrBadHandle);
        require(shaderSuite->createProgram(second.handle, source, 16, &program, nullptr, 0));
        NatronVulkanContextHandle bad = 99;
        check(gpu->createContext(-2, 0, &bad) == kOfxStatErrValue && bad == 0);
        check(gpu->createContext(-1, 0, nullptr) == kOfxStatErrValue);
        Buffer foreign(first.handle, 7, 5);
        std::vector<float> data(7 * 5 * 4);
        check(gpu->upload(second.handle, foreign.handle, data.data(), 7 * 4 * sizeof(float)) == kOfxStatErrBadHandle);
        NatronVulkanImageHandle invalid = 1;
        check(gpu->createImage(first.handle, 0, 5, &invalid) == kOfxStatErrValue && invalid == 0);
        std::vector<std::future<void>> workers;

        for (int i = 0; i < 4; ++i) workers.emplace_back(std::async(std::launch::async, [ &, i] { testBuffers(i < 3 ? first.handle : second.handle, i); }));

        for (auto& worker : workers) {
            worker.get();
        }

        require(gpu->releaseImage(first.handle, foreign.handle));
        foreign.handle = 0;
        recordInfo(first.handle);
        recordInfo(second.handle);
        const auto expired = second.handle;
        require(gpu->releaseContext(expired));
        second.handle = 0;
        NatronVulkanDeviceInfo info{};
        check(gpu->getDeviceInfo(expired, &info) == kOfxStatErrBadHandle);
        check(gpu->releaseContext(expired) == kOfxStatErrBadHandle);
        check(shaderSuite->releaseProgram(expired, program) == kOfxStatErrBadHandle);
        ++selfTests;
        return 1;
    } catch (...) {
        return 0;
    }
}
