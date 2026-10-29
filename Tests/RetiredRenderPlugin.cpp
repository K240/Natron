// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX ABI fixture: declarations and CPU pixels, no graphics linkage.
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "ofxImageEffect.h"
#include "ofxOpenGLRender.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <initializer_list>

namespace
{
OfxHost *host;
const OfxPropertySuiteV1 *properties;
const OfxImageEffectSuiteV1 *effects;
const OfxParameterSuiteV1 *parameters;
const NatronOfxVulkanImageSuiteV1 *gpu;
const NatronOfxVulkanRenderSuiteV1 *nativeRender;
std::atomic<unsigned long long> counts[6][12] {};

void require(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}

void setHost(OfxHost* value)
{
    host = value;
    properties = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    gpu = static_cast<const NatronOfxVulkanImageSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 1));
    nativeRender = static_cast<const NatronOfxVulkanRenderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 1));
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

struct Image {
    OfxPropertySetHandle handle = nullptr;
    char *data = nullptr;
    int bounds[4] {}, rowBytes = 0;
    explicit Image(OfxImageClipHandle clip, double time)
    {
        require(effects->clipGetImage(clip, time, nullptr, &handle));
    }
    void read()
    {
        void *pointer = nullptr;
        require(properties->propGetPointer(handle, kOfxImagePropData, 0, &pointer));
        data = static_cast<char *>(pointer);
        require(properties->propGetIntN(handle, kOfxImagePropBounds, 4, bounds));
        require(properties->propGetInt(handle, kOfxImagePropRowBytes, 0, &rowBytes));
        const char *depth = nullptr;
        require(properties->propGetString(handle, kOfxImageEffectPropPixelDepth, 0, &depth));

        if (!data || std::strcmp(depth, kOfxBitDepthFloat) != 0) {
            throw kOfxStatErrFormat;
        }
    }
    ~Image()
    {
        if (handle) {
            effects->clipReleaseImage(handle);
        }
    }
    char *pixel(int x, int y) const
    {
        return data + std::ptrdiff_t(y - bounds[1]) * rowBytes + std::ptrdiff_t(x - bounds[0]) * 4 * sizeof(float);
    }
};

template<int Kind>
void checkDisabled(OfxPropertySetHandle args)
{
    int enabled = -1;
    require(properties->propGetInt(args, kOfxImageEffectPropOpenGLEnabled, 0, &enabled));
    counts[Kind][7] += enabled != 0;

    if (enabled != 0) {
        throw kOfxStatErrMissingHostFeature;
    }
}

template<int Kind>
OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle)
{
    try {
        if (std::strcmp(action, kOfxActionLoad) == 0) {
            if (!properties || !effects || !parameters) {
                return kOfxStatErrMissingHostFeature;
            }

            const char *support = nullptr;
            require(properties->propGetString(host->host, kOfxImageEffectPropOpenGLRenderSupported, 0, &support));

            if (std::strcmp(support, "false") != 0 || host->fetchSuite(host->host, kOfxOpenGLRenderSuite, 1)) {
                ++counts[Kind][8];
                return kOfxStatErrMissingHostFeature;
            }

            return kOfxStatOK;
        }

        auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            ++counts[Kind][0];
            OfxPropertySetHandle props = nullptr;
            require(effects->getPropertySet(effect, &props));
            const char *labels[] = {"Required OpenGL", "Optional OpenGL CPU", "Explicit CPU", "Context Required OpenGL", "Dual API Declaration", "Default CPU"};
            require(properties->propSetString(props, kOfxPropLabel, 0, labels[Kind]));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1));

            if (Kind != 5) {
                const char *support = Kind == 0 || Kind == 4 ? "needed" : Kind == 1 ? "true" : "false";
                require(properties->propSetString(props, kOfxImageEffectPropOpenGLRenderSupported, 0, support));
            }

            if (Kind == 4) {
                require(properties->propSetInt(props, kNatronOfxImageEffectPropVulkanRenderSupported, 0, 1));
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            ++counts[Kind][1];

            if (Kind == 3) {
                OfxPropertySetHandle props = nullptr;
                require(effects->getPropertySet(effect, &props));
                require(properties->propSetString(props, kOfxImageEffectPropOpenGLRenderSupported, 0, "needed"));
            }

            for (const char * name : {
                     "Source", "Output"
                 }) {
                OfxPropertySetHandle props = nullptr;
                require(effects->clipDefine(effect, name, &props));
                require(properties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));
            }

            if (Kind == 4) {
                OfxParamSetHandle params = nullptr;
                OfxPropertySetHandle props = nullptr;
                require(effects->getParamSet(effect, &params));
                require(parameters->paramDefine(params, kOfxParamTypeBoolean, "nativeAvailable", &props));
                require(properties->propSetInt(props, kOfxParamPropDefault, 0, 1));
            }

            return kOfxStatOK;
        }

        if (Kind == 4 && std::strcmp(action, kOfxActionInstanceChanged) == 0) {
            const char *name = nullptr;
            require(properties->propGetString(args, kOfxPropName, 0, &name));

            if (std::strcmp(name, "nativeAvailable") == 0) {
                OfxParamSetHandle params = nullptr;
                OfxParamHandle param = nullptr;
                OfxPropertySetHandle props = nullptr;
                int available = 0;
                require(effects->getParamSet(effect, &params));
                require(parameters->paramGetHandle(params, name, &param, nullptr));
                require(parameters->paramGetValue(param, &available));
                require(effects->getPropertySet(effect, &props));
                require(properties->propSetInt(props, kNatronOfxImageEffectPropVulkanRenderSupported, 0, available));
                return kOfxStatOK;
            }
        }

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            ++counts[Kind][2];
            return Kind == 0 || Kind == 3 ? kOfxStatErrMissingHostFeature : kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
            ++counts[Kind][3];
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0 ||
            std::strcmp(action, kOfxImageEffectActionEndSequenceRender) == 0) {
            ++counts[Kind][std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0 ? 5 : 6];
            checkDisabled<Kind>(args);
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionRender) == 0) {
            ++counts[Kind][4];
            checkDisabled<Kind>(args);
            int native = 0;
            require(properties->propGetInt(args, kNatronOfxImageEffectPropVulkanRenderingEnabled, 0, &native));
            counts[Kind][9] += native != 0;

            if (Kind == 0 || Kind == 3 || (Kind != 4 && native)) {
                return kOfxStatErrUnsupported;
            }

            double time = 0;
            int window[4] {};
            require(properties->propGetDouble(args, kOfxPropTime, 0, &time));
            require(properties->propGetIntN(args, kOfxImageEffectPropRenderWindow, 4, window));
            OfxImageClipHandle source = nullptr, output = nullptr;
            require(effects->clipGetHandle(effect, "Source", &source, nullptr));
            require(effects->clipGetHandle(effect, "Output", &output, nullptr));

            if (Kind == 4) {
                // This variant only implements native rendering. A CPU retry
                // must fail even when a previous GPU render succeeded.
                if (!native || !gpu || !nativeRender) {
                    return kOfxStatErrMissingHostFeature;
                }

                Context context;
                Image input(source, time), result(output, time);
                NatronVulkanImageView inputView {}, outputView {};
                require(nativeRender->acquireImage(context.handle, input.handle, &inputView));
                require(nativeRender->acquireImage(context.handle, result.handle, &outputView));

                if (inputView.writable || !outputView.writable) {
                    return kOfxStatErrValue;
                }

                NatronVulkanRegion region{uint32_t(window[0] - inputView.bounds[0]), uint32_t(window[1] - inputView.bounds[1]),
                                          uint32_t(window[2] - window[0]), uint32_t(window[3] - window[1])};
                require(gpu->copy(context.handle, inputView.image, outputView.image, &region,
                                  uint32_t(window[0] - outputView.bounds[0]), uint32_t(window[1] - outputView.bounds[1])));
                NatronVulkanDeviceInfo info {};
                require(gpu->getDeviceInfo(context.handle, &info));

                if (info.validationErrors) {
                    return kOfxStatFailed;
                }

                counts[Kind][10] = info.vendorId;
                counts[Kind][11] = info.deviceId;
                return kOfxStatOK;
            }

            Image input(source, time), result(output, time);
            input.read();
            result.read();
            const int x1 = std::max({window[0], input.bounds[0], result.bounds[0]});
            const int y1 = std::max({window[1], input.bounds[1], result.bounds[1]});
            const int x2 = std::min({window[2], input.bounds[2], result.bounds[2]});
            const int y2 = std::min({window[3], input.bounds[3], result.bounds[3]});

            if (x1 >= x2 || y1 >= y2) {
                return kOfxStatErrFormat;
            }

            for (int y = y1; y < y2; ++y) {
                std::memcpy(result.pixel(x1, y), input.pixel(x1, y), std::size_t(x2 - x1) * 4 * sizeof(float));
            }

            return kOfxStatOK;
        }

        return kOfxStatReplyDefault;
    } catch (OfxStatus status) {
        return status;
    } catch (...) {
        return kOfxStatFailed;
    }
}

OfxPlugin plugins[] = {
    {kOfxImageEffectPluginApi, 1, "org.natron.test.RequiredOpenGL", 1, 0, setHost, entry<0>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.OptionalOpenGL", 1, 0, setHost, entry<1>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.ExplicitCPU", 1, 0, setHost, entry<2>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.ContextOpenGL", 1, 0, setHost, entry<3>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.DualRenderAPI", 1, 0, setHost, entry<4>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.DefaultCPU", 1, 0, setHost, entry<5>}
};
}

#ifdef _WIN32
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" FIXTURE_EXPORT int OfxGetNumberOfPlugins()
{
    return 6;
}
extern "C" FIXTURE_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    return index >= 0 && index < 6 ? &plugins[index] : nullptr;
}
extern "C" FIXTURE_EXPORT unsigned long long OfxRetiredRenderCounter(int kind, int index)
{
    return kind >= 0 && kind < 6 && index >= 0 && index < 12 ? counts[kind][index].load() : 0;
}
