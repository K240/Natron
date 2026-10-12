// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX ABI fixture. Full-size generators and a dispersed pixel
// probe bracket a shipped OCIO effect in the application performance test.
#include "Global/OpenFX/NatronVulkanRenderSuite.h"
#include "Global/OpenFX/NatronVulkanShaderSuite.h"
#include "Global/OpenFX/NatronVulkanTestSuite.h"
#include "ofxImageEffect.h"
#include "ofxProperty.h"
#include "ofxParam.h"
#include "ofxNatron.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>
namespace
{
const OfxPropertySuiteV1 *props = nullptr;
const OfxImageEffectSuiteV1 *effects = nullptr;
const OfxParameterSuiteV1 *parameters = nullptr;
const NatronOfxVulkanImageSuiteV1 *images = nullptr;
const NatronOfxVulkanRenderSuiteV1 *clips = nullptr;
const NatronOfxVulkanShaderSuiteV1 *shaders = nullptr;
const NatronOfxVulkanTestSuiteV2 *telemetry = nullptr;
NatronVulkanContextHandle context = 0;
NatronVulkanProgramHandle sourceProgram = 0, probeProgram = 0;
std::mutex nativeMutex;
// source native/CPU, probe native/CPU, full-reference pixels, max error in nanounits,
// actual source width/height and probe input width/height.
std::atomic<uint64_t> counters[10] {};
enum Kind { NativeSource, CpuSource, NativeProbe, CpuProbe };
struct Instance {
    OfxImageClipHandle input = nullptr, output = nullptr;
    OfxParamHandle width = nullptr, height = nullptr, verify = nullptr;
};
void check(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}
struct Clip {
    OfxPropertySetHandle value = nullptr;
    ~Clip()
    {
        if (value) {
            effects->clipReleaseImage(value);
        }
    }
};
void setHost(OfxHost* host)
{
    props = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    images = static_cast<const NatronOfxVulkanImageSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanImageSuite, 1));
    clips = static_cast<const NatronOfxVulkanRenderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanRenderSuite, 1));
    shaders = static_cast<const NatronOfxVulkanShaderSuiteV1 *>(host->fetchSuite(host->host, kNatronOfxVulkanShaderSuite, 1));
    telemetry = static_cast<const NatronOfxVulkanTestSuiteV2 *>(host->fetchSuite(host->host, kNatronOfxVulkanTestSuite, 2));
}
void expected(int x, int y, int frame, float* result)
{
    result[0] = float((x + frame) & 63) / 128.f;
    result[1] = float((y + 3 * frame) & 63) / 128.f;
    result[2] = float((x + y + 7 * frame) & 63) / 128.f;
    result[3] = 1.f;
}
void compile(NatronVulkanProgramHandle& program, const char* source)
{
    if (!program) {
        check(shaders->createProgram(context, source, 16, &program, nullptr, 0));
    }
}
OfxStatus entry(Kind kind, const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
{
    try {
        const bool source = kind == NativeSource || kind == CpuSource;
        const bool native = kind == NativeSource || kind == NativeProbe;
        auto effect = const_cast<OfxImageEffectHandle>(static_cast<const OfxImageEffectStruct*>(handle));

        if (std::strcmp(action, kOfxActionLoad) == 0) {
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionUnload) == 0) {
            std::lock_guard<std::mutex> lock(nativeMutex);

            if (context) {
                images->releaseContext(context);
                context = sourceProgram = probeProgram = 0;
            }

            return kOfxStatOK;
        }

        OfxPropertySetHandle p = nullptr;
        check(effects->getPropertySet(effect, &p));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            check(props->propSetString(p, kOfxPropLabel, 0, "Vulkan application benchmark fixture"));
            check(props->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, source ? kOfxImageEffectContextGenerator : kOfxImageEffectContextFilter));
            check(props->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat));
            check(props->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe));
            check(props->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, 0));
            check(props->propSetString(p, kOfxImageEffectPropClipPreferencesSlaveParam, 0, "width"));
            check(props->propSetString(p, kOfxImageEffectPropClipPreferencesSlaveParam, 1, "height"));

            if (native || images) {
                check(props->propSetInt(p, kNatronOfxImageEffectPropVulkanRenderSupported, 0, native));
                check(props->propSetInt(p, kNatronOfxImageEffectPropVulkanRenderRequired, 0, native));
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            check(effects->clipDefine(effect, kOfxImageEffectOutputClipName, &p));
            check(props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));

            if (!source) {
                check(effects->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &p));
                check(props->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));
            }

            OfxParamSetHandle params = nullptr;
            check(effects->getParamSet(effect, &params));

            for (const auto * name : {
                     "width", "height", "verifyFull"
                 }) {
                check(parameters->paramDefine(params, kOfxParamTypeInteger, name, &p));
                check(props->propSetInt(p, kOfxParamPropDefault, 0, std::strcmp(name, "width") == 0 ? 3840 : std::strcmp(name, "height") == 0 ? 2160 : 0));
            }
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            auto instance = new Instance;
            check(effects->clipGetHandle(effect, kOfxImageEffectOutputClipName, &instance->output, nullptr));

            if (!source) {
                check(effects->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &instance->input, nullptr));
            }

            OfxParamSetHandle params = nullptr;
            check(effects->getParamSet(effect, &params));
            check(parameters->paramGetHandle(params, "width", &instance->width, nullptr));
            check(parameters->paramGetHandle(params, "height", &instance->height, nullptr));
            check(parameters->paramGetHandle(params, "verifyFull", &instance->verify, nullptr));
            check(props->propSetPointer(p, kOfxPropInstanceData, 0, instance));
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0 ||
            std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0 ||
            std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0 ||
            std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0 ||
            std::strcmp(action, kOfxImageEffectActionRender) == 0) {
            void *data = nullptr;
            check(props->propGetPointer(p, kOfxPropInstanceData, 0, &data));
            auto instance = static_cast<Instance *>(data);

            if (!instance) {
                return kOfxStatReplyDefault;
            }

            if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
                delete instance;
                return kOfxStatOK;
            }

            int width = 0, height = 0, verify = 0;
            check(parameters->paramGetValue(instance->width, &width));
            check(parameters->paramGetValue(instance->height, &height));
            check(parameters->paramGetValue(instance->verify, &verify));

            if (width < 64 || height < 48 || width > 16384 || height > 16384) {
                return kOfxStatErrValue;
            }

            if (std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0) {
                check(props->propSetInt(out, kOfxImageEffectFrameVarying, 0, 1));
                const int bounds[4] {0, 0, source ? width : 64, source ? height : 48};
                check(props->propSetIntN(out, kOfxImageClipPropFormat, 4, bounds));
                return kOfxStatOK;
            }

            if (std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
                double scale[2] {};
                check(props->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, scale));

                // This fixture deliberately generates the complete original
                // image. Natron probes RoD at half scale to detect effects
                // requiring full-resolution renders and host downsampling.
                if (scale[0] != 1. || scale[1] != 1.) {
                    return kOfxStatFailed;
                }

                const double bounds[4] {0, 0, double(source ? width : 64), double(source ? height : 48)};
                check(props->propSetDoubleN(out, kOfxImageEffectPropRegionOfDefinition, 4, bounds));
                return kOfxStatOK;
            }

            if (std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0) {
                if (source) {
                    return kOfxStatReplyDefault;
                }

                const double bounds[4] {0, 0, double(width), double(height)};
                check(props->propSetDoubleN(out, "OfxImageClipPropRoI_Source", 4, bounds));
                return kOfxStatOK;
            }

            double time = 0;
            check(props->propGetDouble(in, kOfxPropTime, 0, &time));
            const int frame = int(time);
            Clip output, input;
            check(effects->clipGetImage(instance->output, time, nullptr, &output.value));

            if (!source) {
                check(effects->clipGetImage(instance->input, time, nullptr, &input.value));
            }

            int targetBounds[4] {}, originBounds[4] {}, rowBytes = 0;
            check(props->propGetIntN(output.value, kOfxImagePropBounds, 4, targetBounds));
            const int targetWidth = targetBounds[2] - targetBounds[0], targetHeight = targetBounds[3] - targetBounds[1];

            if (source) {
                counters[6] = targetWidth;
                counters[7] = targetHeight;
            } else {
                check(props->propGetIntN(input.value, kOfxImagePropBounds, 4, originBounds));
                counters[8] = originBounds[2] - originBounds[0];
                counters[9] = originBounds[3] - originBounds[1];

                if (originBounds[0] != 0 || originBounds[1] != 0 || originBounds[2] != width || originBounds[3] != height) {
                    return kOfxStatErrFormat;
                }
            }

            if (source && (targetBounds[0] != 0 || targetBounds[1] != 0 || targetWidth != width || targetHeight != height)) {
                return kOfxStatErrFormat;
            }

            void *destination = nullptr;
            check(props->propGetPointer(output.value, kOfxImagePropData, 0, &destination));

            if (native) {
                if (destination || !images || !clips || !shaders) {
                    return kOfxStatErrUnsupported;
                }

                std::lock_guard<std::mutex> lock(nativeMutex);

                if (!context) {
                    check(images->createContext(-1, 0, &context));
                }

                NatronVulkanImageView target{}, origin{};
                check(clips->acquireImage(context, output.value, &target));
                const NatronVulkanShaderInput empty[4] {};
                int uniforms[4] {width, height, frame, 0};

                if (source) {
                    compile(sourceProgram, "#version 450\nlayout(location=0) out vec4 c;layout(set=0,binding=4,std140) uniform U{ivec4 u;};void main(){ivec2 p=ivec2(gl_FragCoord.xy);c=vec4(float((p.x+u.z)&63),float((p.y+3*u.z)&63),float((p.x+p.y+7*u.z)&63),128.)/128.;}");
                    check(shaders->render(context, sourceProgram, target.image, nullptr, empty, uniforms, 16));
                } else {
                    check(clips->acquireImage(context, input.value, &origin));

                    if (verify) {
                        std::vector<float> pixels(size_t(width) * height * 4);
                        check(images->download(context, origin.image, pixels.data(), int64_t(width) * 16));
                        float maxError = 0;

                        for (int y = 0; y < height; ++y) {
                            for (int x = 0; x < width; ++x) {
                                float reference[4];
                                expected(x, y, frame, reference);
                                const float gain[4] {1.2f, .8f, 1.1f, 1.f};

                                for (unsigned c = 0; c < 4; ++c) {
                                    const float actual = pixels[(size_t(y) * width + x) * 4 + c];

                                    if (!std::isfinite(actual)) {
                                        return kOfxStatFailed;
                                    }

                                    maxError = std::max(maxError, std::abs(actual - reference[c] * gain[c]));
                                }
                            }
                        }

                        counters[4] += uint64_t(width) * height;
                        counters[5] = uint64_t(std::ceil(maxError * 1e9));

                        if (maxError > 2e-6f) {
                            return kOfxStatFailed;
                        }
                    }

                    compile(probeProgram, "#version 450\nlayout(location=0) out vec4 c;layout(set=0,binding=0) uniform sampler2D s;layout(set=0,binding=4,std140) uniform U{ivec4 u;};void main(){ivec2 p=ivec2(gl_FragCoord.xy);ivec2 q=ivec2(p.x*(u.x-1)/63,p.y*(u.y-1)/47);c=texelFetch(s,q,0);}");
                    NatronVulkanShaderInput bindings[4] {{origin.image, kNatronVulkanWrapClamp, 0}, {}, {}, {}};
                    check(shaders->render(context, probeProgram, target.image, nullptr, bindings, uniforms, 16));
                }
            } else {
                if (!destination) {
                    return kOfxStatErrFormat;
                }

                check(props->propGetInt(output.value, kOfxImagePropRowBytes, 0, &rowBytes));
                void *sourcePixels = nullptr;
                int inputRowBytes = 0;

                if (!source) {
                    check(props->propGetPointer(input.value, kOfxImagePropData, 0, &sourcePixels));
                    check(props->propGetInt(input.value, kOfxImagePropRowBytes, 0, &inputRowBytes));

                    if (!sourcePixels) {
                        return kOfxStatErrFormat;
                    }
                }

                for (int y = 0; y < targetHeight; ++y) {
                    auto* row = reinterpret_cast<float *>(static_cast<char*>(destination) + int64_t(y) * rowBytes);

                    for (int x = 0; x < targetWidth; ++x) {
                        if (source) {
                            expected(x, y, frame, row + x * 4);
                        } else {
                            const int sx = (x + targetBounds[0]) * (width - 1) / 63, sy = (y + targetBounds[1]) * (height - 1) / 47;
                            const auto* origin = reinterpret_cast<const float *>(static_cast<const char*>(sourcePixels) + int64_t(sy) * inputRowBytes) + sx * 4;
                            std::copy(origin, origin + 4, row + x * 4);
                        }
                    }
                }
            }

            ++counters[kind];
            return kOfxStatOK;
        }

        return kOfxStatReplyDefault;
    } catch (OfxStatus status) {
        return status;
    } catch (...) {
        return kOfxStatFailed;
    }
}
#define ENTRY(name, kind) OfxStatus name(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) { return entry(kind, action, handle, in, out); }
ENTRY(nativeSource, NativeSource)
ENTRY(cpuSource, CpuSource)
ENTRY(nativeProbe, NativeProbe)
ENTRY(cpuProbe, CpuProbe)
OfxPlugin plugins[] {
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanBenchmarkNativeSource", 1, 0, setHost, nativeSource},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanBenchmarkCpuSource", 1, 0, setHost, cpuSource},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanBenchmarkNativeProbe", 1, 0, setHost, nativeProbe},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.VulkanBenchmarkCpuProbe", 1, 0, setHost, cpuProbe}
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
extern "C" TEST_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    return index >= 0 && index < 4 ? plugins + index : nullptr;
}
extern "C" TEST_EXPORT const NatronOfxVulkanTestSuiteV2 *OfxBenchmarkTelemetrySuite()
{
    return telemetry;
}
extern "C" TEST_EXPORT uint64_t OfxBenchmarkCounter(int index)
{
    return index >= 0 && index < 10 ? counters[index].load() : 0;
}
extern "C" TEST_EXPORT OfxStatus OfxBenchmarkGetDeviceInfo(NatronVulkanDeviceInfo* info)
{
    if (!images || !info) {
        return kOfxStatErrMissingHostFeature;
    }

    NatronVulkanContextHandle handle = 0;
    const auto created = images->createContext(-1, 0, &handle);

    if (created != kOfxStatOK) {
        return created;
    }

    const auto result = images->getDeviceInfo(handle, info);
    images->releaseContext(handle);
    return result;
}
