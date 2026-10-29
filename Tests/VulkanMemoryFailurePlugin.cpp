// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX C ABI fixture. Allocation failures come from the real host suite.
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include "ofxNatron.h"
#include <atomic>
#include <cstring>
#include <new>
namespace
{
const OfxPropertySuiteV1 *props = nullptr;
const OfxImageEffectSuiteV1 *effects = nullptr;
const NatronOfxVulkanImageSuiteV1 *images = nullptr;
const NatronOfxVulkanRenderSuiteV1 *nativeClips = nullptr;
const NatronOfxVulkanShaderSuiteV1 *shaders = nullptr;
std::atomic<unsigned long long> counters[11] {};
struct Instance {
    OfxImageClipHandle output = nullptr;
};
void check(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}
void setHost(OfxHost *host)
{
    props = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    images = static_cast<const NatronOfxVulkanImageSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 1));
    nativeClips = static_cast<const NatronOfxVulkanRenderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 1));
    shaders = static_cast<const NatronOfxVulkanShaderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanShaderSuite, 1));
}
OfxStatus entry(const char *action, const void *handle, OfxPropertySetHandle in, bool required, bool cpuReference, bool nativeSuccess = false, OfxPropertySetHandle out = nullptr)
{
    try {
        auto effect = const_cast<OfxImageEffectHandle>(static_cast<const OfxImageEffectStruct *>(handle));

        if (std::strcmp(action, kOfxActionLoad) == 0 || std::strcmp(action, kOfxActionUnload) == 0) {
            return kOfxStatOK;
        }

        if (nativeSuccess && std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
            const double bounds[4] {0, 0, 1280, 1501};
            check(props->propSetDoubleN(out, kOfxImageEffectPropRegionOfDefinition, 4, bounds));
            return kOfxStatOK;
        }

        if (nativeSuccess && std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
            const int bounds[4] {0, 0, 1280, 1501};
            check(props->propSetIntN(out, kOfxImageClipPropFormat, 4, bounds));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            OfxPropertySetHandle p = nullptr;
            check(effects->getPropertySet(effect, &p));
            check(props->propSetString(p, kOfxPropLabel, 0, "Vulkan memory failure fixture"));
            check(props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGenerator));
            check(props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat));
            check(props->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe));
            check(props->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, 0));
            check(props->propSetInt(p, kNatronOfxImageEffectPropVulkanRenderSupported, 0, cpuReference ? 0 : 1));
            check(props->propSetInt(p, kNatronOfxImageEffectPropVulkanRenderRequired, 0, required ? 1 : 0));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            OfxPropertySetHandle p = nullptr;
            check(effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p));
            check(props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));
            return kOfxStatOK;
        }

        OfxPropertySetHandle p = nullptr;
        check(effects->getPropertySet(effect, &p));

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            auto *instance = new Instance;
            check(effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &instance->output, nullptr));
            check(props->propSetPointer(p, kOfxPropInstanceData, 0, instance));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0 || std::strcmp(action, kOfxImageEffectActionRender) == 0) {
            void *data = nullptr;
            check(props->propGetPointer(p, kOfxPropInstanceData, 0, &data));
            auto *instance = static_cast<Instance *>(data);

            if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
                delete instance;
                return kOfxStatOK;
            }

            double time = 0;
            check(props->propGetDouble(in, kOfxPropTime, 0, &time));
            OfxPropertySetHandle output = nullptr;
            check(effects->clipGetImage(instance->output, time, nullptr, &output));
            struct Release {
                OfxPropertySetHandle image;
                ~Release()
                {
                    if (image) {
                        effects->clipReleaseImage(image);
                    }
                }
            } release{output};
            void *pixels = nullptr;
            check(props->propGetPointer(output, kOfxImagePropData, 0, &pixels));
            const unsigned base = nativeSuccess ? 8 : cpuReference ? 4 : required ? 2 : 0;

            if (!pixels) {
                ++counters[base];

                if (!images) {
                    return kOfxStatErrUnsupported;
                }

                NatronVulkanContextHandle context = 0;
                check(images->createContext(-1, 0, &context));

                if (nativeSuccess) {
                    struct ContextRelease {
                        NatronVulkanContextHandle context;
                        OfxPropertySetHandle &clip;
                        ~ContextRelease()
                        {
                            if (clip) {
                                effects->clipReleaseImage(clip);
                                clip = nullptr;
                            }

                            images->releaseContext(context);
                        }
                    } cleanup{context, release.image};

                    if (!nativeClips || !shaders) {
                        return kOfxStatErrUnsupported;
                    }

                    NatronVulkanImageView view{};
                    check(nativeClips->acquireImage(context, output, &view));
                    NatronVulkanProgramHandle program = 0;
                    const char *source = "#version 450\nlayout(location=0) out vec4 color;\nvoid main(){color=vec4(0,1,0,1);}";
                    const auto created = shaders->createProgram(context, source, 16, &program, nullptr, 0);
                    counters[7] = created;
                    check(created);
                    NatronVulkanShaderInput inputs[4] {};
                    const float uniform[4] {};
                    NatronVulkanRegion region{uint32_t(view.visibleBounds[0] - view.bounds[0]), uint32_t(view.visibleBounds[1] - view.bounds[1]),
                                              uint32_t(view.visibleBounds[2] - view.visibleBounds[0]), uint32_t(view.visibleBounds[3] - view.visibleBounds[1])};
                    const auto rendered = shaders->render(context, program, view.image, &region, inputs, uniform, 16);
                    counters[7] = rendered;
                    check(rendered);
                    ++counters[10];
                    return kOfxStatOK;
                }

                NatronVulkanImageHandle image = 0;
                const OfxStatus status = images->createImage(context, 4096, 4096, &image);
                counters[6] = status;

                if (image) {
                    images->releaseImage(context, image);
                }

                images->releaseContext(context);
                return status == kOfxStatOK ? kOfxStatFailed : status;
            }

            ++counters[base + 1];

            if (required) {
                return kOfxStatErrUnsupported;
            }

            int bounds[4] {}, window[4] {}, stride = 0;
            check(props->propGetIntN(output, kOfxImagePropBounds, 4, bounds));
            check(props->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window));
            check(props->propGetInt(output, kOfxImagePropRowBytes, 0, &stride));

            for (int y = window[1]; y < window[3]; ++y) {
                auto *row = reinterpret_cast<float *>(static_cast<char *>(pixels) + (y - bounds[1]) * stride);

                for (int x = window[0]; x < window[2]; ++x) {
                    const float value[4] {.125f, .25f, .5f, 1.f};
                    std::memcpy(row + (x - bounds[0]) * 4, value, sizeof(value));
                }
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
OfxStatus fallback(const char *a, const void *h, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    return entry(a, h, in, false, false);
}
OfxStatus required(const char *a, const void *h, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    return entry(a, h, in, true, false);
}
OfxStatus reference(const char *a, const void *h, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    return entry(a, h, in, false, true);
}
OfxStatus success(const char *a, const void *h, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    return entry(a, h, in, true, false, true, out);
}
OfxPlugin plugins[] {
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanMemoryFallback", 1, 0, setHost, fallback},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanMemoryRequired", 1, 0, setHost, required},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanMemoryReference", 1, 0, setHost, reference},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanMemorySuccess", 1, 0, setHost, success}
};
}
#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" TEST_EXPORT int OfxGetNumberOfPlugins()
{
    return 4;
}
extern "C" TEST_EXPORT OfxPlugin *OfxGetPlugin(int i)
{
    return i >= 0 && i < 4 ? &plugins[i] : nullptr;
}
extern "C" TEST_EXPORT unsigned long long OfxMemoryCounter(int i)
{
    return i >= 0 && i < 11 ? counters[i].load() : 0;
}
