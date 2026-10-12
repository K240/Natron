// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX ABI fixture: keep native cache inputs resident under pressure.
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "Global/OpenFX/NatronVulkanTestSuite.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include "ofxNatron.h"
#include "ofxParam.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>
namespace
{
const OfxPropertySuiteV1 *props = nullptr;
const OfxImageEffectSuiteV1 *effects = nullptr;
const OfxParameterSuiteV1 *parameters = nullptr;
const NatronOfxVulkanImageSuiteV1 *images = nullptr;
const NatronOfxVulkanRenderSuiteV1 *nativeClips = nullptr;
const NatronOfxVulkanShaderSuiteV1 *shaders = nullptr;
const NatronOfxVulkanTestSuiteV1 *faults = nullptr;
std::atomic<uint64_t> counters[18] {};
std::atomic<uint64_t> actionLosses{0};
std::atomic<uint32_t> actionLossPoint{0};
std::mutex pressureMutex;
std::mutex barrierMutex;
std::condition_variable barrierCondition;
unsigned barrierParticipants = 0, barrierArrivals = 0, barrierActive = 0;
NatronVulkanContextHandle pressureContext = 0;
std::vector<NatronVulkanImageHandle> pressureImages;
enum Kind { Source, CpuSource, Filter, CpuFilter, Inspect, ConcurrentFilter, LossFilter };
struct Instance {
    OfxImageClipHandle input = nullptr, output = nullptr;
    OfxParamHandle requestToken = nullptr;
};
void check(OfxStatus value)
{
    if (value != kOfxStatOK) {
        throw value;
    }
}
struct Clip {
    OfxPropertySetHandle value = nullptr;
    void reset()
    {
        if (value) {
            effects->clipReleaseImage(value);
            value = nullptr;
        }
    }
    ~Clip()
    {
        reset();
    }
};
void setHost(OfxHost* host)
{
    props = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    images = static_cast<const NatronOfxVulkanImageSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 1));
    nativeClips = static_cast<const NatronOfxVulkanRenderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 1));
    shaders = static_cast<const NatronOfxVulkanShaderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanShaderSuite, 1));
    faults = static_cast<const NatronOfxVulkanTestSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanTestSuite, 1));
}
OfxStatus entry(Kind kind, const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    try {
        const bool source = kind == Source || kind == CpuSource;
        const bool cpuOnly = kind == CpuSource || kind == CpuFilter;
        auto effect = const_cast<OfxImageEffectHandle>(static_cast<const OfxImageEffectStruct*>(handle));

        if (std::strcmp(action, kOfxActionLoad) == 0 || std::strcmp(action, kOfxActionUnload) == 0) {
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
            const double bounds[4] {0, 0, 641, 513};
            check(props->propSetDoubleN(out, kOfxImageEffectPropRegionOfDefinition, 4, bounds));
            return kOfxStatOK;
        }

        if (source && std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
            const int bounds[4] {0, 0, 641, 513};
            check(props->propSetIntN(out, kOfxImageClipPropFormat, 4, bounds));
            return kOfxStatOK;
        }

        OfxPropertySetHandle p = nullptr;
        check(effects->getPropertySet(effect, &p));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            check(props->propSetString(p, kOfxPropLabel, 0, "Vulkan cached-input pressure fixture"));
            check(props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0,
                                       source ? kOfxImageEffectContextGenerator : kOfxImageEffectContextFilter));
            check(props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat));
            check(props->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe));
            check(props->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, kind == Inspect));
            check(props->propSetInt(p, kNatronOfxImageEffectPropVulkanRenderSupported, 0, !cpuOnly));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            check(effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p));
            check(props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));

            if (!source) {
                check(effects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p));
                check(props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));
            }

            if (kind == ConcurrentFilter) {
                OfxParamSetHandle parameterSet = nullptr;
                check(effects->getParamSet(effect, &parameterSet));
                check(parameters->paramDefine(parameterSet, kOfxParamTypeInteger, "requestToken", &p));
                check(props->propSetInt(p, kOfxParamPropDefault, 0, 0));
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            auto instance = new Instance;
            check(effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &instance->output, nullptr));

            if (!source) {
                check(effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &instance->input, nullptr));
            }

            if (kind == ConcurrentFilter) {
                OfxParamSetHandle parameterSet = nullptr;
                check(effects->getParamSet(effect, &parameterSet));
                check(parameters->paramGetHandle(parameterSet, "requestToken", &instance->requestToken, nullptr));
            }

            check(props->propSetPointer(p, kOfxPropInstanceData, 0, instance));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) != 0 && std::strcmp(action, kOfxImageEffectActionRender) != 0) {
            return kOfxStatReplyDefault;
        }

        void *data = nullptr;
        check(props->propGetPointer(p, kOfxPropInstanceData, 0, &data));
        auto instance = static_cast<Instance *>(data);

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
            delete instance;
            return kOfxStatOK;
        }

        double time = 0;
        check(props->propGetDouble(in, kOfxPropTime, 0, &time));
        Clip output, input;
        check(effects->clipGetImage(instance->output, time, nullptr, &output.value));
        void *destination = nullptr;
        check(props->propGetPointer(output.value, kOfxImagePropData, 0, &destination));
        unsigned counter = kind == CpuSource ? 4 : kind == CpuFilter ? 5 : kind == Inspect ? 6 : source ? 0 : 2;

        if (kind == Source || kind == Filter || kind == ConcurrentFilter || kind == LossFilter) {
            counter += destination ? 1 : 0;
        }

        ++counters[counter];

        if (!source) {
            check(effects->clipGetImage(instance->input, time, nullptr, &input.value));
        }

        int window[4] {};
        check(props->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, window));

        if (!destination) {
            if (!images || !nativeClips || !shaders) {
                return kOfxStatErrUnsupported;
            }

            NatronVulkanContextHandle context = 0;
            check(images->createContext(-1, 0, &context));
            struct Release {
                NatronVulkanContextHandle context;
                Clip &input;
                Clip &output;
                ~Release()
                {
                    input.reset();
                    output.reset();
                    images->releaseContext(context);
                }
            } release{context, input, output};
            NatronVulkanImageView target{};
            check(nativeClips->acquireImage(context, output.value, &target));
            const NatronVulkanRegion region{uint32_t(window[0] - target.bounds[0]), uint32_t(window[1] - target.bounds[1]),
                                            uint32_t(window[2] - window[0]), uint32_t(window[3] - window[1])};

            if (source) {
                NatronVulkanProgramHandle program = 0;
                check(shaders->createProgram(context, "#version 450\nlayout(location=0) out vec4 color;\nvoid main(){color=vec4(0,1,0,1);}", 16, &program, nullptr, 0));
                const NatronVulkanShaderInput bindings[4] {};
                const float uniforms[4] {};
                check(shaders->render(context, program, target.image, &region, bindings, uniforms, 16));
            } else {
                void *sourcePixels = nullptr;
                check(props->propGetPointer(input.value, kOfxImagePropData, 0, &sourcePixels));

                if (sourcePixels) {
                    return kOfxStatFailed;
                }

                ++counters[10];
                NatronVulkanImageView origin{};
                check(nativeClips->acquireImage(context, input.value, &origin));

                const auto lossPoint = kind == LossFilter ? actionLossPoint.exchange(0) : 0;

                if (lossPoint) {
                    if (!faults) {
                        return kOfxStatErrUnsupported;
                    }

                    check(faults->armDeviceLoss(context, lossPoint));
                    ++actionLosses;
                }

                const NatronVulkanRegion copyRegion{uint32_t(window[0] - origin.bounds[0]), uint32_t(window[1] - origin.bounds[1]), region.width, region.height};
                check(images->copy(context, origin.image, target.image, &copyRegion, region.x, region.y));

                if (kind == ConcurrentFilter) {
                    const float gain[4] {1, 1, 1, 1}, offset[4] {0, 0, 1, 0};
                    check(images->transform(context, target.image, target.image, &region, gain, offset));
                }
            }

            return kOfxStatOK;
        }

        int bounds[4] {}, stride = 0, sourceBounds[4] {}, sourceStride = 0;
        void *sourcePixels = nullptr;
        check(props->propGetIntN(output.value, kOfxImagePropBounds, 4, bounds));
        check(props->propGetInt(output.value, kOfxImagePropRowBytes, 0, &stride));

        if (!source) {
            check(props->propGetPointer(input.value, kOfxImagePropData, 0, &sourcePixels));

            if (!sourcePixels) {
                return kOfxStatFailed;
            }

            ++counters[11];
            check(props->propGetIntN(input.value, kOfxImagePropBounds, 4, sourceBounds));
            check(props->propGetInt(input.value, kOfxImagePropRowBytes, 0, &sourceStride));
        }

        if (kind == ConcurrentFilter) {
            int token = 0;
            check(parameters->paramGetValue(instance->requestToken, &token));

            if (token < 0 || token > 1) {
                return kOfxStatErrValue;
            }

            ++counters[16 + token];
            std::unique_lock<std::mutex> lock(barrierMutex);

            if (barrierParticipants) {
                ++barrierArrivals;
                ++barrierActive;
                counters[13] = barrierArrivals;
                counters[14] = std::max<uint64_t>(counters[14].load(), barrierActive);
                barrierCondition.notify_all();
                const bool ready = barrierCondition.wait_for(lock, std::chrono::seconds(10), [] {
                    return barrierArrivals >= barrierParticipants;
                });
                --barrierActive;

                if (!ready) {
                    ++counters[15];
                    return kOfxStatFailed;
                }
            }
        }

        const float green[4] {0, 1, 0, 1};

        for (int y = window[1]; y < window[3]; ++y) {
            auto row = reinterpret_cast<float *>(static_cast<char*>(destination) + (y - bounds[1]) * stride);

            for (int x = window[0]; x < window[2]; ++x) {
                const void *value = green;

                if (!source) {
                    if (x < sourceBounds[0] || x >= sourceBounds[2] || y < sourceBounds[1] || y >= sourceBounds[3]) {
                        return kOfxStatErrValue;
                    }

                    value = reinterpret_cast<const float *>(static_cast<const char*>(sourcePixels) + (y - sourceBounds[1]) * sourceStride) + (x - sourceBounds[0]) * 4;
                }

                std::memcpy(row + (x - bounds[0]) * 4, value, sizeof(green));

                if (kind == ConcurrentFilter) {
                    row[(x - bounds[0]) * 4 + 2] += 1;
                }
            }
        }

        return kOfxStatOK;
    } catch (OfxStatus status) {
        return status;
    } catch (const std::bad_alloc &) {
        return kOfxStatErrMemory;
    } catch (...) {
        return kOfxStatFailed;
    }
}
#define ENTRY(name, kind) OfxStatus name(const char* a, const void* h, OfxPropertySetHandle in, OfxPropertySetHandle out) { return entry(kind, a, h, in, out); }
ENTRY(source, Source)
ENTRY(cpuSource, CpuSource)
ENTRY(filter, Filter)
ENTRY(cpuFilter, CpuFilter)
ENTRY(inspect, Inspect)
ENTRY(concurrentFilter, ConcurrentFilter)
ENTRY(lossFilter, LossFilter)
OfxPlugin plugins[] {
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureSource", 1, 0, setHost, source},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureCpuSource", 1, 0, setHost, cpuSource},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureFilter", 1, 0, setHost, filter},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureCpuFilter", 1, 0, setHost, cpuFilter},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureInspect", 1, 0, setHost, inspect},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureConcurrentFilter", 1, 0, setHost, concurrentFilter},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanPressureLossFilter", 1, 0, setHost, lossFilter}
};
}
#ifdef _WIN32
#define TEST_EXPORT __declspec(dllexport)
#else
#define TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" TEST_EXPORT int OfxGetNumberOfPlugins()
{
    return 7;
}
extern "C" TEST_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    return index >= 0 && index < 7 ? &plugins[index] : nullptr;
}
extern "C" TEST_EXPORT uint64_t OfxPressureCounter(int index)
{
    return index >= 0 && index < 18 ? counters[index].load() : 0;
}
// Raw C ABI suites are exported only by this independent test module. In a
// normal user profile the host refuses the fault-injection suite entirely.
extern "C" TEST_EXPORT const NatronOfxVulkanImageSuiteV1 *OfxPressureImageSuite()
{
    return images;
}
extern "C" TEST_EXPORT const NatronOfxVulkanShaderSuiteV1 *OfxPressureShaderSuite()
{
    return shaders;
}
extern "C" TEST_EXPORT const NatronOfxVulkanTestSuiteV1 *OfxPressureTestSuite()
{
    return faults;
}
extern "C" TEST_EXPORT uint64_t OfxPressureActionLossCount()
{
    return actionLosses.load();
}
extern "C" TEST_EXPORT OfxStatus OfxPressureArmActionLoss(uint32_t point)
{
    if (point != 1 && point != 2) {
        return kOfxStatErrValue;
    }

    uint32_t expected = 0;
    return actionLossPoint.compare_exchange_strong(expected, point) ? kOfxStatOK : kOfxStatFailed;
}
extern "C" TEST_EXPORT OfxStatus OfxPressureSetCpuBarrier(unsigned participants)
{
    std::lock_guard<std::mutex> lock(barrierMutex);

    if (barrierActive || participants > 2) {
        return kOfxStatErrValue;
    }

    barrierParticipants = participants;
    barrierArrivals = 0;
    counters[12] = participants;
    counters[13] = counters[14] = counters[15] = 0;
    return kOfxStatOK;
}
extern "C" TEST_EXPORT OfxStatus OfxPressureGetTransfers(uint64_t* uploads, uint64_t* downloads)
{
    if (!images || !nativeClips) {
        return kOfxStatErrMissingHostFeature;
    }

    NatronVulkanContextHandle context = 0;
    auto status = images->createContext(-1, 0, &context);

    if (status != kOfxStatOK) {
        return status;
    }

    status = nativeClips->getTransferCounts(context, uploads, downloads);
    images->releaseContext(context);
    return status;
}
extern "C" TEST_EXPORT OfxStatus OfxPressureGetDeviceInfo(NatronVulkanDeviceInfo* info)
{
    if (!images) {
        return kOfxStatErrMissingHostFeature;
    }

    NatronVulkanContextHandle context = 0;
    auto status = images->createContext(-1, 0, &context);

    if (status != kOfxStatOK) {
        return status;
    }

    status = images->getDeviceInfo(context, info);
    images->releaseContext(context);
    return status;
}
extern "C" TEST_EXPORT OfxStatus OfxPressureFillHeap()
{
    std::lock_guard<std::mutex> lock(pressureMutex);

    if (!images) {
        return kOfxStatErrBadHandle;
    }

    // Reserve before retaining any native resources. Never let an allocation
    // exception cross the exported C boundary or lose a retained image handle.
    try {
        pressureImages.reserve(64);
    } catch (const std::bad_alloc &) {
        return kOfxStatErrMemory;
    }

    auto status = pressureContext ? kOfxStatOK : images->createContext(-1, 0, &pressureContext);

    if (status != kOfxStatOK) {
        return status;
    }

    while (pressureImages.size() < 64) {
        NatronVulkanImageHandle image = 0;
        status = images->createImage(pressureContext, 512, 512, &image);
        counters[7] = status;

        if (status != kOfxStatOK) {
            if (status == kOfxStatErrMemory) {
                ++counters[9];
            }

            return status;
        }

        pressureImages.push_back(image);
        counters[8] = pressureImages.size();
    }

    return kOfxStatFailed;
}
extern "C" TEST_EXPORT OfxStatus OfxPressureReleaseHeap()
{
    std::lock_guard<std::mutex> lock(pressureMutex);

    if (!pressureContext) {
        return kOfxStatErrBadHandle;
    }

    auto status = images->releaseContext(pressureContext);
    pressureContext = 0;
    pressureImages.clear();
    return status;
}
