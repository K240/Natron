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

#include "ViewerGL.h"
#include "ViewerGLPrivate.h"

#include <cassert>
#include <algorithm> // min, max
#include <limits>
#include <cstring> // for std::memcpy, std::memset, std::strcmp, std::strchr
#include <stdexcept>


#include <QMenu>
#include <QToolButton>
#include <QApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QWindow>
#include <QPainter>
#include <QResizeEvent>
#include <QPointer>

GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QKeyEvent>
#include <QMouseEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QTreeWidget>
#include <QTabBar>


#include "Engine/Lut.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/Node.h"
#include "Engine/NodeGuiI.h"
#include "Engine/Project.h"
#include "Engine/OfxOverlayInteract.h"
#include "Engine/KnobTypes.h"
#include "Engine/Settings.h"
#include "Engine/Timer.h" // for gettimeofday
#include "Engine/Utils.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include "Gui/ActionShortcuts.h" // kShortcutGroupViewer ...
#include "Gui/CurveWidget.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h" // isMouseShortcut
#include "Gui/GuiDefines.h"
#include "Gui/GuiMacros.h" // buttonDownIsLeft...
#include "Gui/Histogram.h"
#include "Gui/InfoViewerWidget.h"
#include "Gui/Menu.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeGui.h"
#include "Gui/KnobGuiParametric.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/TabWidget.h"
#include "Gui/ViewerTab.h"



#define USER_ROI_BORDER_TICK_SIZE 15.f
#define USER_ROI_CROSS_RADIUS 15.f
#define USER_ROI_SELECTION_POINT_SIZE 8.f
#define USER_ROI_CLICK_TOLERANCE 8.f

#define PERSISTENT_MESSAGE_LEFT_OFFSET_PIXELS 20

#ifndef M_PI
#define M_PI        3.14159265358979323846264338327950288   /* pi             */
#endif
#ifndef M_PI_2
#define M_PI_2      1.57079632679489661923132169163975144   /* pi/2           */
#endif
#ifndef M_PI_4
#define M_PI_4      0.785398163397448309615660845819875721  /* pi/4           */
#endif

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif


// Viewer input handling and engine-facing image metadata.

NATRON_NAMESPACE_ENTER


ViewerGL::ViewerGL(ViewerTab* parent,
                   const QWidget* shareWidget)
    : QWidget(parent)
    , _imp(new Implementation(this, parent))
{
    Q_UNUSED(shareWidget);
    assert(qApp && qApp->thread() == QThread::currentThread());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    initializeVulkan();
    populateMenu();
    QObject::connect(appPTR, SIGNAL(checkerboardSettingsChanged()), this, SLOT(onCheckerboardSettingsChanged()));
}

ViewerGL::~ViewerGL()
{
    _imp->nativeImageTransferAvailable.store(false);
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
}

QSize
ViewerGL::sizeHint() const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    return _imp->sizeH;
}

const QFont &
ViewerGL::textFont() const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    return *_imp->_textFont;
}

/**
 *@returns Returns true if the viewer is displaying something.
 **/
bool
ViewerGL::displayingImage() const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    return _imp->displayTextures[0].isVisible || _imp->displayTextures[1].isVisible;
}

void
ViewerGL::resizeViewport(int w,
                         int h)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if ((h == 0) || (w == 0)) {   // prevent division by 0
        return;
    }

    double zoomWidth = w;
    double zoomHeight = h;

    assert(zoomWidth == width() && zoomHeight == height());
    bool zoomSinceLastFit;
    double oldWidth, oldHeight;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        oldWidth = _imp->zoomCtx.screenWidth();
        oldHeight = _imp->zoomCtx.screenHeight();
        _imp->zoomCtx.setScreenSize(zoomWidth, zoomHeight, /*alignTop=*/ true, /*alignRight=*/ false);
        zoomSinceLastFit = _imp->zoomOrPannedSinceLastFit;
    }
    _imp->ms = eMouseStateUndefined;
    assert(_imp->viewerTab);
    ViewerInstance* viewer = _imp->viewerTab->getInternalNode();
    assert(viewer);

    bool isLoadingProject = _imp->viewerTab->getGui() &&
                            _imp->viewerTab->getGui()->getApp()->getProject()->isLoadingProject();

    if (!zoomSinceLastFit && !isLoadingProject) {
        fitImageToFormat();
    }

    if (viewer->getUiContext() && !isLoadingProject  &&
        ((oldWidth != zoomWidth) || (oldHeight != zoomHeight))) {
        viewer->renderCurrentFrame(true);

        if (!_imp->persistentMessages.empty()) {
            updatePersistentMessage();
        } else {
            update();
        }
    }
}


void
ViewerGL::clearColorBuffer(double r,
                           double g,
                           double b,
                           double a)
{
    _imp->clearColor.setRgbF(r, g, b, a);
    update();
}

void
ViewerGL::toggleOverlays()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->overlay = !_imp->overlay;
    update();
}

void
ViewerGL::toggleWipe()
{
    ViewerCompositingOperatorEnum compOperator = getViewerTab()->getCompositingOperator();

    switch (compOperator) {
    case eViewerCompositingOperatorNone: {
        ViewerCompositingOperatorEnum newOp = getViewerTab()->getCompositingOperatorPrevious();

        if (newOp == eViewerCompositingOperatorNone) {
            newOp = eViewerCompositingOperatorWipeUnder;
        }

        getViewerTab()->setCompositingOperator(newOp);
        break;
    }

    default: {
        getViewerTab()->setCompositingOperator(eViewerCompositingOperatorNone);
        break;
    }
    }
}

void
ViewerGL::centerWipe()
{
    QPoint pos = mapFromGlobal(QCursor::pos());
    int x = pos.x();
    int y = pos.y();

    QPointF zoomPos = toZoomCoordinates(QPointF(x, y));

    {
        QMutexLocker l(&_imp->wipeControlsMutex);
        _imp->wipeCenter.rx() = zoomPos.x();
        _imp->wipeCenter.ry() = zoomPos.y();
    }

    update();
}






NATRON_NAMESPACE_ANONYMOUS_ENTER

static QStringList
explode(const QString& str)
{
    QRegularExpression rx(QString::fromUtf8("[ ./\\t\\n-]")); //RegEx for ' ' '/' '.' '-' '\t' '\n'
    QStringList ret;
    int startIndex = 0;

    while (true) {
        int index = rx.match(str, startIndex).capturedStart();

        if (index == -1) {
            ret.push_back(str.mid(startIndex));

            return ret;
        }

        QString word = str.mid(startIndex, index - startIndex);
        const QChar& nextChar = str[index];

        // Dashes and the likes should stick to the word occurring before it. Whitespace doesn't have to.
        if (nextChar.isSpace()) {
            ret.push_back(word);
            ret.push_back(nextChar);
        } else {
            ret.push_back(word + nextChar);
        }

        startIndex = index + 1;
    }

    return ret;
}

static QStringList
wordWrap(const QFontMetrics& fm,
         const QString& str,
         int width)
{
    QStringList words = explode(str);
    int curLineLength = 0;
    QStringList stringL;
    QString curString;

    for (int i = 0; i < words.size(); ++i) {
        QString word = words[i];
        int wordPixels = fm.horizontalAdvance(word);

        // If adding the new word to the current line would be too long,
        // then put it on a new line (and split it up if it's too long).
        if (curLineLength + wordPixels > width) {
            // Only move down to a new line if we have text on the current line.
            // Avoids situation where wrapped whitespace causes emptylines in text.
            if (curLineLength > 0) {
                if (!curString.isEmpty()) {
                    stringL.push_back(curString);
                    curString.clear();
                }

                //tmp.append('\n');
                curLineLength = 0;
            }

            // If the current word is too long to fit on a line even on it's own then
            // split the word up.
            while (wordPixels > width) {
                curString.clear();
                curString.append(word.mid(0, width - 1));
                word = word.mid(width - 1);

                if (!curString.isEmpty()) {
                    stringL.push_back(curString);
                    curString.clear();
                }

                wordPixels = fm.horizontalAdvance(word);
                //tmp.append('\n');
            }

            // Remove leading whitespace from the word so the new line starts flush to the left.
            word = word.trimmed();
        }

        curString.append(word);
        curLineLength += wordPixels;
    }

    if (!curString.isEmpty()) {
        stringL.push_back(curString);
    }

    return stringL;
} // wordWrap

NATRON_NAMESPACE_ANONYMOUS_EXIT





ViewerInstance *
ViewerGL::getInternalViewerNode() const
{
    return getInternalNode();
}

/**
 *@returns Returns the current zoom factor that is applied to the display.
 **/
double
ViewerGL::getZoomFactor() const
{
    // MT-SAFE
    QMutexLocker l(&_imp->zoomCtxMutex);

    return _imp->zoomCtx.factor();
}

///imageRoD is in PIXEL COORDINATES
RectI
ViewerGL::getImageRectangleDisplayed(const RectI & imageRoDPixel, // in pixel coordinates
                                     const double par,
                                     unsigned int mipmapLevel)
{
    // MT-SAFE
    RectD visibleArea;
    RectI ret;

    if (getViewerTab()->isFullFrameProcessingEnabled()) {
        ret = imageRoDPixel;
    } else {
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            QPointF topLeft =  _imp->zoomCtx.toZoomCoordinates(0, 0);
            visibleArea.x1 =  topLeft.x();
            visibleArea.y2 =  topLeft.y();
            // RectI uses exclusive upper bounds. Cover the outer pixel edges
            // so an exactly fitted image includes the last column and row.
            QPointF bottomRight = _imp->zoomCtx.toZoomCoordinates(width(), height());
            visibleArea.x2 = bottomRight.x();
            visibleArea.y1 = bottomRight.y();
        }

        if (mipmapLevel != 0) {
            // for the viewer, we need the smallest enclosing rectangle at the mipmap level, in order to avoid black borders
            ret = visibleArea.toPixelEnclosing(mipmapLevel, par);
        } else {
            ret.x1 = std::floor(visibleArea.x1 / par);
            ret.x2 = std::ceil(visibleArea.x2 / par);
            ret.y1 = std::floor(visibleArea.y1);
            ret.y2 = std::ceil(visibleArea.y2);
        }

        /// Clip the roi to the image's Region of Definition or return an empty rectangle if they don't intersect.
        ret = ret.intersect(imageRoDPixel);
    }

    ///to clip against the user roi however clip it against the mipmaplevel of the zoomFactor+proxy

    RectD userRoI;
    bool userRoiEnabled;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoiEnabled = _imp->userRoIEnabled;
        userRoI = _imp->userRoI;
    }

    if (userRoiEnabled) {
        ///If the user roi is enabled, we want to render the smallest enclosing rectangle in order to avoid black borders.
        const RectI userRoIpixel = userRoI.toPixelEnclosing(mipmapLevel, par);

        ///If the user roi doesn't intersect the actually visible portion on the viewer, return an empty rectangle.
        ret = ret.intersect(userRoIpixel);
    }

    return ret;
} // ViewerGL::getImageRectangleDisplayed

RectI
ViewerGL::getExactImageRectangleDisplayed(int texIndex,
        const RectD & rod,
        const double par,
        unsigned int mipmapLevel)
{
    bool clipToFormat = isClippingImageToFormat();
    const RectD clippedRod = clipToFormat ? rod.intersect(_imp->displayTextures[texIndex].format) : rod;
    const RectI bounds = clippedRod.toPixelEnclosing(mipmapLevel, par);
    return getImageRectangleDisplayed(bounds, par, mipmapLevel);
}

RectI
ViewerGL::getImageRectangleDisplayedRoundedToTileSize(int texIndex,
        const RectD & rod,
        const double par,
        unsigned int mipmapLevel,
        std::vector<RectI> *tiles,
        std::vector<RectI> *tilesRounded,
        int *viewerTileSize,
        RectI* roiNotRounded)
{
    bool clipToProject = isClippingImageToFormat();
    const RectD clippedRod = clipToProject ? rod.intersect(_imp->displayTextures[texIndex].format) : rod;
    const RectI bounds = clippedRod.toPixelEnclosing(mipmapLevel, par);
    const RectI roi = getImageRectangleDisplayed(bounds, par, mipmapLevel);

    ////Texrect is the coordinates of the 4 corners of the texture in the bounds with the current zoom
    ////factor taken into account.
    RectI texRect;
    int tileSize = ipow(2, appPTR->getCurrentSettings()->getViewerTilesPowerOf2());
    texRect.x1 = std::floor(((double)roi.x1) / tileSize) * (double)tileSize;
    texRect.y1 = std::floor(((double)roi.y1) / tileSize) * (double)tileSize;
    texRect.x2 = std::ceil(((double)roi.x2) / tileSize) * (double)tileSize;
    texRect.y2 = std::ceil(((double)roi.y2) / tileSize) * (double)tileSize;

    // Make sure the bounds of the area to render in the texture lies in the bounds
    if (roiNotRounded) {
        *roiNotRounded = roi;
    }

    if (tilesRounded) {
        for (int y = texRect.y1; y < texRect.y2; y += tileSize) {
            int y2 = std::min(y + tileSize, texRect.y2);

            for (int x = texRect.x1; x < texRect.x2; x += tileSize) {
                tilesRounded->resize(tilesRounded->size() + 1);
                RectI& tile = tilesRounded->back();
                tile.x1 = x;
                tile.x2 = std::min(x + tileSize, texRect.x2);
                tile.y1 = y;
                tile.y2 = y2;

                if (tiles) {
                    tiles->push_back(tile.intersect(bounds)); // tileRectRounded
                }

                assert(texRect.contains(tile));
            }
        }
    }

    if (viewerTileSize) {
        *viewerTileSize = tileSize;
    }

    return texRect;
}



void
ViewerGL::clearPartialUpdateTextures()
{
    _imp->partialUpdateTextures.clear();
}

bool
ViewerGL::isViewerUIVisible() const
{
    if (!_imp->viewerTab) {
        return false;
    }

    TabWidget* tabWidget = _imp->viewerTab->getParentPane();

    if (!tabWidget) {
        return isVisible();
    }

    if (tabWidget->currentWidget() != _imp->viewerTab) {
        return false;
    }

    return true;
}

void
ViewerGL::endTransferBufferFromRAMToGPU(int textureIndex,
                                        const ViewerFrameTexturePtr& texture,
                                        const ImagePtr& image,
                                        int time,
                                        const RectD& rod,
                                        double par,
                                        ImageBitDepthEnum depth,
                                        unsigned int mipmapLevel,
                                        ImagePremultiplicationEnum premult,
                                        double gain,
                                        double gamma,
                                        double offset,
                                        int lut,
                                        bool recenterViewer,
                                        const Point& viewportCenter,
                                        bool isPartialRect)
{
    if (recenterViewer) {
        QMutexLocker k(&_imp->zoomCtxMutex);
        double curCenterX = (_imp->zoomCtx.left() + _imp->zoomCtx.right()) / 2.;
        double curCenterY = (_imp->zoomCtx.bottom() + _imp->zoomCtx.top()) / 2.;
        _imp->zoomCtx.translate(viewportCenter.x - curCenterX, viewportCenter.y - curCenterY);
    }

    if (isPartialRect) {
        TextureInfo info;
        info.texture = std::dynamic_pointer_cast<ViewerGpuTexture>(texture);
        info.rod = rod;
        info.format = _imp->displayTextures[textureIndex].format;
        info.pixelAspectRatio = par;
        info.roiNotRoundedToTileSize = info.texture->rectangle;
        info.gain = gain;
        info.gamma = gamma;
        info.offset = offset;
        info.mipmapLevel = mipmapLevel;
        info.premult = premult;
        info.time = time;
        info.memoryHeldByLastRenderedImages = 0;
        info.isPartialImage = true;
        info.isVisible = true;
        _imp->partialUpdateTextures.push_back(info);
        // Update time otherwise overlays won't refresh
        _imp->displayTextures[0].time = time;
        _imp->displayTextures[1].time = time;
    } else {
        ViewerInstance* internalNode = getInternalNode();
        _imp->displayTextures[textureIndex].texture = std::dynamic_pointer_cast<ViewerGpuTexture>(texture);
        _imp->displayTextures[textureIndex].isVisible = true;
        _imp->displayTextures[textureIndex].gain = gain;
        _imp->displayTextures[textureIndex].gamma = gamma;
        _imp->displayTextures[textureIndex].offset = offset;
        _imp->displayTextures[textureIndex].mipmapLevel = mipmapLevel;
        _imp->displayingImageLut = (ViewerColorSpaceEnum)lut;
        _imp->displayTextures[textureIndex].premult = premult;
        _imp->displayTextures[textureIndex].time = time;

        if (_imp->displayTextures[textureIndex].memoryHeldByLastRenderedImages > 0) {
            internalNode->unregisterPluginMemory(_imp->displayTextures[textureIndex].memoryHeldByLastRenderedImages);
            _imp->displayTextures[textureIndex].memoryHeldByLastRenderedImages = 0;
        }


        if (image) {
            _imp->viewerTab->setImageFormat(textureIndex, image->getComponents(), depth);
            {
                QMutexLocker k(&_imp->lastRenderedImageMutex);
                _imp->displayTextures[textureIndex].lastRenderedTiles[mipmapLevel] = image;
            }
            _imp->displayTextures[textureIndex].memoryHeldByLastRenderedImages = 0;
            _imp->displayTextures[textureIndex].memoryHeldByLastRenderedImages += image->size();

            internalNode->registerPluginMemory(_imp->displayTextures[textureIndex].memoryHeldByLastRenderedImages);
            Q_EMIT imageChanged(textureIndex, true);
        } else {
            if (!_imp->displayTextures[textureIndex].lastRenderedTiles[mipmapLevel]) {
                Q_EMIT imageChanged(textureIndex, false);
            } else {
                Q_EMIT imageChanged(textureIndex, true);
            }
        }

        setRegionOfDefinition(rod, par, textureIndex);
    }

    update();
} // ViewerGL::endTransferBufferFromRAMToGPU

void
ViewerGL::transferBufferFromRAMtoGPU(const unsigned char* ramBuffer,
                                     size_t bytesCount,
                                     const RectI &roiRoundedToTileSize,
                                     const RectI& roi,
                                     const TextureRect & tileRect,
                                     int textureIndex,
                                     bool isPartialRect,
                                     bool isFirstTile,
                                     ViewerFrameTexturePtr* texture)

try
{
    assert(qApp && qApp->thread() == QThread::currentThread());
    assert(textureIndex == 0 || textureIndex == 1);
    Q_UNUSED(isFirstTile);
    const Gpu::ViewerStorage storage = getBitDepth() == eImageBitDepthByte ?
                                       Gpu::ViewerStorage::BGRA8 : Gpu::ViewerStorage::RGBA32F;
    TextureRect rectangle;
    rectangle.set(isPartialRect ? static_cast<const RectI &>(tileRect) : roiRoundedToTileSize);
    rectangle.par = tileRect.par;
    rectangle.closestPo2 = tileRect.closestPo2;
    std::shared_ptr<ViewerGpuTexture> uploaded;

    if (!isPartialRect) {
        uploaded = _imp->displayTextures[textureIndex].texture;
    }

    if (!uploaded || uploaded->rectangle != rectangle || uploaded->frame.storage() != storage) {
        uploaded = std::make_shared<ViewerGpuTexture>(_imp->canvas ? _imp->canvas->device() : nullptr, rectangle, storage);
    }

    uploaded->frame.uploadTile({tileRect.x1, tileRect.y1, tileRect.x2, tileRect.y2}, ramBuffer, bytesCount);

    if (!isPartialRect) {
        _imp->displayTextures[textureIndex].texture = uploaded;
        _imp->displayTextures[textureIndex].roiNotRoundedToTileSize.set(roi);
        _imp->displayTextures[textureIndex].roiNotRoundedToTileSize.closestPo2 = tileRect.closestPo2;
        _imp->displayTextures[textureIndex].roiNotRoundedToTileSize.par = tileRect.par;
    }

    *texture = uploaded;
} // ViewerGL::transferBufferFromRAMtoGPU
catch (const Gpu::VulkanError& error)
{
    if (_imp->canvas && error.deviceLost()) {
        texture->reset();
        recoverVulkanDisplay(tr("Viewer RAM image upload: %1").arg(QString::fromUtf8(error.what())));
        return;
    }

    if (!_imp->canvas || !error.deviceMemoryFailure()) {
        throw;
    }

    texture->reset();
    activateRasterDisplay(tr("Viewer RAM image upload: %1").arg(QString::fromUtf8(error.what())));
    transferBufferFromRAMtoGPU(ramBuffer, bytesCount, roiRoundedToTileSize, roi, tileRect,
                               textureIndex, isPartialRect, isFirstTile, texture);
}

void
ViewerGL::clearLastRenderedImage()
{
    assert(qApp && qApp->thread() == QThread::currentThread());

    ViewerInstance* internalNode = getInternalNode();

    for (int i = 0; i < 2; ++i) {
        for (U32 j = 0; j < _imp->displayTextures[i].lastRenderedTiles.size(); ++j) {
            _imp->displayTextures[i].lastRenderedTiles[j].reset();
        }

        if (_imp->displayTextures[i].memoryHeldByLastRenderedImages > 0) {
            internalNode->unregisterPluginMemory(_imp->displayTextures[i].memoryHeldByLastRenderedImages);
            _imp->displayTextures[i].memoryHeldByLastRenderedImages = 0;
        }
    }
}

void
ViewerGL::disconnectInputTexture(int textureIndex, bool clearRoD)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    assert(textureIndex == 0 || textureIndex == 1);

    if (_imp->displayTextures[textureIndex].isVisible) {
        _imp->displayTextures[textureIndex].isVisible = false;

        if (clearRoD) {
            RectI r(0, 0, 0, 0);
            _imp->infoViewer[textureIndex]->setDataWindow(r);
        }
    }
}

void
ViewerGL::setGain(double d)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->displayTextures[0].gain = d;
    _imp->displayTextures[1].gain = d;
}

void
ViewerGL::setGamma(double g)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->displayTextures[0].gamma = g;
    _imp->displayTextures[1].gamma = g;
}

void
ViewerGL::setLut(int lut)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->displayingImageLut = (ViewerColorSpaceEnum)lut;
}

#define QMouseEventLocalPos(e) ( e->localPos() )

void
ViewerGL::mousePressEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab->getGui()) {
        return;
    }

    _imp->viewerTab->onMousePressCalledInViewer();

    _imp->hasMovedSincePress = false;
    _imp->pressureOnRelease = 1.;

    if ((e->buttons() == Qt::LeftButton) && !(e->modifiers() &  Qt::MetaModifier)) {
        _imp->pointerTypeOnPress = ePenTypeLMB;
    } else if ((e->buttons() == Qt::RightButton) || (e->buttons() == Qt::LeftButton  && (e->modifiers() &  Qt::MetaModifier))) {
        _imp->pointerTypeOnPress = ePenTypeRMB;
    } else if ((e->buttons() == Qt::MiddleButton)  || (e->buttons() == Qt::LeftButton  && (e->modifiers() &  Qt::AltModifier))) {
        _imp->pointerTypeOnPress = ePenTypeMMB;
    }


    ///Set focus on user click
    setFocus();

    Qt::KeyboardModifiers modifiers = e->modifiers();
    Qt::MouseButton button = e->button();

    if (buttonDownIsLeft(e)) {
        NodeGuiIPtr gui_i = _imp->viewerTab->getInternalNode()->getNode()->getNodeGui();
        assert(gui_i);
        NodeGuiPtr gui = std::dynamic_pointer_cast<NodeGui>(gui_i);
        _imp->viewerTab->getGui()->selectNode(gui);
    }

    _imp->oldClick = e->pos();
    _imp->lastMousePosition = e->pos();
    QPointF zoomPos;
    double zoomScreenPixelWidth, zoomScreenPixelHeight; // screen pixel size in zoom coordinates
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomPos = _imp->zoomCtx.toZoomCoordinates(e->x(), e->y());
        zoomScreenPixelWidth = _imp->zoomCtx.screenPixelWidth();
        zoomScreenPixelHeight = _imp->zoomCtx.screenPixelHeight();
    }
    RectD userRoI;
    bool userRoIEnabled;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoI = _imp->userRoI;
        userRoIEnabled = _imp->userRoIEnabled;
    }
    bool overlaysCaught = false;
    bool mustRedraw = false;
    bool hasPickers = _imp->viewerTab->getGui()->hasPickers();

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        _imp->buildUserRoIOnNextPress) {
        _imp->draggedUserRoI.x1 = zoomPos.x();
        _imp->draggedUserRoI.y1 = zoomPos.y();
        _imp->draggedUserRoI.x2 = _imp->draggedUserRoI.x1 + 1;
        _imp->draggedUserRoI.y2 = _imp->draggedUserRoI.y1 + 1;
        _imp->buildUserRoIOnNextPress = false;
        _imp->ms = eMouseStateBuildingUserRoI;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        (buttonDownIsMiddle(e) ||
         (((e)->buttons() == Qt::RightButton) && (buttonMetaAlt(e) == Qt::AltModifier))) &&
        !modifierHasControl(e)) {
        // middle (or Alt + left) or Alt + right = pan
        _imp->ms = eMouseStateDraggingImage;
        overlaysCaught = true;
    }

    if (!overlaysCaught &&
        (((e->buttons() & Qt::MiddleButton) &&
          ((buttonMetaAlt(e) == Qt::AltModifier) || (e->buttons() & Qt::LeftButton))) ||
         ((e->buttons() & Qt::LeftButton) &&
          (buttonMetaAlt(e) == (Qt::AltModifier | Qt::MetaModifier))))) {
        // Alt + middle or Left + middle or Crtl + Alt + Left = zoom
        _imp->ms = eMouseStateZoomingImage;
        overlaysCaught = true;
    }


    // process the wipe tool events before the plugin overlays
    if (!overlaysCaught &&
        _imp->overlay &&
        isWipeHandleVisible() &&
        buttonDownIsLeft(e) &&
        _imp->isNearbyWipeCenter(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        _imp->ms = eMouseStateDraggingWipeCenter;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        _imp->overlay &&
        isWipeHandleVisible() &&
        buttonDownIsLeft(e) &&
        _imp->isNearbyWipeMixHandle(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        _imp->ms = eMouseStateDraggingWipeMixHandle;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        _imp->overlay &&
        isWipeHandleVisible() &&
        buttonDownIsLeft(e) &&
        _imp->isNearbyWipeRotateBar(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        _imp->ms = eMouseStateRotatingWipeHandle;
        overlaysCaught = true;
        mustRedraw = true;
    }

    // process plugin overlays
    if (!overlaysCaught &&
        (_imp->ms == eMouseStateUndefined) &&
        _imp->overlay) {
        overlaysCaught = _imp->viewerTab->notifyOverlaysPenDown(RenderScale::fromMipmapLevel(getCurrentMipmapLevel()), _imp->pointerTypeOnPress, QMouseEventLocalPos(e), zoomPos, _imp->pressureOnPress, currentTimeForEvent(e));

        if (overlaysCaught) {
            mustRedraw = true;
        }
    }

    if (!overlaysCaught &&
        isMouseShortcut(kShortcutGroupViewer, kShortcutIDMousePickColor, modifiers, button) &&
        displayingImage()) {
        // picker with single-point selection
        _imp->pickerState = ePickerStatePoint;

        if (pickColor(e->x(), e->y(), false)) {
            _imp->ms = eMouseStatePickingColor;
            mustRedraw = true;
            overlaysCaught = true;
        }
    }

    if (!overlaysCaught &&
        isMouseShortcut(kShortcutGroupViewer, kShortcutIDMousePickInputColor, modifiers, button) &&
        displayingImage()) {
        // picker with single-point selection
        _imp->pickerState = ePickerStatePoint;

        if (pickColor(e->x(), e->y(), true)) {
            _imp->ms = eMouseStatePickingInputColor;
            mustRedraw = true;
            overlaysCaught = true;
        }
    }

    if (!overlaysCaught &&
        isMouseShortcut(kShortcutGroupViewer, kShortcutIDMouseRectanglePick, modifiers, button) &&
        displayingImage()) {
        // start picker with rectangle selection (picked color is the average over the rectangle)
        _imp->pickerState = ePickerStateRectangle;
        _imp->pickerRect.setTopLeft(zoomPos);
        _imp->pickerRect.setBottomRight(zoomPos);
        _imp->ms = eMouseStateBuildingPickerRectangle;
        mustRedraw = true;
        overlaysCaught = true;
    }

    if (!overlaysCaught &&
        (_imp->pickerState != ePickerStateInactive) &&
        buttonDownIsLeft(e) &&
        displayingImage()) {
        // disable picker if picker is set when clicking
        _imp->pickerState = ePickerStateInactive;
        mustRedraw = true;
        overlaysCaught = true;
    }

    if (hasPickers && (_imp->pickerState == ePickerStateInactive)) {
        _imp->viewerTab->getGui()->clearColorPickers();
        unsetCursor();
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoIBottomEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the bottom edge of the user ROI
        _imp->ms = eMouseStateDraggingRoiBottomEdge;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoILeftEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the left edge of the user ROI
        _imp->ms = eMouseStateDraggingRoiLeftEdge;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled && isNearByUserRoIRightEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the right edge of the user ROI
        _imp->ms = eMouseStateDraggingRoiRightEdge;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoITopEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the top edge of the user ROI
        _imp->ms = eMouseStateDraggingRoiTopEdge;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoI((userRoI.x1 + userRoI.x2) / 2., (userRoI.y1 + userRoI.y2) / 2.,
                        zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the midpoint of the user ROI
        _imp->ms = eMouseStateDraggingRoiCross;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoI(userRoI.x1, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the topleft corner of the user ROI
        _imp->ms = eMouseStateDraggingRoiTopLeft;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoI(userRoI.x2, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the topright corner of the user ROI
        _imp->ms = eMouseStateDraggingRoiTopRight;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoI(userRoI.x1, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the bottomleft corner of the user ROI
        _imp->ms = eMouseStateDraggingRoiBottomLeft;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e) &&
        userRoIEnabled &&
        isNearByUserRoI(userRoI.x2, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
        // start dragging the bottomright corner of the user ROI
        _imp->ms = eMouseStateDraggingRoiBottomRight;
        overlaysCaught = true;
        mustRedraw = true;
    }

    if (!overlaysCaught &&
        buttonDownIsRight(e)) {
        _imp->menu->exec(mapToGlobal(e->pos()));
        overlaysCaught = true;
    }

    if (!overlaysCaught &&
        buttonDownIsLeft(e)) {
        ///build selection rectangle
        _imp->selectionRectangle.setTopLeft(zoomPos);
        _imp->selectionRectangle.setBottomRight(zoomPos);
        _imp->lastDragStartPos = zoomPos;
        _imp->ms = eMouseStateSelecting;

        if (!modCASIsControl(e)) {
            Q_EMIT selectionCleared();
            mustRedraw = true;
        }

        overlaysCaught = true;
    }

    Q_UNUSED(overlaysCaught);

    if (mustRedraw) {
        update();
    }
} // mousePressEvent

void
ViewerGL::mouseReleaseEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab->getGui()) {
        return;
    }

    _imp->pressureOnPress = 1;
    _imp->pointerTypeOnPress = ePenTypeLMB;

    if (_imp->ms == eMouseStateBuildingPickerRectangle) {
        updateRectangleColorPicker();
    }

    if (_imp->ms == eMouseStateSelecting) {
        if (_imp->hasMovedSincePress) {
            Q_EMIT selectionRectangleChanged(true);
        }
    } else if (_imp->ms == eMouseStateBuildingUserRoI) {
        QMutexLocker k(&_imp->userRoIMutex);
        _imp->userRoI = _imp->draggedUserRoI;
    }


    _imp->hasMovedSincePress = false;

    // Always redraw if mouse state changes (e.g. from hovering something)
    bool mustRedraw = (_imp->ms != eMouseStateUndefined);
    _imp->ms = eMouseStateUndefined;
    QPointF zoomPos;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomPos = _imp->zoomCtx.toZoomCoordinates(e->x(), e->y());
    }

    if (_imp->viewerTab->notifyOverlaysPenUp(RenderScale::fromMipmapLevel(getCurrentMipmapLevel()), QMouseEventLocalPos(e), zoomPos, currentTimeForEvent(e), _imp->pressureOnRelease)) {
        mustRedraw = true;
    }

    if (mustRedraw) {
        update();
    }

    if (_imp->renderOnPenUp) {
        _imp->renderOnPenUp = false;
        getInternalNode()->renderCurrentFrame(true);
    }
} // ViewerGL::mouseReleaseEvent

void
ViewerGL::mouseMoveEvent(QMouseEvent* e)
{
    if (!penMotionInternal(e->x(), e->y(), /*pressure=*/ 1., currentTimeForEvent(e), e)) {
        QWidget::mouseMoveEvent(e);
    }
} // mouseMoveEvent

void
ViewerGL::tabletEvent(QTabletEvent* e)
{
    qreal pressure = e->pressure();

    if (pressure <= 0.) {
        // Some tablets seem to return pressure between -0.5 and 0.5
        // This is probably a Qt bug, see https://github.com/MrKepzie/Natron/issues/1697
        pressure += 1.;
    }

    switch (e->type()) {
    case QEvent::TabletPress: {
        switch (e->pointerType()) {
        case QPointingDevice::PointerType::Cursor:
            _imp->pointerTypeOnPress  = ePenTypeCursor;
            break;

        case QPointingDevice::PointerType::Eraser:
            _imp->pointerTypeOnPress  = ePenTypeEraser;
            break;

        case QPointingDevice::PointerType::Pen:
        default:
            _imp->pointerTypeOnPress  = ePenTypePen;
            break;
        }

        _imp->pressureOnPress = pressure;
        QWidget::tabletEvent(e);
        break;
    }

    case QEvent::TabletRelease: {
        _imp->pressureOnRelease = pressure;
        QWidget::tabletEvent(e);
        break;
    }

    case QEvent::TabletMove: {
        if (!penMotionInternal(e->x(), e->y(), pressure, currentTimeForEvent(e), e)) {
            QWidget::tabletEvent(e);
        } else {
            e->accept();
        }

        break;
    }

    default:
        break;
    }
}

bool
ViewerGL::penMotionInternal(int x,
                            int y,
                            double pressure,
                            double timestamp,
                            QInputEvent* e)
{
    Q_UNUSED(e);
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    ///The app is closing don't do anything
    Gui* gui = _imp->viewerTab->getGui();

    if (!gui || !getInternalNode()) {
        return false;
    }

    _imp->hasMovedSincePress = true;

    QPointF zoomPos;

    // if the picker was deselected, this fixes the picer State
    // (see issue #133 https://github.com/MrKepzie/Natron/issues/133 )
    // Edited: commented-out we need the picker state to be active even if there are no color pickers active because parametric
    // parameters may use it
    //if ( !gui->hasPickers() ) {
    //    _imp->pickerState = ePickerStateInactive;
    //}

    double zoomScreenPixelWidth, zoomScreenPixelHeight; // screen pixel size in zoom coordinates
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        zoomPos = _imp->zoomCtx.toZoomCoordinates(x, y);
        zoomScreenPixelWidth = _imp->zoomCtx.screenPixelWidth();
        zoomScreenPixelHeight = _imp->zoomCtx.screenPixelHeight();
    }

    updateInfoWidgetColorPicker(zoomPos, QPoint(x, y));

    if (_imp->viewerTab->isViewersSynchroEnabled()) {
        const std::list<ViewerTab *> &allViewers = gui->getViewersList();

        for (std::list<ViewerTab * >::const_iterator it = allViewers.begin(); it != allViewers.end(); ++it) {
            if ((*it)->getViewer() != this) {
                (*it)->getViewer()->updateInfoWidgetColorPicker(zoomPos, QPoint(x, y));
            }
        }
    }

    //update the cursor if it is hovering an overlay and we're not dragging the image
    bool userRoIEnabled;
    RectD userRoI;
    {
        QMutexLocker l(&_imp->userRoIMutex);
        userRoI = _imp->userRoI;
        userRoIEnabled = _imp->userRoIEnabled;
    }
    bool mustRedraw = false;
    bool wasHovering = _imp->hs != eHoverStateNothing;
    bool cursorSet = false;

    if ((_imp->ms != eMouseStateDraggingImage) && _imp->overlay) {
        _imp->hs = eHoverStateNothing;

        if (isWipeHandleVisible() && _imp->isNearbyWipeCenter(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
            setCursor(QCursor(Qt::SizeAllCursor));
            cursorSet = true;
        } else if (isWipeHandleVisible() && _imp->isNearbyWipeMixHandle(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
            _imp->hs = eHoverStateWipeMix;
            mustRedraw = true;
        } else if (isWipeHandleVisible() && _imp->isNearbyWipeRotateBar(zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)) {
            _imp->hs = eHoverStateWipeRotateHandle;
            mustRedraw = true;
        } else if (userRoIEnabled) {
            if (isNearByUserRoIBottomEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                || isNearByUserRoITopEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                || (_imp->ms == eMouseStateDraggingRoiBottomEdge)
                || (_imp->ms == eMouseStateDraggingRoiTopEdge)) {
                setCursor(QCursor(Qt::SizeVerCursor));
                cursorSet = true;
            } else if (isNearByUserRoILeftEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                       || isNearByUserRoIRightEdge(userRoI, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                       || (_imp->ms == eMouseStateDraggingRoiLeftEdge)
                       || (_imp->ms == eMouseStateDraggingRoiRightEdge)) {
                setCursor(QCursor(Qt::SizeHorCursor));
                cursorSet = true;
            } else if (isNearByUserRoI((userRoI.x1 + userRoI.x2) / 2, (userRoI.y1 + userRoI.y2) / 2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight)
                       || (_imp->ms == eMouseStateDraggingRoiCross)) {
                setCursor(QCursor(Qt::SizeAllCursor));
                cursorSet = true;
            } else if (isNearByUserRoI(userRoI.x2, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                       isNearByUserRoI(userRoI.x1, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                       (_imp->ms == eMouseStateDraggingRoiBottomRight) ||
                       (_imp->ms == eMouseStateDraggingRoiTopLeft)) {
                setCursor(QCursor(Qt::SizeFDiagCursor));
                cursorSet = true;
            } else if (isNearByUserRoI(userRoI.x1, userRoI.y1, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                       isNearByUserRoI(userRoI.x2, userRoI.y2, zoomPos, zoomScreenPixelWidth, zoomScreenPixelHeight) ||
                       (_imp->ms == eMouseStateDraggingRoiBottomLeft) ||
                       (_imp->ms == eMouseStateDraggingRoiTopRight)) {
                setCursor(QCursor(Qt::SizeBDiagCursor));
                cursorSet = true;
            }
        }
    }

    if ((_imp->hs == eHoverStateNothing) && wasHovering) {
        mustRedraw = true;
    }

    QPoint newClick(x, y);
    QPoint oldClick = _imp->oldClick;
    QPointF newClick_opengl, oldClick_opengl, oldPosition_opengl;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        newClick_opengl = _imp->zoomCtx.toZoomCoordinates(newClick.x(), newClick.y());
        oldClick_opengl = _imp->zoomCtx.toZoomCoordinates(oldClick.x(), oldClick.y());
        oldPosition_opengl = _imp->zoomCtx.toZoomCoordinates(_imp->lastMousePosition.x(), _imp->lastMousePosition.y());
    }
    double dx = (oldClick_opengl.x() - newClick_opengl.x());
    double dy = (oldClick_opengl.y() - newClick_opengl.y());
    double dxSinceLastMove = (oldPosition_opengl.x() - newClick_opengl.x());
    double dySinceLastMove = (oldPosition_opengl.y() - newClick_opengl.y());
    bool overlaysCaughtByPlugin = false;

    switch (_imp->ms) {
    case eMouseStateDraggingImage: {
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            _imp->zoomCtx.translate(dx, dy);
            _imp->zoomOrPannedSinceLastFit = true;
        }
        _imp->oldClick = newClick;

        if (_imp->viewerTab->isViewersSynchroEnabled()) {
            _imp->viewerTab->synchronizeOtherViewersProjection();
        }

        checkIfViewPortRoIValidOrRender();

        //  else {
        mustRedraw = true;
        // }
        // no need to update the color picker or mouse posn: they should be unchanged
        break;
    }

    case eMouseStateZoomingImage: {
        double zoomFactor;
        int delta = 2 * ((x - _imp->lastMousePosition.x()) - (y - _imp->lastMousePosition.y()));
        double scaleFactor = std::pow(NATRON_WHEEL_ZOOM_PER_DELTA, delta); // no need to use ipow() here, because the result is not cast to int
        {
            QMutexLocker l(&_imp->zoomCtxMutex);
            _imp->zoomCtx.zoom(oldClick_opengl.x(), oldClick_opengl.y(), scaleFactor);
            zoomFactor = _imp->zoomCtx.factor();
        }
        _imp->zoomOrPannedSinceLastFit = true;
        int zoomValue = (int)(100 * zoomFactor);

        if (zoomValue == 0) {
            zoomValue = 1;     // sometimes, floor(100*0.01) makes 0
        }

        assert(zoomValue > 0);
        Q_EMIT zoomChanged(zoomValue);

        if (_imp->viewerTab->isViewersSynchroEnabled()) {
            _imp->viewerTab->synchronizeOtherViewersProjection();
        }

        //_imp->oldClick = newClick; // don't update oldClick! this is the zoom center
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(true);

        //  else {
        mustRedraw = true;
        // }
        // no need to update the color picker or mouse posn: they should be unchanged
        break;
    }

    case eMouseStateDraggingRoiBottomEdge: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.y1 - dySinceLastMove) < _imp->userRoI.y2) {
            _imp->userRoI.y1 -= dySinceLastMove;
            l.unlock();

            if (displayingImage()) {
                _imp->renderOnPenUp = true;
            }

            mustRedraw = true;
        }

        break;
    }

    case eMouseStateDraggingRoiLeftEdge: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.x1 - dxSinceLastMove) < _imp->userRoI.x2) {
            _imp->userRoI.x1 -= dxSinceLastMove;
            l.unlock();

            if (displayingImage()) {
                _imp->renderOnPenUp = true;
            }

            mustRedraw = true;
        }

        break;
    }

    case eMouseStateDraggingRoiRightEdge: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.x2 - dxSinceLastMove) > _imp->userRoI.x1) {
            _imp->userRoI.x2 -= dxSinceLastMove;
            l.unlock();

            if (displayingImage()) {
                _imp->renderOnPenUp = true;
            }

            mustRedraw = true;
        }

        break;
    }

    case eMouseStateDraggingRoiTopEdge: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.y2 - dySinceLastMove) > _imp->userRoI.y1) {
            _imp->userRoI.y2 -= dySinceLastMove;
            l.unlock();

            if (displayingImage()) {
                _imp->renderOnPenUp = true;
            }

            mustRedraw = true;
        }

        break;
    }

    case eMouseStateDraggingRoiCross: {
        {
            QMutexLocker l(&_imp->userRoIMutex);
            _imp->userRoI.translate(-dxSinceLastMove, -dySinceLastMove);
        }

        if (displayingImage()) {
            _imp->renderOnPenUp = true;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStateDraggingRoiTopLeft: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.y2 - dySinceLastMove) > _imp->userRoI.y1) {
            _imp->userRoI.y2 -= dySinceLastMove;
        }

        if ((_imp->userRoI.x1 - dxSinceLastMove) < _imp->userRoI.x2) {
            _imp->userRoI.x1 -= dxSinceLastMove;
        }

        l.unlock();

        if (displayingImage()) {
            _imp->renderOnPenUp = true;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStateDraggingRoiTopRight: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.y2 - dySinceLastMove) > _imp->userRoI.y1) {
            _imp->userRoI.y2 -= dySinceLastMove;
        }

        if ((_imp->userRoI.x2 - dxSinceLastMove) > _imp->userRoI.x1) {
            _imp->userRoI.x2 -= dxSinceLastMove;
        }

        l.unlock();

        if (displayingImage()) {
            _imp->renderOnPenUp = true;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStateDraggingRoiBottomRight: {
        QMutexLocker l(&_imp->userRoIMutex);

        if ((_imp->userRoI.x2 - dxSinceLastMove) > _imp->userRoI.x1) {
            _imp->userRoI.x2 -= dxSinceLastMove;
        }

        if ((_imp->userRoI.y1 - dySinceLastMove) < _imp->userRoI.y2) {
            _imp->userRoI.y1 -= dySinceLastMove;
        }

        l.unlock();

        if (displayingImage()) {
            _imp->renderOnPenUp = true;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStateDraggingRoiBottomLeft: {
        if ((_imp->userRoI.y1 - dySinceLastMove) < _imp->userRoI.y2) {
            _imp->userRoI.y1 -= dySinceLastMove;
        }

        if ((_imp->userRoI.x1 - dxSinceLastMove) < _imp->userRoI.x2) {
            _imp->userRoI.x1 -= dxSinceLastMove;
        }

        _imp->userRoIMutex.unlock();

        if (displayingImage()) {
            _imp->renderOnPenUp = true;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStateBuildingUserRoI: {
        if ((_imp->draggedUserRoI.x2 - dxSinceLastMove) > _imp->draggedUserRoI.x1) {
            _imp->draggedUserRoI.x2 -= dxSinceLastMove;
        }

        if ((_imp->draggedUserRoI.y1 - dySinceLastMove) < _imp->draggedUserRoI.y2) {
            _imp->draggedUserRoI.y1 -= dySinceLastMove;
        }

        if (displayingImage()) {
            _imp->renderOnPenUp = true;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStateDraggingWipeCenter: {
        QMutexLocker l(&_imp->wipeControlsMutex);
        _imp->wipeCenter.rx() -= dxSinceLastMove;
        _imp->wipeCenter.ry() -= dySinceLastMove;
        mustRedraw = true;
        break;
    }

    case eMouseStateDraggingWipeMixHandle: {
        QMutexLocker l(&_imp->wipeControlsMutex);
        double angle = std::atan2(zoomPos.y() - _imp->wipeCenter.y(), zoomPos.x() - _imp->wipeCenter.x());
        double prevAngle = std::atan2(oldPosition_opengl.y() - _imp->wipeCenter.y(),
                                      oldPosition_opengl.x() - _imp->wipeCenter.x());
        _imp->mixAmount -= (angle - prevAngle);
        _imp->mixAmount = std::max(0., std::min(_imp->mixAmount, 1.));
        mustRedraw = true;
        break;
    }

    case eMouseStateRotatingWipeHandle: {
        QMutexLocker l(&_imp->wipeControlsMutex);
        double angle = std::atan2(zoomPos.y() - _imp->wipeCenter.y(), zoomPos.x() - _imp->wipeCenter.x());
        _imp->wipeAngle = angle;
        double closestPI2 = M_PI_2 * std::floor(_imp->wipeAngle / M_PI_2 + 0.5);

        if (std::fabs(_imp->wipeAngle - closestPI2) < 0.1) {
            // snap to closest multiple of PI / 2.
            _imp->wipeAngle = closestPI2;
        }

        mustRedraw = true;
        break;
    }

    case eMouseStatePickingColor: {
        pickColor(newClick.x(), newClick.y(), false);
        mustRedraw = true;
        break;
    }

    case eMouseStatePickingInputColor: {
        pickColor(newClick.x(), newClick.y(), true);
        mustRedraw = true;
        break;
    }

    case eMouseStateBuildingPickerRectangle: {
        QPointF btmRight = _imp->pickerRect.bottomRight();
        btmRight.rx() -= dxSinceLastMove;
        btmRight.ry() -= dySinceLastMove;
        _imp->pickerRect.setBottomRight(btmRight);
        mustRedraw = true;
        break;
    }

    case eMouseStateSelecting: {
        _imp->refreshSelectionRectangle(zoomPos);
        mustRedraw = true;
        Q_EMIT selectionRectangleChanged(false);
    };

    break;
    default: {
        QPointF localPos(x, y);

        if (_imp->overlay &&
            _imp->viewerTab->notifyOverlaysPenMotion(RenderScale::fromMipmapLevel(getCurrentMipmapLevel()), localPos, zoomPos, pressure, timestamp)) {
            mustRedraw = true;
            overlaysCaughtByPlugin = true;
        }

        break;
    }
    } // switch

    if (mustRedraw) {
        update();
    }

    _imp->lastMousePosition = newClick;

    if (!cursorSet) {
        if (_imp->viewerTab->getGui()->hasPickers()) {
            setCursor(appPTR->getColorPickerCursor());
            _imp->setColorPickerCursor = true;
        } else if (!overlaysCaughtByPlugin && _imp->setColorPickerCursor) {
            unsetCursor();
            _imp->setColorPickerCursor = false;
        }
    }

    return true;
} // ViewerGL::penMotionInternal

void
ViewerGL::mouseDoubleClickEvent(QMouseEvent* e)
{
    QPointF pos_opengl;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        pos_opengl = _imp->zoomCtx.toZoomCoordinates(e->x(), e->y());
    }

    if (_imp->viewerTab->notifyOverlaysPenDoubleClick(RenderScale::fromMipmapLevel(getCurrentMipmapLevel()), QMouseEventLocalPos(e), pos_opengl)) {
        update();
    }

    QWidget::mouseDoubleClickEvent(e);
}

QPointF
ViewerGL::toZoomCoordinates(const QPointF& position) const
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    return _imp->zoomCtx.toZoomCoordinates(position.x(), position.y());
}

QPointF
ViewerGL::toWidgetCoordinates(const QPointF& position) const
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    return _imp->zoomCtx.toWidgetCoordinates(position.x(), position.y());
}

void
ViewerGL::getTopLeftAndBottomRightInZoomCoords(QPointF* topLeft,
        QPointF* bottomRight) const
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    *topLeft = _imp->zoomCtx.toZoomCoordinates(0, 0);
    *bottomRight = _imp->zoomCtx.toZoomCoordinates(_imp->zoomCtx.screenWidth() - 1, _imp->zoomCtx.screenHeight() - 1);
}

// used to update the information bar at the bottom of the viewer (not for the ctrl-click color picker)
void
ViewerGL::updateColorPicker(int textureIndex,
                            int x,
                            int y)
{
    if ((_imp->pickerState != ePickerStateInactive) || !_imp->viewerTab || !_imp->viewerTab->getGui() || _imp->viewerTab->getGui()->isGUIFrozen()) {
        return;
    }

    const std::list<Histogram *> &histograms = _imp->viewerTab->getGui()->getHistograms();

    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!displayingImage()) {
        for (std::list<Histogram * >::const_iterator it = histograms.begin(); it != histograms.end(); ++it) {
            if ((*it)->getViewerTextureInputDisplayed() == textureIndex) {
                (*it)->hideViewerCursor();
            }
        }

        if (_imp->infoViewer[textureIndex]->colorVisible()) {
            _imp->infoViewer[textureIndex]->hideColorInfo();
        }

        if (textureIndex == 0) {
            setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
        }

        return;
    }


    QPoint pos;
    bool xInitialized = false;
    bool yInitialized = false;

    if (x != std::numeric_limits<int>::max()) {
        xInitialized = true;
        pos.setX(x);
    }

    if (y != std::numeric_limits<int>::max()) {
        yInitialized = true;
        pos.setY(y);
    }

    QPointF imgPosCanonical;

    if (!xInitialized || !yInitialized) {
        if (!_imp->viewerTab->isViewersSynchroEnabled()) {
            pos = mapFromGlobal(QCursor::pos());
            QMutexLocker l(&_imp->zoomCtxMutex);
            imgPosCanonical = _imp->zoomCtx.toZoomCoordinates(pos.x(), pos.y());
        } else {
            ViewerTab* masterViewer = getViewerTab()->getGui()->getMasterSyncViewer();

            if (masterViewer) {
                pos = masterViewer->getViewer()->mapFromGlobal(QCursor::pos());
                imgPosCanonical = masterViewer->getViewer()->toZoomCoordinates(pos);
            } else {
                pos = mapFromGlobal(QCursor::pos());
                QMutexLocker l(&_imp->zoomCtxMutex);
                imgPosCanonical = _imp->zoomCtx.toZoomCoordinates(pos.x(), pos.y());
            }
        }
    } else {
        QMutexLocker l(&_imp->zoomCtxMutex);
        imgPosCanonical = _imp->zoomCtx.toZoomCoordinates(pos.x(), pos.y());
    }

    float r, g, b, a;
    bool linear = appPTR->getCurrentSettings()->getColorPickerLinear();
    bool picked = false;
    RectD rod = getRoD(textureIndex);
    RectD formatCanonical = getCanonicalFormat(textureIndex);
    unsigned int mmLevel;

    if ((imgPosCanonical.x() >= rod.left()) &&
        (imgPosCanonical.x() < rod.right()) &&
        (imgPosCanonical.y() >= rod.bottom()) &&
        (imgPosCanonical.y() < rod.top())) {
        if ((pos.x() >= 0) && (pos.x() < width()) &&
            (pos.y() >= 0) && (pos.y() < height())) {
            ///if the clip to project format is enabled, make sure it is in the project format too
            bool clipping = isClippingImageToFormat();

            if (!clipping ||
                ((imgPosCanonical.x() >= formatCanonical.left()) &&
                 (imgPosCanonical.x() < formatCanonical.right()) &&
                 (imgPosCanonical.y() >= formatCanonical.bottom()) &&
                 (imgPosCanonical.y() < formatCanonical.top()))) {
                //imgPos must be in canonical coordinates
                picked = getColorAt(imgPosCanonical.x(), imgPosCanonical.y(), linear, textureIndex, &r, &g, &b, &a, &mmLevel);
            }
        }
    }

    if (!picked) {
        _imp->infoViewer[textureIndex]->setColorValid(false);

        if (textureIndex == 0) {
            setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
        }
    } else {
        _imp->infoViewer[textureIndex]->setColorApproximated(mmLevel > 0);
        _imp->infoViewer[textureIndex]->setColorValid(true);

        if (!_imp->infoViewer[textureIndex]->colorVisible()) {
            _imp->infoViewer[textureIndex]->showColorInfo();
        }

        _imp->infoViewer[textureIndex]->setColor(r, g, b, a);

        if (textureIndex == 0) {
            OfxRGBAColourD interactColor = {r, g, b, a};
            setParametricParamsPickerColor(interactColor, true, true);
        }

        std::vector<double> colorVec(4);
        colorVec[0] = r;
        colorVec[1] = g;
        colorVec[2] = b;
        colorVec[3] = a;

        for (std::list<Histogram * >::const_iterator it = histograms.begin(); it != histograms.end(); ++it) {
            if ((*it)->getViewerTextureInputDisplayed() == textureIndex) {
                (*it)->setViewerCursor(colorVec);
            }
        }
    }
} // updateColorPicker

void
ViewerGL::setParametricParamsPickerColor(const OfxRGBAColourD& color, bool setColor, bool hasColor)
{
    if (!_imp->viewerTab->getGui()) {
        return;
    }

    std::vector<QPointer<DockablePanel>> panels;

    for (auto * panel : _imp->viewerTab->getGui()->getVisiblePanels()) {
        panels.emplace_back(panel);
    }

    // A colour-picker notification can rebuild another node's controls.
    // Keep a guarded snapshot across those callbacks.
    for (const auto& panel : panels) {
        NodeSettingsPanel* nodePanel = dynamic_cast<NodeSettingsPanel *>(panel.data());

        if (!nodePanel) {
            continue;
        }

        NodeGuiPtr node = nodePanel->getNode();

        if (!node) {
            continue;
        }

        node->getNode()->getEffectInstance()->setInteractColourPicker_public(color, setColor, hasColor);
    }
}

bool
ViewerGL::checkIfViewPortRoIValidOrRenderForInput(int texIndex)
{

    unsigned int mipmapLevel = std::max(getInternalNode()->getMipmapLevelFromZoomFactor(), getInternalNode()->getMipmapLevel());
    int closestPo2 = 1 << mipmapLevel;

    if (!_imp->displayTextures[texIndex].texture) {
        return false;
    }

    if (closestPo2 != _imp->displayTextures[texIndex].texture->rectangle.closestPo2) {
        return false;
    }

    RectI roiNotRounded;
    RectI roi = getImageRectangleDisplayedRoundedToTileSize(texIndex, _imp->displayTextures[texIndex].rod, _imp->displayTextures[texIndex].texture->rectangle.par, mipmapLevel, 0, 0, 0, &roiNotRounded);
    const RectI& currentTexRoi = _imp->displayTextures[texIndex].texture->rectangle;

    if (!currentTexRoi.contains(roi)) {
        return false;
    }

    _imp->displayTextures[texIndex].roiNotRoundedToTileSize.set(roiNotRounded);

    return true;
}

void
ViewerGL::checkIfViewPortRoIValidOrRender()
{
    for (int i = 0; i < 2; ++i) {
        if (!checkIfViewPortRoIValidOrRenderForInput(i)) {
            if (!getViewerTab()->getGui()->getApp()->getProject()->isLoadingProject()) {
                ViewerInstance* viewer = getInternalNode();
                assert(viewer);

                if (viewer) {
                    viewer->getRenderEngine()->abortRenderingAutoRestart();
                    viewer->renderCurrentFrame(true);
                }
            }

            break;
        }
    }
}

void
ViewerGL::wheelEvent(QWheelEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab) {
        return QWidget::wheelEvent(e);
    }

    // delta=120 is a standard wheel mouse click, but mice may be more accurate
    // - Apple Trackpad has an accuracy of delta=2
    // - Apple Magic Mouse has an accuracy of delta=2
    // - Apple Mighty Mouse has an accuracy of delta=28
    // - Logitech Wheel Mouse has an accuracy of delta=120
    // The Qt Unit is 8 deltas per degree.
    // delta/2 gives a reasonable value in pixels (for panning, etc).

    // frame seeking works both with horizontal and vertical wheel + control modifier
    if (modCASIsControl(e)) {
        const int delta_max = 28;
        // threshold delta to the range -delta_max..delta_max
        int delta = std::max(-delta_max, std::min(e->angleDelta().y(), delta_max));

        _imp->wheelDeltaSeekFrame += delta;

        if (_imp->wheelDeltaSeekFrame <= -delta_max) {
            _imp->wheelDeltaSeekFrame += delta_max;
            _imp->viewerTab->nextFrame();
        } else if (_imp->wheelDeltaSeekFrame >= delta_max) {
            _imp->wheelDeltaSeekFrame -= delta_max;
            _imp->viewerTab->previousFrame();
        }

        return;
    }

    if (e->angleDelta().x() != 0) {
        // we only handle vertical motion for zooming
        return QWidget::wheelEvent(e);
    }


    Gui* gui = _imp->viewerTab->getGui();

    if (!gui) {
        return QWidget::wheelEvent(e);
    }

    NodeGuiIPtr nodeGui_i = _imp->viewerTab->getInternalNode()->getNode()->getNodeGui();
    NodeGuiPtr nodeGui = std::dynamic_pointer_cast<NodeGui>(nodeGui_i);
    gui->selectNode(nodeGui);

    double zoomFactor;
    unsigned int oldMipmapLevel, newMipmapLevel;
    double scaleFactor = std::pow(NATRON_WHEEL_ZOOM_PER_DELTA, e->angleDelta().y());
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        QPointF zoomCenter = _imp->zoomCtx.toZoomCoordinates(e->position().x(), e->position().y());

        zoomFactor = _imp->zoomCtx.factor();
        //oldMipmapLevel = std::log( zoomFactor >= 1 ? 1 : ipow( 2, -std::ceil(std::log(zoomFactor) / M_LN2) ) ) / M_LN2;
        oldMipmapLevel = zoomFactor >= 1 ? 0 : -std::ceil(std::log(zoomFactor) / M_LN2);

        _imp->zoomCtx.zoom(zoomCenter.x(), zoomCenter.y(), scaleFactor);
        zoomFactor = _imp->zoomCtx.factor();
    }
    //newMipmapLevel = std::log( zoomFactor >= 1 ? 1 : std::pow( 2, -std::ceil(std::log(zoomFactor) / M_LN2) ) ) / M_LN2;
    newMipmapLevel = zoomFactor >= 1 ? 0 : -std::ceil(std::log(zoomFactor) / M_LN2);
    _imp->zoomOrPannedSinceLastFit = true;
    int zoomValue = (int)(100 * zoomFactor);

    if (zoomValue == 0) {
        zoomValue = 1; // sometimes, floor(100*0.01) makes 0
    }

    assert(zoomValue > 0);
    Q_EMIT zoomChanged(zoomValue);

    if (_imp->viewerTab->isViewersSynchroEnabled()) {
        _imp->viewerTab->synchronizeOtherViewersProjection();
    }

    checkIfViewPortRoIValidOrRender();


    ///Clear green cached line so the user doesn't expect to see things in the cache
    ///since we're changing the zoom factor
    if (oldMipmapLevel != newMipmapLevel) {
        _imp->viewerTab->clearTimelineCacheLine();
    }

    update();
} // ViewerGL::wheelEvent

void
ViewerGL::zoomSlot(double v)
{
    zoomSlot((int)(std::floor(v + 0.5)));
}

void
ViewerGL::zoomSlot(int v)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    assert(v > 0);
    double newZoomFactor = v / 100.;
    unsigned int oldMipmapLevel, newMipmapLevel;
    //newMipmapLevel = std::log( newZoomFactor >= 1 ? 1 :
    //                           std::pow( 2, -std::ceil(std::log(newZoomFactor) / M_LN2) ) ) / M_LN2;
    newMipmapLevel = newZoomFactor >= 1 ? 0 : -std::ceil(std::log(newZoomFactor) / M_LN2);
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        //oldMipmapLevel = std::log( _imp->zoomCtx.factor() >= 1 ? 1 :
        //                           std::pow( 2, -std::ceil(std::log( _imp->zoomCtx.factor() ) / M_LN2) ) ) / M_LN2;
        oldMipmapLevel = _imp->zoomCtx.factor() >= 1 ? 0 : -std::ceil(std::log(_imp->zoomCtx.factor()) / M_LN2);
        double scale = newZoomFactor / _imp->zoomCtx.factor();
        double centerX = (_imp->zoomCtx.left() + _imp->zoomCtx.right()) / 2.;
        double centerY = (_imp->zoomCtx.top() + _imp->zoomCtx.bottom()) / 2.;
        _imp->zoomCtx.zoom(centerX, centerY, scale);
        _imp->zoomOrPannedSinceLastFit = true;
    }

    ///Clear green cached line so the user doesn't expect to see things in the cache
    ///since we're changing the zoom factor
    if (newMipmapLevel != oldMipmapLevel) {
        _imp->viewerTab->clearTimelineCacheLine();
    }

    checkIfViewPortRoIValidOrRender();
}

void
ViewerGL::fitImageToFormat()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    // size in Canonical = Zoom coordinates !
    double w, h;
    const RectD& tex0Format = _imp->displayTextures[0].format;

    if (tex0Format.isNull()) {
        return;
    }

    w = tex0Format.width();
    h = tex0Format.height();

    if (w <= 0 || h <= 0) {
        return;
    }

    double old_zoomFactor;
    double zoomFactor;
    unsigned int oldMipmapLevel, newMipmapLevel;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        old_zoomFactor = _imp->zoomCtx.factor();
        //oldMipmapLevel = std::log( old_zoomFactor >= 1 ? 1 :
        //                           std::pow( 2, -std::ceil(std::log(old_zoomFactor) / M_LN2) ) ) / M_LN2;
        oldMipmapLevel = old_zoomFactor >= 1 ? 0 : -std::ceil(std::log(old_zoomFactor) / M_LN2);

        // set the PAR first
        //_imp->zoomCtx.setZoom(0., 0., 1., 1.);
        // leave 4% of margin around
        _imp->zoomCtx.fit(-0.02 * w, 1.02 * w, -0.02 * h, 1.02 * h);
        zoomFactor = _imp->zoomCtx.factor();
    }
    _imp->zoomOrPannedSinceLastFit = false;
    //newMipmapLevel = std::log( zoomFactor >= 1 ? 1 :
    //                           std::pow( 2, -std::ceil(std::log(zoomFactor) / M_LN2) ) ) / M_LN2;
    newMipmapLevel = zoomFactor >= 1 ? 0 : -std::ceil(std::log(zoomFactor) / M_LN2);

    _imp->oldClick = QPoint(); // reset mouse posn

    if (old_zoomFactor != zoomFactor) {
        int zoomFactorInt = zoomFactor  * 100;

        if (zoomFactorInt == 0) {
            zoomFactorInt = 1;
        }

        Q_EMIT zoomChanged(zoomFactorInt);
    }

    if (_imp->viewerTab->isViewersSynchroEnabled()) {
        _imp->viewerTab->synchronizeOtherViewersProjection();
    }

    if (newMipmapLevel != oldMipmapLevel) {
        ///Clear green cached line so the user doesn't expect to see things in the cache
        ///since we're changing the zoom factor
        _imp->viewerTab->clearTimelineCacheLine();
    }

    checkIfViewPortRoIValidOrRender();

    update();
} // ViewerGL::fitImageToFormat

/**
 *@brief Turns on the overlays on the viewer.
 **/
void
ViewerGL::turnOnOverlay()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->overlay = true;
}

/**
 *@brief Turns off the overlays on the viewer.
 **/
void
ViewerGL::turnOffOverlay()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->overlay = false;
}

void
ViewerGL::setInfoViewer(InfoViewerWidget* i,
                        int textureIndex)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->infoViewer[textureIndex] = i;
}

void
ViewerGL::disconnectViewer()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (displayingImage()) {
        Format f;
        getViewerTab()->getGui()->getApp()->getProject()->getProjectDefaultFormat(&f);
        RectD canonicalFormat = f.toCanonicalFormat();

        for (int i = 0; i < 2; ++i) {
            setRegionOfDefinition(canonicalFormat, f.getPixelAspectRatio(), i);
        }
    }

    //resetWipeControls();
    clearViewer();
}

/* The dataWindow of the currentFrame(BBOX) in canonical coordinates */
const RectD&
ViewerGL::getRoD(int textureIndex) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    return _imp->displayTextures[textureIndex].rod;
}

/*The displayWindow of the currentFrame(Resolution)
   This is the same for both as we only use the display window as to indicate the project window.*/
RectD
ViewerGL::getCanonicalFormat(int texIndex) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    return _imp->displayTextures[texIndex].format;
}

double
ViewerGL::getPAR(int texIndex) const
{
    return _imp->displayTextures[texIndex].pixelAspectRatio;
}

void
ViewerGL::setRegionOfDefinition(const RectD & rod,
                                double par,
                                int textureIndex)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab->getGui()) {
        return;
    }

    const RectI pixelRoD = rod.toPixelEnclosing(0, par);

    _imp->displayTextures[textureIndex].rod = rod;

    if (_imp->infoViewer[textureIndex] && !_imp->viewerTab->getGui()->isGUIFrozen()) {
        _imp->infoViewer[textureIndex]->setDataWindow(pixelRoD);
    }

    QString x1, y1, x2, y2;
    x1.setNum(pixelRoD.x1);
    y1.setNum(pixelRoD.y1);
    x2.setNum(pixelRoD.x2);
    y2.setNum(pixelRoD.y2);


    _imp->currentViewerInfo_btmLeftBBOXoverlay[textureIndex] = x1 + QLatin1Char(',') + y1;
    _imp->currentViewerInfo_topRightBBOXoverlay[textureIndex] = x2 + QLatin1Char(',') + y2;
}

void
ViewerGL::setFormat(const std::string& formatName, const RectD& format, double par, int textureIndex)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab->getGui()) {
        return;
    }

    if (_imp->displayTextures[textureIndex].format != format || _imp->displayTextures[textureIndex].pixelAspectRatio != par) {
        _imp->displayTextures[textureIndex].format = format;
        _imp->displayTextures[textureIndex].pixelAspectRatio = par;

        if (!getZoomOrPannedSinceLastFit() && _imp->zoomCtx.screenWidth() != 0 && _imp->zoomCtx.screenHeight() != 0) {
            fitImageToFormat();
        }
    }

    if (_imp->userRoI.isNull()) {
        _imp->userRoI.x1 = format.width() / 4. + format.x1;
        _imp->userRoI.x2 = format.width() - format.width() / 4. + format.x1;
        _imp->userRoI.y1 = format.height() / 4. + format.y1;
        _imp->userRoI.y2 = format.height() - format.height() / 4. + format.y1;
    }

    _imp->currentViewerInfo_resolutionOverlay[textureIndex] = QString::fromUtf8(formatName.c_str());
}

void
ViewerGL::setClipToFormat(bool b)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    {
        QMutexLocker l(&_imp->clipToDisplayWindowMutex);
        _imp->clipToDisplayWindow = b;
    }
    ViewerInstance* viewer = _imp->viewerTab->getInternalNode();
    assert(viewer);

    if (viewer->getUiContext() && !_imp->viewerTab->getGui()->getApp()->getProject()->isLoadingProject()) {
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(true);
    }
}

bool
ViewerGL::isClippingImageToFormat() const
{
    // MT-SAFE
    QMutexLocker l(&_imp->clipToDisplayWindowMutex);

    return _imp->clipToDisplayWindow;
}

/*display black in the viewer*/
void
ViewerGL::clearViewer()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->displayTextures[0].isVisible = false;
    _imp->displayTextures[1].isVisible = false;
    update();
}

/*overload of QT enter/leave/resize events*/
void
ViewerGL::focusInEvent(QFocusEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    ///The app is closing don't do anything
    if (!_imp->viewerTab->getGui()) {
        return;
    }

    _imp->overlayFocusGainPending = true;
    flushOverlayFocusNotifications();
    QWidget::focusInEvent(e);
}

void
ViewerGL::focusOutEvent(QFocusEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab->getGui()) {
        return;
    }

    _imp->overlayFocusLossPending |= _imp->overlaysHaveFocus;
    _imp->overlayFocusGainPending = false;
    flushOverlayFocusNotifications();
    QWidget::focusOutEvent(e);
}

void
ViewerGL::flushOverlayFocusNotifications()
{
    Gui* gui = _imp->viewerTab->getGui();

    if (!gui || !gui->getApp() || gui->getApp()->isClosing() || gui->getApp()->isShowingDialog()) {
        // A plugin may own the modal loop. Preserve the transition and deliver
        // it after that callback returns, before another draw or input action.
        return;
    }

    bool redraw = false;
    const RenderScale scale = RenderScale::fromMipmapLevel(getCurrentMipmapLevel());

    if (_imp->overlayFocusLossPending) {
        _imp->overlayFocusLossPending = false;
        _imp->overlaysHaveFocus = false;
        redraw |= _imp->viewerTab->notifyOverlaysFocusLost(scale);
    }

    if (_imp->overlayFocusGainPending) {
        _imp->overlayFocusGainPending = false;

        if (!_imp->overlaysHaveFocus) {
            _imp->overlaysHaveFocus = true;
            redraw |= _imp->viewerTab->notifyOverlaysFocusGained(scale);
        }
    }

    if (redraw) {
        update();
    }
}

void
ViewerGL::enterEvent(QtCompat::QEnterEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->infoViewer[0]->showMouseInfo();
    _imp->infoViewer[1]->showMouseInfo();
    QWidget::enterEvent(e);
}

void
ViewerGL::leaveEvent(QEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (_imp->pickerState == ePickerStateInactive) {
        _imp->infoViewer[0]->hideColorInfo();
        _imp->infoViewer[1]->hideColorInfo();
        setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
    }

    _imp->infoViewer[0]->hideMouseInfo();
    _imp->infoViewer[1]->hideMouseInfo();
    QWidget::leaveEvent(e);
}

void
ViewerGL::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    resizeViewport(e->size().width(), e->size().height());
    update();
}

ImageBitDepthEnum
ViewerGL::getBitDepth() const
{
    return appPTR->getCurrentSettings()->getViewersBitDepth();
}

void
ViewerGL::populateMenu()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    _imp->menu->clear();
    QAction* displayOverlaysAction = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideOverlays, kShortcutDescActionHideOverlays, _imp->menu);

    displayOverlaysAction->setCheckable(true);
    displayOverlaysAction->setChecked(_imp->overlay);
    QObject::connect(displayOverlaysAction, SIGNAL(triggered()), this, SLOT(toggleOverlays()));
    QAction* toggleWipe = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDToggleWipe, kShortcutDescToggleWipe, _imp->menu);
    toggleWipe->setCheckable(true);
    toggleWipe->setChecked(getViewerTab()->getCompositingOperator() != eViewerCompositingOperatorNone);
    QObject::connect(toggleWipe, SIGNAL(triggered()), this, SLOT(toggleWipe()));
    _imp->menu->addAction(toggleWipe);


    QAction* centerWipe = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDCenterWipe, kShortcutDescCenterWipe, _imp->menu);
    QObject::connect(centerWipe, SIGNAL(triggered()), this, SLOT(centerWipe()));
    _imp->menu->addAction(centerWipe);

    QAction* goToPrevLayer = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDPrevLayer, kShortcutDescPrevLayer, _imp->menu);
    QObject::connect(goToPrevLayer, SIGNAL(triggered()), _imp->viewerTab, SLOT(previousLayer()));
    _imp->menu->addAction(goToPrevLayer);
    QAction* goToNextLayer = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDNextLayer, kShortcutDescNextLayer, _imp->menu);
    QObject::connect(goToNextLayer, SIGNAL(triggered()), _imp->viewerTab, SLOT(nextLayer()));
    _imp->menu->addAction(goToNextLayer);

    QAction* switchAB = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDSwitchInputAAndB, kShortcutDescSwitchInputAAndB, _imp->menu);
    QObject::connect(switchAB, SIGNAL(triggered()), _imp->viewerTab, SLOT(switchInputAAndB()));
    _imp->menu->addAction(switchAB);

    Menu* showHideMenu = new Menu(tr("Show/Hide"), _imp->menu);
    //showHideMenu->setFont(QFont(appFont,appFontSize));
    _imp->menu->addAction(showHideMenu->menuAction());


    QAction* showHidePlayer, *showHideLeftToolbar, *showHideRightToolbar, *showHideTopToolbar, *showHideInfobar, *showHideTimeline;
    QAction* showAll, *hideAll;

    showHidePlayer = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHidePlayer, kShortcutDescActionHidePlayer,
                                            showHideMenu);

    showHideLeftToolbar = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideLeft, kShortcutDescActionHideLeft,
        showHideMenu);
    showHideRightToolbar = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideRight, kShortcutDescActionHideRight,
        showHideMenu);
    showHideTopToolbar = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideTop, kShortcutDescActionHideTop,
        showHideMenu);
    showHideInfobar = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideInfobar, kShortcutDescActionHideInfobar,
        showHideMenu);
    showHideTimeline = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideTimeline, kShortcutDescActionHideTimeline,
        showHideMenu);


    showAll = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionShowAll, kShortcutDescActionShowAll,
                                     showHideMenu);

    hideAll = new ActionWithShortcut(kShortcutGroupViewer, kShortcutIDActionHideAll, kShortcutDescActionHideAll,
                                     showHideMenu);

    QObject::connect(showHidePlayer, SIGNAL(triggered()), _imp->viewerTab, SLOT(togglePlayerVisibility()));
    QObject::connect(showHideLeftToolbar, SIGNAL(triggered()), _imp->viewerTab, SLOT(toggleLeftToolbarVisiblity()));
    QObject::connect(showHideRightToolbar, SIGNAL(triggered()), _imp->viewerTab, SLOT(toggleRightToolbarVisibility()));
    QObject::connect(showHideTopToolbar, SIGNAL(triggered()), _imp->viewerTab, SLOT(toggleTopToolbarVisibility()));
    QObject::connect(showHideInfobar, SIGNAL(triggered()), _imp->viewerTab, SLOT(toggleInfobarVisbility()));
    QObject::connect(showHideTimeline, SIGNAL(triggered()), _imp->viewerTab, SLOT(toggleTimelineVisibility()));
    QObject::connect(showAll, SIGNAL(triggered()), _imp->viewerTab, SLOT(showAllToolbars()));
    QObject::connect(hideAll, SIGNAL(triggered()), _imp->viewerTab, SLOT(hideAllToolbars()));

    showHideMenu->addAction(showHidePlayer);
    showHideMenu->addAction(showHideTimeline);
    showHideMenu->addAction(showHideInfobar);
    showHideMenu->addAction(showHideLeftToolbar);
    showHideMenu->addAction(showHideRightToolbar);
    showHideMenu->addAction(showHideTopToolbar);
    showHideMenu->addAction(showAll);
    showHideMenu->addAction(hideAll);

    _imp->menu->addAction(displayOverlaysAction);
} // ViewerGL::populateMenu

bool
ViewerGL::renderText(double x,
                     double y,
                     const std::string &string,
                     double r,
                     double g,
                     double b,
                     int flags)
{
    QColor c;

    c.setRgbF(Image::clamp(r, 0., 1.), Image::clamp(g, 0., 1.), Image::clamp(b, 0., 1.));
    renderText(x, y, QString::fromUtf8(string.c_str()), c, font(), flags);

    return true;
}

void
ViewerGL::renderText(double x,
                     double y,
                     const QString &text,
                     const QColor &color,
                     const QFont &font,
                     int flags)
{
    if (_imp->drawingNodeOverlays) {
        OverlayDrawScope scope(_imp->nodeOverlayCommands);
        _imp->nodeOverlayCommands.color(color.redF(), color.greenF(), color.blueF(), color.alphaF());
        _imp->nodeOverlayCommands.text(x, y, text.toUtf8().toStdString(), flags);
        return;
    }

    if (!_imp->overlayPainter || text.isEmpty()) {
        return;
    }

    QPainter& painter = *_imp->overlayPainter;
    painter.save();
    painter.setPen(color);
    painter.setFont(font);
    const QPointF point = toWidgetCoordinates(QPointF(x, y));
    const QFontMetricsF metrics(font);
    QPointF baseline = point;

    if (flags & Qt::AlignHCenter) {
        baseline.rx() -= metrics.horizontalAdvance(text) / 2;
    } else if (flags & Qt::AlignRight) {
        baseline.rx() -= metrics.horizontalAdvance(text);
    }

    if (flags & Qt::AlignTop) {
        baseline.ry() += metrics.ascent();
    } else if (flags & Qt::AlignVCenter) {
        baseline.ry() += (metrics.ascent() - metrics.descent()) / 2;
    }

    painter.drawText(baseline, text);
    painter.restore();
}

void
ViewerGL::updatePersistentMessageToWidth(int w)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->viewerTab || !_imp->viewerTab->getGui() || !_imp->_textFont) {
        return;
    }

    const std::list<DockablePanel *> &openedPanels = _imp->viewerTab->getGui()->getVisiblePanels();

    _imp->persistentMessages.clear();
    QStringList allMessages;
    int type = 0;
    ///Draw overlays in reverse order of appearance
    std::list<DockablePanel *>::const_iterator next = openedPanels.begin();

    if (next != openedPanels.end()) {
        ++next;
    }

    int nbNonEmpty = 0;

    for (std::list<DockablePanel * >::const_iterator it = openedPanels.begin(); it != openedPanels.end(); ++it) {
        const NodeSettingsPanel* isNodePanel = dynamic_cast<const NodeSettingsPanel *>(*it);

        if (!isNodePanel) {
            continue;
        }

        NodePtr node = isNodePanel->getNode()->getNode();

        if (!node) {
            continue;
        }

        QString mess;
        int nType;
        node->getPersistentMessage(&mess, &nType);

        if (!mess.isEmpty()) {
            allMessages.append(mess);
            ++nbNonEmpty;
        }

        if (next != openedPanels.end()) {
            ++next;
        }

        if (!mess.isEmpty()) {
            type = (nbNonEmpty == 1 && nType == 2) ? 2 : 1;
        }
    }

    _imp->persistentMessageType = type;

    assert(_imp->_textFont);
    QFontMetrics fm(*_imp->_textFont);

    for (int i = 0; i < allMessages.size(); ++i) {
        QStringList wordWrapped = wordWrap(fm, allMessages[i],
                                           std::max(2, w - PERSISTENT_MESSAGE_LEFT_OFFSET_PIXELS));

        for (int j = 0; j < wordWrapped.size(); ++j) {
            _imp->persistentMessages.push_back(wordWrapped[j]);
        }
    }

    _imp->displayPersistentMessage = !_imp->persistentMessages.isEmpty();
    update();
} // ViewerGL::updatePersistentMessageToWidth

void
ViewerGL::updatePersistentMessage()
{
    updatePersistentMessageToWidth(width() - 20);
}

void
ViewerGL::getProjection(double *zoomLeft,
                        double *zoomBottom,
                        double *zoomFactor,
                        double *zoomAspectRatio) const
{
    // MT-SAFE
    QMutexLocker l(&_imp->zoomCtxMutex);

    *zoomLeft = _imp->zoomCtx.left();
    *zoomBottom = _imp->zoomCtx.bottom();
    *zoomFactor = _imp->zoomCtx.factor();
    *zoomAspectRatio = _imp->zoomCtx.aspectRatio();
}

void
ViewerGL::setProjection(double zoomLeft,
                        double zoomBottom,
                        double zoomFactor,
                        double zoomAspectRatio)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    QMutexLocker l(&_imp->zoomCtxMutex);
    _imp->zoomCtx.setZoom(zoomLeft, zoomBottom, zoomFactor, zoomAspectRatio);
    Q_EMIT zoomChanged(100 * zoomFactor);
}

RectD
ViewerGL::getViewportRect() const
{
    RectD bbox;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        bbox.x1 = _imp->zoomCtx.left();
        bbox.y1 = _imp->zoomCtx.bottom();
        bbox.x2 = _imp->zoomCtx.right();
        bbox.y2 = _imp->zoomCtx.top();
    }

    return bbox;
}

void
ViewerGL::getCursorPosition(double &x,
                            double &y) const
{
    QPoint p = QCursor::pos();

    p = mapFromGlobal(p);
    QPointF mappedPos = toZoomCoordinates(p);
    x = mappedPos.x();
    y = mappedPos.y();
}

void
ViewerGL::setBuildNewUserRoI(bool b)
{
    _imp->buildUserRoIOnNextPress = b;

    QMutexLocker k(&_imp->userRoIMutex);
    _imp->draggedUserRoI = _imp->userRoI;
}

void
ViewerGL::setUserRoIEnabled(bool b)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    {
        QMutexLocker l(&_imp->userRoIMutex);
        _imp->userRoIEnabled = b;
    }

    if (!b) {
        _imp->buildUserRoIOnNextPress = false;
    }

    if (displayingImage()) {
        _imp->viewerTab->getInternalNode()->renderCurrentFrame(true);
    }

    update();
}

bool
ViewerGL::isNearByUserRoITopEdge(const RectD & roi,
                                 const QPointF & zoomPos,
                                 double zoomScreenPixelWidth,
                                 double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    double length = std::min(roi.x2 - roi.x1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth) * 2);
    RectD r(roi.x1 + length / 2,
            roi.y2 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight,
            roi.x2 - length / 2,
            roi.y2 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight);

    return r.contains(zoomPos.x(), zoomPos.y());
}

bool
ViewerGL::isNearByUserRoIRightEdge(const RectD & roi,
                                   const QPointF & zoomPos,
                                   double zoomScreenPixelWidth,
                                   double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    double length = std::min(roi.y2 - roi.y1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight) * 2);
    RectD r(roi.x2 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y1 + length / 2,
            roi.x2 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y2 - length / 2);

    return r.contains(zoomPos.x(), zoomPos.y());
}

bool
ViewerGL::isNearByUserRoILeftEdge(const RectD & roi,
                                  const QPointF & zoomPos,
                                  double zoomScreenPixelWidth,
                                  double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    double length = std::min(roi.y2 - roi.y1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight) * 2);
    RectD r(roi.x1 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y1 + length / 2,
            roi.x1 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth,
            roi.y2 - length / 2);

    return r.contains(zoomPos.x(), zoomPos.y());
}

bool
ViewerGL::isNearByUserRoIBottomEdge(const RectD & roi,
                                    const QPointF & zoomPos,
                                    double zoomScreenPixelWidth,
                                    double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    double length = std::min(roi.x2 - roi.x1 - 10, (USER_ROI_CLICK_TOLERANCE * zoomScreenPixelWidth) * 2);
    RectD r(roi.x1 + length / 2,
            roi.y1 - USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight,
            roi.x2 - length / 2,
            roi.y1 + USER_ROI_CLICK_TOLERANCE * zoomScreenPixelHeight);

    return r.contains(zoomPos.x(), zoomPos.y());
}

bool
ViewerGL::isNearByUserRoI(double x,
                          double y,
                          const QPointF & zoomPos,
                          double zoomScreenPixelWidth,
                          double zoomScreenPixelHeight)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    RectD r(x - USER_ROI_CROSS_RADIUS * zoomScreenPixelWidth,
            y - USER_ROI_CROSS_RADIUS * zoomScreenPixelHeight,
            x + USER_ROI_CROSS_RADIUS * zoomScreenPixelWidth,
            y + USER_ROI_CROSS_RADIUS * zoomScreenPixelHeight);

    return r.contains(zoomPos.x(), zoomPos.y());
}

bool
ViewerGL::isUserRegionOfInterestEnabled() const
{
    // MT-SAFE
    QMutexLocker l(&_imp->userRoIMutex);

    return _imp->userRoIEnabled;
}

RectD
ViewerGL::getUserRegionOfInterest() const
{
    // MT-SAFE
    QMutexLocker l(&_imp->userRoIMutex);

    return _imp->userRoI;
}

void
ViewerGL::setUserRoI(const RectD & r)
{
    // MT-SAFE
    QMutexLocker l(&_imp->userRoIMutex);
    _imp->userRoI = r;
}

/**
 * @brief Swap the OpenGL buffers.
 **/
void
ViewerGL::swapOpenGLBuffers()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    // Presentation is scheduled by the native Vulkan window.
    update();
}

/**
 * @brief Repaint
 **/
void
ViewerGL::redraw()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    update();
}

void
ViewerGL::redrawNow()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    update();
}

/**
 * @brief Returns the width and height of the viewport in window coordinates.
 **/
void
ViewerGL::getViewportSize(double &width,
                          double &height) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    QMutexLocker l(&_imp->zoomCtxMutex);
    width = _imp->zoomCtx.screenWidth();
    height = _imp->zoomCtx.screenHeight();
}

/**
 * @brief Returns the pixel scale of the viewport.
 **/
void
ViewerGL::getPixelScale(double &xScale,
                        double &yScale) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    QMutexLocker l(&_imp->zoomCtxMutex);
    xScale = _imp->zoomCtx.screenPixelWidth();
    yScale = _imp->zoomCtx.screenPixelHeight();
}

#ifdef OFX_EXTENSIONS_NATRON
double
ViewerGL::getScreenPixelRatio() const
{
    return devicePixelRatio();
}
#endif

/**
 * @brief Returns the colour of the background (i.e: clear color) of the viewport.
 **/
void
ViewerGL::getBackgroundColour(double &r,
                              double &g,
                              double &b) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    r = _imp->clearColor.redF();
    g = _imp->clearColor.greenF();
    b = _imp->clearColor.blueF();
}

/**
 * @brief Returns the font height, i.e: the height of the highest letter for this font
 **/
int
ViewerGL::getWidgetFontHeight() const
{
    return fontMetrics().height();
}

/**
 * @brief Returns for a string the estimated pixel size it would take on the widget
 **/
int
ViewerGL::getStringWidthForCurrentFont(const std::string& string) const
{
    return fontMetrics().horizontalAdvance(QString::fromUtf8(string.c_str()));
}

void
ViewerGL::makeOpenGLcontextCurrent()
{
    // Legacy OverlaySupport entry point; Vulkan node overlays are migrated separately.
}

void
ViewerGL::removeGUI()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (_imp->viewerTab && _imp->viewerTab->getGui()) {
        _imp->viewerTab->discardInternalNodePointer();
        _imp->viewerTab->getGui()->removeViewerTab(_imp->viewerTab, true, true);
    }
}

ViewIdx
ViewerGL::getCurrentView() const
{
    // MT-SAFE

    ///protected in viewerTab (which is const)
    return _imp->viewerTab->getCurrentView();
}

ViewerInstance *
ViewerGL::getInternalNode() const
{
    return _imp->viewerTab->getInternalNode();
}

ViewerTab *
ViewerGL::getViewerTab() const
{
    return _imp->viewerTab;
}

bool
ViewerGL::pickColorInternal(double x,
                            double y,
                            bool /*pickInput*/)
{

//#pragma message WARN("Todo: use pickInput")
    float r, g, b, a;
    QPointF imgPos;
    {
        QMutexLocker l(&_imp->zoomCtxMutex);
        imgPos = _imp->zoomCtx.toZoomCoordinates(x, y);
    }

    _imp->lastPickerPos = imgPos;
    bool linear = appPTR->getCurrentSettings()->getColorPickerLinear();
    bool ret = false;

    for (int i = 0; i < 2; ++i) {
        // imgPos must be in canonical coordinates
        unsigned int mmLevel;
        bool picked = getColorAt(imgPos.x(), imgPos.y(), linear, i, &r, &g, &b, &a, &mmLevel);

        if (picked) {
            if (i == 0) {
                _imp->viewerTab->getGui()->setColorPickersColor(r, g, b, a);
            }

            _imp->infoViewer[i]->setColorApproximated(mmLevel > 0);
            _imp->infoViewer[i]->setColorValid(true);

            if (!_imp->infoViewer[i]->colorVisible()) {
                _imp->infoViewer[i]->showColorInfo();
            }

            _imp->infoViewer[i]->setColor(r, g, b, a);
            ret = true;
            {
                OfxRGBAColourD interactColor = {r, g, b, a};
                setParametricParamsPickerColor(interactColor, true, true);
            }
        } else {
            _imp->infoViewer[i]->setColorValid(false);
            setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
        }
    }

    return ret;
}

// used for the ctrl-click color picker (not the information bar at the bottom of the viewer)
bool
ViewerGL::pickColor(double x,
                    double y,
                    bool pickInput)
{
    bool isSync = _imp->viewerTab->isViewersSynchroEnabled();

    if (isSync) {
        bool res = false;
        const std::list<ViewerTab *> &allViewers = _imp->viewerTab->getGui()->getViewersList();

        for (std::list<ViewerTab * >::const_iterator it = allViewers.begin(); it != allViewers.end(); ++it) {
            bool ret = (*it)->getViewer()->pickColorInternal(x, y, pickInput);

            if ((*it)->getViewer() == this) {
                res = ret;
            }
        }

        return res;
    } else {
        return pickColorInternal(x, y, pickInput);
    }
}

void
ViewerGL::updateInfoWidgetColorPicker(const QPointF & imgPos,
                                      const QPoint & widgetPos)
{
    NodePtr rotoPaintNode;
    RotoStrokeItemPtr curStroke;
    bool isDrawing;

    _imp->viewerTab->getGui()->getApp()->getActiveRotoDrawingStroke(&rotoPaintNode, &curStroke, &isDrawing);

    if (!isDrawing) {
        for (int i = 0; i < 2; ++i) {
            const RectD& rod = getRoD(i);
            RectD canonicalDispW = getCanonicalFormat(i);
            updateInfoWidgetColorPickerInternal(imgPos, widgetPos, width(), height(), rod, canonicalDispW, i);
        }
    }
}

void
ViewerGL::updateInfoWidgetColorPickerInternal(const QPointF & imgPos,
        const QPoint & widgetPos,
        int width,
        int height,
        const RectD & rod, // in canonical coordinates
        const RectD & dispW, // in canonical coordinates
        int texIndex)
{
    if (!_imp->viewerTab || !_imp->viewerTab->getGui() || _imp->viewerTab->getGui()->isGUIFrozen()) {
        return;
    }

    const std::list<Histogram *> &histograms = _imp->viewerTab->getGui()->getHistograms();

    if (_imp->displayTextures[texIndex].isVisible &&
        (imgPos.x() >= rod.left()) &&
        (imgPos.x() < rod.right()) &&
        (imgPos.y() >= rod.bottom()) &&
        (imgPos.y() < rod.top()) &&
        (widgetPos.x() >= 0) && (widgetPos.x() < width) &&
        (widgetPos.y() >= 0) && (widgetPos.y() < height)) {
        ///if the clip to project format is enabled, make sure it is in the project format too
        if (isClippingImageToFormat() &&
            ((imgPos.x() < dispW.left()) ||
             (imgPos.x() >= dispW.right()) ||
             (imgPos.y() < dispW.bottom()) ||
             (imgPos.y() >= dispW.top()))) {
            if (_imp->infoViewer[texIndex]->colorVisible() && _imp->pickerState == ePickerStateInactive) {
                _imp->infoViewer[texIndex]->hideColorInfo();
            }

            for (std::list<Histogram * >::const_iterator it = histograms.begin(); it != histograms.end(); ++it) {
                if ((*it)->getViewerTextureInputDisplayed() == texIndex) {
                    (*it)->hideViewerCursor();
                }
            }

            if (texIndex == 0 && _imp->pickerState == ePickerStateInactive) {
                setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
            }
        } else {
            if (_imp->pickerState == ePickerStateInactive) {
                //if ( !_imp->viewerTab->getInternalNode()->getRenderEngine()->hasThreadsWorking() ) {
                updateColorPicker(texIndex, widgetPos.x(), widgetPos.y());
                // }
            } else if ((_imp->pickerState == ePickerStatePoint) || (_imp->pickerState == ePickerStateRectangle)) {
                if (!_imp->infoViewer[texIndex]->colorVisible()) {
                    _imp->infoViewer[texIndex]->showColorInfo();
                }
            } else {
                ///unkwn state
                assert(false);
            }

            // Show the picker on parametric params without updating the color value
            if (texIndex == 0) {
                setParametricParamsPickerColor(OfxRGBAColourD(), false, true);
            }
        }
    } else {
        if (_imp->infoViewer[texIndex]->colorVisible() && _imp->pickerState == ePickerStateInactive) {
            _imp->infoViewer[texIndex]->hideColorInfo();
        }

        for (std::list<Histogram * >::const_iterator it = histograms.begin(); it != histograms.end(); ++it) {
            if ((*it)->getViewerTextureInputDisplayed() == texIndex) {
                (*it)->hideViewerCursor();
            }
        }

        if (texIndex == 0 && _imp->pickerState == ePickerStateInactive) {
            setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
        }
    }

    double par = _imp->displayTextures[texIndex].pixelAspectRatio;
    QPoint imgPosPixel;
    imgPosPixel.rx() = std::floor(imgPos.x() / par);
    imgPosPixel.ry() = std::floor(imgPos.y());
    _imp->infoViewer[texIndex]->setMousePos(imgPosPixel);
    //if ( !_imp->infoViewer[texIndex]->mouseVisible() ) { // done in enterEvent
    //    _imp->infoViewer[texIndex]->showMouseInfo();
    //}
} // ViewerGL::updateInfoWidgetColorPickerInternal

void
ViewerGL::updateRectangleColorPicker()
{
    bool isSync = _imp->viewerTab->isViewersSynchroEnabled();

    if (isSync) {
        const std::list<ViewerTab *> &allViewers = _imp->viewerTab->getGui()->getViewersList();

        for (std::list<ViewerTab * >::const_iterator it = allViewers.begin(); it != allViewers.end(); ++it) {
            (*it)->getViewer()->updateRectangleColorPickerInternal();
        }
    } else {
        updateRectangleColorPickerInternal();
    }
}

void
ViewerGL::updateRectangleColorPickerInternal()
{
    float r, g, b, a;
    bool linear = appPTR->getCurrentSettings()->getColorPickerLinear();
    QPointF topLeft = _imp->pickerRect.topLeft();
    QPointF btmRight = _imp->pickerRect.bottomRight();
    RectD rect;

    rect.set_left(std::min(topLeft.x(), btmRight.x()));
    rect.set_right(std::max(topLeft.x(), btmRight.x()));
    rect.set_bottom(std::min(topLeft.y(), btmRight.y()));
    rect.set_top(std::max(topLeft.y(), btmRight.y()));

    for (int i = 0; i < 2; ++i) {
        unsigned int mm;
        bool picked = getColorAtRect(rect, linear, i, &r, &g, &b, &a, &mm);

        if (picked) {
            if (i == 0) {
                _imp->viewerTab->getGui()->setColorPickersColor(r, g, b, a);
            }

            _imp->infoViewer[i]->setColorValid(true);

            if (!_imp->infoViewer[i]->colorVisible()) {
                _imp->infoViewer[i]->showColorInfo();
            }

            _imp->infoViewer[i]->setColorApproximated(mm > 0);
            _imp->infoViewer[i]->setColor(r, g, b, a);

            OfxRGBAColourD c = {r, g, b, a};
            setParametricParamsPickerColor(c, true, true);
        } else {
            _imp->infoViewer[i]->setColorValid(false);
            setParametricParamsPickerColor(OfxRGBAColourD(), false, false);
        }
    }
}

void
ViewerGL::resetWipeControls()
{
    RectD rod;

    if (_imp->displayTextures[1].isVisible) {
        rod = getRoD(1);
    } else if (_imp->displayTextures[0].isVisible) {
        rod = getRoD(0);
    } else {
        rod = _imp->displayTextures[0].format;
    }

    {
        QMutexLocker l(&_imp->wipeControlsMutex);
        _imp->wipeCenter.setX((rod.x1 + rod.x2) / 2.);
        _imp->wipeCenter.setY((rod.y1 + rod.y2) / 2.);
        _imp->wipeAngle = 0;
        _imp->mixAmount = 1.;
    }
}

bool
ViewerGL::isWipeHandleVisible() const
{
    return _imp->viewerTab->getCompositingOperator() != eViewerCompositingOperatorNone;
}

void
ViewerGL::setZoomOrPannedSinceLastFit(bool enabled)
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    _imp->zoomOrPannedSinceLastFit = enabled;
}

bool
ViewerGL::getZoomOrPannedSinceLastFit() const
{
    QMutexLocker l(&_imp->zoomCtxMutex);

    return _imp->zoomOrPannedSinceLastFit;
}

ViewerCompositingOperatorEnum
ViewerGL::getCompositingOperator() const
{
    return _imp->viewerTab->getCompositingOperator();
}

void
ViewerGL::setCompositingOperator(ViewerCompositingOperatorEnum op)
{
    _imp->viewerTab->setCompositingOperator(op);
}

void
ViewerGL::getTextureColorAt(int x,
                            int y,
                            double *r,
                            double *g,
                            double *b,
                            double *a)
{
    *r = *g = *b = *a = 0;
    const QImage frame = grabFrame();
    const QPointF point = toWidgetCoordinates(QPointF(x, y)) * devicePixelRatioF();

    if (frame.rect().contains(point.toPoint())) {
        const QColor pixel = frame.pixelColor(point.toPoint());
        *r = pixel.redF();
        *g = pixel.greenF();
        *b = pixel.blueF();
        *a = pixel.alphaF();
    }
}

void
ViewerGL::getSelectionRectangle(double &left,
                                double &right,
                                double &bottom,
                                double &top) const
{
    QPointF topLeft = _imp->selectionRectangle.topLeft();
    QPointF btmRight = _imp->selectionRectangle.bottomRight();

    left = std::min(topLeft.x(), btmRight.x());
    right = std::max(topLeft.x(), btmRight.x());
    bottom = std::min(topLeft.y(), btmRight.y());
    top = std::max(topLeft.y(), btmRight.y());
}

TimeLinePtr
ViewerGL::getTimeline() const
{
    return _imp->viewerTab->getTimeLine();
}

void
ViewerGL::onCheckerboardSettingsChanged()
{
    update();
}

// used by the RAII class OGLContextSaver
void
ViewerGL::saveOpenGLContext()
{
    // Legacy OverlaySupport entry point; Vulkan node overlays are migrated separately.
}

// used by the RAII class OGLContextSaver
void
ViewerGL::restoreOpenGLContext()
{
    // Legacy OverlaySupport entry point; Vulkan node overlays are migrated separately.
}

void
ViewerGL::clearLastRenderedTexture()
{
    {
        QMutexLocker l(&_imp->lastRenderedImageMutex);
        U64 toUnRegister = 0;

        for (int i = 0; i < 2; ++i) {
            for (U32 j = 0; j < _imp->displayTextures[i].lastRenderedTiles.size(); ++j) {
                _imp->displayTextures[i].lastRenderedTiles[j].reset();
            }

            toUnRegister += _imp->displayTextures[i].memoryHeldByLastRenderedImages;
        }

        if (toUnRegister > 0) {
            getInternalNode()->unregisterPluginMemory(toUnRegister);
        }
    }
}

ImagePtr
ViewerGL::getLastRenderedImage(int textureIndex) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!getInternalNode()->getNode()->isActivated()) {
        return ImagePtr();
    }

    QMutexLocker l(&_imp->lastRenderedImageMutex);

    for (U32 i = 0; i < _imp->displayTextures[textureIndex].lastRenderedTiles.size(); ++i) {
        ImagePtr& mipmap = _imp->displayTextures[textureIndex].lastRenderedTiles[i];

        if (mipmap) {
            if (mipmap->getStorageMode() == eStorageModeVulkan) {
                mipmap = Image::convertToStorage(mipmap, eStorageModeRAM, nullptr);
            }

            return mipmap;
        }
    }

    return ImagePtr();
}

ImagePtr
ViewerGL::getLastRenderedImageByMipmapLevel(int textureIndex,
        unsigned int mipmapLevel) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!getInternalNode()->getNode()->isActivated()) {
        return ImagePtr();
    }

    QMutexLocker l(&_imp->lastRenderedImageMutex);
    assert(_imp->displayTextures[textureIndex].lastRenderedTiles.size() > mipmapLevel);

    // Histogram and color-picker consumers request CPU pixels explicitly.
    // Keep one lazy snapshot per displayed level; ordinary display does not
    // download the Engine image and repeated picks reuse this snapshot.
    auto cpuImage = [&](unsigned int level) {
        ImagePtr& image = _imp->displayTextures[textureIndex].lastRenderedTiles[level];

        if (image && image->getStorageMode() == eStorageModeVulkan) {
            image = Image::convertToStorage(image, eStorageModeRAM, nullptr);
        }

        return image;
    };

    ImagePtr mipmap = _imp->displayTextures[textureIndex].lastRenderedTiles[mipmapLevel];

    if (mipmap) {
        return cpuImage(mipmapLevel);
    }

    //Find an image at higher scale
    if (mipmapLevel > 0) {
        for (int i = (int)mipmapLevel - 1; i >= 0; --i) {
            mipmap = _imp->displayTextures[textureIndex].lastRenderedTiles[i];

            if (mipmap) {
                return cpuImage(i);
            }
        }
    }

    //Find an image at lower scale
    for (U32 i = mipmapLevel + 1; i < _imp->displayTextures[textureIndex].lastRenderedTiles.size(); ++i) {
        mipmap = _imp->displayTextures[textureIndex].lastRenderedTiles[i];

        if (mipmap) {
            return cpuImage(i);
        }
    }

    return ImagePtr();
}

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif
unsigned int
ViewerGL::getMipmapLevelCombinedToZoomFactor() const
{
    if (!getInternalNode()) {
        return 0;
    }

    unsigned int mmLvl = getInternalNode()->getMipmapLevel();
    double factor = getZoomFactor();

    if (factor > 1) {
        factor = 1;
    }

    mmLvl = std::max((double)mmLvl, -std::ceil(std::log(factor) / M_LN2));

    return mmLvl;
}

unsigned int
ViewerGL::getCurrentMipmapLevel() const
{
    return getMipmapLevelCombinedToZoomFactor();
}

template <typename PIX, int maxValue>
static
bool
getColorAtInternal(const ImagePtr& image,
                   int x,
                   int y,             // in pixel coordinates
                   bool forceLinear,
                   const Color::Lut* srcColorSpace,
                   const Color::Lut* dstColorSpace,
                   float *r,
                   float *g,
                   float *b,
                   float *a)
{
    if (image->getBounds().contains(x, y)) {
        Image::ReadAccess racc(image.get());
        const PIX* pix = (const PIX*)racc.pixelAt(x, y);

        if (!pix) {
            return false;
        }

        int nComps = image->getComponents().getNumComponents();

        if (nComps >= 4) {
            *r = pix[0] * (1.f / maxValue);
            *g = pix[1] * (1.f / maxValue);
            *b = pix[2] * (1.f / maxValue);
            *a = pix[3] * (1.f / maxValue);
        } else if (nComps == 3) {
            *r = pix[0] * (1.f / maxValue);
            *g = pix[1] * (1.f / maxValue);
            *b = pix[2] * (1.f / maxValue);
            *a = 1.;
        } else if (nComps == 2) {
            *r = pix[0] * (1.f / maxValue);
            *g = pix[1] * (1.f / maxValue);
            *b = 1.;
            *a = 1.;
        } else {
            *r = *g = *b = *a = pix[0] * (1.f / maxValue);
        }


        ///convert to linear
        if (srcColorSpace) {
            *r = srcColorSpace->fromColorSpaceFloatToLinearFloat(*r);
            *g = srcColorSpace->fromColorSpaceFloatToLinearFloat(*g);
            *b = srcColorSpace->fromColorSpaceFloatToLinearFloat(*b);
        }

        if (!forceLinear && dstColorSpace) {
            ///convert to dst color space
            float from[3];
            from[0] = *r;
            from[1] = *g;
            from[2] = *b;
            float to[3];
            dstColorSpace->to_float_planar(to, from, 3);
            *r = to[0];
            *g = to[1];
            *b = to[2];
        }

        return true;
    }


    return false;
} // getColorAtInternal

bool
ViewerGL::getColorAt(double x,
                     double y,           // x and y in canonical coordinates
                     bool forceLinear,
                     int textureIndex,
                     float *r,
                     float *g,
                     float *b,
                     float *a,
                     unsigned int *imgMmlevel)                               // output values
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    assert(r && g && b && a);
    assert(textureIndex == 0 || textureIndex == 1);


    unsigned int mipmapLevel = (unsigned int)getMipmapLevelCombinedToZoomFactor();
    ImagePtr image = getLastRenderedImageByMipmapLevel(textureIndex, mipmapLevel);


    if (!image) {
        return false;
    }

    ImageBitDepthEnum depth = image->getBitDepth();
    ViewerColorSpaceEnum srcCS = _imp->viewerTab->getGui()->getApp()->getDefaultColorSpaceForBitDepth(depth);
    const Color::Lut* dstColorSpace;
    const Color::Lut* srcColorSpace;

    if ((srcCS == _imp->displayingImageLut)
        && ((_imp->displayingImageLut == eViewerColorSpaceLinear) || !forceLinear)) {
        // identity transform
        srcColorSpace = 0;
        dstColorSpace = 0;
    } else {
        if (image->getComponents().isColorPlane()) {
            srcColorSpace = ViewerInstance::lutFromColorspace(srcCS);
            dstColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
        } else {
            srcColorSpace = dstColorSpace = 0;
        }
    }

    const double par = image->getPixelAspectRatio();
    double scale = 1. / (1 << image->getMipmapLevel());

    ///Convert to pixel coords
    int xPixel = std::floor(x  * scale / par);
    int yPixel = std::floor(y * scale);
    bool gotval;

    switch (depth) {
    case eImageBitDepthByte:
        gotval = getColorAtInternal<unsigned char, 255>(image,
                 xPixel, yPixel,
                 forceLinear,
                 srcColorSpace,
                 dstColorSpace,
                 r, g, b, a);
        break;

    case eImageBitDepthShort:
        gotval = getColorAtInternal<unsigned short, 65535>(image,
                 xPixel, yPixel,
                 forceLinear,
                 srcColorSpace,
                 dstColorSpace,
                 r, g, b, a);
        break;

    case eImageBitDepthFloat:
        gotval = getColorAtInternal<float, 1>(image,
                                              xPixel, yPixel,
                                              forceLinear,
                                              srcColorSpace,
                                              dstColorSpace,
                                              r, g, b, a);
        break;

    default:
        gotval = false;
        break;
    }

    *imgMmlevel = image->getMipmapLevel();

    return gotval;
} // getColorAt

bool
ViewerGL::getColorAtRect(const RectD &rect, // rectangle in canonical coordinates
                         bool forceLinear,
                         int textureIndex,
                         float *r,
                         float *g,
                         float *b,
                         float *a,
                         unsigned int *imgMm)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    assert(r && g && b && a);
    assert(textureIndex == 0 || textureIndex == 1);

    unsigned int mipmapLevel = (unsigned int)getMipmapLevelCombinedToZoomFactor();
    ImagePtr image = getLastRenderedImageByMipmapLevel(textureIndex, mipmapLevel);

    if (image) {
        mipmapLevel = image->getMipmapLevel();
        *imgMm = mipmapLevel;
    }

    ///Convert to pixel coords
    RectI rectPixel;
    rectPixel.x1 = int(std::floor(rect.left())) >> mipmapLevel;
    rectPixel.y1 = int(std::floor(rect.bottom())) >> mipmapLevel;
    rectPixel.x2 = int(std::floor(rect.right())) >> mipmapLevel;
    rectPixel.y2 = int(std::floor(rect.top())) >> mipmapLevel;
    assert(rect.bottom() <= rect.top() && rect.left() <= rect.right());
    assert(rectPixel.y1 <= rectPixel.y2 && rectPixel.x1 <= rectPixel.x2);
    double rSum = 0.;
    double gSum = 0.;
    double bSum = 0.;
    double aSum = 0.;

    if (!image) {
        return false;
    }


    ImageBitDepthEnum depth = image->getBitDepth();
    ViewerColorSpaceEnum srcCS = _imp->viewerTab->getGui()->getApp()->getDefaultColorSpaceForBitDepth(depth);
    const Color::Lut* dstColorSpace;
    const Color::Lut* srcColorSpace;

    if ((srcCS == _imp->displayingImageLut) && ((_imp->displayingImageLut == eViewerColorSpaceLinear) || !forceLinear)) {
        // identity transform
        srcColorSpace = 0;
        dstColorSpace = 0;
    } else {
        srcColorSpace = ViewerInstance::lutFromColorspace(srcCS);
        dstColorSpace = ViewerInstance::lutFromColorspace(_imp->displayingImageLut);
    }

    unsigned long area = 0;

    for (int yPixel = rectPixel.bottom(); yPixel < rectPixel.top(); ++yPixel) {
        for (int xPixel = rectPixel.x1; xPixel < rectPixel.x2; ++xPixel) {
            float rPix, gPix, bPix, aPix;
            bool gotval = false;

            switch (depth) {
            case eImageBitDepthByte:
                gotval = getColorAtInternal<unsigned char, 255>(image,
                         xPixel, yPixel,
                         forceLinear,
                         srcColorSpace,
                         dstColorSpace,
                         &rPix, &gPix, &bPix, &aPix);
                break;

            case eImageBitDepthShort:
                gotval = getColorAtInternal<unsigned short, 65535>(image,
                         xPixel, yPixel,
                         forceLinear,
                         srcColorSpace,
                         dstColorSpace,
                         &rPix, &gPix, &bPix, &aPix);
                break;

            case eImageBitDepthHalf:
                break;

            case eImageBitDepthFloat:
                gotval = getColorAtInternal<float, 1>(image,
                                                      xPixel, yPixel,
                                                      forceLinear,
                                                      srcColorSpace,
                                                      dstColorSpace,
                                                      &rPix, &gPix, &bPix, &aPix);
                break;

            case eImageBitDepthNone:
                break;
            }

            if (gotval) {
                rSum += rPix;
                gSum += gPix;
                bSum += bPix;
                aSum += aPix;
                ++area;
            }
        }
    }

    if (area > 0) {
        *r = rSum / area;
        *g = gSum / area;
        *b = bSum / area;
        *a = aSum / area;

        return true;
    }

    return false;
} // getColorAtRect

int
ViewerGL::getCurrentlyDisplayedTime() const
{
    QMutexLocker k(&_imp->lastRenderedImageMutex);

    if (_imp->displayTextures[0].isVisible) {
        return _imp->displayTextures[0].time;
    } else {
        return _imp->viewerTab->getTimeLine()->currentFrame();
    }
}

void
ViewerGL::getViewerFrameRange(int* first,
                              int *last) const
{
    _imp->viewerTab->getTimelineBounds(first, last);
}

double
ViewerGL::currentTimeForEvent(QInputEvent* e)
{
    // timestamp() is usually in milliseconds
    if (e->timestamp()) {
        return (double)e->timestamp() / 1000000;
    }

    // Qt 4 has no event timestamp, use gettimeofday (defined in Timer.cpp for windows)
    struct timeval now;
    gettimeofday(&now, 0);

    return now.tv_sec + now.tv_usec / 1000000.0;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ViewerGL.cpp"
