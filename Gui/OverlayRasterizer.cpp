// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Gui/OverlayRasterizer.h"
#include <algorithm>
#include <cmath>
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <QImage>
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlaySupport.h"

NATRON_NAMESPACE_ENTER
namespace
{
QColor overlayColor(const std::array<double, 4> &rgba)
{
    auto finite = [](double value) {
        return std::isfinite(value) ? std::clamp(value, 0., 1.) : 0.;
    };
    return QColor::fromRgbF(finite(rgba[0]), finite(rgba[1]), finite(rgba[2]), finite(rgba[3]));
}
QPen overlayPen(const OverlayCommand& command, const QColor& color, double devicePixelRatio)
{
    QPen pen(color, command.lineWidth, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin);

    if (command.stipple != 0xffff && command.stipple) {
        unsigned start = 0;

        // Rotate to a rising edge so Qt's first run is an on run.
        for (; start < 16; ++start) {
            if ((command.stipple & (1u << start)) && !(command.stipple & (1u << ((start + 15) % 16)))) {
                break;
            }
        }

        QList<qreal> dashes;
        bool on = true;
        unsigned run = 0;
        const double unit = command.lineWidth == 0 ? command.stippleFactor * devicePixelRatio :
                            command.stippleFactor / command.lineWidth;

        for (unsigned i = 0; i < 16; ++i) {
            const bool bit = (command.stipple & (1u << ((start + i) % 16))) != 0;

            if (bit != on) {
                dashes.push_back(run * unit);
                run = 0;
                on = bit;
            }

            ++run;
        }

        dashes.push_back(run * unit);
        pen.setDashPattern(dashes);
        pen.setDashOffset(((16 - start) % 16) * unit);
    }

    return pen;
}

struct ImageSpan {
    double start, end, sourceStart, sourceEnd;
};

std::vector<ImageSpan> clampedImageSpans(double low, double high, int pixels)
{
    std::vector<double> cuts{0, 1};

    for (double edge : {
             0., 1.
         }) {
        double cut = (edge - low) / (high - low);

        if (cut > 0 && cut < 1) {
            cuts.push_back(cut);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    std::vector<ImageSpan> result;

    for (size_t i = 1; i < cuts.size(); ++i) {
        const double a = cuts[i - 1], b = cuts[i];
        const double u = low + (high - low) * a, v = low + (high - low) * b;

        if (v <= 0) {
            result.push_back({a, b, 0, 1});
        } else if (u >= 1) {
            result.push_back({a, b, double(pixels - 1), double(pixels)});
        } else {
            result.push_back({a, b, std::clamp(u, 0., 1.) * pixels, std::clamp(v, 0., 1.) * pixels});
        }
    }

    return result;
}

void drawOverlayImage(const OverlayCommand& command, const QPolygonF& destination, QPainter& painter)
{
    QTransform placement;
    const QPolygonF unit{QPointF(0, 0), QPointF(1, 0), QPointF(1, 1), QPointF(0, 1)};

    if (!QTransform::quadToQuad(unit, destination, placement)) {
        return;
    }

    const auto& image = *command.image;
    const QImage pixels(image.pixels.data(), image.width, image.height,
                        qsizetype(image.width) * 4, QImage::Format_RGBA8888_Premultiplied);
    const auto x = clampedImageSpans(command.imageUV[0], command.imageUV[2], image.width);
    const auto y = clampedImageSpans(command.imageUV[1], command.imageUV[3], image.height);
    // Native marker previews use linear minification and nearest magnification
    // so enlarged source pixels stay distinct when positioning a track.
    const auto physical = painter.deviceTransform().map(destination);
    auto distance = [](const QPointF & a, const QPointF & b) {
        return std::hypot(a.x() - b.x(), a.y() - b.y());
    };
    const double width = std::max(distance(physical[0], physical[1]), distance(physical[3], physical[2]));
    const double height = std::max(distance(physical[0], physical[3]), distance(physical[1], physical[2]));
    const bool minified = width < (command.imageUV[2] - command.imageUV[0]) * image.width ||
                          height < (command.imageUV[3] - command.imageUV[1]) * image.height;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, minified);
    painter.setTransform(placement, true);
    painter.setClipRect(QRectF(0, 0, 1, 1), Qt::IntersectClip);

    for (const auto& row : y) {
        for (const auto& column : x) {
            painter.drawImage(QRectF(column.start, row.start, column.end - column.start, row.end - row.start), pixels,
                              QRectF(column.sourceStart, row.sourceStart, column.sourceEnd - column.sourceStart,
                                     row.sourceEnd - row.sourceStart));
        }
    }

    painter.restore();
}
}

void rasterizeOverlayCommands(const OverlayDrawList& list, const OverlaySupport& viewport, QPainter& painter)
{
    rasterizeOverlayCommands(list, [&viewport](const QPointF & point) {
        return viewport.toWidgetCoordinates(point);
    }, painter);
}

void rasterizeOverlayCommands(const OverlayDrawList& list,
                              const std::function<QPointF(const QPointF &)> &toWidget, QPainter& painter,
                              bool antialiasing)
{
    painter.save();
    const auto inheritedComposition = painter.compositionMode();
    painter.setRenderHint(QPainter::Antialiasing, antialiasing);
    auto point = [&](const OverlayVertex & vertex) {
        return toWidget(QPointF(vertex.x, vertex.y));
    };

    for (const auto& command : list.commands()) {
        const auto& vertices = command.vertices;

        if (vertices.empty()) {
            continue;
        }

        painter.setBrush(Qt::NoBrush);
        const bool stroke = command.primitive == OverlayPrimitive::Lines || command.primitive == OverlayPrimitive::LineStrip ||
                            command.primitive == OverlayPrimitive::LineLoop;
        painter.setCompositionMode(command.blendMode == OverlayBlendMode::Inherit ? inheritedComposition :
                                   command.blendMode == OverlayBlendMode::Add ? QPainter::CompositionMode_Plus : QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing, antialiasing && command.antialiasing && (!stroke || command.lineWidth != 0));

        if (command.primitive == OverlayPrimitive::Image) {
            if (vertices.size() == 4 && std::all_of(vertices.begin(), vertices.end(), [](const auto & vertex) {
            return vertex.visible;
        })) {
                QPolygonF destination;

                for (const auto& vertex : vertices) {
                    destination.push_back(point(vertex));
                }

                drawOverlayImage(command, destination, painter);
            }
        } else if (command.primitive == OverlayPrimitive::Text) {
            if (!vertices.front().visible) {
                continue;
            }

            painter.setPen(overlayColor(vertices.front().color));
            const QString text = QString::fromUtf8(command.text.data(), int(command.text.size()));
            const QPointF position = point(vertices.front());
            const QRectF bounds(0, 0, painter.fontMetrics().horizontalAdvance(text), painter.fontMetrics().height());
            // Native interact labels historically anchor their lower-left cell
            // corner at the canonical point. Qt paints from the upper-left.
            double x = position.x(), y = position.y() - bounds.height();

            if (command.textFlags & Qt::AlignRight) {
                x -= bounds.width();
            } else if (command.textFlags & Qt::AlignHCenter) {
                x -= bounds.width() / 2;
            }

            if (command.textFlags & Qt::AlignTop) {
                y = position.y();
            } else if (command.textFlags & Qt::AlignVCenter) {
                y = position.y() - bounds.height() / 2;
            } else if (command.textFlags & OverlayTextBaseline) {
                y = position.y() - painter.fontMetrics().ascent();
            }

            painter.drawText(QRectF(x, y, bounds.width() + 1, bounds.height() + 1), Qt::AlignLeft | Qt::AlignTop, text);
        } else if (command.primitive == OverlayPrimitive::Points) {
            painter.setPen(Qt::NoPen);
            const double half = command.pointSize / 2;

            for (const auto& vertex : vertices) {
                if (!vertex.visible) {
                    continue;
                }

                painter.setBrush(overlayColor(vertex.color));
                const QRectF rect(point(vertex) - QPointF(half, half), QSizeF(command.pointSize, command.pointSize));

                if (command.roundPoints) {
                    painter.drawEllipse(rect);
                } else {
                    painter.drawRect(rect);
                }
            }
        } else if (command.primitive == OverlayPrimitive::GradientQuad) {
            if (vertices.size() != 4 || !std::all_of(vertices.begin(), vertices.end(), [](const auto & v) {
            return v.visible;
        })) {
                continue;
            }
            QPolygonF polygon;

            for (const auto& vertex : vertices) {
                polygon.push_back(point(vertex));
            }

            const QPointF edge = polygon[1] - polygon[0];
            const QPointF normal(-edge.y(), edge.x());
            const double length = QPointF::dotProduct(normal, normal);

            if (length == 0) {
                continue;
            }

            const double distance = QPointF::dotProduct(polygon[3] - polygon[0], normal) / length;
            QLinearGradient gradient(polygon[0], polygon[0] + normal * distance);
            gradient.setColorAt(0, overlayColor(vertices[0].color));
            gradient.setColorAt(1, overlayColor(vertices[2].color));
            painter.setPen(Qt::NoPen);
            painter.setBrush(gradient);
            painter.drawPolygon(polygon, Qt::WindingFill);
        } else if (command.primitive == OverlayPrimitive::Polygon) {
            QPolygonF polygon;

            for (const auto& vertex : vertices) {
                if (vertex.visible) {
                    polygon.push_back(point(vertex));
                }
            }

            painter.setPen(Qt::NoPen);
            painter.setBrush(overlayColor(vertices.front().color));
            painter.drawPolygon(polygon, Qt::WindingFill);
        } else {
            if (!command.stipple) {
                continue;
            }

            const bool uniform = std::all_of(vertices.begin(), vertices.end(), [&](const auto & vertex) {
                return vertex.visible && vertex.color == vertices.front().color;
            });
            painter.setPen(overlayPen(command, overlayColor(vertices.front().color), painter.device()->devicePixelRatioF()));

            if (uniform) {
                QPainterPath path;

                if (command.primitive == OverlayPrimitive::Lines) {
                    for (size_t i = 1; i < vertices.size(); i += 2) {
                        path.moveTo(point(vertices[i - 1]));
                        path.lineTo(point(vertices[i]));
                    }
                } else {
                    path.moveTo(point(vertices.front()));

                    for (size_t i = 1; i < vertices.size(); ++i) {
                        path.lineTo(point(vertices[i]));
                    }

                    if (command.primitive == OverlayPrimitive::LineLoop) {
                        path.closeSubpath();
                    }
                }

                painter.drawPath(path);
            } else {
                auto segment = [&](const OverlayVertex & a, const OverlayVertex & b) {
                    if (!a.visible || !b.visible) {
                        return;
                    }

                    const QPointF start = point(a), end = point(b);
                    QLinearGradient gradient(start, end);
                    gradient.setColorAt(0, overlayColor(a.color));
                    gradient.setColorAt(1, overlayColor(b.color));
                    QPen pen = overlayPen(command, overlayColor(a.color), painter.device()->devicePixelRatioF());
                    pen.setBrush(gradient);

                    painter.setPen(pen);
                    painter.drawLine(start, end);
                };
                const size_t step = command.primitive == OverlayPrimitive::Lines ? 2 : 1;

                for (size_t i = 1; i < vertices.size(); i += step) {
                    segment(vertices[i - 1], vertices[i]);
                }

                if (command.primitive == OverlayPrimitive::LineLoop && vertices.size() > 1) {
                    segment(vertices.back(), vertices.front());
                }
            }
        }
    }

    painter.restore();
}
NATRON_NAMESPACE_EXIT
