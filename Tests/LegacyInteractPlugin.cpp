// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX ABI fixture. No graphics API is linked or called.
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include <atomic>
#include <cstring>
#include <initializer_list>

namespace
{
const OfxPropertySuiteV1 *properties;
const OfxImageEffectSuiteV1 *effects;
const OfxParameterSuiteV1 *parameters;
std::atomic<unsigned long long> counts[3][8] {};

void require(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}

template<int Kind>
OfxStatus legacyInteract(const char* action, const void*, OfxPropertySetHandle, OfxPropertySetHandle)
{
    const char *actions[] = {kOfxActionDescribe, kOfxActionCreateInstance, kOfxInteractActionDraw,
                             kOfxInteractActionPenDown, kOfxActionDestroyInstance
                            };

    for (int i = 0; i < 5; ++i) {
        if (std::strcmp(action, actions[i]) == 0) {
            ++counts[Kind][i];
            return kOfxStatOK;
        }
    }

    ++counts[Kind][5];
    return kOfxStatReplyDefault;
}

void setHost(OfxHost* host)
{
    properties = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
}

template<int Kind>
OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle out)
{
    try {
        if (std::strcmp(action, kOfxActionLoad) == 0) {
            return properties && effects && parameters ? kOfxStatOK : kOfxStatErrMissingHostFeature;
        }

        auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            OfxPropertySetHandle props = nullptr;
            require(effects->getPropertySet(effect, &props));
            const char *labels[] = {"Legacy Viewer Overlay", "Legacy Custom UI", "No Overlay CPU"};
            require(properties->propSetString(props, kOfxPropLabel, 0, labels[Kind]));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1));

            if (Kind == 0) {
                require(properties->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0, reinterpret_cast<void*>(legacyInteract<0>)));
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
            for (const char * name : {
                     "Source", "Output"
                 }) {
                OfxPropertySetHandle props = nullptr;
                require(effects->clipDefine(effect, name, &props));
                require(properties->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA));
            }
            OfxParamSetHandle set = nullptr;
            OfxPropertySetHandle props = nullptr;
            require(effects->getParamSet(effect, &set));
            require(parameters->paramDefine(set, kOfxParamTypeDouble, "gain", &props));
            require(properties->propSetDouble(props, kOfxParamPropDefault, 0, 1));

            if (Kind == 1) {
                require(properties->propSetPointer(props, kOfxParamPropInteractV1, 0, reinterpret_cast<void*>(legacyInteract<1>)));
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            ++counts[Kind][6];
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
            ++counts[Kind][7];
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
            double time = 0;
            require(properties->propGetDouble(args, kOfxPropTime, 0, &time));
            require(properties->propSetString(out, kOfxPropName, 0, "Source"));
            require(properties->propSetDouble(out, kOfxPropTime, 0, time));
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
    {kOfxImageEffectPluginApi, 1, "org.natron.test.LegacyOverlay", 1, 0, setHost, entry<0>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.LegacyCustomUI", 1, 0, setHost, entry<1>},
    {kOfxImageEffectPluginApi, 1, "org.natron.test.NoOverlay", 1, 0, setHost, entry<2>}
};
}

#ifdef _WIN32
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" FIXTURE_EXPORT int OfxGetNumberOfPlugins()
{
    return 3;
}
extern "C" FIXTURE_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    return index >= 0 && index < 3 ? &plugins[index] : nullptr;
}
extern "C" FIXTURE_EXPORT unsigned long long OfxLegacyInteractCounter(int kind, int index)
{
    return kind >= 0 && kind < 3 && index >= 0 && index < 8 ? counts[kind][index].load() : 0;
}
