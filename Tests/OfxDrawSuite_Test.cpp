// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include <gtest/gtest.h>
#include "Engine/OfxDrawSuite.h"
#include <limits>
#include <thread>
#include <QtCore/Qt>

NATRON_NAMESPACE_USING

TEST(OfxDrawSuite, ContextLifetimeThreadAndNestedDraws)
{
    const auto& suite = ofxDrawSuite();
    OfxRGBAColourF color{};
    EXPECT_EQ(kOfxStatFailed, suite.getColour(nullptr, kOfxStandardColourOverlayText, &color));
    OverlayDrawList list;
    OfxDrawContextHandle expired;
    {
        OfxDrawSession outer(list, OfxDrawSession::defaultColors(), 1, 1, 1.5);
        expired = outer.handle();
        EXPECT_EQ(kOfxStatOK, suite.getColour(expired, kOfxStandardColourOverlaySelected, &color));
        EXPECT_FLOAT_EQ(0.8f, color.r);
        OfxStatus foreignThread = kOfxStatOK;
        std::thread thread([&] { foreignThread = suite.setLineWidth(expired, 0); });
        thread.join();
        EXPECT_EQ(kOfxStatFailed, foreignThread);
        {
            OfxDrawSession inner(list, OfxDrawSession::defaultColors(), 2, 2, 1);
            EXPECT_NE(expired, inner.handle());
            EXPECT_EQ(kOfxStatOK, suite.setLineWidth(expired, 3));
            EXPECT_EQ(kOfxStatOK, suite.setLineWidth(inner.handle(), 2));
        }
        EXPECT_EQ(kOfxStatOK, suite.setLineWidth(expired, 4));
    }
    EXPECT_EQ(kOfxStatFailed, suite.setLineWidth(expired, 1));
    {
        OfxDrawSession next(list, OfxDrawSession::defaultColors(), 1, 1, 1);
        EXPECT_NE(expired, next.handle());
        EXPECT_EQ(kOfxStatFailed, suite.setLineWidth(expired, 1));
    }
    EXPECT_DOUBLE_EQ(1, list.state().lineWidth);
}

TEST(OfxDrawSuite, InvalidCallsDoNotPoisonFollowingDrawing)
{
    const auto& suite = ofxDrawSuite();
    OverlayDrawList list;
    OfxDrawSession session(list, OfxDrawSession::defaultColors(), 1, 1, 1);
    const auto handle = session.handle();
    const OfxPointD points[] = {{1, 2}, {3, 4}, {5, 6}};
    EXPECT_EQ(kOfxStatErrValue, suite.getColour(handle, static_cast<OfxStandardColour>(99), nullptr));
    EXPECT_EQ(kOfxStatErrValue, suite.setColour(handle, nullptr));
    EXPECT_EQ(kOfxStatErrValue, suite.setLineWidth(handle, -1));
    EXPECT_EQ(kOfxStatErrValue, suite.setLineWidth(handle, std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ(kOfxStatErrValue, suite.setLineStipple(handle, static_cast<OfxDrawLineStipplePattern>(99)));
    EXPECT_EQ(kOfxStatErrValue, suite.draw(handle, kOfxDrawPrimitiveLines, points, 3));
    EXPECT_EQ(kOfxStatErrValue, suite.draw(handle, kOfxDrawPrimitiveLines, nullptr, 2));
    EXPECT_EQ(kOfxStatErrValue, suite.draw(handle, kOfxDrawPrimitiveEllipse, points, 3));
    const OfxPointD invalid[] = {{1, 2}, {std::numeric_limits<double>::quiet_NaN(), 0}};
    EXPECT_EQ(kOfxStatErrValue, suite.draw(handle, kOfxDrawPrimitiveLineStrip, invalid, 2));
    EXPECT_TRUE(list.commands().empty());
    list.begin(OverlayPrimitive::Points);
    EXPECT_EQ(kOfxStatFailed, suite.draw(handle, kOfxDrawPrimitiveLines, points, 2));
    list.cancelPrimitive();
    EXPECT_EQ(kOfxStatOK, suite.draw(handle, kOfxDrawPrimitiveLines, points, 2));
    ASSERT_EQ(1u, list.commands().size());
    EXPECT_EQ(2u, list.commands().front().vertices.size());
}

TEST(OfxDrawSuite, ShapesTransformAlphaAndStateRestoration)
{
    const auto& suite = ofxDrawSuite();
    OverlayDrawList list;
    list.translate(10, 20);
    list.color(0, 0, 1);
    {
        OfxDrawSession session(list, OfxDrawSession::defaultColors(), 0.1, 0.1, 1.5);
        const OfxRGBAColourF red{1, 0, 0, 0.25f};
        EXPECT_EQ(kOfxStatOK, suite.setColour(session.handle(), &red));
        EXPECT_EQ(kOfxStatOK, suite.setLineWidth(session.handle(), 0));
        EXPECT_EQ(kOfxStatOK, suite.setLineStipple(session.handle(), kOfxDrawLineStipplePatternDash));
        const OfxPointD corners[] = {{5, 8}, {1, 2}};
        EXPECT_EQ(kOfxStatOK, suite.draw(session.handle(), kOfxDrawPrimitiveRectangle, corners, 2));
        EXPECT_EQ(kOfxStatOK, suite.draw(session.handle(), kOfxDrawPrimitiveEllipse, corners, 2));
    }
    ASSERT_EQ(2u, list.commands().size());
    const auto& rectangle = list.commands()[0];
    EXPECT_EQ(OverlayPrimitive::Polygon, rectangle.primitive);
    EXPECT_EQ(4u, rectangle.vertices.size());
    EXPECT_DOUBLE_EQ(11, rectangle.vertices[0].x);
    EXPECT_DOUBLE_EQ(22, rectangle.vertices[0].y);
    EXPECT_DOUBLE_EQ(0.25, rectangle.vertices[0].color[3]);
    const auto& ellipse = list.commands()[1];
    EXPECT_EQ(OverlayPrimitive::LineLoop, ellipse.primitive);
    EXPECT_GE(ellipse.vertices.size(), 16u);
    EXPECT_DOUBLE_EQ(15, ellipse.vertices[0].x);
    EXPECT_DOUBLE_EQ(25, ellipse.vertices[0].y);
    EXPECT_DOUBLE_EQ(0, ellipse.lineWidth);
    EXPECT_EQ(0x0f0f, ellipse.stipple);
    EXPECT_DOUBLE_EQ(1, list.state().lineWidth);
    EXPECT_DOUBLE_EQ(1, list.state().color[2]);
}

TEST(OfxDrawSuite, Utf8AndTextAnchors)
{
    const auto& suite = ofxDrawSuite();
    OverlayDrawList list;
    OfxDrawSession session(list, OfxDrawSession::defaultColors(), 1, 1, 1);
    const OfxPointD position{3, 4};
    EXPECT_EQ(kOfxStatOK, suite.drawText(session.handle(), "UTF-8: \xe6\x97\xa5", &position, kOfxDrawTextAlignmentRight | kOfxDrawTextAlignmentBaseline));
    EXPECT_EQ(kOfxStatOK, suite.drawText(session.handle(), "center", &position, kOfxDrawTextAlignmentCenterH | kOfxDrawTextAlignmentCenterV));
    EXPECT_EQ(kOfxStatErrValue, suite.drawText(session.handle(), nullptr, &position, 0));
    EXPECT_EQ(kOfxStatErrValue, suite.drawText(session.handle(), "bad", &position, kOfxDrawTextAlignmentTop | kOfxDrawTextAlignmentBottom));
    ASSERT_EQ(2u, list.commands().size());
    EXPECT_EQ(std::string("UTF-8: \xe6\x97\xa5"), list.commands()[0].text);
    EXPECT_TRUE(list.commands()[0].textFlags & OverlayTextBaseline);
    EXPECT_TRUE(list.commands()[0].textFlags & Qt::AlignRight);
    EXPECT_TRUE(list.commands()[1].textFlags & Qt::AlignVCenter);
    EXPECT_TRUE(list.commands()[1].textFlags & Qt::AlignHCenter);
}

TEST(OfxDrawSuite, NativeGradientBlendAndScopedState)
{
    const auto& suite = ofxNatronDrawSuite();
    OverlayDrawList list;
    OfxDrawContextHandle handle;
    const OfxPointD points[] {{0, 0}, {0, 2}, {3, 2}, {3, 0}};
    const OfxRGBAColourF colors[] {{1, 0, 0, 1}, {0, 0, 1, 1}};
    {
        OfxDrawSession session(list, OfxDrawSession::defaultColors(), .1, .1, 1.5);
        handle = session.handle();
        ASSERT_EQ(kOfxStatOK, suite.setBlendMode(handle, kNatronOfxDrawBlendAdd));
        ASSERT_EQ(kOfxStatOK, suite.setAntialiasing(handle, 0));
        ASSERT_EQ(kOfxStatOK, suite.drawGradientQuad(handle, points, colors));
        ASSERT_EQ(1u, list.commands().size());
        const auto& command = list.commands()[0];
        EXPECT_EQ(OverlayPrimitive::GradientQuad, command.primitive);
        EXPECT_EQ(OverlayBlendMode::Add, command.blendMode);
        EXPECT_FALSE(command.antialiasing);
        ASSERT_EQ(4u, command.vertices.size());
        EXPECT_DOUBLE_EQ(1, command.vertices[1].color[0]);
        EXPECT_DOUBLE_EQ(1, command.vertices[2].color[2]);
        EXPECT_DOUBLE_EQ(3, command.vertices[3].x);
        EXPECT_DOUBLE_EQ(.8f, list.state().color[0]);
    }
    EXPECT_EQ(OverlayBlendMode::Inherit, list.state().blendMode);
    EXPECT_TRUE(list.state().antialiasing);
    EXPECT_EQ(kOfxStatFailed, suite.drawGradientQuad(handle, points, colors));
    EXPECT_EQ(kOfxStatFailed, suite.setBlendMode(handle, kNatronOfxDrawBlendSourceOver));
    EXPECT_EQ(kOfxStatFailed, suite.setAntialiasing(handle, 1));
}

TEST(OfxDrawSuite, NativeGradientRejectsInvalidInputWithoutCommands)
{
    const auto& suite = ofxNatronDrawSuite();
    OverlayDrawList list;
    OfxDrawSession session(list, OfxDrawSession::defaultColors(), 1, 1, 1);
    const auto handle = session.handle();
    OfxPointD points[] {{0, 0}, {0, 2}, {4, 3}, {3, 0}};
    OfxRGBAColourF colors[] {{1, 0, 0, 1}, {0, 0, 1, 1}};
    EXPECT_EQ(kOfxStatErrValue, suite.drawGradientQuad(handle, points, colors));
    points[2] = {3, 2};
    EXPECT_EQ(kOfxStatErrValue, suite.drawGradientQuad(handle, nullptr, colors));
    EXPECT_EQ(kOfxStatErrValue, suite.drawGradientQuad(handle, points, nullptr));
    colors[1].a = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(kOfxStatErrValue, suite.drawGradientQuad(handle, points, colors));
    EXPECT_EQ(kOfxStatErrValue, suite.setBlendMode(handle, static_cast<NatronOfxDrawBlendMode>(20)));
    EXPECT_EQ(kOfxStatErrValue, suite.setAntialiasing(handle, 2));
    EXPECT_TRUE(list.commands().empty());
}
