// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GUI_OVERLAYRASTERIZER_H
#define NATRON_GUI_OVERLAYRASTERIZER_H
#include <Python.h>
#include "Global/Macros.h"
#include <QPointF>
#include <functional>
class QPainter;
NATRON_NAMESPACE_ENTER
class OverlayDrawList;
class OverlaySupport;
void rasterizeOverlayCommands(const OverlayDrawList& commands, const OverlaySupport& viewport, QPainter& painter);
void rasterizeOverlayCommands(const OverlayDrawList& commands,
                              const std::function<QPointF(const QPointF &)> &toWidget, QPainter& painter,
                              bool antialiasing = true);
NATRON_NAMESPACE_EXIT
#endif
