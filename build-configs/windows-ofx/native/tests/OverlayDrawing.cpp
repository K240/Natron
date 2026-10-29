// SPDX-License-Identifier: GPL-2.0-or-later
#include "NatronOverlayInteract.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace
{
struct Command {
    OfxDrawPrimitive primitive;
    std::vector<OfxPointD> points;
    OfxRGBAColourF color;
};
struct Session {
    std::vector<Command> commands;
    OfxRGBAColourF color{1, 1, 1, 1};
    float width = 1;
    OfxDrawLineStipplePattern stipple = kOfxDrawLineStipplePatternSolid;
    OfxPointD textPosition{};
    std::string text;
    int alignment = 0;
};
Session &session(OfxDrawContextHandle handle)
{
    return *reinterpret_cast<Session*>(handle);
}
OfxStatus getColour(OfxDrawContextHandle, OfxStandardColour, OfxRGBAColourF*)
{
    return kOfxStatErrUnsupported;
}
OfxStatus colour(OfxDrawContextHandle h, const OfxRGBAColourF* c)
{
    session(h).color = *c;
    return kOfxStatOK;
}
OfxStatus width(OfxDrawContextHandle h, float w)
{
    session(h).width = w;
    return kOfxStatOK;
}
OfxStatus stipple(OfxDrawContextHandle h, OfxDrawLineStipplePattern p)
{
    session(h).stipple = p;
    return kOfxStatOK;
}
OfxStatus draw(OfxDrawContextHandle h, OfxDrawPrimitive p, const OfxPointD* points, int count)
{
    session(h).commands.push_back({p, {points, points + count}, session(h).color});
    return kOfxStatOK;
}
OfxStatus text(OfxDrawContextHandle h, const char* value, const OfxPointD* p, int alignment)
{
    auto& s = session(h);
    s.text = value;
    s.textPosition = *p;
    s.alignment = alignment;
    return kOfxStatOK;
}
int checks = 0;
void require(bool condition, const char* message)
{
    ++checks;

    if (!condition) {
        throw std::runtime_error(message);
    }
}
void at(const OfxPointD& p, double x, double y)
{
    require(std::abs(p.x - x) < 1e-10 && std::abs(p.y - y) < 1e-10, "Wrong canonical geometry");
}
template<class CALL>
void rejected(CALL call)
{
    bool caught = false;

    try {
        call();
    } catch (OfxStatus) {
        caught = true;
    }

    require(caught, "Invalid drawing accepted");
}
}
int main()
{
    try {
        Session s;
        const auto handle = reinterpret_cast<OfxDrawContextHandle>(&s);
        const OfxDrawSuiteV1 suite{getColour, colour, width, stipple, draw, text};
        NatronOFX::OverlayDrawing drawing(handle, &suite, nullptr, {2, 3});
        drawing.lineWidth(1.5f);
        drawing.dashed(true);
        require(s.width == 1.5f && s.stipple == kOfxDrawLineStipplePatternDot, "Wrong logical line style");
        drawing.begin(NatronOFX::OverlayPrimitive::Lines);
        drawing.colour(1, 0, 0);
        drawing.vertex(-17, -13);
        drawing.vertex(5, 7);
        drawing.colour(0, 1, 0);
        drawing.vertex(9, 11);
        drawing.vertex(12, 15);
        drawing.end();
        require(s.commands.size() == 2 && s.commands[0].color.r == 1 && s.commands[1].color.g == 1, "Per-segment colours lost");
        at(s.commands[0].points[0], -17, -13);
        drawing.pushTransform();
        drawing.translate(10, -6);
        drawing.rotate(90);
        double matrix[16] {};
        matrix[0] = 2;
        matrix[1] = 1;
        matrix[4] = 3;
        matrix[5] = 4;
        matrix[10] = matrix[15] = 1;
        drawing.affine(matrix);
        drawing.offsetShadow(3, -2);
        drawing.begin(NatronOFX::OverlayPrimitive::LineStrip);
        drawing.vertex(2, 5);
        drawing.vertex(1, 1);
        drawing.end();
        at(s.commands.back().points[0], -9, 11);
        drawing.popTransform();
        drawing.offsetShadow(-3, 2);
        drawing.begin(NatronOFX::OverlayPrimitive::LineLoop);
        drawing.vertex(-8, -4);
        drawing.vertex(2, -4);
        drawing.vertex(2, 6);
        drawing.end();
        at(s.commands.back().points[0], -8, -4);
        require(s.commands.back().primitive == kOfxDrawPrimitiveLineLoop, "Outline type lost");
        drawing.pointSize(4);
        drawing.roundPoints(false);
        drawing.begin(NatronOFX::OverlayPrimitive::Points);
        drawing.vertex(10, 20);
        drawing.end();
        const auto& square = s.commands.back();
        require(square.primitive == kOfxDrawPrimitiveRectangle && square.points.size() == 2, "Point rectangle missing");
        at(square.points[0], 6, 14);
        at(square.points[1], 14, 26);
        drawing.roundPoints(true);
        drawing.begin(NatronOFX::OverlayPrimitive::Points);
        drawing.vertex(10, 20);
        drawing.end();
        const auto& circle = s.commands.back();
        require(circle.primitive == kOfxDrawPrimitivePolygon && circle.points.size() == 16, "Smoothed point polygon missing");
        at(circle.points[0], 14, 20);
        at(circle.points[4], 10, 26);
        drawing.text(-30, 70, "rampPoint0");
        require(s.text == "rampPoint0" && s.alignment == (kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline), "Host text alignment wrong");
        at(s.textPosition, -30, 70);
        drawing.dashed(false);
        require(s.stipple == kOfxDrawLineStipplePatternSolid, "Stipple state not reset");
        rejected([&] { drawing.popTransform(); });
        rejected([&] { drawing.end(); });
        rejected([&] { drawing.vertex(1, 2); });
        rejected([&] { drawing.pointSize(0); });
        rejected([&] { drawing.pointSize(std::numeric_limits<double>::infinity()); });
        rejected([&] { drawing.translate(0, 0, 1); });
        drawing.begin(NatronOFX::OverlayPrimitive::Lines);
        rejected([&] { drawing.begin(NatronOFX::OverlayPrimitive::Lines); });
        drawing.vertex(0, 0);
        rejected([&] { drawing.end(); });
        rejected([&] { NatronOFX::OverlayDrawing invalid(handle, &suite, nullptr, {0, 1}); });
        rejected([&] { NatronOFX::OverlayDrawing invalid(nullptr, &suite, nullptr, {1, 1}); });
        rejected([&] { NatronOFX::overlayDrawing(); });
        std::cout << "{\"passed\":true,\"checks\":" << checks << "}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
    } catch (...) {
        std::cerr << "Unexpected overlay status\n";
    }

    return 1;
}
