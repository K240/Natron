// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_OVERLAYDRAWLIST_H
#define NATRON_ENGINE_OVERLAYDRAWLIST_H
#include <Python.h>
#include "Global/Macros.h"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "Engine/Transform.h"

NATRON_NAMESPACE_ENTER

// Canonical coordinates, straight RGBA, and logical screen-pixel widths.
// This list has no graphics API or GUI dependency. A viewport consumes it only
// during its draw action; interaction never requires a current GPU context.
enum class OverlayPrimitive { Points, Lines, LineStrip, LineLoop, Polygon, Text, Image, GradientQuad };
enum class OverlayBlendMode { Inherit, SourceOver, Add };

// Additional text anchor used by DrawSuite, independent of Qt alignment bits.
constexpr int OverlayTextBaseline = 0x1000000;

// Top-down premultiplied RGBA8, retained until the viewport finishes consuming
// the command. Marker previews are opaque; no GPU context is needed to create
// or destroy their pixels.
struct OverlayImage {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;
};

struct OverlayVertex {
    double x, y;
    bool visible;
    std::array<double, 4> color;
};

struct OverlayCommand {
    OverlayPrimitive primitive;
    std::vector<OverlayVertex> vertices;
    double lineWidth, pointSize;
    bool roundPoints;
    uint16_t stipple;
    unsigned stippleFactor;
    std::string text;
    int textFlags = 0;
    std::shared_ptr<const OverlayImage> image;
    std::array<double, 4> imageUV{0, 0, 1, 1};
    OverlayBlendMode blendMode = OverlayBlendMode::Inherit;
    bool antialiasing = true;
};

class OverlayDrawList
{
public:
    struct State {
        Transform::Matrix3x3 transform{1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::array<double, 4> color{1, 1, 1, 1};
        double lineWidth = 1, pointSize = 1;
        double outputOffsetX = 0, outputOffsetY = 0;
        bool roundPoints = false;
        uint16_t stipple = 0xffff;
        unsigned stippleFactor = 1;
        OverlayBlendMode blendMode = OverlayBlendMode::Inherit;
        bool antialiasing = true;
    };

    void clear();
    void begin(OverlayPrimitive primitive);
    void vertex(double x, double y);
    void end();
    void cancelPrimitive() noexcept;
    void color(double r, double g, double b, double a = 1);
    void color(const double* rgba)
    {
        color(rgba[0], rgba[1], rgba[2], rgba[3]);
    }
    void text(double x, double y, const std::string& text, int flags);
    // Canonical bottom-left/top-right bounds; normalized top-down UV bounds.
    // Sampling outside the image clamps to its edge, including magnification.
    void image(const std::shared_ptr<const OverlayImage> &image,
               double x1, double y1, double x2, double y2,
               double u1 = 0, double v1 = 0, double u2 = 1, double v2 = 1);
    void lineWidth(double width);
    void blendMode(OverlayBlendMode mode)
    {
        _state.blendMode = mode;
    }
    void antialiasing(bool enabled)
    {
        _state.antialiasing = enabled;
    }
    void pointSize(double size);
    void roundPoints(bool round)
    {
        _state.roundPoints = round;
    }
    void stipple(unsigned factor, uint16_t pattern);
    void solidLine()
    {
        _state.stipple = 0xffff;
    }
    void transform(const Transform::Matrix3x3& matrix);
    void translate(double x, double y);
    void rotate(double degrees);
    // A screen-space shadow is applied after the node's canonical transform.
    void translateOutput(double x, double y);
    void pushTransform();
    void popTransform();
    void setTransform(const Transform::Matrix3x3& matrix)
    {
        _state.transform = matrix;
    }
    OverlayVertex map(double x, double y) const;
    State state() const
    {
        return _state;
    }
    void restore(const State& state);
    const std::vector<OverlayCommand> &commands() const
    {
        return _commands;
    }

private:
    State _state;
    OverlayCommand _pending{};
    bool _building = false;
    std::vector<OverlayCommand> _commands;
    std::vector<Transform::Matrix3x3> _transforms;
};

class OverlayDrawScope
{
public:
    explicit OverlayDrawScope(OverlayDrawList& list) : _list(list), _state(list.state()) {}
    ~OverlayDrawScope()
    {
        _list.restore(_state);
    }
    OverlayDrawScope(const OverlayDrawScope &) = delete;
    OverlayDrawScope &operator=(const OverlayDrawScope &) = delete;
private:
    OverlayDrawList &_list;
    OverlayDrawList::State _state;
};

class OverlayTransformScope
{
public:
    explicit OverlayTransformScope(OverlayDrawList& list) : _list(list), _matrix(list.state().transform) {}
    ~OverlayTransformScope()
    {
        _list.setTransform(_matrix);
    }
    OverlayTransformScope(const OverlayTransformScope &) = delete;
    OverlayTransformScope &operator=(const OverlayTransformScope &) = delete;
private:
    OverlayDrawList &_list;
    Transform::Matrix3x3 _matrix;
};

NATRON_NAMESPACE_EXIT
#endif
