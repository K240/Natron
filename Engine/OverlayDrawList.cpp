// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/OverlayDrawList.h"
#include <cmath>
#include <stdexcept>
#include <utility>

NATRON_NAMESPACE_ENTER

void OverlayDrawList::clear()
{
    _commands.clear();
    _transforms.clear();
    _pending = {};
    _state = {};
    _building = false;
}

void OverlayDrawList::begin(OverlayPrimitive primitive)
{
    if (_building) {
        throw std::logic_error("Nested overlay primitive.");
    }

    _pending = {primitive, {}, _state.lineWidth, _state.pointSize,
                _state.roundPoints, _state.stipple, _state.stippleFactor
               };
    _pending.blendMode = _state.blendMode;
    _pending.antialiasing = _state.antialiasing;
    _building = true;
}

OverlayVertex OverlayDrawList::map(double x, double y) const
{
    const auto p = Transform::matApply(_state.transform, Transform::Point3D(x, y, 1));
    const bool visible = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && p.z > 0;
    return {visible ? p.x / p.z + _state.outputOffsetX : 0,
            visible ? p.y / p.z + _state.outputOffsetY : 0, visible, _state.color};
}

void OverlayDrawList::vertex(double x, double y)
{
    if (!_building) {
        throw std::logic_error("Overlay vertex outside a primitive.");
    }

    _pending.vertices.push_back(map(x, y));
}

void OverlayDrawList::end()
{
    if (!_building) {
        throw std::logic_error("Overlay end without a primitive.");
    }

    if (!_pending.vertices.empty()) {
        _commands.push_back(std::move(_pending));
    }

    _pending = {};
    _building = false;
}

void OverlayDrawList::color(double r, double g, double b, double a)
{
    _state.color = {r, g, b, a};
}
void OverlayDrawList::cancelPrimitive() noexcept
{
    _pending = {};
    _building = false;
}
void OverlayDrawList::text(double x, double y, const std::string& text, int flags)
{
    if (_building) {
        throw std::logic_error("Overlay text inside a primitive.");
    }

    OverlayCommand command{OverlayPrimitive::Text, {map(x, y)}, 1, 1, false, 0xffff, 1};
    command.text = text;
    command.textFlags = flags;
    command.blendMode = _state.blendMode;
    command.antialiasing = _state.antialiasing;
    _commands.push_back(std::move(command));
}
void OverlayDrawList::image(const std::shared_ptr<const OverlayImage> &image,
                            double x1, double y1, double x2, double y2,
                            double u1, double v1, double u2, double v2)
{
    if (_building) {
        throw std::logic_error("Overlay image inside a primitive.");
    }

    if (!image || image->width <= 0 || image->height <= 0 ||
        uint64_t(image->width) * uint64_t(image->height) * 4 != image->pixels.size() ||
        !std::isfinite(x1) || !std::isfinite(y1) || !std::isfinite(x2) || !std::isfinite(y2) ||
        !std::isfinite(u1) || !std::isfinite(v1) || !std::isfinite(u2) || !std::isfinite(v2) ||
        x2 <= x1 || y2 <= y1 || u2 <= u1 || v2 <= v1) {
        throw std::invalid_argument("Invalid overlay image or rectangle.");
    }

    OverlayCommand command{OverlayPrimitive::Image,
        {map(x1, y2), map(x2, y2), map(x2, y1), map(x1, y1)},
        1, 1, false, 0xffff, 1};
    command.image = image;
    command.imageUV = {u1, v1, u2, v2};
    command.blendMode = _state.blendMode;
    command.antialiasing = _state.antialiasing;
    _commands.push_back(std::move(command));
}

void OverlayDrawList::lineWidth(double width)
{
    if (!std::isfinite(width) || width < 0) {
        throw std::invalid_argument("Invalid overlay line width.");
    }

    _state.lineWidth = width;
}
void OverlayDrawList::pointSize(double size)
{
    if (!std::isfinite(size) || size <= 0) {
        throw std::invalid_argument("Invalid overlay point size.");
    }

    _state.pointSize = size;
}
void OverlayDrawList::stipple(unsigned factor, uint16_t pattern)
{
    _state.stippleFactor = factor ? factor : 1;
    _state.stipple = pattern;
}
void OverlayDrawList::transform(const Transform::Matrix3x3& matrix)
{
    _state.transform = Transform::matMul(_state.transform, matrix);
}
void OverlayDrawList::translate(double x, double y)
{
    transform(Transform::Matrix3x3(1, 0, x, 0, 1, y, 0, 0, 1));
}
void OverlayDrawList::rotate(double degrees)
{
    const double radians = degrees * 3.14159265358979323846 / 180;
    const double c = std::cos(radians), s = std::sin(radians);
    transform(Transform::Matrix3x3(c, -s, 0, s, c, 0, 0, 0, 1));
}
void OverlayDrawList::translateOutput(double x, double y)
{
    _state.outputOffsetX += x;
    _state.outputOffsetY += y;
}
void OverlayDrawList::restore(const State& state)
{
    _state = state;
}

void OverlayDrawList::pushTransform()
{
    _transforms.push_back(_state.transform);
}
void OverlayDrawList::popTransform()
{
    if (_transforms.empty()) {
        throw std::logic_error("Overlay transform stack underflow.");
    }

    _state.transform = _transforms.back();
    _transforms.pop_back();
}

NATRON_NAMESPACE_EXIT
