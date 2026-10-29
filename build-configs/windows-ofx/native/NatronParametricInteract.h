// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_NATIVE_PARAMETRIC_INTERACT_H
#define NATRON_OFX_NATIVE_PARAMETRIC_INTERACT_H
#include "Global/OpenFX/ParamInteractDrawSuite.h"
#include "Global/OpenFX/NatronDrawSuite.h"
#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include <array>
#include <cstring>
#include <memory>
#include <vector>

namespace NatronOFX {
inline void require(OfxStatus status) { if (status != kOfxStatOK) { throw status; } }
struct Context {
    OfxDrawContextHandle handle;
    const OfxDrawSuiteV1* standard;
    const NatronOfxDrawSuiteV1* native;
};
inline thread_local const Context* currentContext = nullptr;

enum class Primitive { Lines, Quads, TriangleStrip };
// Collect the original canonical geometry without a graphics context. Gradient
// strip pairs become parallelograms with the same interpolated edge colours.
class Draw {
    Primitive _primitive = Primitive::Lines;
    std::vector<OfxPointD> _points;
    std::vector<OfxRGBAColourF> _colors;
    OfxRGBAColourF _color{1, 1, 1, 1};
    const Context& context() const { if (!currentContext) { throw kOfxStatFailed; } return *currentContext; }
public:
    void color(double r, double g, double b, double a = 1) { _color = {float(r), float(g), float(b), float(a)}; }
    void lineWidth(float width) { const auto& c = context(); require(c.standard->setLineWidth(c.handle, width)); }
    void addBlend() { const auto& c = context(); require(c.native->setBlendMode(c.handle, kNatronOfxDrawBlendAdd)); }
    void begin(Primitive primitive) { _primitive = primitive; _points.clear(); _colors.clear(); }
    void vertex(double x, double y) { _points.push_back({x, y}); _colors.push_back(_color); }
    void end() {
        const auto& c = context();
        if (_primitive == Primitive::TriangleStrip) {
            if (_points.size() % 2) { throw kOfxStatErrValue; }
            for (size_t i = 2; i < _points.size(); i += 2) {
                const OfxPointD points[]{_points[i-2], _points[i-1], _points[i+1], _points[i]};
                const OfxRGBAColourF colors[]{_colors[i-2], _colors[i]};
                require(c.native->drawGradientQuad(c.handle, points, colors));
            }
        } else {
            const size_t count = _primitive == Primitive::Lines ? 2 : 4;
            if (_points.size() % count) { throw kOfxStatErrValue; }
            for (size_t i = 0; i < _points.size(); i += count) {
                require(c.standard->setColour(c.handle, &_colors[i]));
                require(c.standard->draw(c.handle, _primitive == Primitive::Lines ? kOfxDrawPrimitiveLines : kOfxDrawPrimitivePolygon,
                                         _points.data()+i, int(count)));
            }
        }
    }
};

template<class DESC, class INSTANCE>
class ParametricDescriptor : public OFX::DefaultParamInteractDescriptor<DESC, INSTANCE> {
    static OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
        try {
            Context context{};
            if (std::strcmp(action, kOfxInteractActionDraw) == 0) {
                OFX::PropertySet args(in);
                context.handle = reinterpret_cast<OfxDrawContextHandle>(args.propGetPointer(kOfxInteractPropDrawContext));
                context.standard = static_cast<const OfxDrawSuiteV1*>(OFX::fetchSuite(kOfxDrawSuite, 1));
                context.native = static_cast<const NatronOfxDrawSuiteV1*>(OFX::fetchSuite(kNatronOfxDrawSuite, 1));
                if (!context.handle || !context.standard || !context.native) { return kOfxStatErrMissingHostFeature; }
                // Old background fills are unsmoothed; adjacent gradient slices
                // and additive histogram bars must share edges without gaps.
                require(context.native->setAntialiasing(context.handle, 0));
            }
            struct Scope {
                const Context* previous = currentContext;
                explicit Scope(const Context* context) { currentContext = context; }
                ~Scope() { currentContext = previous; }
            } scope(context.handle ? &context : nullptr);
            static DESC descriptor;
            return OFX::Private::interactMainEntry(action, handle, in, out, descriptor);
        } catch (OfxStatus status) { return status; }
          catch (const std::bad_alloc&) { return kOfxStatErrMemory; }
          catch (...) { return kOfxStatFailed; }
    }
public:
    OfxPluginEntryPoint* getMainEntry() override { return entry; }
};

template<class DESC>
void setBackground(OFX::ParametricParamDescriptor* param, DESC* descriptor) {
    std::unique_ptr<DESC> owner(descriptor);
    if (!OFX::fetchSuite(kOfxDrawSuite, 1, true) || !OFX::fetchSuite(kNatronOfxDrawSuite, 1, true)) { return; }
    const auto entry = descriptor->getMainEntry();
    param->setInteractDescriptor(owner.release());
    param->getPropertySet().propSetPointer(kOfxParamPropParametricInteractBackground, nullptr);
    param->getPropertySet().propSetPointer(kNatronOfxParamPropParametricInteractBackgroundDrawSuiteV1, reinterpret_cast<void*>(entry));
}
}
#endif
