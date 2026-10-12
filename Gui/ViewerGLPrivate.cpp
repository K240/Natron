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

#include "ViewerGLPrivate.h"

#include <cassert>
#include <algorithm> // min, max
#include <cmath> // sin, cos
#include <cstring> // for std::memcpy
#include <stdexcept>

#include <QApplication> // qApp


#include "Engine/Lut.h" // Color
#include "Engine/Settings.h"

#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h" // appFont
#include "Gui/Menu.h"
#include "Gui/ViewerTab.h"


#ifndef M_PI
#define M_PI        3.14159265358979323846264338327950288   /* pi             */
#endif
#ifndef M_PI_2
#define M_PI_2      1.57079632679489661923132169163975144   /* pi/2           */
#endif
#ifndef M_PI_4
#define M_PI_4      0.785398163397448309615660845819875721  /* pi/4           */
#endif



NATRON_NAMESPACE_ENTER

// Interaction state shared by the native Vulkan Viewer.
ViewerGL::Implementation::Implementation(ViewerGL* this_,
                                         ViewerTab* parent)
    : _this(this_)
    , displayTextures()
    , partialUpdateTextures()
    , infoViewer()
    , viewerTab(parent)
    , zoomOrPannedSinceLastFit(false)
    , oldClick()
    , displayingImageLut(eViewerColorSpaceSRGB)
    , ms(eMouseStateUndefined)
    , hs(eHoverStateNothing)
    , textRenderingColor(200, 200, 200, 255)
    , displayWindowOverlayColor(125, 125, 125, 255)
    , rodOverlayColor(100, 100, 100, 255)
    , _textFont()
    , overlay(true)
    , updatingTexture(false)
    , clearColor(0, 0, 0, 255)
    , menu( new Menu(_this) )
    , persistentMessages()
    , persistentMessageType(0)
    , displayPersistentMessage(false)
    , lastMousePosition()
    , lastDragStartPos()
    , hasMovedSincePress(false)
    , setColorPickerCursor(false)
    , currentViewerInfo_btmLeftBBOXoverlay()
    , currentViewerInfo_topRightBBOXoverlay()
    , currentViewerInfo_resolutionOverlay()
    , pickerState(ePickerStateInactive)
    , lastPickerPos()
    , userRoIEnabled(false)   // protected by mutex
    , userRoI()   // protected by mutex
    , buildUserRoIOnNextPress(false)
    , draggedUserRoI()
    , zoomCtx(0.01, 1024.)   // protected by mutex
    , clipToDisplayWindow(true)   // protected by mutex
    , wipeControlsMutex()
    , mixAmount(1.)   // protected by mutex
    , wipeAngle(M_PI_2)   // protected by mutex
    , wipeCenter()
    , wipeInitialized(false)
    , selectionRectangle()
    , lastRenderedImageMutex()
    , sizeH()
    , pointerTypeOnPress(ePenTypeLMB)
    , pressureOnPress(1.)
    , pressureOnRelease(1.)
    , wheelDeltaSeekFrame(0)
    , isUpdatingTexture(false)
    , renderOnPenUp(false)
{
    _textFont.reset(new QFont(appFont, appFontSize));
    infoViewer[0] = 0;
    infoViewer[1] = 0;

    assert( qApp && qApp->thread() == QThread::currentThread() );
    //menu->setFont( QFont(appFont,appFontSize) );

    //        QDesktopWidget* desktop = QApplication::desktop();
    //        QRect r = desktop->screenGeometry();
    //        sizeH = r.size();
    sizeH.setWidth(10000);
    sizeH.setHeight(10000);
}

ViewerGL::Implementation::~Implementation()
{
    // Tear down the native surface while its instance and callback owner live.
    if (canvas) {
        canvas->setFrameRenderer({});
        canvas->removeEventFilter(_this);
    }
    if (windowContainer) {
        windowContainer->removeEventFilter(_this);
        delete windowContainer;
    }
    canvas = nullptr;
    windowContainer = nullptr;
}
bool
ViewerGL::Implementation::isNearbyWipeCenter(const QPointF & pos,
                                             double zoomScreenPixelWidth,
                                             double zoomScreenPixelHeight) const
{
    double toleranceX = zoomScreenPixelWidth * 8.;
    double toleranceY = zoomScreenPixelHeight * 8.;
    QMutexLocker l(&wipeControlsMutex);

    if ( ( pos.x() >= (wipeCenter.x() - toleranceX) ) && ( pos.x() <= (wipeCenter.x() + toleranceX) ) &&
         ( pos.y() >= (wipeCenter.y() - toleranceY) ) && ( pos.y() <= (wipeCenter.y() + toleranceY) ) ) {
        return true;
    }

    return false;
}

bool
ViewerGL::Implementation::isNearbyWipeRotateBar(const QPointF & pos,
                                                double zoomScreenPixelWidth,
                                                double zoomScreenPixelHeight) const
{
    double toleranceX = zoomScreenPixelWidth * 8.;
    double toleranceY = zoomScreenPixelHeight * 8.;
    double rotateX, rotateY, rotateOffsetX, rotateOffsetY;

    rotateX = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelWidth;
    rotateY = WIPE_ROTATE_HANDLE_LENGTH * zoomScreenPixelHeight;
    rotateOffsetX = WIPE_ROTATE_OFFSET * zoomScreenPixelWidth;
    rotateOffsetY = WIPE_ROTATE_OFFSET * zoomScreenPixelHeight;

    QMutexLocker l(&wipeControlsMutex);
    QPointF outterPoint;

    outterPoint.setX( wipeCenter.x() + std::cos(wipeAngle) * (rotateX - rotateOffsetX) );
    outterPoint.setY( wipeCenter.y() + std::sin(wipeAngle) * (rotateY - rotateOffsetY) );
    if ( ( ( ( pos.y() >= (wipeCenter.y() - toleranceY) ) && ( pos.y() <= (outterPoint.y() + toleranceY) ) ) ||
           ( ( pos.y() >= (outterPoint.y() - toleranceY) ) && ( pos.y() <= (wipeCenter.y() + toleranceY) ) ) ) &&
         ( ( ( pos.x() >= (wipeCenter.x() - toleranceX) ) && ( pos.x() <= (outterPoint.x() + toleranceX) ) ) ||
           ( ( pos.x() >= (outterPoint.x() - toleranceX) ) && ( pos.x() <= (wipeCenter.x() + toleranceX) ) ) ) ) {
        Point a;
        a.x = ( outterPoint.x() - wipeCenter.x() );
        a.y = ( outterPoint.y() - wipeCenter.y() );
        double norm = sqrt(a.x * a.x + a.y * a.y);

        ///The point is in the bounding box of the segment, if it is vertical it must be on the segment anyway
        if (norm == 0) {
            return false;
        }

        a.x /= norm;
        a.y /= norm;
        Point b;
        b.x = ( pos.x() - wipeCenter.x() );
        b.y = ( pos.y() - wipeCenter.y() );
        norm = sqrt(b.x * b.x + b.y * b.y);

        ///This vector is not vertical
        if (norm != 0) {
            b.x /= norm;
            b.y /= norm;

            double crossProduct = b.y * a.x - b.x * a.y;
            if (std::abs(crossProduct) <  0.1) {
                return true;
            }
        }
    }

    return false;
}

bool
ViewerGL::Implementation::isNearbyWipeMixHandle(const QPointF & pos,
                                                double zoomScreenPixelWidth,
                                                double zoomScreenPixelHeight) const
{
    double toleranceX = zoomScreenPixelWidth * 8.;
    double toleranceY = zoomScreenPixelHeight * 8.;
    QMutexLocker l(&wipeControlsMutex);
    ///mix 1 is at rotation bar + pi / 8
    ///mix 0 is at rotation bar + 3pi / 8
    double alphaMix1, alphaMix0, alphaCurMix;

    alphaMix1 = wipeAngle + M_PI_4 / 2;
    alphaMix0 = wipeAngle + 3 * M_PI_4 / 2;
    alphaCurMix = mixAmount * (alphaMix1 - alphaMix0) + alphaMix0;
    QPointF mixPos;
    double mixX = WIPE_MIX_HANDLE_LENGTH * zoomScreenPixelWidth;
    double mixY = WIPE_MIX_HANDLE_LENGTH * zoomScreenPixelHeight;

    mixPos.setX(wipeCenter.x() + std::cos(alphaCurMix) * mixX);
    mixPos.setY(wipeCenter.y() + std::sin(alphaCurMix) * mixY);
    if ( ( pos.x() >= (mixPos.x() - toleranceX) ) && ( pos.x() <= (mixPos.x() + toleranceX) ) &&
         ( pos.y() >= (mixPos.y() - toleranceY) ) && ( pos.y() <= (mixPos.y() + toleranceY) ) ) {
        return true;
    }

    return false;
}

void
ViewerGL::Implementation::refreshSelectionRectangle(const QPointF & pos)
{
    double xmin = std::min( pos.x(), lastDragStartPos.x() );
    double xmax = std::max( pos.x(), lastDragStartPos.x() );
    double ymin = std::min( pos.y(), lastDragStartPos.y() );
    double ymax = std::max( pos.y(), lastDragStartPos.y() );

    selectionRectangle.setRect(xmin, ymin, xmax - xmin, ymax - ymin);
}

NATRON_NAMESPACE_EXIT
