// SPDX-License-Identifier: GPL-2.0-or-later
// An independent C ABI plugin: no Natron, Qt or graphics API linkage.
#include "Global/OpenFX/OverlayInteractV2.h"
#include "ofxImageEffect.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

namespace
{
const OfxPropertySuiteV1 *properties;
const OfxImageEffectSuiteV1 *effects;
const OfxParameterSuiteV1 *parameters;
const OfxInteractSuiteV1 *interacts;
const OfxDrawSuiteV1 *drawing;
const OfxMessageSuiteV1 *messages;
std::atomic<unsigned long long> counters[11] {};
bool insideModal = false;

void require(OfxStatus status)
{
    if (status != kOfxStatOK) {
        throw status;
    }
}

struct State {
    OfxImageEffectHandle effect = nullptr;
    OfxParamHandle center = nullptr;
    OfxParamHandle showModal = nullptr;
    OfxDrawContextHandle lastContext = nullptr;
    bool dragging = false;
    OfxPointD delta{};
};

void checkExpiredContext(State& state, OfxPropertySetHandle args)
{
    OfxRGBAColourF color{};

    if (state.lastContext) {
        if (drawing->getColour(state.lastContext, kOfxStandardColourOverlayText, &color) == kOfxStatFailed) {
            ++counters[2];
        } else {
            ++counters[3];
        }
    }

    void *context = nullptr;
    properties->propGetPointer(args, kOfxInteractPropDrawContext, 0, &context);

    if (context) {
        ++counters[3];
    }
}

OfxStatus legacyOverlay(const char*, const void*, OfxPropertySetHandle, OfxPropertySetHandle)
{
    ++counters[0];
    return kOfxStatFailed;
}

OfxStatus overlay(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle)
{
    try {
        auto interact = static_cast<OfxInteractHandle>(const_cast<void *>(handle));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            return kOfxStatOK;
        }

        OfxPropertySetHandle props = nullptr;
        require(interacts->interactGetPropertySet(interact, &props));

        if (std::strcmp(action, kOfxActionCreateInstance) == 0) {
            OfxImageEffectHandle effect = nullptr;
            require(properties->propGetPointer(props, kOfxPropEffectInstance, 0, reinterpret_cast<void**>(&effect)));
            OfxParamSetHandle paramSet = nullptr;
            require(effects->getParamSet(effect, &paramSet));
            auto state = std::make_unique<State>();
            state->effect = effect;
            require(parameters->paramGetHandle(paramSet, "center", &state->center, nullptr));
            require(parameters->paramGetHandle(paramSet, "showModal", &state->showModal, nullptr));
            require(properties->propSetPointer(props, kOfxPropInstanceData, 0, state.get()));
            require(properties->propSetString(props, kOfxInteractPropSlaveToParam, 0, "center"));
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

        double x = 0, y = 0;
        require(parameters->paramGetValue(state->center, &x, &y));

        if (std::strcmp(action, kOfxInteractActionDraw) == 0) {
            OfxDrawContextHandle context = nullptr;
            require(properties->propGetPointer(args, kOfxInteractPropDrawContext, 0, reinterpret_cast<void**>(&context)));
            state->lastContext = context;
            OfxRGBAColourF color{};

            for (int i = 0; i != 7; ++i) {
                require(drawing->getColour(context, static_cast<OfxStandardColour>(i), &color));
            }

            color = {1, 0, 0, 0.5f};
            require(drawing->setColour(context, &color));
            const OfxPointD rectangle[] {{x - 6, y - 4}, {x + 6, y + 4}};
            require(drawing->draw(context, kOfxDrawPrimitiveRectangle, rectangle, 2));
            color = {1, 1, 1, 1};
            require(drawing->setColour(context, &color));
            require(drawing->setLineWidth(context, 1.5f));
            const OfxPointD cross[] {{x - 3, y}, {x + 3, y}, {x, y - 3}, {x, y + 3}};
            require(drawing->draw(context, kOfxDrawPrimitiveLines, cross, 4));
            const OfxPointD ellipse[] {{x - 8, y - 6}, {x + 8, y + 6}};
            require(drawing->draw(context, kOfxDrawPrimitiveEllipse, ellipse, 2));
            color = {0, 1, 0, 0.75f};
            require(drawing->setColour(context, &color));
            const OfxPointD triangle[] {{x - 14, y + 10}, {x - 10, y + 16}, {x - 6, y + 10}};
            require(drawing->draw(context, kOfxDrawPrimitivePolygon, triangle, 3));
            color = {1, 1, 0, 1};
            require(drawing->setColour(context, &color));
            require(drawing->setLineStipple(context, kOfxDrawLineStipplePatternDash));
            const OfxPointD loop[] {{x - 16, y - 12}, {x + 16, y - 12}, {x + 16, y + 12}, {x - 16, y + 12}};
            require(drawing->draw(context, kOfxDrawPrimitiveLineLoop, loop, 4));
            require(drawing->setLineStipple(context, kOfxDrawLineStipplePatternSolid));
            require(drawing->setLineWidth(context, 0));
            const OfxPointD hairline[] {{x - 10, y - 9}, {x + 10, y - 9}};
            require(drawing->draw(context, kOfxDrawPrimitiveLineStrip, hairline, 2));
            const OfxPointD textPosition{x + 10, y + 15};
            require(drawing->drawText(context, "OFX UTF-8 \xc3\xa9", &textPosition,
                                      kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
            ++counters[1];
            return kOfxStatOK;
        }

        checkExpiredContext(*state, args);

        if (std::strcmp(action, kOfxInteractActionGainFocus) == 0) {
            ++counters[5];

            if (insideModal) {
                ++counters[7];
            }

            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxInteractActionLoseFocus) == 0) {
            ++counters[6];

            if (insideModal) {
                ++counters[7];
            }

            counters[8] = 0;
            state->dragging = false;
            counters[10] = 0;
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxInteractActionKeyDown) == 0 || std::strcmp(action, kOfxInteractActionKeyUp) == 0) {
            int key = 0;
            require(properties->propGetInt(args, kOfxPropKeySym, 0, &key));

            if (key == kOfxKey_Control_L || key == kOfxKey_Control_R) {
                counters[8] = std::strcmp(action, kOfxInteractActionKeyDown) == 0;
                return kOfxStatOK;
            }
        }

        if (std::strcmp(action, kOfxInteractActionPenDown) == 0) {
            double pos[2] {}, scale[2] {1, 1};
            require(properties->propGetDoubleN(args, kOfxInteractPropPenPosition, 2, pos));
            require(properties->propGetDoubleN(args, kOfxInteractPropPixelScale, 2, scale));
            state->dragging = std::abs(pos[0] - x) <= 10 * scale[0] && std::abs(pos[1] - y) <= 10 * scale[1];
            counters[10] = state->dragging;
            state->delta = {x - pos[0], y - pos[1]};
            int showModal = 0;
            require(parameters->paramGetValue(state->showModal, &showModal));

            if (state->dragging && showModal && messages) {
                struct ModalScope {
                    ModalScope()
                    {
                        insideModal = true;
                    }
                    ~ModalScope()
                    {
                        insideModal = false;
                    }
                } scope;
                ++counters[9];
                require(messages->message(state->effect, kOfxMessageMessage, "DrawSuiteSmoke", "Automated OFX focus interruption."));
            }

            return state->dragging ? kOfxStatOK : kOfxStatReplyDefault;
        }

        if (std::strcmp(action, kOfxInteractActionPenMotion) == 0 && state->dragging) {
            double pos[2] {};
            require(properties->propGetDoubleN(args, kOfxInteractPropPenPosition, 2, pos));
            require(parameters->paramSetValue(state->center, pos[0] + state->delta.x, pos[1] + state->delta.y));
            require(interacts->interactRedraw(interact));
            ++counters[4];
            return kOfxStatOK;
        }

        if (std::strcmp(action, kOfxInteractActionPenUp) == 0) {
            const bool dragging = state->dragging;
            state->dragging = false;
            counters[10] = 0;
            return dragging ? kOfxStatOK : kOfxStatReplyDefault;
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

void setHost(OfxHost* host)
{
    properties = static_cast<const OfxPropertySuiteV1 *>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    effects = static_cast<const OfxImageEffectSuiteV1 *>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    parameters = static_cast<const OfxParameterSuiteV1 *>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    interacts = static_cast<const OfxInteractSuiteV1 *>(host->fetchSuite(host->host, kOfxInteractSuite, 1));
    drawing = static_cast<const OfxDrawSuiteV1 *>(host->fetchSuite(host->host, kOfxDrawSuite, 1));
    messages = static_cast<const OfxMessageSuiteV1 *>(host->fetchSuite(host->host, kOfxMessageSuite, 1));
}

OfxStatus mainEntry(const char* action, const void* handle, OfxPropertySetHandle args, OfxPropertySetHandle out)
{
    try {
        if (std::strcmp(action, kOfxActionLoad) == 0) {
            return properties && effects && parameters && interacts && drawing ? kOfxStatOK : kOfxStatErrMissingHostFeature;
        }

        auto effect = static_cast<OfxImageEffectHandle>(const_cast<void *>(handle));

        if (std::strcmp(action, kOfxActionDescribe) == 0) {
            OfxPropertySetHandle props = nullptr;
            require(effects->getPropertySet(effect, &props));
            require(properties->propSetString(props, kOfxPropLabel, 0, "DrawSuite Smoke"));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte));
            require(properties->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat));
            require(properties->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 1));
            require(properties->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0, reinterpret_cast<void*>(legacyOverlay)));
            require(properties->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV2, 0, reinterpret_cast<void*>(overlay)));
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
            require(parameters->paramDefine(paramSet, kOfxParamTypeDouble2D, "center", &props));
            require(properties->propSetString(props, kOfxPropLabel, 0, "Center"));
            require(properties->propSetDouble(props, kOfxParamPropDefault, 0, 32));
            require(properties->propSetDouble(props, kOfxParamPropDefault, 1, 24));
            require(parameters->paramDefine(paramSet, kOfxParamTypeBoolean, "showModal", &props));
            require(properties->propSetInt(props, kOfxParamPropDefault, 0, 0));
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

OfxPlugin plugin{kOfxImageEffectPluginApi, 1, "org.natron.test.DrawSuite", 1, 0, setHost, mainEntry};
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
extern "C" SMOKE_EXPORT unsigned long long OfxDrawSuiteSmokeCounter(int index)
{
    return index >= 0 && index < 11 ? counters[index].load() : 0;
}
