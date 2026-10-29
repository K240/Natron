// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/OfxDrawSuite.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <vector>
#include <QtCore/Qt>

NATRON_NAMESPACE_ENTER
namespace
{
std::atomic<uintptr_t> nextToken{1};
thread_local std::vector<OfxDrawSession *> activeSessions;

template<class Action>
OfxStatus invoke(OfxDrawContextHandle handle, Action action) noexcept
{
    const auto found = std::find_if(activeSessions.begin(), activeSessions.end(),
    [handle](const auto * session) {
        return session->handle() == handle;
    });

    if (found == activeSessions.end()) {
        return kOfxStatFailed;
    }

    try {
        return action(**found);
    } catch (const std::bad_alloc &) {
        return kOfxStatErrMemory;
    } catch (const std::invalid_argument &) {
        return kOfxStatErrValue;
    } catch (...) {
        return kOfxStatFailed;
    }
}

OfxStatus getColour(OfxDrawContextHandle handle, OfxStandardColour color, OfxRGBAColourF* result) noexcept
{
    return invoke(handle, [&](OfxDrawSession & session) {
        if (!result || color < kOfxStandardColourOverlayBackground || color > kOfxStandardColourOverlayText) {
            return kOfxStatErrValue;
        }

        *result = session.colors()[size_t(color)];
        return kOfxStatOK;
    });
}

OfxStatus setColour(OfxDrawContextHandle handle, const OfxRGBAColourF* color) noexcept
{
    return invoke(handle, [&](OfxDrawSession & session) {
        if (!color || !std::isfinite(color->r) || !std::isfinite(color->g) ||
            !std::isfinite(color->b) || !std::isfinite(color->a)) {
            return kOfxStatErrValue;
        }

        session.commands().color(color->r, color->g, color->b, color->a);
        return kOfxStatOK;
    });
}

OfxStatus setLineWidth(OfxDrawContextHandle handle, float width) noexcept
{
    return invoke(handle, [&](OfxDrawSession & session) {
        if (!std::isfinite(width) || width < 0) {
            return kOfxStatErrValue;
        }

        session.commands().lineWidth(width);
        return kOfxStatOK;
    });
}

OfxStatus setLineStipple(OfxDrawContextHandle handle, OfxDrawLineStipplePattern pattern) noexcept
{
    return invoke(handle, [&](OfxDrawSession & session) {
        switch (pattern) {
        case kOfxDrawLineStipplePatternSolid:
            session.commands().solidLine();
            break;

        case kOfxDrawLineStipplePatternDot:
            session.commands().stipple(1, 0x5555);
            break;

        case kOfxDrawLineStipplePatternDash:
            session.commands().stipple(1, 0x0f0f);
            break;

        case kOfxDrawLineStipplePatternAltDash:
            session.commands().stipple(1, 0xf0f0);
            break;

        case kOfxDrawLineStipplePatternDotDash:
            session.commands().stipple(1, 0x4f4f);
            break;

        default:
            return kOfxStatErrValue;
        }

        return kOfxStatOK;
    });
}

OfxStatus draw(OfxDrawContextHandle handle, OfxDrawPrimitive primitive, const OfxPointD* points, int count) noexcept
{
    return invoke(handle, [&](OfxDrawSession & session) {
        if (count < 0 || (count && !points)) {
            return kOfxStatErrValue;
        }

        OverlayPrimitive kind;

        switch (primitive) {
        case kOfxDrawPrimitiveLines:
            if (count % 2) {
                return kOfxStatErrValue;
            }

            kind = OverlayPrimitive::Lines;
            break;

        case kOfxDrawPrimitiveLineStrip:
            kind = OverlayPrimitive::LineStrip;
            break;

        case kOfxDrawPrimitiveLineLoop:
            kind = OverlayPrimitive::LineLoop;
            break;

        case kOfxDrawPrimitiveRectangle:
        case kOfxDrawPrimitiveEllipse:
            if (count != 2) {
                return kOfxStatErrValue;
            }

            kind = primitive == kOfxDrawPrimitiveRectangle ? OverlayPrimitive::Polygon : OverlayPrimitive::LineLoop;
            break;

        case kOfxDrawPrimitivePolygon:
            if (count && count < 3) {
                return kOfxStatErrValue;
            }

            kind = OverlayPrimitive::Polygon;
            break;

        default:
            return kOfxStatErrValue;
        }

        if (count == 0) {
            return kOfxStatOK;
        }

        if (count < 2) {
            return kOfxStatErrValue;
        }

        for (int i = 0; i < count; ++i) {
            if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) {
                return kOfxStatErrValue;
            }
        }

        auto& list = session.commands();
        list.begin(kind);

        try {
            if (primitive == kOfxDrawPrimitiveRectangle || primitive == kOfxDrawPrimitiveEllipse) {
                const double x1 = std::min(points[0].x, points[1].x), x2 = std::max(points[0].x, points[1].x);
                const double y1 = std::min(points[0].y, points[1].y), y2 = std::max(points[0].y, points[1].y);

                if (primitive == kOfxDrawPrimitiveRectangle) {
                    list.vertex(x1, y1);
                    list.vertex(x2, y1);
                    list.vertex(x2, y2);
                    list.vertex(x1, y2);
                } else {
                    const double rx = (x2 - x1) / 2, ry = (y2 - y1) / 2;
                    const double cx = x1 + rx, cy = y1 + ry;
                    const double radius = std::max(rx / session.pixelScaleX(), ry / session.pixelScaleY()) * session.devicePixelRatio();
                    constexpr double pi = 3.14159265358979323846;
                    const double step = std::acos(std::clamp(1 - 0.25 / std::max(0.25, radius), 0., 1.));
                    const int segments = step > 0 ? int(std::clamp(std::ceil(pi / step), 16., 4096.)) : 4096;

                    for (int i = 0; i < segments; ++i) {
                        const double angle = 2 * pi * i / segments;
                        list.vertex(cx + rx * std::cos(angle), cy + ry * std::sin(angle));
                    }
                }
            } else {
                for (int i = 0; i < count; ++i) {
                    list.vertex(points[i].x, points[i].y);
                }
            }

            list.end();
        } catch (...) {
            list.cancelPrimitive();
            throw;
        }

        return kOfxStatOK;
    });
}

OfxStatus drawText(OfxDrawContextHandle handle, const char* text, const OfxPointD* position, int alignment) noexcept
{
    return invoke(handle, [&](OfxDrawSession & session) {
        if (!text || !position || !std::isfinite(position->x) || !std::isfinite(position->y) ||
            (alignment & ~0x1f)) {
            return kOfxStatErrValue;
        }

        int flags = 0;
        const int horizontal = alignment & kOfxDrawTextAlignmentCenterH;

        if (horizontal == kOfxDrawTextAlignmentCenterH) {
            flags |= Qt::AlignHCenter;
        } else if (horizontal == kOfxDrawTextAlignmentRight) {
            flags |= Qt::AlignRight;
        }

        const int vertical = alignment & (kOfxDrawTextAlignmentTop | kOfxDrawTextAlignmentBottom | kOfxDrawTextAlignmentBaseline);

        switch (vertical) {
        case 0:
        case kOfxDrawTextAlignmentBaseline:
            flags |= OverlayTextBaseline;
            break;

        case kOfxDrawTextAlignmentTop:
            flags |= Qt::AlignTop;
            break;

        case kOfxDrawTextAlignmentBottom:
            flags |= Qt::AlignBottom;
            break;

        case kOfxDrawTextAlignmentCenterV:
            flags |= Qt::AlignVCenter;
            break;

        default:
            return kOfxStatErrValue;
        }

        session.commands().text(position->x, position->y, text, flags);
        return kOfxStatOK;
    });
}
}

const NatronOfxDrawSuiteV1 &ofxNatronDrawSuite()
{
    static const NatronOfxDrawSuiteV1 suite {
        [](OfxDrawContextHandle handle, const OfxPointD * vertices, const OfxRGBAColourF * colors) -> OfxStatus {
            return invoke(handle, [&](OfxDrawSession & session)
            {
                if (!vertices || !colors) {
                    return kOfxStatErrValue;
                }

                double magnitude = 1;

                for (int i = 0; i < 4; ++i) {
                    if (!std::isfinite(vertices[i].x) || !std::isfinite(vertices[i].y)) {
                        return kOfxStatErrValue;
                    }

                    magnitude = std::max({magnitude, std::abs(vertices[i].x), std::abs(vertices[i].y)});
                }

                for (int i = 0; i < 2; ++i) {
                    const auto& c = colors[i];

                    if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b) || !std::isfinite(c.a)) {
                        return kOfxStatErrValue;
                    }
                }

                const double area = (vertices[1].x - vertices[0].x) * (vertices[3].y - vertices[0].y) -
                                    (vertices[1].y - vertices[0].y) * (vertices[3].x - vertices[0].x);

                if (!std::isfinite(area) || area == 0 ||
                    std::abs((vertices[0].x - vertices[1].x) + (vertices[2].x - vertices[3].x)) > magnitude * 1e-9 ||
                    std::abs((vertices[0].y - vertices[1].y) + (vertices[2].y - vertices[3].y)) > magnitude * 1e-9) {
                    return kOfxStatErrValue;
                }

                auto& list = session.commands();
                OverlayDrawScope scope(list);
                list.begin(OverlayPrimitive::GradientQuad);

                try {
                    for (int i = 0; i < 4; ++i) {
                        const auto& c = colors[i < 2 ? 0 : 1];
                        list.color(c.r, c.g, c.b, c.a);
                        list.vertex(vertices[i].x, vertices[i].y);
                    }

                    list.end();
                } catch (...) {
                    list.cancelPrimitive();
                    throw;
                }

                return kOfxStatOK;
            });
        },
        [](OfxDrawContextHandle handle, NatronOfxDrawBlendMode mode) -> OfxStatus {
            return invoke(handle, [&](OfxDrawSession & session)
            {
                if (mode != kNatronOfxDrawBlendSourceOver && mode != kNatronOfxDrawBlendAdd) {
                    return kOfxStatErrValue;
                }

                session.commands().blendMode(mode == kNatronOfxDrawBlendAdd ? OverlayBlendMode::Add : OverlayBlendMode::SourceOver);
                return kOfxStatOK;
            });
        },
        [](OfxDrawContextHandle handle, int enabled) -> OfxStatus {
            return invoke(handle, [&](OfxDrawSession & session)
            {
                if (enabled != 0 && enabled != 1) {
                    return kOfxStatErrValue;
                }

                session.commands().antialiasing(enabled != 0);
                return kOfxStatOK;
            });
        }
    };
    return suite;
}

OfxDrawSession::Colors OfxDrawSession::defaultColors()
{
    return {{{0, 0, 0, 1}, {0, 1, 0, 1}, {0.8f, 0.8f, 0.8f, 1},
            {0.5f, 0.5f, 0.5f, 1}, {1, 0.8f, 0, 1}, {0, 0, 0, 0.8f}, {0.8f, 0.8f, 0.8f, 1}
        }};
}

OfxDrawSession::OfxDrawSession(OverlayDrawList& commands, const Colors& colors,
                               double pixelScaleX, double pixelScaleY, double devicePixelRatio)
    : _commands(commands), _scope(commands), _colors(colors), _pixelScaleX(pixelScaleX),
      _pixelScaleY(pixelScaleY), _devicePixelRatio(devicePixelRatio),
      _handle(reinterpret_cast<OfxDrawContextHandle>(nextToken.fetch_add(1, std::memory_order_relaxed)))
{
    if (!std::isfinite(pixelScaleX) || !std::isfinite(pixelScaleY) || !std::isfinite(devicePixelRatio) ||
        pixelScaleX <= 0 || pixelScaleY <= 0 || devicePixelRatio <= 0) {
        throw std::invalid_argument("Invalid DrawSuite viewport scale.");
    }

    _commands.color(colors[kOfxStandardColourOverlaySelected].r, colors[kOfxStandardColourOverlaySelected].g,
                    colors[kOfxStandardColourOverlaySelected].b, colors[kOfxStandardColourOverlaySelected].a);
    _commands.lineWidth(1);
    _commands.blendMode(OverlayBlendMode::SourceOver);
    _commands.antialiasing(true);
    _commands.solidLine();
    activeSessions.push_back(this);
}

OfxDrawSession::~OfxDrawSession()
{
    const auto found = std::find(activeSessions.begin(), activeSessions.end(), this);

    if (found != activeSessions.end()) {
        activeSessions.erase(found);
    }
}

const OfxDrawSuiteV1 &ofxDrawSuite()
{
    static const OfxDrawSuiteV1 suite{getColour, setColour, setLineWidth, setLineStipple, draw, drawText};
    return suite;
}
NATRON_NAMESPACE_EXIT
