// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include <gtest/gtest.h>
#include "Engine/OverlayDrawList.h"
#include <stdexcept>

NATRON_NAMESPACE_USING

TEST(OverlayDrawList, RetainedImagePlacementAndValidation)
{
    OverlayDrawList list;
    auto image = std::make_shared<OverlayImage>();
    image->width = image->height = 2;
    image->pixels.resize(16, 255);
    std::weak_ptr<const OverlayImage> lifetime = image;
    list.translate(10, 20);
    list.image(image, 1, 2, 5, 8, -0.2, 0.1, 0.8, 1.1);
    image.reset();
    ASSERT_FALSE(lifetime.expired());
    const auto& command = list.commands().front();
    EXPECT_EQ(OverlayPrimitive::Image, command.primitive);
    ASSERT_EQ(4u, command.vertices.size());
    EXPECT_DOUBLE_EQ(11, command.vertices[0].x);
    EXPECT_DOUBLE_EQ(28, command.vertices[0].y);
    EXPECT_DOUBLE_EQ(15, command.vertices[2].x);
    EXPECT_DOUBLE_EQ(22, command.vertices[2].y);
    EXPECT_DOUBLE_EQ(-0.2, command.imageUV[0]);
    EXPECT_DOUBLE_EQ(1.1, command.imageUV[3]);
    EXPECT_THROW(list.image(command.image, 1, 2, 0, 8), std::invalid_argument);
    auto invalid = std::make_shared<OverlayImage>();
    invalid->width = invalid->height = 2;
    EXPECT_THROW(list.image(invalid, 0, 0, 1, 1), std::invalid_argument);
    list.begin(OverlayPrimitive::Points);
    EXPECT_THROW(list.image(command.image, 0, 0, 1, 1), std::logic_error);
    list.clear();
    EXPECT_TRUE(lifetime.expired());
}

TEST(OverlayDrawList, NodeTransformAndScreenShadow)
{
    OverlayDrawList list;
    list.translate(100, 200);
    list.rotate(90);
    list.translateOutput(1, -1);
    list.begin(OverlayPrimitive::Lines);
    list.vertex(0, 0);
    list.vertex(10, 0);
    list.end();
    const auto& vertices = list.commands().front().vertices;
    EXPECT_DOUBLE_EQ(101, vertices[0].x);
    EXPECT_DOUBLE_EQ(199, vertices[0].y);
    EXPECT_NEAR(101, vertices[1].x, 1e-12);
    EXPECT_NEAR(209, vertices[1].y, 1e-12);
}

TEST(OverlayDrawList, ScopeAndPerVertexColor)
{
    OverlayDrawList list;
    list.color(1, 0, 0, 0.5);
    list.lineWidth(1.5);
    {
        OverlayDrawScope scope(list);
        list.pointSize(8);
        list.stipple(2, 0xaaaa);
        list.pushTransform();
        list.translate(3, 4);
        list.begin(OverlayPrimitive::Points);
        list.vertex(5, 6);
        list.color(0, 1, 0);
        list.vertex(7, 8);
        list.end();
        list.popTransform();
    }
    list.begin(OverlayPrimitive::Points);
    list.vertex(5, 6);
    list.end();
    ASSERT_EQ(2u, list.commands().size());
    const auto& first = list.commands()[0];
    EXPECT_DOUBLE_EQ(8, first.vertices[0].x);
    EXPECT_DOUBLE_EQ(10, first.vertices[0].y);
    EXPECT_DOUBLE_EQ(0.5, first.vertices[0].color[3]);
    EXPECT_DOUBLE_EQ(1, first.vertices[1].color[1]);
    EXPECT_DOUBLE_EQ(8, first.pointSize);
    EXPECT_EQ(0xaaaa, first.stipple);
    const auto& next = list.commands()[1];
    EXPECT_DOUBLE_EQ(5, next.vertices[0].x);
    EXPECT_DOUBLE_EQ(1, next.pointSize);
    EXPECT_DOUBLE_EQ(1.5, next.lineWidth);
    EXPECT_EQ(0xffff, next.stipple);
    EXPECT_DOUBLE_EQ(0.5, next.vertices[0].color[3]);
}

TEST(OverlayDrawList, PerspectiveAndInvalidGeometry)
{
    OverlayDrawList list;
    list.transform(Transform::Matrix3x3(2, 0, 0, 0, 2, 0, 1, 0, 1));
    list.begin(OverlayPrimitive::Points);
    list.vertex(1, 2);
    list.vertex(-1, 2);
    list.vertex(-2, 2);
    list.end();
    const auto& vertices = list.commands()[0].vertices;
    EXPECT_DOUBLE_EQ(1, vertices[0].x);
    EXPECT_DOUBLE_EQ(2, vertices[0].y);
    EXPECT_TRUE(vertices[0].visible);
    EXPECT_FALSE(vertices[1].visible);
    EXPECT_FALSE(vertices[2].visible);
    EXPECT_THROW(list.vertex(0, 0), std::logic_error);
    EXPECT_THROW(list.end(), std::logic_error);
    EXPECT_THROW(list.popTransform(), std::logic_error);
    EXPECT_THROW(list.lineWidth(-1), std::invalid_argument);
    list.begin(OverlayPrimitive::Lines);
    EXPECT_THROW(list.begin(OverlayPrimitive::Points), std::logic_error);
    list.clear();
    EXPECT_TRUE(list.commands().empty());
    list.begin(OverlayPrimitive::Points);
    list.vertex(1, 2);
    list.end();
    EXPECT_DOUBLE_EQ(2, list.commands()[0].vertices[0].y);
}
