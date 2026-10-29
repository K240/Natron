// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_OFX_OVERLAY_INTERACT_H
#define NATRON_OFX_OVERLAY_INTERACT_H
#include "Global/OpenFX/OverlayInteractV2.h"
#include "Global/OpenFX/NatronDrawSuite.h"
#include "ofxsImageEffect.h"
#include "ofxsInteract.h"
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace NatronOFX
{
enum class OverlayPrimitive { Points, Lines, LineStrip, LineLoop, Polygon };
// Canonical geometry, colour and small affine transforms. No graphics handles
// or graphics API state cross the standard DrawSuite boundary.
class OverlayDrawing
{
    using Matrix = std::array<double, 6>;
    OfxDrawContextHandle _context;
    const OfxDrawSuiteV1 *_suite;
    const NatronOfxDrawSuiteV1 *_native;
    OfxPointD _pixelScale;
    Matrix _matrix{1, 0, 0, 1, 0, 0};
    std::vector<Matrix> _stack;
    OfxPointD _shadow{0, 0};
    OverlayPrimitive _primitive = OverlayPrimitive::Lines;
    std::vector<OfxPointD> _points;
    std::vector<OfxRGBAColourF> _colors;
    OfxRGBAColourF _color{1, 1, 1, 1};
    double _pointSize = 5;
    bool _roundPoints = false, _smoothLines = true, _drawing = false;
    static void check(OfxStatus status)
    {
        if (status != kOfxStatOK) {
            throw status;
        }
    }
    void antialias(bool enabled)
    {
        if (_native) {
            check(_native->setAntialiasing(_context, enabled));
        }
    }
    OfxPointD position(double x, double y) const
    {
        return {_matrix[0] *x + _matrix[2] *y + _matrix[4] + _shadow.x,
                _matrix[1] *x + _matrix[3] *y + _matrix[5] + _shadow.y};
    }
    void multiply(const Matrix& b)
    {
        const auto a = _matrix;
        _matrix = {a[0] *b[0] + a[2] *b[1], a[1] *b[0] + a[3] *b[1],
                   a[0] *b[2] + a[2] *b[3], a[1] *b[2] + a[3] *b[3],
                   a[0] *b[4] + a[2] *b[5] + a[4], a[1] *b[4] + a[3] *b[5] + a[5]
                  };
    }
public:
    OverlayDrawing(OfxDrawContextHandle context, const OfxDrawSuiteV1* suite,
                   const NatronOfxDrawSuiteV1* native, OfxPointD pixelScale)
        : _context(context), _suite(suite), _native(native), _pixelScale(pixelScale)
    {
        if (!context || !suite) {
            throw kOfxStatErrMissingHostFeature;
        }

        if (!std::isfinite(pixelScale.x) || !std::isfinite(pixelScale.y) || pixelScale.x <= 0 || pixelScale.y <= 0) {
            throw kOfxStatErrValue;
        }
    }
    void colour(double r, double g, double b, double a = 1)
    {
        _color = {float(r), float(g), float(b), float(a)};
    }
    void lineWidth(float width)
    {
        check(_suite->setLineWidth(_context, width));
    }
    void pointSize(double size)
    {
        if (!std::isfinite(size) || size <= 0) {
            throw kOfxStatErrValue;
        }

        _pointSize = size;
    }
    void roundPoints(bool enabled)
    {
        _roundPoints = enabled;
    }
    void smoothLines(bool enabled)
    {
        _smoothLines = enabled;
    }
    void dashed(bool enabled)
    {
        check(_suite->setLineStipple(_context, enabled ? kOfxDrawLineStipplePatternDot : kOfxDrawLineStipplePatternSolid));
    }
    void pushTransform()
    {
        _stack.push_back(_matrix);
    }
    void popTransform()
    {
        if (_stack.empty()) {
            throw kOfxStatErrValue;
        }

        _matrix = _stack.back();
        _stack.pop_back();
    }
    void translate(double x, double y, double z = 0)
    {
        if (z != 0) {
            throw kOfxStatErrValue;
        }

        multiply({1, 0, 0, 1, x, y});
    }
    void rotate(double degrees)
    {
        const double angle = degrees * 3.14159265358979323846 / 180;
        const double c = std::cos(angle), s = std::sin(angle);
        multiply({c, s, -s, c, 0, 0});
    }
    void affine(const double matrix[16])
    {
        multiply({matrix[0], matrix[1], matrix[4], matrix[5], matrix[12], matrix[13]});
    }
    void offsetShadow(double x, double y)
    {
        _shadow.x += x;
        _shadow.y += y;
    }
    void begin(OverlayPrimitive primitive)
    {
        if (_drawing) {
            throw kOfxStatErrValue;
        }

        _drawing = true;
        _primitive = primitive;
        _points.clear();
        _colors.clear();
    }
    void vertex(double x, double y)
    {
        if (!_drawing) {
            throw kOfxStatErrValue;
        }

        _points.push_back(position(x, y));
        _colors.push_back(_color);
    }
    void end()
    {
        if (!_drawing) {
            throw kOfxStatErrValue;
        }

        _drawing = false;

        if (_points.empty()) {
            return;
        }

        if (_primitive == OverlayPrimitive::Points) {
            antialias(_roundPoints);

            for (size_t i = 0; i < _points.size(); ++i) {
                check(_suite->setColour(_context, &_colors[i]));
                const auto& p = _points[i];
                const double rx = _pointSize * _pixelScale.x * .5, ry = _pointSize * _pixelScale.y * .5;

                if (_roundPoints) {
                    std::array<OfxPointD, 16> circle;

                    for (size_t j = 0; j < circle.size(); ++j) {
                        const double angle = j * (2 * 3.14159265358979323846 / circle.size());
                        circle[j] = {p.x + rx *std::cos(angle), p.y + ry * std::sin(angle)};
                    }

                    check(_suite->draw(_context, kOfxDrawPrimitivePolygon, circle.data(), int(circle.size())));
                } else {
                    const OfxPointD corners[] = {{p.x - rx, p.y - ry}, {p.x + rx, p.y + ry}};
                    check(_suite->draw(_context, kOfxDrawPrimitiveRectangle, corners, 2));
                }
            }
        } else {
            antialias(_smoothLines);

            if (_primitive == OverlayPrimitive::Lines) {
                if (_points.size() % 2) {
                    throw kOfxStatErrValue;
                }

                for (size_t i = 0; i < _points.size(); i += 2) {
                    check(_suite->setColour(_context, &_colors[i]));
                    check(_suite->draw(_context, kOfxDrawPrimitiveLines, _points.data() + i, 2));
                }
            } else {
                check(_suite->setColour(_context, &_colors.front()));
                const auto primitive = _primitive == OverlayPrimitive::LineStrip ? kOfxDrawPrimitiveLineStrip :
                                       _primitive == OverlayPrimitive::LineLoop ? kOfxDrawPrimitiveLineLoop : kOfxDrawPrimitivePolygon;
                check(_suite->draw(_context, primitive, _points.data(), int(_points.size())));
            }
        }
    }
    void text(double x, double y, const char* value)
    {
        const auto at = position(x, y);
        check(_suite->setColour(_context, &_color));
        check(_suite->drawText(_context, value, &at, kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline));
    }
};
inline thread_local OverlayDrawing *currentOverlayDrawing = nullptr;
inline OverlayDrawing &overlayDrawing()
{
    if (!currentOverlayDrawing) {
        throw kOfxStatFailed;
    }

    return *currentOverlayDrawing;
}
template<class DESC, class INSTANCE>
class OverlayDescriptor : public OFX::DefaultEffectOverlayDescriptor<DESC, INSTANCE>
{
    static OfxStatus entry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
    {
        try {
            std::unique_ptr<OverlayDrawing> drawing;

            if (std::strcmp(action, kOfxInteractActionDraw) == 0) {
                OFX::PropertySet args(in);
                const auto context = reinterpret_cast<OfxDrawContextHandle>(args.propGetPointer(kOfxInteractPropDrawContext));
                const auto standard = static_cast<const OfxDrawSuiteV1 *>(OFX::fetchSuite(kOfxDrawSuite, 1));
                const auto native = static_cast<const NatronOfxDrawSuiteV1 *>(OFX::fetchSuite(kNatronOfxDrawSuite, 1, true));
                drawing = std::make_unique<OverlayDrawing>(context, standard, native,
                          OfxPointD{args.propGetDouble(kOfxInteractPropPixelScale, 0), args.propGetDouble(kOfxInteractPropPixelScale, 1)});
            }

            struct Scope {
                OverlayDrawing *previous = currentOverlayDrawing;
                explicit Scope(OverlayDrawing* current)
                {
                    currentOverlayDrawing = current;
                }
                ~Scope()
                {
                    currentOverlayDrawing = previous;
                }
            } scope(drawing.get());

            static DESC descriptor;
            return OFX::Private::interactMainEntry(action, handle, in, out, descriptor);
        } catch (OfxStatus status) {
            return status;
        } catch (const std::bad_alloc &) {
            return kOfxStatErrMemory;
        } catch (...) {
            return kOfxStatFailed;
        }
    }
public:
    OfxPluginEntryPoint *getMainEntry() override
    {
        return entry;
    }
};
template<class DESC>
void setOverlay(OFX::ImageEffectDescriptor& effect, DESC* descriptor)
{
    std::unique_ptr<DESC> owner(descriptor);

    if (!OFX::fetchSuite(kOfxDrawSuite, 1, true)) {
        return;
    }

    const auto entry = descriptor->getMainEntry();
    effect.setOverlayInteractDescriptor(owner.release());
    effect.getPropertySet().propSetPointer(kOfxImageEffectPluginPropOverlayInteractV1, nullptr);
    effect.getPropertySet().propSetPointer(kOfxImageEffectPluginPropOverlayInteractV2, reinterpret_cast<void*>(entry));
}
}
#endif
