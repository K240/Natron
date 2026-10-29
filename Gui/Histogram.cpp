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

#include "Histogram.h"

#include <algorithm> // min, max
#include <cmath>
#include <QPainter>
#include <QResizeEvent>
#include <QStringList>
#include <QVariant>
#include <stdexcept>

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QCheckBox>
#include <QSplitter>
GCC_DIAG_UNUSED_PRIVATE_FIELD_OFF
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QMouseEvent>
GCC_DIAG_UNUSED_PRIVATE_FIELD_ON
#include <QDebug>
#include <QApplication>
#include <QToolButton>
#include <QActionGroup>

#include <QWindow>

#include "Engine/HistogramCPU.h"
#include "Engine/Image.h"
#include "Engine/OverlayDrawList.h"
#include "Gui/OverlayRasterizer.h"
#include "Engine/Node.h"
#include "Engine/ViewerInstance.h"

#include "Gui/ActionShortcuts.h"
#include "Gui/ClickableLabel.h"
#include "Gui/ComboBox.h"
#include "Gui/Gui.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiDefines.h"
#include "Gui/GuiMacros.h"
#include "Gui/Menu.h"
#include "Gui/NodeGraph.h"
#include "Gui/ViewerGL.h"
#include "Gui/ViewerTab.h"
#include "Gui/ZoomContext.h"
#include "Gui/ticks.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

enum EventStateEnum {
    eEventStateDraggingView = 0,
    eEventStateZoomingView,
    eEventStateNone
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

struct HistogramPrivate {
    Q_DECLARE_TR_FUNCTIONS(Histogram)

public:
    HistogramPrivate(Histogram* widget)
        : mainLayout(NULL)
        , rightClickMenu(NULL)
        , histogramSelectionMenu(NULL)
        , histogramSelectionGroup(NULL)
        , viewerCurrentInputMenu(NULL)
        , viewerCurrentInputGroup(NULL)
        , modeActions(0)
        , modeMenu(NULL)
        , fullImage(NULL)
        , filterActions(0)
        , filterMenu(NULL)
        , widget(widget)
        , mode(Histogram::eDisplayModeRGB)
        , oldClick()
        , zoomCtx(0.000001, 1000000.)
        , state(eEventStateNone)
        , hasBeenModifiedSinceResize(false)
        , _baseAxisColor(118, 215, 90, 255)
        , _scaleColor(67, 123, 52, 255)
        , _textFont()
        , drawCoordinates(false)
        , xCoordinateStr()
        , rValueStr()
        , gValueStr()
        , bValueStr()
        , filterSize(0)
        , histogramThread()
        , histogram1()
        , histogram2()
        , histogram3()
        , pixelsCount(0)
        , vmin(0)
        , vmax(0)
        , binsCount(0)
        , mipmapLevel(0)
        , hasImage(false)
        , sizeH()
        , showViewerPicker(false)
        , viewerPickerColor()
    {
    }

    ImagePtr getHistogramImage(RectI* imagePortion) const;

    void showMenu(const QPoint & globalPos);

    void drawScale();

    void drawPicker();

    void drawWarnings();

    void drawMissingImage();

    void updatePicker(double x);

    void drawViewerPicker();

    void drawHistogramCPU();

    //////////////////////////////////
    // data members

    QVBoxLayout *mainLayout;

    ///////// OPTIONS
    Menu *rightClickMenu;
    QMenu *histogramSelectionMenu;
    QActionGroup *histogramSelectionGroup;
    Menu *viewerCurrentInputMenu;
    QActionGroup *viewerCurrentInputGroup;
    QActionGroup *modeActions;
    Menu *modeMenu;
    QAction *fullImage;
    QActionGroup *filterActions;
    Menu *filterMenu;
    Histogram *widget;
    Histogram::DisplayModeEnum mode;
    QPoint oldClick; /// the last click pressed, in widget coordinates [ (0,0) == top left corner ]
    ZoomContext zoomCtx;
    EventStateEnum state;
    bool hasBeenModifiedSinceResize; //< true if the user panned or zoomed since the last resize
    QColor _baseAxisColor;
    QColor _scaleColor;
    std::unique_ptr<QFont> _textFont;
    OverlayDrawList drawCommands;
    unsigned long long histogramGeneration = 0;
    int computedMode = -1;
    bool drawCoordinates;
    QString xCoordinateStr;
    QString rValueStr, gValueStr, bValueStr;
    int filterSize;

    HistogramCPU histogramThread;

    ///up to 3 histograms (in the RGB) case. FOr all other cases just histogram1 is used.
    std::vector<float> histogram1;
    std::vector<float> histogram2;
    std::vector<float> histogram3;
    unsigned int pixelsCount;
    double vmin, vmax; //< the x range of the histogram
    unsigned int binsCount;
    unsigned int mipmapLevel;
    bool hasImage;

    QSize sizeH;
    bool showViewerPicker;
    std::vector<double> viewerPickerColor;
};

Histogram::Histogram(Gui* gui)
    : QWidget(gui)
    , PanelWidget(this, gui)
    , _imp(new HistogramPrivate(this))
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setProperty("graphicsBackend", QString::fromUtf8("Raster"));
    QObject::connect(&_imp->histogramThread, SIGNAL(histogramProduced()), this, SLOT(onCPUHistogramComputed()));

//    QDesktopWidget* desktop = QApplication::desktop();
//    _imp->sizeH = desktop->screenGeometry().size();
    _imp->sizeH = QSize(10000, 10000);

    _imp->rightClickMenu = new Menu(this);
    //_imp->rightClickMenu->setFont( QFont(appFont,appFontSize) );

    _imp->histogramSelectionMenu = new Menu(tr("Viewer target"), _imp->rightClickMenu);
    //_imp->histogramSelectionMenu->setFont( QFont(appFont,appFontSize) );
    _imp->rightClickMenu->addAction(_imp->histogramSelectionMenu->menuAction());

    _imp->histogramSelectionGroup = new QActionGroup(_imp->histogramSelectionMenu);

    _imp->viewerCurrentInputMenu = new Menu(tr("Viewer input"), _imp->rightClickMenu);
    //_imp->viewerCurrentInputMenu->setFont( QFont(appFont,appFontSize) );
    _imp->rightClickMenu->addAction(_imp->viewerCurrentInputMenu->menuAction());

    _imp->viewerCurrentInputGroup = new QActionGroup(_imp->viewerCurrentInputMenu);

    QAction* inputAAction = new QAction(_imp->viewerCurrentInputMenu);
    inputAAction->setText(tr("Input A"));
    inputAAction->setData(0);
    inputAAction->setCheckable(true);
    inputAAction->setChecked(true);
    QObject::connect(inputAAction, SIGNAL(triggered()), this, SLOT(computeHistogramAndRefresh()));
    _imp->viewerCurrentInputGroup->addAction(inputAAction);
    _imp->viewerCurrentInputMenu->addAction(inputAAction);

    QAction* inputBAction = new QAction(_imp->viewerCurrentInputMenu);
    inputBAction->setText(tr("Input B"));
    inputBAction->setData(1);
    inputBAction->setCheckable(true);
    inputBAction->setChecked(false);
    QObject::connect(inputBAction, SIGNAL(triggered()), this, SLOT(computeHistogramAndRefresh()));
    _imp->viewerCurrentInputGroup->addAction(inputBAction);
    _imp->viewerCurrentInputMenu->addAction(inputBAction);

    _imp->modeMenu = new Menu(tr("Display mode"), _imp->rightClickMenu);
    //_imp->modeMenu->setFont( QFont(appFont,appFontSize) );
    _imp->rightClickMenu->addAction(_imp->modeMenu->menuAction());

    _imp->fullImage = new QAction(_imp->rightClickMenu);
    _imp->fullImage->setText(tr("Full image"));
    _imp->fullImage->setCheckable(true);
    _imp->fullImage->setChecked(false);
    QObject::connect(_imp->fullImage, SIGNAL(triggered()), this, SLOT(computeHistogramAndRefresh()));
    _imp->rightClickMenu->addAction(_imp->fullImage);

    _imp->filterMenu = new Menu(tr("Smoothing"), _imp->rightClickMenu);
    //_imp->filterMenu->setFont( QFont(appFont,appFontSize) );
    _imp->rightClickMenu->addAction(_imp->filterMenu->menuAction());

    _imp->modeActions = new QActionGroup(_imp->modeMenu);
    QAction* rgbAction = new QAction(_imp->modeMenu);
    rgbAction->setText(QString::fromUtf8("RGB"));
    rgbAction->setData(0);
    _imp->modeActions->addAction(rgbAction);

    QAction* aAction = new QAction(_imp->modeMenu);
    aAction->setText(QString::fromUtf8("A"));
    aAction->setData(1);
    _imp->modeActions->addAction(aAction);

    QAction* yAction = new QAction(_imp->modeMenu);
    yAction->setText(QString::fromUtf8("Y"));
    yAction->setData(2);
    _imp->modeActions->addAction(yAction);

    QAction* rAction = new QAction(_imp->modeMenu);
    rAction->setText(QString::fromUtf8("R"));
    rAction->setData(3);
    _imp->modeActions->addAction(rAction);

    QAction* gAction = new QAction(_imp->modeMenu);
    gAction->setText(QString::fromUtf8("G"));
    gAction->setData(4);
    _imp->modeActions->addAction(gAction);

    QAction* bAction = new QAction(_imp->modeMenu);
    bAction->setText(QString::fromUtf8("B"));
    bAction->setData(5);
    _imp->modeActions->addAction(bAction);
    QList<QAction *> actions = _imp->modeActions->actions();

    for (int i = 0; i < actions.size(); ++i) {
        _imp->modeMenu->addAction(actions.at(i));
    }

    QObject::connect(_imp->modeActions, SIGNAL(triggered(QAction*)), this, SLOT(onDisplayModeChanged(QAction*)));

    _imp->filterActions = new QActionGroup(_imp->filterMenu);
    QAction* noSmoothAction = new QAction(_imp->filterActions);
    noSmoothAction->setText(tr("Small"));
    noSmoothAction->setData(1);
    noSmoothAction->setCheckable(true);
    noSmoothAction->setChecked(true);
    _imp->filterActions->addAction(noSmoothAction);

    QAction* size3Action = new QAction(_imp->filterActions);
    size3Action->setText(tr("Medium"));
    size3Action->setData(3);
    size3Action->setCheckable(true);
    size3Action->setChecked(false);
    _imp->filterActions->addAction(size3Action);

    QAction* size5Action = new QAction(_imp->filterActions);
    size5Action->setText(tr("High"));
    size5Action->setData(5);
    size5Action->setCheckable(true);
    size5Action->setChecked(false);
    _imp->filterActions->addAction(size5Action);

    actions = _imp->filterActions->actions();

    for (int i = 0; i < actions.size(); ++i) {
        _imp->filterMenu->addAction(actions.at(i));
    }

    QObject::connect(_imp->filterActions, SIGNAL(triggered(QAction*)), this, SLOT(onFilterChanged(QAction*)));
    QObject::connect(getGui(), SIGNAL(viewersChanged()), this, SLOT(populateViewersChoices()));
    populateViewersChoices();
}

Histogram::~Histogram()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

}

int
Histogram::getViewerTextureInputDisplayed() const
{
    int textureIndex = 0;

    if (_imp->viewerCurrentInputGroup) {
        QAction* selectedInputAction = _imp->viewerCurrentInputGroup->checkedAction();

        if (selectedInputAction) {
            textureIndex = selectedInputAction->data().toInt();
        }
    }

    return textureIndex;
}

ImagePtr HistogramPrivate::getHistogramImage(RectI* imagePortion) const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    bool useImageRoD = fullImage->isChecked();
    int index = 0;
    std::string viewerName;
    QAction* selectedHistAction = histogramSelectionGroup->checkedAction();

    if (selectedHistAction) {
        index = selectedHistAction->data().toInt();
        viewerName = selectedHistAction->text().toStdString();
    }

    int textureIndex = 0;
    QAction* selectedInputAction = viewerCurrentInputGroup->checkedAction();

    if (selectedInputAction) {
        textureIndex = selectedInputAction->data().toInt();
    }

    ViewerTab* viewer = 0;

    if (index == 0) {
        //no viewer selected
        imagePortion->clear();

        return ImagePtr();
    } else if (index == 1) {
        //current viewer
        viewer = widget->getGui()->getActiveViewer();
    } else {
        ImagePtr ret;
        const std::list<ViewerTab *> &viewerTabs = widget->getGui()->getViewersList();

        for (std::list<ViewerTab * >::const_iterator it = viewerTabs.begin(); it != viewerTabs.end(); ++it) {
            if ((*it)->getInternalNode()->getScriptName_mt_safe() == viewerName) {
                viewer = *it;
                break;
            }
        }
    }

    ImagePtr image;

    if (viewer) {
        image = viewer->getViewer()->getLastRenderedImageByMipmapLevel(textureIndex, viewer->getInternalNode()->getMipmapLevelFromZoomFactor());
    }

    if (!useImageRoD) {
        if (viewer) {
            RectI bounds;
            double par = 1.;
            unsigned int mipmapLevel = 0;

            if (image) {
                bounds = image->getBounds();
                par = image->getPixelAspectRatio();
                mipmapLevel = image->getMipmapLevel();
            }

            *imagePortion = viewer->getViewer()->getImageRectangleDisplayed(bounds, par, mipmapLevel);
        }
    } else {
        if (image) {
            *imagePortion = image->getBounds();
        }
    }

    return image;
} // getHistogramImage

void
HistogramPrivate::showMenu(const QPoint & globalPos)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    rightClickMenu->exec(globalPos);
}

void
Histogram::populateViewersChoices()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    QString currentSelection;
    assert(_imp->histogramSelectionGroup);
    QAction* checkedAction = _imp->histogramSelectionGroup->checkedAction();

    if (checkedAction) {
        currentSelection = checkedAction->text();
    }

    delete _imp->histogramSelectionGroup;
    _imp->histogramSelectionGroup = new QActionGroup(_imp->histogramSelectionMenu);

    _imp->histogramSelectionMenu->clear();

    QAction* noneAction = new QAction(_imp->histogramSelectionGroup);
    noneAction->setText(QString::fromUtf8("-"));
    noneAction->setData(0);
    noneAction->setCheckable(true);
    noneAction->setChecked(false);
    _imp->histogramSelectionGroup->addAction(noneAction);
    _imp->histogramSelectionMenu->addAction(noneAction);

    QAction* currentAction = new QAction(_imp->histogramSelectionGroup);
    currentAction->setText(tr("Current Viewer"));
    currentAction->setData(1);
    currentAction->setCheckable(true);
    currentAction->setChecked(false);
    _imp->histogramSelectionGroup->addAction(currentAction);
    _imp->histogramSelectionMenu->addAction(currentAction);

    const std::list<ViewerTab *> &viewerTabs = getGui()->getViewersList();
    int c = 2;

    for (std::list<ViewerTab * >::const_iterator it = viewerTabs.begin(); it != viewerTabs.end(); ++it) {
        if ((*it)->getInternalNode()->getNode()->isActivated()) {
            QAction* ac = new QAction(_imp->histogramSelectionGroup);
            ac->setText(QString::fromUtf8((*it)->getInternalNode()->getScriptName_mt_safe().c_str()));
            ac->setCheckable(true);
            ac->setChecked(false);
            ac->setData(c);
            _imp->histogramSelectionGroup->addAction(ac);
            _imp->histogramSelectionMenu->addAction(ac);
            ++c;
        }
    }

    _imp->histogramSelectionGroup->blockSignals(true);

    if (!currentSelection.isEmpty()) {
        QList<QAction *> actions = _imp->histogramSelectionGroup->actions();

        for (int i = 0; i < actions.size(); ++i) {
            if (actions.at(i)->text() == currentSelection) {
                actions.at(i)->setChecked(true);
            }
        }
    } else {
        currentAction->setChecked(true);
    }

    _imp->histogramSelectionGroup->blockSignals(false);

    QObject::connect(_imp->histogramSelectionGroup, SIGNAL(triggered(QAction*)), this, SLOT(onCurrentViewerChanged(QAction*)));
} // populateViewersChoices

void
Histogram::onCurrentViewerChanged(QAction*)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    computeHistogramAndRefresh();
}

void
Histogram::onViewerImageChanged(ViewerGL* viewer,
                                int texIndex,
                                bool hasImageBackend)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (viewer && hasImageBackend) {
        QString viewerName = QString::fromUtf8(viewer->getInternalNode()->getScriptName_mt_safe().c_str());
        ViewerTab* lastSelectedViewer = getGui()->getNodeGraph()->getLastSelectedViewer();
        QString currentViewerName;

        if (lastSelectedViewer) {
            currentViewerName = QString::fromUtf8(lastSelectedViewer->getInternalNode()->getScriptName_mt_safe().c_str());
        }

        QAction* selectedHistAction = _imp->histogramSelectionGroup->checkedAction();

        if (selectedHistAction) {
            int actionIndex = selectedHistAction->data().toInt();

            if (((actionIndex == 1) && (lastSelectedViewer == viewer->getViewerTab()))
                || ((actionIndex > 1) && (selectedHistAction->text() == viewerName))) {
                QAction* currentInput = _imp->viewerCurrentInputGroup->checkedAction();

                if (currentInput && (currentInput->data().toInt() == texIndex)) {
                    computeHistogramAndRefresh();

                    return;
                } else {
                    return;
                }
            }
        }
    }

    _imp->hasImage = false;
    update();
}

QSize
Histogram::sizeHint() const
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    return _imp->sizeH;
}

void
Histogram::onFilterChanged(QAction* action)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    _imp->filterSize = action->data().toInt();
    computeHistogramAndRefresh();
}

void
Histogram::onDisplayModeChanged(QAction* action)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    _imp->mode = (Histogram::DisplayModeEnum)action->data().toInt();
    computeHistogramAndRefresh();
}

void
Histogram::paintEvent(QPaintEvent* /*event*/)
{
    assert(qApp && qApp->thread() == QThread::currentThread());
    QPainter painter(this);
    painter.fillRect(rect(), Qt::black);

    if (width() <= 1 || height() <= 1 || !std::isfinite(_imp->zoomCtx.factor()) || _imp->zoomCtx.factor() <= 0) {
        return;
    }


    _imp->_textFont.reset(new QFont(appFont, appFontSize));
    painter.setFont(*_imp->_textFont);
    _imp->drawCommands.clear();
    unsigned commandCount = 0;
    QStringList labels;
    auto flush = [&](bool antialiasing = true) {
        commandCount += unsigned(_imp->drawCommands.commands().size());

        for (const auto& command : _imp->drawCommands.commands()) {
            if (command.primitive == OverlayPrimitive::Text) {
                labels.push_back(QString::fromUtf8(command.text.data(), int(command.text.size())));
            }
        }

        rasterizeOverlayCommands(_imp->drawCommands, [this](const QPointF & point) {
            return _imp->zoomCtx.toWidgetCoordinates(point.x(), point.y());
        }, painter, antialiasing);
        _imp->drawCommands.clear();
    };
    _imp->drawScale();
    flush();

    if (_imp->hasImage) {
        // RGB histogram channels add to white where their distributions overlap.
        painter.setCompositionMode(QPainter::CompositionMode_Plus);

        if (_imp->computedMode == int(_imp->mode)) {
            _imp->drawHistogramCPU();
        }

        // Partial coverage before saturated Plus composition would tint RGB overlaps.
        flush(false);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

        if (_imp->drawCoordinates) {
            _imp->drawPicker();
        }

        _imp->drawWarnings();

        if (_imp->showViewerPicker) {
            _imp->drawViewerPicker();
        }
    } else {
        _imp->drawMissingImage();
    }

    flush();
    setProperty("histogramViewport", QRectF(_imp->zoomCtx.left(), _imp->zoomCtx.bottom(),
                                            _imp->zoomCtx.right() - _imp->zoomCtx.left(), _imp->zoomCtx.top() - _imp->zoomCtx.bottom()));
    setProperty("histogramHasImage", _imp->hasImage);
    setProperty("histogramMode", int(_imp->mode));
    setProperty("histogramComputedMode", _imp->computedMode);
    setProperty("histogramGeneration", qulonglong(_imp->histogramGeneration));
    setProperty("histogramBinCount", _imp->binsCount);
    setProperty("histogramPixelCount", _imp->pixelsCount);
    setProperty("histogramDataRange", QPointF(_imp->vmin, _imp->vmax));
    setProperty("histogramCommandCount", commandCount);
    setProperty("histogramFontPointSize", _imp->_textFont->pointSizeF());
    setProperty("histogramLabels", labels);
    setProperty("histogramViewerPicker", _imp->showViewerPicker);
    setProperty("histogramFilterSize", _imp->filterSize);
    QVariantList peaks, samples;

    for (const auto * values : {
             &_imp->histogram1, &_imp->histogram2, &_imp->histogram3
         }) {
        if (!values->empty() && _imp->binsCount) {
            const auto maximum = std::max_element(values->begin(), values->end());
            peaks.push_back(_imp->vmin + (std::distance(values->begin(), maximum) + 0.5) * (_imp->vmax - _imp->vmin) / _imp->binsCount);
            QVariantList neighbors;
            const int peak = int(std::distance(values->begin(), maximum));

            for (int index = std::max(0, peak - 5); index <= std::min(int(values->size()) - 1, peak + 5); ++index) {
                neighbors.push_back(double((*values)[index]));
            }

            samples.push_back(neighbors);
        }
    }
    setProperty("histogramPeaks", peaks);
    setProperty("histogramPeakSamples", samples);
}

void
Histogram::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    // QWidget and mouse events use logical coordinates; Qt handles physical DPI.
    _imp->zoomCtx.setScreenSize(std::max(width(), 1), std::max(height(), 1));

    if (!_imp->hasBeenModifiedSinceResize) {
        _imp->zoomCtx.fill(0., 1., 0., 10.);
    }

    computeHistogramAndRefresh();
}

void
Histogram::mousePressEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    takeClickFocus();

    ////
    // middle button: scroll view
    if (buttonDownIsMiddle(e)) {
        _imp->state = eEventStateDraggingView;
        _imp->oldClick = e->pos();
    } else if (buttonDownIsRight(e)) {
        _imp->showMenu(e->globalPos());
    } else if (((e->buttons() & Qt::MiddleButton) &&
                ((buttonMetaAlt(e) == Qt::AltModifier) || (e->buttons() & Qt::LeftButton))) ||
               ((e->buttons() & Qt::LeftButton) &&
                (buttonMetaAlt(e) == (Qt::AltModifier | Qt::MetaModifier)))) {
        // Alt + middle or left + middle or Meta + Alt + left = zoom
        _imp->state = eEventStateZoomingView;
        _imp->oldClick = e->pos();

        return;
    }
}

void
Histogram::mouseMoveEvent(QMouseEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    QPointF newClickZoom = _imp->zoomCtx.toZoomCoordinates(e->x(), e->y());
    QPointF oldClickZoom = _imp->zoomCtx.toZoomCoordinates(_imp->oldClick.x(), _imp->oldClick.y());

    _imp->oldClick = e->pos();
    _imp->drawCoordinates = true;

    double dx = (oldClickZoom.x() - newClickZoom.x());
    double dy = (oldClickZoom.y() - newClickZoom.y());

    switch (_imp->state) {
    case eEventStateDraggingView:
        _imp->zoomCtx.translate(dx, dy);
        _imp->hasBeenModifiedSinceResize = true;
        computeHistogramAndRefresh();
        break;

    case eEventStateZoomingView: {
        int delta = 2 * ((e->x() - _imp->oldClick.x()) - (e->y() - _imp->oldClick.y()));
        const double zoomFactor_min = 0.000001;
        const double zoomFactor_max = 1000000.;
        double zoomFactor;
        double scaleFactor = std::pow(NATRON_WHEEL_ZOOM_PER_DELTA, delta);  // no need to use ipow() here, because the result is not cast to int
        QPointF zoomCenter = _imp->zoomCtx.toZoomCoordinates(e->x(), e->y());

        // Wheel: zoom values and time, keep point under mouse
        zoomFactor = _imp->zoomCtx.factor() * scaleFactor;

        if (zoomFactor <= zoomFactor_min) {
            zoomFactor = zoomFactor_min;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        } else if (zoomFactor > zoomFactor_max) {
            zoomFactor = zoomFactor_max;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        }

        _imp->zoomCtx.zoom(zoomCenter.x(), zoomCenter.y(), scaleFactor);

        _imp->hasBeenModifiedSinceResize = true;

        computeHistogramAndRefresh();
        break;
    }

    case eEventStateNone:
        _imp->updatePicker(newClickZoom.x());
        update();
        break;
    }

    QWidget::mouseMoveEvent(e);
} // Histogram::mouseMoveEvent

void
HistogramPrivate::updatePicker(double x)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    xCoordinateStr = QString::fromUtf8("x=") + QString::number(x, 'f', 6);
    const double binSize = binsCount ? (vmax - vmin) / binsCount : 0;
    int index = -1;

    if (binSize > 0 && std::isfinite(x)) {
        const double position = (x - vmin) / binSize;

        if (position >= 0 && position < binsCount) {
            index = int(position);
        }
    }

    auto sample = [index](const std::vector<float> &values) {
        return index >= 0 && size_t(index) < values.size() ? values[index] : 0.f;
    };
    rValueStr.clear();
    gValueStr.clear();
    bValueStr.clear();

    if (mode == Histogram::eDisplayModeRGB) {
        float r = sample(histogram1);
        float g = sample(histogram2);
        float b = sample(histogram3);
        rValueStr = QString::fromUtf8("r=") + QString::number(r);
        gValueStr = QString::fromUtf8("g=") + QString::number(g);
        bValueStr = QString::fromUtf8("b=") + QString::number(b);
    } else if (mode == Histogram::eDisplayModeY) {
        float y = sample(histogram1);
        rValueStr = QString::fromUtf8("y=") + QString::number(y);
    } else if (mode == Histogram::eDisplayModeA) {
        float a = sample(histogram1);
        rValueStr = QString::fromUtf8("a=") + QString::number(a);
    } else if (mode == Histogram::eDisplayModeR) {
        float r = sample(histogram1);
        rValueStr = QString::fromUtf8("r=") + QString::number(r);
    } else if (mode == Histogram::eDisplayModeG) {
        float g = sample(histogram1);
        gValueStr = QString::fromUtf8("g=") + QString::number(g);
    } else if (mode == Histogram::eDisplayModeB) {
        float b = sample(histogram1);
        bValueStr = QString::fromUtf8("b=") + QString::number(b);
    } else {
        assert(false);
    }
}

void
Histogram::mouseReleaseEvent(QMouseEvent* /*e*/)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    _imp->state = eEventStateNone;
}

void
Histogram::wheelEvent(QWheelEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    // don't handle horizontal wheel (e.g. on trackpad or Might Mouse)
    if (e->angleDelta().x() != 0) {
        return;
    }

    const double zoomFactor_min = 0.000001;
    const double zoomFactor_max = 1000000.;
    const double par_min = 0.000001;
    const double par_max = 1000000.;
    double zoomFactor;
    double par;
    double scaleFactor = std::pow(NATRON_WHEEL_ZOOM_PER_DELTA, e->angleDelta().y());
    QPointF zoomCenter = _imp->zoomCtx.toZoomCoordinates(e->position().x(), e->position().y());

    if (modCASIsControlShift(e)) {
        // Alt + Shift + Wheel: zoom values only, keep point under mouse
        zoomFactor = _imp->zoomCtx.factor() * scaleFactor;

        if (zoomFactor <= zoomFactor_min) {
            zoomFactor = zoomFactor_min;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        } else if (zoomFactor > zoomFactor_max) {
            zoomFactor = zoomFactor_max;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        }

        par = _imp->zoomCtx.aspectRatio() / scaleFactor;

        if (par <= par_min) {
            par = par_min;
            scaleFactor = par / _imp->zoomCtx.aspectRatio();
        } else if (par > par_max) {
            par = par_max;
            scaleFactor = par / _imp->zoomCtx.aspectRatio();
        }

        _imp->zoomCtx.zoomy(zoomCenter.x(), zoomCenter.y(), scaleFactor);
    } else if (modCASIsControl(e)) {
        // Alt + Wheel: zoom time only, keep point under mouse
        par = _imp->zoomCtx.aspectRatio() * scaleFactor;

        if (par <= par_min) {
            par = par_min;
            scaleFactor = par / _imp->zoomCtx.aspectRatio();
        } else if (par > par_max) {
            par = par_max;
            scaleFactor = par / _imp->zoomCtx.aspectRatio();
        }

        _imp->zoomCtx.zoomx(zoomCenter.x(), zoomCenter.y(), scaleFactor);
    } else {
        // Wheel: zoom values and time, keep point under mouse
        zoomFactor = _imp->zoomCtx.factor() * scaleFactor;

        if (zoomFactor <= zoomFactor_min) {
            zoomFactor = zoomFactor_min;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        } else if (zoomFactor > zoomFactor_max) {
            zoomFactor = zoomFactor_max;
            scaleFactor = zoomFactor / _imp->zoomCtx.factor();
        }

        _imp->zoomCtx.zoom(zoomCenter.x(), zoomCenter.y(), scaleFactor);
    }

    _imp->hasBeenModifiedSinceResize = true;

    computeHistogramAndRefresh();
} // wheelEvent

void
Histogram::keyPressEvent(QKeyEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    Qt::KeyboardModifiers modifiers = e->modifiers();
    Qt::Key key = (Qt::Key)e->key();
    bool accept = true;

    if (isKeybind(kShortcutGroupViewer, kShortcutIDActionFitViewer, modifiers, key)) {
        _imp->hasBeenModifiedSinceResize = false;
        _imp->zoomCtx.fill(0., 1., 0., 10.);
        computeHistogramAndRefresh();
    } else {
        accept = false;
    }

    if (accept) {
        takeClickFocus();
        e->accept();
    } else {
        handleUnCaughtKeyPressEvent(e);
        QWidget::keyPressEvent(e);
    }
}

void
Histogram::keyReleaseEvent(QKeyEvent* e)
{
    handleUnCaughtKeyUpEvent(e);
    QWidget::keyReleaseEvent(e);
}

void
Histogram::enterEvent(QtCompat::QEnterEvent* e)
{
    enterEventBase();
    QWidget::enterEvent(e);
}

void
Histogram::leaveEvent(QEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    leaveEventBase();
    _imp->drawCoordinates = false;
    QWidget::leaveEvent(e);
}

void
Histogram::showEvent(QShowEvent* e)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    QWidget::showEvent(e);

    if ((width() != 0) && (height() != 0)) {
        computeHistogramAndRefresh(true);
    }
}

void
Histogram::computeHistogramAndRefresh(bool forceEvenIfNotVisible)
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!isVisible() && !forceEvenIfNotVisible) {
        return;
    }

    QPointF btmLeft = _imp->zoomCtx.toZoomCoordinates(0, height() - 1);
    QPointF topRight = _imp->zoomCtx.toZoomCoordinates(width() - 1, 0);
    double vmin = btmLeft.x();
    double vmax = topRight.x();
    assert(vmax > vmin);

    RectI rect;
    ImagePtr image = _imp->getHistogramImage(&rect);

    if (image) {
        _imp->histogramThread.computeHistogram(_imp->mode, image, rect, width(), vmin, vmax, _imp->filterSize);
    } else {
        _imp->hasImage = false;
    }

    QPointF oldClickZoom = _imp->zoomCtx.toZoomCoordinates(_imp->oldClick.x(), _imp->oldClick.y());
    _imp->updatePicker(oldClickZoom.x());

    update();
}

void
Histogram::onCPUHistogramComputed()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    std::vector<float> histogram1, histogram2, histogram3;
    unsigned binsCount, pixelsCount, mipmapLevel;
    double vmin, vmax;
    int mode;

    if (!_imp->histogramThread.getMostRecentlyProducedHistogram(&histogram1, &histogram2, &histogram3,
            &binsCount, &pixelsCount, &mode, &vmin, &vmax, &mipmapLevel)) {
        return;
    }

    const double currentMin = _imp->zoomCtx.toZoomCoordinates(0, height() - 1).x();
    const double currentMax = _imp->zoomCtx.toZoomCoordinates(width() - 1, 0).x();
    const double tolerance = std::max(1., std::abs(currentMax - currentMin)) * 1e-10;
    RectI rect;

    if (mode != int(_imp->mode) || binsCount != unsigned(width()) ||
        std::abs(vmin - currentMin) > tolerance || std::abs(vmax - currentMax) > tolerance ||
        !_imp->getHistogramImage(&rect)) {
        return;
    }

    _imp->histogram1.swap(histogram1);
    _imp->histogram2.swap(histogram2);
    _imp->histogram3.swap(histogram3);
    _imp->binsCount = binsCount;
    _imp->pixelsCount = pixelsCount;
    _imp->vmin = vmin;
    _imp->vmax = vmax;
    _imp->mipmapLevel = mipmapLevel;
    _imp->hasImage = true;
    _imp->computedMode = mode;
    ++_imp->histogramGeneration;
    _imp->updatePicker(_imp->zoomCtx.toZoomCoordinates(_imp->oldClick.x(), _imp->oldClick.y()).x());
    update();
}

double
Histogram::getScreenPixelRatio() const
{
    return devicePixelRatio();
}

void
HistogramPrivate::drawScale()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    QPointF btmLeft = zoomCtx.toZoomCoordinates(0, widget->height() - 1);
    QPointF topRight = zoomCtx.toZoomCoordinates(widget->width() - 1, 0);

    ///don't attempt to draw a scale on a widget with an invalid height
    if (widget->height() <= 1) {
        return;
    }

    QFontMetrics fm(*_textFont);
    const double smallestTickSizePixel = 5.; // tick size (in pixels) for alpha = 0.
    const double largestTickSizePixel = 1000.; // tick size (in pixels) for alpha = 1.

    {

        for (int axis = 0; axis < 2; ++axis) {
            const double rangePixel = (axis == 0) ? widget->width() : widget->height(); // AXIS-SPECIFIC
            const double range_min = (axis == 0) ? btmLeft.x() : btmLeft.y(); // AXIS-SPECIFIC
            const double range_max = (axis == 0) ? topRight.x() : topRight.y(); // AXIS-SPECIFIC
            const double range = range_max - range_min;
            double smallTickSize;
            bool half_tick;
            ticks_size(range_min, range_max, rangePixel, smallestTickSizePixel, &smallTickSize, &half_tick);
            int m1, m2;
            const int ticks_max = 1000;
            double offset;
            ticks_bounds(range_min, range_max, smallTickSize, half_tick, ticks_max, &offset, &m1, &m2);
            std::vector<int> ticks;
            ticks_fill(half_tick, ticks_max, m1, m2, &ticks);
            const double smallestTickSize = range * smallestTickSizePixel / rangePixel;
            const double largestTickSize = range * largestTickSizePixel / rangePixel;
            const double minTickSizeTextPixel = ((axis == 0) ? fm.horizontalAdvance(QLatin1String("00")) : fm.height());
            const double minTickSizeText = range * minTickSizeTextPixel / rangePixel;

            for (int i = m1; i <= m2; ++i) {
                double value = i * smallTickSize + offset;
                const double tickSize = ticks[i - m1] * smallTickSize;
                const double alpha = ticks_alpha(smallestTickSize, largestTickSize, tickSize);

                drawCommands.color(_baseAxisColor.redF(), _baseAxisColor.greenF(), _baseAxisColor.blueF(), alpha);

                drawCommands.lineWidth(1.);
                drawCommands.begin(OverlayPrimitive::Lines);

                if (axis == 0) {
                    drawCommands.vertex(value, btmLeft.y());   // AXIS-SPECIFIC
                    drawCommands.vertex(value, topRight.y());   // AXIS-SPECIFIC
                } else {
                    drawCommands.vertex(btmLeft.x(), value); // AXIS-SPECIFIC
                    drawCommands.vertex(topRight.x(), value); // AXIS-SPECIFIC
                }

                drawCommands.end();

                if (tickSize > minTickSizeText) {
                    const int tickSizePixel = rangePixel * tickSize / range;
                    const QString s = QString::number(value);
                    const double sSizePixel = ((axis == 0) ? fm.horizontalAdvance(s) : fm.height());   // AXIS-SPECIFIC

                    if (tickSizePixel > sSizePixel) {
                        const double sSizeFullPixel = sSizePixel + minTickSizeTextPixel;
                        double alphaText = 1.0; //alpha;

                        if (tickSizePixel < sSizeFullPixel) {
                            // when the text size is between sSizePixel and sSizeFullPixel,
                            // draw it with a lower alpha
                            alphaText *= (tickSizePixel - sSizePixel) / (double)minTickSizeTextPixel;
                        }

                        //alphaText = std::min(alphaText, alpha); // don't draw more opaque than ticks
                        QColor c = _scaleColor;
                        c.setAlpha(255 * alphaText);

                        if (axis == 0) {
                            widget->renderText(value, btmLeft.y(), s, c, *_textFont, Qt::AlignHCenter); // AXIS-SPECIFIC
                        } else {
                            widget->renderText(btmLeft.x(), value, s, c, *_textFont, Qt::AlignVCenter); // AXIS-SPECIFIC
                        }
                    }
                }
            }
        }
    }
} // drawScale

void
HistogramPrivate::drawWarnings()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (mipmapLevel > 0) {
        QFontMetrics fm(*_textFont);
        QString str(tr("Image downscaled"));
        double strWidth = fm.horizontalAdvance(str);
        double strHeight = fm.height();
        QPointF pos = zoomCtx.toZoomCoordinates(widget->width() - strWidth - 10, 5 * strHeight + 30);
        widget->renderText(pos.x(), pos.y(), str, QColor(220, 220, 0), *_textFont);
    }
}

void
HistogramPrivate::drawMissingImage()
{
    QPointF topLeft = zoomCtx.toZoomCoordinates(0, 0);
    QPointF btmRight = zoomCtx.toZoomCoordinates(widget->width(), widget->height());
    QPointF topRight(btmRight.x(), topLeft.y());
    QPointF btmLeft(topLeft.x(), btmRight.y());
    {

        drawCommands.color(0.9, 0.9, 0, 1);
        drawCommands.lineWidth(1.5);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(topLeft.x(), topLeft.y());
        drawCommands.vertex(btmRight.x(), btmRight.y());
        drawCommands.vertex(btmLeft.x(), btmLeft.y());
        drawCommands.vertex(topRight.x(), topRight.y());
        drawCommands.end();
        drawCommands.lineWidth(1.);
    }
    QString txt(tr("Missing image"));
    QFontMetrics fm(*_textFont);
    int strWidth = fm.horizontalAdvance(txt);
    QPointF pos = zoomCtx.toZoomCoordinates(widget->width() / 2. - strWidth / 2., fm.height() + 10);
    widget->renderText(pos.x(), pos.y(), txt, QColor(220, 0, 0), *_textFont);
}

void
HistogramPrivate::drawViewerPicker()
{
    // always running in the main thread

    assert(qApp && qApp->thread() == QThread::currentThread());

    double wHeight = widget->height();
    QPointF topLeft = zoomCtx.toZoomCoordinates(0, 0);
    QPointF btmRight = zoomCtx.toZoomCoordinates(widget->width(), wHeight);

    //QFontMetrics fm(_textFont, 0);
    //double yPos = zoomCtx.toZoomCoordinates(0,wHeight - fm.height() * 2.).y();
    QColor color;
    double imgColor[4] = {0., 0., 0., 0.};

    for (std::size_t i = 0; i < (std::size_t)std::min((int)viewerPickerColor.size(), 4); ++i) {
        imgColor[i] = viewerPickerColor[i];
    }

    if (mode == Histogram::eDisplayModeY) {
        drawCommands.color(0.398979, 0.398979, 0.398979);
        double luminance = 0.299 * imgColor[0] + 0.587 * imgColor[1] + 0.114 * imgColor[2];
        drawCommands.lineWidth(2.);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(luminance, topLeft.y());
        drawCommands.vertex(luminance, btmRight.y());
        drawCommands.end();
    } else if (mode == Histogram::eDisplayModeR) {
        drawCommands.color(0.398979, 0.398979, 0.398979);
        drawCommands.lineWidth(2.);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(imgColor[0], topLeft.y());
        drawCommands.vertex(imgColor[0], btmRight.y());
        drawCommands.end();
    } else if (mode == Histogram::eDisplayModeG) {
        drawCommands.color(0.398979, 0.398979, 0.398979);
        drawCommands.lineWidth(2.);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(imgColor[1], topLeft.y());
        drawCommands.vertex(imgColor[1], btmRight.y());
        drawCommands.end();
    } else if (mode == Histogram::eDisplayModeB) {
        drawCommands.color(0.398979, 0.398979, 0.398979);
        drawCommands.lineWidth(2.);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(imgColor[2], topLeft.y());
        drawCommands.vertex(imgColor[2], btmRight.y());
        drawCommands.end();
    } else if (mode == Histogram::eDisplayModeA) {
        drawCommands.color(0.398979, 0.398979, 0.398979);
        drawCommands.lineWidth(2.);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(imgColor[3], topLeft.y());
        drawCommands.vertex(imgColor[3], btmRight.y());
        drawCommands.end();
    } else if (mode == Histogram::eDisplayModeRGB) {
        drawCommands.color(0.851643, 0.196936, 0.196936);
        drawCommands.lineWidth(2.);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(imgColor[0], topLeft.y());
        drawCommands.vertex(imgColor[0], btmRight.y());

        drawCommands.color(0, 0.654707, 0);
        drawCommands.vertex(imgColor[1], topLeft.y());
        drawCommands.vertex(imgColor[1], btmRight.y());

        drawCommands.color(0.345293, 0.345293, 1);
        drawCommands.vertex(imgColor[2], topLeft.y());
        drawCommands.vertex(imgColor[2], btmRight.y());
        drawCommands.end();

        drawCommands.color(0.398979, 0.398979, 0.398979);
        drawCommands.begin(OverlayPrimitive::Lines);
        drawCommands.vertex(imgColor[3], topLeft.y());
        drawCommands.vertex(imgColor[3], btmRight.y());
        drawCommands.end();
    }

    drawCommands.lineWidth(1.);
} // HistogramPrivate::drawViewerPicker

void
HistogramPrivate::drawPicker()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    QFontMetrics fm(*_textFont, 0);
    double strWidth = std::max(std::max(std::max(fm.horizontalAdvance(rValueStr), fm.horizontalAdvance(gValueStr)), fm.horizontalAdvance(bValueStr)), fm.horizontalAdvance(xCoordinateStr));
    double strHeight = fm.height();
    QPointF xPos = zoomCtx.toZoomCoordinates(widget->width() - strWidth - 10, strHeight + 10);
    QPointF rPos = zoomCtx.toZoomCoordinates(widget->width() - strWidth - 10, 2 * strHeight + 15);
    QPointF gPos = zoomCtx.toZoomCoordinates(widget->width() - strWidth - 10, 3 * strHeight + 20);
    QPointF bPos = zoomCtx.toZoomCoordinates(widget->width() - strWidth - 10, 4 * strHeight + 25);
    QColor xColor, rColor, gColor, bColor;

    // Text-aware Magic colors (see recipe below):
    // - they all have the same luminance
    // - red, green and blue are complementary, but they sum up to more than 1
    xColor.setRgbF(0.398979, 0.398979, 0.398979);

    switch (mode) {
    case Histogram::eDisplayModeRGB:
        rColor.setRgbF(0.851643, 0.196936, 0.196936);
        break;

    case Histogram::eDisplayModeY:
    case Histogram::eDisplayModeA:
    case Histogram::eDisplayModeR:
    case Histogram::eDisplayModeG:
    case Histogram::eDisplayModeB:
    default:
        rColor = xColor;
        break;
    }

    gColor.setRgbF(0, 0.654707, 0);

    bColor.setRgbF(0.345293, 0.345293, 1);
    widget->renderText(xPos.x(), xPos.y(), xCoordinateStr, xColor, *_textFont);
    widget->renderText(rPos.x(), rPos.y(), rValueStr, rColor, *_textFont);
    widget->renderText(gPos.x(), gPos.y(), gValueStr, gColor, *_textFont);
    widget->renderText(bPos.x(), bPos.y(), bValueStr, bColor, *_textFont);
}

void
HistogramPrivate::drawHistogramCPU()
{
    // always running in the main thread
    assert(qApp && qApp->thread() == QThread::currentThread());
    {
        if (!binsCount || !pixelsCount || vmax <= vmin) {
            return;
        }

        // RGB channels have equal luminance and add to white.

        double binSize = (vmax - vmin) / binsCount;

        drawCommands.lineWidth(1.);
        drawCommands.begin(OverlayPrimitive::Lines);

        for (unsigned int i = 0; i < binsCount; ++i) {
            double binMinX = vmin + i * binSize;

            if (mode == Histogram::eDisplayModeRGB) {
                if (histogram1.empty() || histogram2.empty() || histogram3.empty()) {
                    break;
                }

                double rTotNormalized = ((double)histogram1[i] / (double)pixelsCount) / binSize;
                double gTotNormalized = ((double)histogram2[i] / (double)pixelsCount) / binSize;
                double bTotNormalized = ((double)histogram3[i] / (double)pixelsCount) / binSize;

                // use three colors with equal luminance (0.33), so that the blue is visible and their sum is white
                drawCommands.color(0.711519527404004, 0.164533420851110, 0.164533420851110);
                drawCommands.vertex(binMinX, 0);
                drawCommands.vertex(binMinX,  rTotNormalized);
                drawCommands.color(0., 0.546986106552894, 0.);
                drawCommands.vertex(binMinX, 0);
                drawCommands.vertex(binMinX,  gTotNormalized);
                drawCommands.color(0.288480472595996, 0.288480472595996, 0.835466579148890);
                drawCommands.vertex(binMinX, 0);
                drawCommands.vertex(binMinX,  bTotNormalized);
            } else {
                if (histogram1.empty()) {
                    break;
                }

                double vTotNormalized = (double)histogram1[i] / (double)pixelsCount / binSize;

                // all the following colors have the same luminance (~0.4)
                switch (mode) {
                case Histogram::eDisplayModeR:
                    drawCommands.color(0.851643, 0.196936, 0.196936);
                    break;

                case Histogram::eDisplayModeG:
                    drawCommands.color(0, 0.654707, 0);
                    break;

                case Histogram::eDisplayModeB:
                    drawCommands.color(0.345293, 0.345293, 1);
                    break;

                case Histogram::eDisplayModeA:
                    drawCommands.color(0.398979, 0.398979, 0.398979);
                    break;

                case Histogram::eDisplayModeY:
                    drawCommands.color(0.398979, 0.398979, 0.398979);
                    break;

                default:
                    assert(false);
                    break;
                }

                drawCommands.vertex(binMinX, 0);
                drawCommands.vertex(binMinX,  vTotNormalized);
            }
        }

        drawCommands.end();
    }
} // drawHistogramCPU

void
Histogram::renderText(double x, double y, const QString& text, const QColor& color,
                      const QFont & /*font*/, int flags) const
{
    if (text.isEmpty()) {
        return;
    }

    const auto utf8 = text.toUtf8();
    OverlayDrawScope scope(_imp->drawCommands);
    _imp->drawCommands.color(color.redF(), color.greenF(), color.blueF(), color.alphaF());
    _imp->drawCommands.text(x, y, std::string(utf8.constData(), utf8.size()), flags);
}

void
Histogram::setViewerCursor(const std::vector<double> &pickerColor)
{
    _imp->showViewerPicker = true;
    _imp->viewerPickerColor = pickerColor;
    update();
}

void
Histogram::hideViewerCursor()
{
    if (!_imp->showViewerPicker) {
        return;
    }

    _imp->showViewerPicker = false;
    update();
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_Histogram.cpp"
