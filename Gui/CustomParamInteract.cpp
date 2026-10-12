/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "CustomParamInteract.h"
#include <QByteArray>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QThread>
#include "Gui/KnobGui.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/QtEnumConvert.h"
#include "Gui/OverlayRasterizer.h"
#include "Engine/OfxOverlayInteract.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/Knob.h"
#include "Engine/AppInstance.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

struct CustomParamInteractPrivate {
    KnobGuiWPtr knob;
    OfxParamOverlayInteractPtr entryPoint;
    QSize preferredSize;
    OverlayDrawList commands;
    bool dispatching = false;
    bool hasInteractFocus = false;
    bool focusGainPending = false;
    bool focusLossPending = false;

    CustomParamInteractPrivate(const KnobGuiPtr& knob, void* ofxParamHandle,
                               const OfxParamOverlayInteractPtr& entryPoint)
        : knob(knob), entryPoint(entryPoint)
    {
        assert(entryPoint && entryPoint->usesDrawSuite() && ofxParamHandle);
        assert(reinterpret_cast<OFX::Host::Param::Instance*>(ofxParamHandle)->verifyMagic());
        int w, h;
        entryPoint->getPreferredSize(w, h);
        preferredSize = QSize(w, h);
    }

    double time() const
    {
        return knob.lock()->getKnob()->getHolder()->getApp()->getTimeLine()->currentFrame();
    }
    const OfxRGBAColourD *picker() const
    {
        return entryPoint->hasColorPicker() ? &entryPoint->getLastColorPickerColor() : nullptr;
    }
};

CustomParamInteract::CustomParamInteract(const KnobGuiPtr& knob, void* ofxParamHandle,
        const OfxParamOverlayInteractPtr& entryPoint, QWidget* parent)
    : QWidget(parent), _imp(new CustomParamInteractPrivate(knob, ofxParamHandle, entryPoint))
{
    double w, h;
    entryPoint->getMinimumSize(w, h);
    setMinimumSize(int(w), int(h));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setObjectName(QString::fromUtf8(knob->getKnob()->getName().c_str()) + QString::fromUtf8("/customInteract"));
    setProperty("graphicsBackend", QString::fromUtf8("Raster"));
    setProperty("graphicsRole", QString::fromUtf8("OfxCustomParamInteract"));
    entryPoint->setCallingViewport(this);
}

CustomParamInteract::~CustomParamInteract()
{
    if (_imp->entryPoint->getLastCallingViewport() == this) {
        auto knob = _imp->knob.lock();
        _imp->entryPoint->setCallingViewport(knob ? knob->getKnob().get() : nullptr);
    }
}

bool
CustomParamInteract::canDispatchInteract() const
{
    auto knob = _imp->knob.lock();
    Gui* gui = knob ? knob->getGui() : nullptr;
    return !_imp->dispatching && gui && gui->getApp() &&
           !gui->getApp()->isClosing() && !gui->getApp()->isShowingDialog();
}

void
CustomParamInteract::flushFocusNotifications()
{
    if (!canDispatchInteract()) {
        return;
    }

    const OfxPointD scale{1., 1.};
    QScopedValueRollback<bool> dispatch(_imp->dispatching, true);

    if (_imp->focusLossPending) {
        _imp->focusLossPending = false;
        _imp->hasInteractFocus = false;
        _imp->entryPoint->loseFocusAction(_imp->time(), scale, 0, _imp->picker());
        update();
    }

    if (_imp->focusGainPending) {
        _imp->focusGainPending = false;

        if (!_imp->hasInteractFocus && hasFocus()) {
            _imp->hasInteractFocus = true;
            _imp->entryPoint->gainFocusAction(_imp->time(), scale, 0, _imp->picker());
            update();
        }
    }
}

void
CustomParamInteract::paintEvent(QPaintEvent*)
{
    assert(qApp && qApp->thread() == QThread::currentThread());
    flushFocusNotifications();

    if (canDispatchInteract()) {
        _imp->commands.clear();
        QScopedValueRollback<bool> dispatch(_imp->dispatching, true);
        const OfxPointD scale{1., 1.};
        const OfxStatus status = _imp->entryPoint->drawAction(_imp->time(), scale, 0, _imp->picker());
        setProperty("customInteractDrawStatus", status);
    }

    flushFocusNotifications();

    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);
    rasterizeOverlayCommands(_imp->commands, *this, painter);
    setProperty("customInteractCommandCount", int(_imp->commands.commands().size()));
    setProperty("customInteractViewport", QRectF(-.5, -.5, width(), height()));
}

void
CustomParamInteract::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    _imp->entryPoint->setSize(e->size().width(), e->size().height());
    update();
}

QSize CustomParamInteract::sizeHint() const
{
    return _imp->preferredSize;
}
OverlayDrawList *CustomParamInteract::overlayDrawList()
{
    return &_imp->commands;
}
void CustomParamInteract::swapOpenGLBuffers()
{
    update();
}
void CustomParamInteract::redraw()
{
    update();
}
void CustomParamInteract::saveOpenGLContext() {}
void CustomParamInteract::restoreOpenGLContext() {}

void
CustomParamInteract::getViewportSize(double &w, double &h) const
{
    w = width();
    h = height();
}

void
CustomParamInteract::getPixelScale(double &x, double &y) const
{
    x = y = 1.;
}

#ifdef OFX_EXTENSIONS_NATRON
double CustomParamInteract::getScreenPixelRatio() const
{
    return devicePixelRatioF();
}
#endif

void
CustomParamInteract::getBackgroundColour(double &r, double &g, double &b) const
{
    r = g = b = 0.;
}

void
CustomParamInteract::toWidgetCoordinates(double* x, double* y) const
{
    Q_UNUSED(x);
    *y = height() - 1 - *y;
}

void CustomParamInteract::toCanonicalCoordinates(double* x, double* y) const
{
    toWidgetCoordinates(x, y);
}

int CustomParamInteract::getWidgetFontHeight() const
{
    return fontMetrics().height();
}
int CustomParamInteract::getStringWidthForCurrentFont(const std::string& text) const
{
    return fontMetrics().horizontalAdvance(QString::fromUtf8(text.c_str()));
}

RectD
CustomParamInteract::getViewportRect() const
{
    RectD rect;
    rect.x1 = rect.y1 = -.5;
    rect.x2 = width() - .5;
    rect.y2 = height() - .5;
    return rect;
}

void
CustomParamInteract::getCursorPosition(double &x, double &y) const
{
    const QPoint p = mapFromGlobal(QCursor::pos());
    x = p.x();
    y = p.y();
    toCanonicalCoordinates(&x, &y);
}

void
CustomParamInteract::dispatchPen(QMouseEvent* e, int action)
{
    if (!canDispatchInteract()) {
        e->ignore();
        return;
    }

    flushFocusNotifications();
    OfxStatus status;
    {
        QScopedValueRollback<bool> dispatch(_imp->dispatching, true);
        const OfxPointD scale{1., 1.};
        const OfxPointD pos{e->position().x(), height() - 1 - e->position().y()};
        const OfxPointI viewportPos{int(pos.x), int(pos.y)};

        if (action == 0) {
            status = _imp->entryPoint->penDownAction(_imp->time(), scale, 0, _imp->picker(), pos, viewportPos, 1.);
        } else if (action == 1) {
            status = _imp->entryPoint->penMotionAction(_imp->time(), scale, 0, _imp->picker(), pos, viewportPos, 1.);
        } else {
            status = _imp->entryPoint->penUpAction(_imp->time(), scale, 0, _imp->picker(), pos, viewportPos, 1.);
        }
    }
    flushFocusNotifications();

    if (status == kOfxStatOK) {
        e->accept();
        update();
    } else {
        e->ignore();
    }
}

void
CustomParamInteract::mousePressEvent(QMouseEvent* e)
{
    if (canDispatchInteract()) {
        setFocus(Qt::MouseFocusReason);
    }

    dispatchPen(e, 0);
}
void CustomParamInteract::mouseMoveEvent(QMouseEvent* e)
{
    dispatchPen(e, 1);
}
void CustomParamInteract::mouseReleaseEvent(QMouseEvent* e)
{
    dispatchPen(e, 2);
}

void
CustomParamInteract::focusInEvent(QFocusEvent* e)
{
    _imp->focusGainPending = true;
    flushFocusNotifications();
    QWidget::focusInEvent(e);
}

void
CustomParamInteract::focusOutEvent(QFocusEvent* e)
{
    _imp->focusLossPending |= _imp->hasInteractFocus;
    _imp->focusGainPending = false;
    flushFocusNotifications();
    QWidget::focusOutEvent(e);
}

void
CustomParamInteract::dispatchKey(QKeyEvent* e, bool release)
{
    if (!canDispatchInteract()) {
        e->ignore();
        return;
    }

    flushFocusNotifications();
    OfxStatus status;
    {
        QScopedValueRollback<bool> dispatch(_imp->dispatching, true);
        const OfxPointD scale{1., 1.};
        QByteArray text = e->text().toUtf8();
        const int key = int(QtEnumConvert::fromQtKey(Qt::Key(e->key())));

        if (release) {
            status = _imp->entryPoint->keyUpAction(_imp->time(), scale, 0, _imp->picker(), key, text.data());
        } else if (e->isAutoRepeat()) {
            status = _imp->entryPoint->keyRepeatAction(_imp->time(), scale, 0, _imp->picker(), key, text.data());
        } else {
            status = _imp->entryPoint->keyDownAction(_imp->time(), scale, 0, _imp->picker(), key, text.data());
        }
    }
    flushFocusNotifications();

    if (status == kOfxStatOK) {
        e->accept();
        update();
    } else {
        e->ignore();
    }
}
void CustomParamInteract::keyPressEvent(QKeyEvent* e)
{
    dispatchKey(e, false);
}
void CustomParamInteract::keyReleaseEvent(QKeyEvent* e)
{
    dispatchKey(e, true);
}

NATRON_NAMESPACE_EXIT
