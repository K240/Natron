// SPDX-License-Identifier: GPL-2.0-or-later
// Independent OFX C ABI fixture for native parametric curve backgrounds.
#include "Global/OpenFX/ParamInteractDrawSuite.h"
#include "Global/OpenFX/NatronDrawSuite.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxParam.h"
#include "ofxParametricParam.h"
#include "ofxProperty.h"
#include <atomic>
#include <cstring>
#include <memory>

namespace
{
OfxHost *host;
const OfxPropertySuiteV1 *properties;
const OfxImageEffectSuiteV1 *effects;
const OfxParameterSuiteV1 *parameters;
const OfxParametricParameterSuiteV1 *curves;
const OfxInteractSuiteV1 *interacts;
const OfxDrawSuiteV1 *drawing;
const NatronOfxDrawSuiteV1 *nativeDrawing;
std::atomic<unsigned long long> counters[5] {};
void require(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}
struct State {
    OfxDrawContextHandle previous = nullptr;
};

OfxStatus legacy(const char*, const void*, OfxPropertySetHandle, OfxPropertySetHandle)
{
    ++counters[0];
    return kOfxStatFailed;
}

OfxStatus background(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle)
{
    try {
        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            return kOfxStatOK;
        }

        OfxPropertySetHandle props = nullptr;
        require(interacts->interactGetPropertySet(static_cast<OfxInteractHandle>(const_cast<void*>(handle)), &props));

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            auto state = std::make_unique<State>();
            require(properties->propSetPointer(props, kOfxPropInstanceData, 0, state.get()));
            state.release();
            return kOfxStatOK;
        }

        State* state = nullptr;
        require(properties->propGetPointer(props, kOfxPropInstanceData, 0, reinterpret_cast<void**>(&state)));

        if (!state) {
            return kOfxStatFailed;
        }

        if (std::strcmp(action, kOfxActionDestroyInstance) == 0) {
            delete state;
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxInteractActionDraw) != 0) {
            ++counters[4];
            return kOfxStatReplyDefault;
        }

        if (state->previous) {
            if (nativeDrawing->setAntialiasing(state->previous, 0) == kOfxStatFailed) {
                ++counters[2];
            } else {
                ++counters[3];
            }
        }

        OfxDrawContextHandle context = nullptr;
        require(properties->propGetPointer(args, kOfxInteractPropDrawContext, 0, reinterpret_cast<void**>(&context)));
        state->previous = context;
        require(nativeDrawing->setAntialiasing(context, 0));
        const OfxPointD ramp[] {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
        const OfxRGBAColourF gray[] {{0, 0, 0, 1}, {1, 1, 1, 1}};
        require(nativeDrawing->drawGradientQuad(context, ramp, gray));
        const OfxPointD skew[] {{.65, .06}, {.68, .21}, {.95, .21}, {.92, .06}};
        const OfxRGBAColourF redBlue[] {{.3f, 0, 0, 1}, {0, 0, .3f, 1}};
        require(nativeDrawing->drawGradientQuad(context, skew, redBlue));
        require(nativeDrawing->setBlendMode(context, kNatronOfxDrawBlendAdd));
        const OfxPointD bar[] {{.07, .07}, {.13, .21}};
        const OfxRGBAColourF rgb[] {
            {.711519527404004f / 2, .164533420851110f / 2, .164533420851110f / 2, 1},
            {0, .546986106552894f / 2, 0, 1},
            {.288480472595996f / 2, .288480472595996f / 2, .835466579148890f / 2, 1}
        };

        for (const auto& color : rgb) {
            require(drawing->setColour(context, &color));
            require(drawing->draw(context, kOfxDrawPrimitiveRectangle, bar, 2));
        }

        require(nativeDrawing->setBlendMode(context, kNatronOfxDrawBlendSourceOver));
        require(nativeDrawing->setAntialiasing(context, 1));
        ++counters[1];
        return kOfxStatOK;
    } catch (OfxStatus status) {
        return status;
    } catch (...) {
        return kOfxStatFailed;
    }
}

void setHost(OfxHost* h)
{
    host = h;
    properties = static_cast<const OfxPropertySuiteV1 *>(h->fetchSuite(h->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(h->fetchSuite(h->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(h->fetchSuite(h->host, kOfxParameterSuite, 1));
    curves = static_cast<const OfxParametricParameterSuiteV1 *>(h->fetchSuite(h->host, kOfxParametricParameterSuite, 1));
    interacts = static_cast<const OfxInteractSuiteV1 *>(h->fetchSuite(h->host, kOfxInteractSuite, 1));
    drawing = static_cast<const OfxDrawSuiteV1 *>(h->fetchSuite(h->host, kOfxDrawSuite, 1));
    nativeDrawing = static_cast<const NatronOfxDrawSuiteV1 *>(h->fetchSuite(h->host, kNatronOfxDrawSuite, 1));
}

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle out)
{
    try {
        if (std::strcmp(action, kOfxActionLoad) == 0) {
            if (!properties || !effects || !parameters || !curves || !interacts || !drawing || !nativeDrawing) {
                return kOfxStatErrMissingHostFeature;
            }

            int supported = 0;
            require(properties->propGetInt(host->host, kNatronOfxParamHostPropSupportsDrawSuiteParametricBackground, 0, &supported));
            return supported == 1 ? kOfxStatOK : kOfxStatErrMissingHostFeature;
        }

        auto effect = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            OfxPropertySetHandle props = nullptr;
            require(effects->getPropertySet(effect, &props));
            require(properties->propSetString(props, kOfxPropLabel, 0, "Curve DrawSuite Smoke"));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1));
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
            OfxParamSetHandle paramSet = nullptr;
            OfxPropertySetHandle props = nullptr;
            require(effects->getParamSet(effect, &paramSet));
            require(parameters->paramDefine(paramSet, kOfxParamTypeParametric, "curve", &props));
            require(properties->propSetInt(props, kOfxParamPropParametricDimension, 0, 1));
            require(properties->propSetDouble(props, kOfxParamPropParametricRange, 0, 0));
            require(properties->propSetDouble(props, kOfxParamPropParametricRange, 1, 1));
            require(properties->propSetDouble(props, kOfxParamPropDisplayMin, 0, 0));
            require(properties->propSetDouble(props, kOfxParamPropDisplayMax, 0, 1));
            const double color[] {0, .65, 1};
            require(properties->propSetDoubleN(props, kOfxParamPropParametricUIColour, 3, color));
            require(properties->propSetPointer(props, kOfxParamPropParametricInteractBackground, 0, reinterpret_cast<void*>(legacy)));
            require(properties->propSetPointer(props, kNatronOfxParamPropParametricInteractBackgroundDrawSuiteV1, 0, reinterpret_cast<void*>(background)));

            OfxParamHandle curve = nullptr;
            require(parameters->paramGetHandle(paramSet, "curve", &curve, nullptr));
            for (const auto point : {
                     OfxPointD{.1, .25}, OfxPointD{.5, .7}, OfxPointD{.9, .35}
                 }) {
                require(curves->parametricParamAddControlPoint(curve, 0, 0, point.x, point.y, false));
            }
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
OfxPlugin plugin{kOfxImageEffectPluginApi, 1, "org.natron.test.CurveDrawSuite", 1, 0, setHost, mainEntry};
}
#ifdef _WIN32
#define SMOKE_EXPORT __declspec(dllexport)
#else
#define SMOKE_EXPORT __attribute__((visibility("default")))
#endif
extern "C" SMOKE_EXPORT int OfxGetNumberOfPlugins()
{
    return 1;
}
extern "C" SMOKE_EXPORT OfxPlugin *OfxGetPlugin(int index)
{
    return index == 0 ? &plugin : nullptr;
}
extern "C" SMOKE_EXPORT unsigned long long OfxCurveDrawSuiteSmokeCounter(int index)
{
    return index >= 0 && index < 5 ? counters[index].load() : 0;
}
