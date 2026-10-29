// SPDX-License-Identifier: GPL-2.0-or-later
// Python must precede all standard and Qt headers in Gui translation units.
#include <Python.h>
#include "Gui/ViewerGL.h"
#include "Gui/ViewerGLPrivate.h"
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <cmath>
#include <cstring>
#include "Gpu/RasterViewerRenderer.h"
#include "Engine/Lut.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/Settings.h"
#include "Engine/Node.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ViewerInstance.h"
#include "Engine/VulkanImageContextPool.h"
#include "Engine/VulkanImageStorage.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/ViewerTab.h"
#include "Gui/Gui.h"
#include "Gui/OverlayRasterizer.h"

NATRON_NAMESPACE_ENTER

namespace
{
std::array<float, 4> gpuRectangle(const RectD& rectangle)
{
    return {float(rectangle.x1), float(rectangle.y1), float(rectangle.x2), float(rectangle.y2)};
}

std::array<float, 4> gpuColor(const QColor& color)
{
    return {float(color.redF()), float(color.greenF()), float(color.blueF()), float(color.alphaF())};
}

QRectF widgetRectangle(const ViewerGL* viewer, const RectD& rect)
{
    return QRectF(viewer->toWidgetCoordinates(QPointF(rect.x1, rect.y2)),
                  viewer->toWidgetCoordinates(QPointF(rect.x2, rect.y1))).normalized();
}
}

bool ViewerGL::transferImageFromGPU(const UpdateViewerParams& params, ViewerFrameTexturePtr* texture) try
{
    if (!_imp->canvas) {
        return false;
    }

    const ImagePtr& image = params.colorImage;
    const RectI& roi = params.roi;
    const int textureIndex = params.textureIndex;
    assert(qApp && qApp->thread() == QThread::currentThread());
    assert(textureIndex == 0 || textureIndex == 1);

    if (!image || image->getStorageMode() != eStorageModeVulkan || roi.isNull()) {
        throw std::invalid_argument("Viewer requires a completed native image region.");
    }

    const auto access = image->getReadRights();
    const RectI bounds = access.bounds();
    const auto& native = image->getVulkanImage();

    if (_imp->canvas->device()->lost()) {
        recoverVulkanDisplay(tr("Vulkan display device was lost."));
        texture->reset();
        return false;
    }

    if (native && native->context()->device()->lost()) {
        // A queued frame can arrive after another frame recreated the Viewer.
        // Its pixels are undefined; discard it and pull fresh native inputs.
        texture->reset();
        QMetaObject::invokeMethod(this, [this]() {
            if (auto viewer = getInternalNode()) {
                viewer->renderCurrentFrame(true);
            }
        }, Qt::QueuedConnection);
        return false;
    }

    if (!bounds.contains(roi) || !native || native->context()->device() != _imp->canvas->device()) {
        throw std::invalid_argument("Viewer native image has foreign bounds or device.");
    }

    TextureRect rectangle;
    rectangle.set(roi);
    rectangle.par = image->getPixelAspectRatio();
    rectangle.closestPo2 = 1 << image->getMipmapLevel();
    // Display owns a snapshot. Holding a cache image for sampling would expose
    // subsequent writes and keep its read lock alive throughout playback.
    auto copied = _imp->displayTextures[textureIndex].texture;

    const auto storage = params.depth == eImageBitDepthByte ? Gpu::ViewerStorage::BGRA8 : Gpu::ViewerStorage::RGBA32F;

    if (!copied || copied->rectangle != rectangle || copied->frame.storage() != storage) {
        copied = std::make_shared<ViewerGpuTexture>(_imp->canvas->device(), rectangle, storage);
    }

    if (storage == Gpu::ViewerStorage::BGRA8) {
        if (!_imp->byteDisplayConversion) {
            _imp->byteDisplayConversion = std::make_unique<Gpu::ViewerDisplayConversion>(_imp->canvas->device());
        }

        const bool firstTables = !_imp->byteDisplayTables;

        if (firstTables) {
            _imp->byteDisplayTables = std::make_unique<Gpu::ViewerDisplayTables>();
        }

        if (firstTables || _imp->byteDisplayLut != params.lut) {
            const auto lut = ViewerInstance::lutFromColorspace(params.lut);

            for (uint32_t i = 0; i < _imp->byteDisplayTables->color.size(); ++i) {
                uint32_t bits = i << 16;
                float value;
                std::memcpy(&value, &bits, sizeof(value));
                _imp->byteDisplayTables->color[i] = lut ? lut->toColorSpaceUint8xxFromLinearFloatFast(value) : 0;
            }

            _imp->byteDisplayLut = params.lut;
        }

        if (firstTables || _imp->byteDisplayGamma != params.gamma) {
            for (size_t i = 0; i < _imp->byteDisplayTables->gamma.size(); ++i) {
                _imp->byteDisplayTables->gamma[i] = params.gamma <= 0 ? (i == 1023 ? 1.f : 0.f) :
                                                    float(std::clamp(std::pow(double(i) / 1023, 1 / params.gamma), 0., 1.));
            }

            _imp->byteDisplayGamma = params.gamma;
        }

        Gpu::ViewerDisplayCorrection correction;
        correction.gain = float(params.gain);
        correction.offset = float(params.offset);
        correction.gamma = float(params.gamma);
        correction.linear = params.lut == eViewerColorSpaceLinear;
        correction.opaque = image->getPremultiplication() == eImagePremultiplicationOpaque;
        correction.originX = roi.x1;
        correction.originY = roi.y1;
        native->displayTo(*_imp->byteDisplayConversion, *copied->frame.image(), roi.x1 - bounds.x1, roi.y1 - bounds.y1,
                          roi.width(), roi.height(), correction, *_imp->byteDisplayTables);
    } else {
        native->copyTo(*copied->frame.image(), roi.x1 - bounds.x1, roi.y1 - bounds.y1, roi.width(), roi.height(), 0, 0,
                       image->getPremultiplication() == eImagePremultiplicationOpaque);
    }

    _imp->displayTextures[textureIndex].texture = copied;
    _imp->displayTextures[textureIndex].roiNotRoundedToTileSize = rectangle;
    *texture = copied;
    ++_imp->nativeImageTransfers;
    return true;
} catch (const Gpu::VulkanError& error)
{
    if (error.deviceLost()) {
        texture->reset();
        recoverVulkanDisplay(tr("Native Viewer image transfer: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }

    if (!error.deviceMemoryFailure()) {
        throw;
    }

    texture->reset();
    activateRasterDisplay(tr("Native Viewer image transfer: %1").arg(QString::fromUtf8(error.what())));
    return false;
}

qulonglong ViewerGL::nativeImageTransfers() const
{
    return _imp->nativeImageTransfers;
}

void ViewerGL::initializeVulkan()
{
    // Qt windows and their surfaces are created on the GUI thread. Weak owners
    // let the runtime disappear after the last Viewer, without a shutdown leak.
    static std::weak_ptr<Gpu::QtVulkanInstance> sharedInstance;
    static unsigned viewerSerial = 0;
    setObjectName(QString::fromUtf8("NatronVulkanViewer%1").arg(++viewerSerial));

    // Compare the CPU display without disabling the Engine device used by
    // the isolated benchmark's transfer and memory telemetry.
    if (qEnvironmentVariableIsSet("NATRON_TEST_PROFILE") &&
        qEnvironmentVariable("NATRON_TEST_VIEWER_BACKEND") == QString::fromUtf8("Raster")) {
        return;
    }

    try {
        _imp->vulkanContext = sharedVulkanImageContext();
        const auto& device = _imp->vulkanContext->device();
        _imp->vulkanInstance = sharedInstance.lock();

        if (!_imp->vulkanInstance || _imp->vulkanInstance->native() != device->instance()) {
            _imp->vulkanInstance = Gpu::QtVulkanInstance::adopt(device->instance());
            sharedInstance = _imp->vulkanInstance;
        }

        int index = -1;

        if (qEnvironmentVariableIsSet("NATRON_VULKAN_DEVICE")) {
            bool valid = false;
            index = qEnvironmentVariableIntValue("NATRON_VULKAN_DEVICE", &valid);

            if (!valid || index < 0) {
                throw std::invalid_argument("NATRON_VULKAN_DEVICE must be a nonnegative device index.");
            }
        }

        auto canvas = std::make_unique<Gpu::VulkanCanvas>(_imp->vulkanInstance, index, device);
        _imp->renderer = std::make_unique<Gpu::ViewerRenderer>(canvas->device());
        _imp->canvas = canvas.get();
        _imp->canvas->setObjectName(objectName() + QString::fromUtf8("/surface"));
        _imp->windowContainer = QWidget::createWindowContainer(canvas.release(), this);
        _imp->windowContainer->setFocusPolicy(Qt::NoFocus);
        _imp->windowContainer->setMouseTracking(true);
        auto displayLayout = layout();

        if (!displayLayout) {
            displayLayout = new QVBoxLayout(this);
            displayLayout->setContentsMargins(0, 0, 0, 0);
            displayLayout->setSpacing(0);
        }

        displayLayout->addWidget(_imp->windowContainer);
        _imp->canvas->installEventFilter(this);
        _imp->windowContainer->installEventFilter(this);
        _imp->canvas->setFrameRenderer([this](Gpu::VulkanImage & destination) {
            renderVulkanFrame(destination);
        });
        _imp->canvas->setPresentedHandler([this](uint64_t serial) {
            recordPresentation(serial);
        });
        _imp->canvas->setFailureHandler([this](const Gpu::VulkanError & error) {
            if (error.deviceLost()) {
                recoverVulkanDisplay(tr("Vulkan frame rendering: %1").arg(QString::fromUtf8(error.what())));
                return true;
            }

            if (!error.deviceMemoryFailure()) {
                return false;
            }

            const auto reason = tr("Vulkan frame rendering: %1").arg(QString::fromUtf8(error.what()));
            QMetaObject::invokeMethod(this, [this, reason]() {
                activateRasterDisplay(reason);
            }, Qt::QueuedConnection);
            return true;
        });
        _imp->nativeImageTransferAvailable.store(true);
        _imp->graphicsFailure.clear();
        setToolTip(QString());
        _imp->windowContainer->show();
    } catch (const std::exception& error) {
        _imp->graphicsFailure = QString::fromUtf8(error.what());
        disableVulkanImageRuntime(error.what());
        delete _imp->windowContainer;
        _imp->windowContainer = nullptr;
        _imp->canvas = nullptr;
        _imp->renderer.reset();
        _imp->vulkanInstance.reset();
        _imp->vulkanContext.reset();
        setToolTip(tr("CPU raster display: %1").arg(_imp->graphicsFailure));
    }

}

void ViewerGL::activateRasterDisplay(const QString& reason)
{
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->nativeImageTransferAvailable.exchange(false) && !_imp->canvas) {
        return;
    }

    _imp->graphicsFailure = reason;
    delete _imp->windowContainer;
    _imp->windowContainer = nullptr;
    _imp->canvas = nullptr;
    _imp->renderer.reset();
    _imp->byteDisplayConversion.reset();
    _imp->rasterOverlay.reset();
    _imp->partialUpdateTextures.clear();

    for (auto& info : _imp->displayTextures) {
        info.texture.reset();
        info.isVisible = false;
    }

    _imp->vulkanInstance.reset();
    _imp->vulkanContext.reset();
    setToolTip(tr("CPU raster display: %1").arg(reason));
    update();
    QMetaObject::invokeMethod(this, [this]() {
        if (auto viewer = getInternalNode()) {
            viewer->renderCurrentFrame(true);
        }
    }, Qt::QueuedConnection);
}

void ViewerGL::recoverVulkanDisplay(const QString& reason)
{
    assert(qApp && qApp->thread() == QThread::currentThread());

    if (!_imp->canvas || _imp->deviceRecoveryPending) {
        return;
    }

    const auto generation = _imp->canvas->device()->generation();

    if (_imp->canvas->presentedFrames() > 0) {
        _imp->consecutiveDeviceRecoveries = 0;
    }

    const bool retry = _imp->consecutiveDeviceRecoveries++ == 0;
    _imp->deviceRecoveryPending = true;
    _imp->nativeImageTransferAvailable.store(false);
    // Do not delete a QWindow from its render/failure callback. All old display
    // snapshots are released on this queued GUI turn before initializing anew.
    QMetaObject::invokeMethod(this, [this, generation, reason, retry]() {
        _imp->deviceRecoveryPending = false;

        if (!_imp->canvas || _imp->canvas->device()->generation() != generation) {
            return;
        }

        activateRasterDisplay(reason);

        if (retry) {
            initializeVulkan();
        }
    }, Qt::QueuedConnection);
}

bool ViewerGL::supportsNativeImageTransfer() const
{
    return _imp->nativeImageTransferAvailable.load();
}

QString ViewerGL::graphicsBackend() const
{
    return QString::fromUtf8(_imp->canvas ? "Vulkan" : "Raster");
}
QString ViewerGL::graphicsError() const
{
    return _imp->canvas ? QString::fromStdString(_imp->canvas->error()) : _imp->graphicsFailure;
}
QString ViewerGL::gpuDeviceName() const
{
    return _imp->canvas ? QString::fromUtf8(_imp->canvas->device()->properties().deviceName) : tr("CPU");
}
qulonglong ViewerGL::presentedFrames() const
{
    return _imp->canvas ? _imp->canvas->presentedFrames() : _imp->rasterFrames;
}
qulonglong ViewerGL::sharedDeviceId() const
{
    return _imp->canvas ? reinterpret_cast<qulonglong>(_imp->canvas->device()->handle()) : 0;
}
qulonglong ViewerGL::engineDeviceId() const
{
    return _imp->vulkanContext ? reinterpret_cast<qulonglong>(_imp->vulkanContext->device()->handle()) : 0;
}
uint ViewerGL::validationErrors() const
{
    return _imp->vulkanInstance ? _imp->vulkanInstance->native()->validationErrorCount() : 0;
}
QRectF ViewerGL::canonicalViewport() const
{
    QMutexLocker lock(&_imp->zoomCtxMutex);
    return QRectF(_imp->zoomCtx.left(), _imp->zoomCtx.bottom(),
                  _imp->zoomCtx.right() - _imp->zoomCtx.left(), _imp->zoomCtx.top() - _imp->zoomCtx.bottom());
}
QImage ViewerGL::grabFrame()
{
    if (_imp->canvas) {
        return _imp->canvas->grabFrame();
    }

    renderViewerFrame(nullptr);
    return _imp->rasterFrame;
}

QVariantMap ViewerGL::lastPresentation() const
{
    return _imp->lastPresentation;
}

void ViewerGL::recordPresentation(qulonglong serial)
{
    if (!qEnvironmentVariableIsSet("NATRON_TEST_PROFILE")) {
        return;
    }

    QVariantMap metadata;
    metadata.insert(QString::fromUtf8("serial"), QVariant::fromValue(serial));
    metadata.insert(QString::fromUtf8("backend"), graphicsBackend());
    metadata.insert(QString::fromUtf8("overlays"), _imp->overlay);
    metadata.insert(QString::fromUtf8("nativeTransfers"), QVariant::fromValue(nativeImageTransfers()));
    metadata.insert(QString::fromUtf8("capture"), _imp->canvas && _imp->canvas->captureEnabled());
    metadata.insert(QString::fromUtf8("generation"), QVariant::fromValue<qulonglong>(_imp->canvas ? _imp->canvas->device()->generation() : 0));
    metadata.insert(QString::fromUtf8("surfaceSize"), _imp->canvas ? _imp->canvas->swapchainExtent() :
                    QSize(int(std::lround(width() * devicePixelRatioF())), int(std::lround(height() * devicePixelRatioF()))));
    metadata.insert(QString::fromUtf8("viewport"), canonicalViewport());

    for (int i = 0; i < 2; ++i) {
        const auto& input = _imp->displayTextures[i];
        const QString suffix = QString::number(i);
        metadata.insert(QString::fromUtf8("time") + suffix, input.isVisible ? double(input.time) : -1.);
        metadata.insert(QString::fromUtf8("mipmap") + suffix, input.mipmapLevel);
        metadata.insert(QString::fromUtf8("format") + suffix, QRectF(input.format.x1, input.format.y1, input.format.width(), input.format.height()));
        metadata.insert(QString::fromUtf8("rod") + suffix, QRectF(input.rod.x1, input.rod.y1, input.rod.width(), input.rod.height()));

        if (input.texture) {
            const auto& bounds = input.texture->rectangle;
            metadata.insert(QString::fromUtf8("bounds") + suffix, QRect(bounds.x1, bounds.y1, bounds.width(), bounds.height()));
            metadata.insert(QString::fromUtf8("storage") + suffix,
                            input.texture->frame.storage() == Gpu::ViewerStorage::BGRA8 ? QString::fromUtf8("8u") : QString::fromUtf8("32f"));
        }
    }

    _imp->lastPresentation = metadata;
    Q_EMIT framePresented(metadata);
}

OverlayDrawList *ViewerGL::overlayDrawList()
{
    return _imp->drawingNodeOverlays ? &_imp->nodeOverlayCommands : nullptr;
}
qulonglong ViewerGL::nodeOverlayCommandCount() const
{
    return _imp->nodeOverlayCommands.commands().size();
}
QStringList ViewerGL::nodeOverlayStatus() const
{
    if (!_imp->viewerTab->getGui()) {
        return {};
    }

    NodesList nodes;
    _imp->viewerTab->getGui()->getNodesEntitledForOverlays(nodes);
    QStringList result;

    for (const auto& node : nodes) {
        result.push_back(QString::fromUtf8("%1:host=%2:native=%3").arg(QString::fromStdString(node->getScriptName()))
                         .arg(node->hasHostOverlay()).arg(node->getEffectInstance()->supportsOverlayDrawList()));
    }

    return result;
}

void ViewerGL::update()
{
    QWidget::update();

    if (_imp->canvas) {
        _imp->canvas->requestUpdate();
    }
}

void ViewerGL::paintEvent(QPaintEvent* event)
{
    if (_imp->canvas) {
        QWidget::paintEvent(event);
        _imp->canvas->requestUpdate();
    } else {
        renderViewerFrame(nullptr);
        QPainter painter(this);
        painter.drawImage(rect(), _imp->rasterFrame);
        painter.end();
        recordPresentation(_imp->rasterFrames);
    }
}

bool ViewerGL::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _imp->canvas || watched == _imp->windowContainer) {
        if (event->type() != QEvent::FocusIn && event->type() != QEvent::FocusOut) {
            flushOverlayFocusNotifications();
        }

        if (event->type() == QEvent::KeyPress) {
            // A native window can receive keys while Qt's QWidget focus still
            // points at a parameter editor. Establish overlay focus before
            // keyDown so a subsequent FocusOut always releases held modifiers.
            setFocus(Qt::OtherFocusReason);
            _imp->overlayFocusGainPending = true;
            flushOverlayFocusNotifications();
        }

        switch (event->type()) {
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            // QWindow/container focus transitions must reach the interact even
            // when the QWidget keeps Qt's logical focus. Keep Qt's own native
            // window handling, and deduplicate notifications in the widget.
            QApplication::sendEvent(this, event);
            break;

        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
            setFocus(Qt::MouseFocusReason);
            Q_FALLTHROUGH();

        case QEvent::MouseButtonRelease:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::TabletPress:
        case QEvent::TabletMove:
        case QEvent::TabletRelease:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::Enter:
        case QEvent::Leave:
            // The native window fills the widget; coordinates remain logical
            // pixels so existing ROI, picker, zoom, and pan handlers keep working.
            QApplication::sendEvent(this, event);
            _imp->canvas->setCursor(cursor());
            return true;

        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void ViewerGL::renderVulkanFrame(Gpu::VulkanImage& destination)
{
    renderViewerFrame(&destination);
}

void ViewerGL::renderViewerFrame(Gpu::VulkanImage* destination)
{
    flushOverlayFocusNotifications();
    Gpu::ViewerSettings settings;
    settings.background = gpuColor(_imp->clearColor);
    {
        QMutexLocker lock(&_imp->zoomCtxMutex);

        if (_imp->zoomCtx.right() <= _imp->zoomCtx.left() || _imp->zoomCtx.top() <= _imp->zoomCtx.bottom()) {
            if (destination) {
                destination->clear(settings.background);
            }

            return;
        }

        settings.viewport = {float(_imp->zoomCtx.left()), float(_imp->zoomCtx.bottom()),
                             float(_imp->zoomCtx.right()), float(_imp->zoomCtx.top())
                            };
    }
    ViewerInstance* viewer = getInternalNode();

    if (!viewer || !_imp->viewerTab->getGui()) {
        if (destination) {
            destination->clear(settings.background);
        }

        return;
    }

    const auto operation = _imp->viewerTab->getCompositingOperator();
    settings.operation = static_cast<Gpu::ViewerOperator>(operation);
    settings.checkerboard = _imp->viewerTab->isCheckerboardEnabled();
    auto preferences = appPTR->getCurrentSettings();
    double r, g, b, a;
    preferences->getCheckerboardColor1(&r, &g, &b, &a);
    settings.checkerA = {float(r), float(g), float(b), float(a)};
    preferences->getCheckerboardColor2(&r, &g, &b, &a);
    settings.checkerB = {float(r), float(g), float(b), float(a)};
    settings.checkerSize = std::max(1, int(std::lround(preferences->getCheckerboardTileSize() * devicePixelRatioF())));
    {
        QMutexLocker lock(&_imp->wipeControlsMutex);
        settings.wipe = {float(_imp->wipeCenter.x()), float(_imp->wipeCenter.y()),
                         float(_imp->wipeAngle), float(_imp->mixAmount)
                        };
    }
    const bool clipToFormat = isClippingImageToFormat();
    const bool roiEnabled = isUserRegionOfInterestEnabled();
    const RectD roi = getUserRegionOfInterest();
    const double zoom = getZoomFactor();
    auto input = [&](const TextureInfo & info) {
        Gpu::ViewerInput result;

        if (!info.texture || !info.isVisible) {
            return result;
        }

        const TextureRect& rectangle = info.texture->rectangle;
        const RectD bounds = rectangle.toCanonical_noClipping(info.mipmapLevel, rectangle.par);
        RectD clipped = info.roiNotRoundedToTileSize.toCanonical_noClipping(info.mipmapLevel, rectangle.par);
        clipped = clipped.intersect(info.rod);

        if (clipToFormat) {
            clipped = clipped.intersect(info.format);
        }

        if (roiEnabled) {
            clipped = clipped.intersect(roi);
        }

        result.image = info.texture->frame.image().get();
        result.bounds = gpuRectangle(bounds);
        result.clip = gpuRectangle(clipped);
        result.format = gpuRectangle(info.format);
        result.pixelGrid = {float(rectangle.par * (1 << info.mipmapLevel)), float(1 << info.mipmapLevel),
                            float(rectangle.x1), float(rectangle.y1)
                           };
        result.gain = float(info.gain);
        result.offset = float(info.offset);
        result.gamma = float(info.gamma);
        result.lut = static_cast<Gpu::ViewerLut>(_imp->displayingImageLut);
        result.premult = static_cast<Gpu::ViewerPremult>(info.premult);
        result.transform = info.texture->frame.storage() == Gpu::ViewerStorage::RGBA32F;
        const double scale = zoom * (1 << info.mipmapLevel);
        result.linearFilter = scale < 1 || scale * rectangle.par < 1;
        return result;
    };
    std::array<Gpu::ViewerInput, 2> inputs{input(_imp->displayTextures[0]), input(_imp->displayTextures[1])};
    int activeA, activeB;
    viewer->getActiveInputs(activeA, activeB);

    if (operation == eViewerCompositingOperatorNone ||
        (activeA == activeB && operation != eViewerCompositingOperatorWipeMinus && operation != eViewerCompositingOperatorStackMinus)) {
        inputs[1] = {};
    }

    if (!destination) {
        const uint32_t w = uint32_t(std::lround(width() * devicePixelRatioF()));
        const uint32_t h = uint32_t(std::lround(height() * devicePixelRatioF()));

        if (!w || !h) {
            return;
        }

        auto rasterInput = [&](const TextureInfo & info, const Gpu::ViewerInput & display) {
            Gpu::RasterViewerInput result;

            if (!info.texture || !info.isVisible) {
                return result;
            }

            const auto& pixels = info.texture->frame.pixels();
            result.display = display;
            result.pixels = pixels.empty() ? nullptr : pixels.data();
            result.width = uint32_t(info.texture->rectangle.width());
            result.height = uint32_t(info.texture->rectangle.height());
            result.storage = info.texture->frame.storage();
            return result;
        };
        std::array<Gpu::RasterViewerInput, 2> rasterInputs{
            rasterInput(_imp->displayTextures[0], inputs[0]), rasterInput(_imp->displayTextures[1], inputs[1])};

        if (operation == eViewerCompositingOperatorNone ||
            (activeA == activeB && operation != eViewerCompositingOperatorWipeMinus && operation != eViewerCompositingOperatorStackMinus)) {
            rasterInputs[1] = {};
        }

        std::vector<float> pixels;
        Gpu::renderRasterViewer(rasterInputs, settings, w, h, pixels);
        settings.preserveDestination = true;
        settings.operation = Gpu::ViewerOperator::None;
        settings.checkerboard = false;

        for (const auto& partial : _imp->partialUpdateTextures) {
            Gpu::renderRasterViewer({rasterInput(partial, input(partial)), {}}, settings, w, h, pixels);
        }

        QImage frame(int(w), int(h), QImage::Format_RGBA8888);
        frame.setDevicePixelRatio(devicePixelRatioF());

        for (uint32_t y = 0; y < h; ++y) {
            auto* row = frame.scanLine(int(y));

            for (size_t x = 0; x < size_t(w) * 4; ++x) {
                row[x] = static_cast<unsigned char>(std::lround(std::clamp(pixels[size_t(y) * w * 4 + x], 0.f, 1.f) * 255));
            }
        }

        if (_imp->overlay || _imp->displayPersistentMessage || _imp->ms == eMouseStateSelecting) {
            drawRasterOverlays(frame);
        }

        _imp->rasterFrame = std::move(frame);
        ++_imp->rasterFrames;
        return;
    }

    _imp->renderer->render(inputs, settings, *destination);
    settings.preserveDestination = true;
    settings.operation = Gpu::ViewerOperator::None;
    settings.checkerboard = false;

    for (const auto& partial : _imp->partialUpdateTextures) {
        _imp->renderer->render({input(partial), {}}, settings, *destination);
    }

    if (_imp->overlay || _imp->displayPersistentMessage || _imp->ms == eMouseStateSelecting) {
        QImage raster(int(destination->width()), int(destination->height()), QImage::Format_RGBA8888_Premultiplied);
        raster.setDevicePixelRatio(devicePixelRatioF());
        raster.fill(Qt::transparent);
        drawRasterOverlays(raster);

        if (!_imp->rasterOverlay || _imp->rasterOverlay->width() != destination->width() ||
            _imp->rasterOverlay->height() != destination->height()) {
            _imp->rasterOverlay = std::make_shared<Gpu::VulkanImage>(_imp->canvas->device(), destination->width(),
                                  destination->height(), VK_FORMAT_R8G8B8A8_UNORM);
        }

        _imp->rasterOverlay->upload(raster.constBits(), size_t(raster.sizeInBytes()));
        Gpu::ViewerInput overlay;
        overlay.image = _imp->rasterOverlay.get();
        overlay.bounds = overlay.clip = overlay.format = settings.viewport;
        overlay.transform = false;
        overlay.topDown = true;
        overlay.premult = Gpu::ViewerPremult::Premultiplied;
        _imp->renderer->render({overlay, {}}, settings, *destination);
    }
}

void ViewerGL::drawRasterOverlays(QImage& image)
{
    const QFont overlayFont(appFont, appFontSize);

    if (*_imp->_textFont != overlayFont) {
        *_imp->_textFont = overlayFont;
    }

    QPainter painter(&image);
    QScopedValueRollback<QPainter *> activePainter(_imp->overlayPainter, &painter);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(*_imp->_textFont);
    _imp->nodeOverlayCommands.clear();
    auto rectangle = [&](const RectD & rect, const QColor & color, Qt::PenStyle style) {
        painter.setPen(QPen(color, 1, style));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(widgetRectangle(this, rect));
    };

    if (_imp->overlay) {
        for (int i = 0; i < 2; ++i) {
            if (i == 1 && _imp->viewerTab->getCompositingOperator() == eViewerCompositingOperatorNone) {
                break;
            }

            const RectD format = getCanonicalFormat(i);
            rectangle(format, _imp->displayWindowOverlayColor, Qt::SolidLine);
            renderText(format.right(), format.bottom(), _imp->currentViewerInfo_resolutionOverlay[i],
                       _imp->textRenderingColor, *_imp->_textFont);

            if (_imp->displayTextures[i].isVisible && getRoD(i) != format) {
                const RectD rod = getRoD(i);
                rectangle(rod, _imp->rodOverlayColor, Qt::DashLine);
                renderText(rod.right(), rod.top(), _imp->currentViewerInfo_topRightBBOXoverlay[i], _imp->rodOverlayColor, *_imp->_textFont);
                renderText(rod.left(), rod.bottom(), _imp->currentViewerInfo_btmLeftBBOXoverlay[i], _imp->rodOverlayColor, *_imp->_textFont);
            }
        }

        if (isUserRegionOfInterestEnabled()) {
            const bool building = _imp->buildUserRoIOnNextPress || _imp->ms == eMouseStateBuildingUserRoI;
            const RectD roi = building ? _imp->draggedUserRoI : getUserRegionOfInterest();
            rectangle(roi, QColor(230, 230, 230), _imp->buildUserRoIOnNextPress ? Qt::DashLine : Qt::SolidLine);
            const QRectF widget = widgetRectangle(this, roi);
            const QPointF center = widget.center();
            painter.drawLine(QPointF(widget.left(), center.y()), QPointF(widget.left() - 15, center.y()));
            painter.drawLine(QPointF(widget.right(), center.y()), QPointF(widget.right() + 15, center.y()));
            painter.drawLine(QPointF(center.x(), widget.top()), QPointF(center.x(), widget.top() - 15));
            painter.drawLine(QPointF(center.x(), widget.bottom()), QPointF(center.x(), widget.bottom() + 15));
            painter.drawLine(center - QPointF(15, 0), center + QPointF(15, 0));
            painter.drawLine(center - QPointF(0, 15), center + QPointF(0, 15));
            painter.setBrush(QColor(230, 230, 230));
            const std::array<QPointF, 9> handles{center, widget.topLeft(), widget.topRight(), widget.bottomLeft(), widget.bottomRight(),
                                                 QPointF(center.x(), widget.top()), QPointF(center.x(), widget.bottom()),
                                                 QPointF(widget.left(), center.y()), QPointF(widget.right(), center.y())};

            for (const QPointF& point : handles) {
                painter.drawRect(QRectF(point - QPointF(4, 4), QSizeF(8, 8)));
            }

            painter.setBrush(Qt::NoBrush);
        }

        if (operatorIsWipe(_imp->viewerTab->getCompositingOperator())) {
            double angle, mix;
            QPointF canonicalCenter;
            {
                QMutexLocker lock(&_imp->wipeControlsMutex);
                angle = _imp->wipeAngle;
                mix = _imp->mixAmount;
                canonicalCenter = _imp->wipeCenter;
            }
            const QPointF center = toWidgetCoordinates(canonicalCenter);
            const QPointF axis(std::cos(angle), -std::sin(angle));
            const QPointF normal(-std::sin(angle), -std::cos(angle));
            painter.setPen(QPen(QColor(200, 200, 200), 1.5));
            painter.drawLine(center - axis * WIPE_ROTATE_OFFSET, center + axis * (WIPE_ROTATE_HANDLE_LENGTH - WIPE_ROTATE_OFFSET));
            painter.drawLine(center - normal * WIPE_ROTATE_HANDLE_LENGTH / 2, center + normal * WIPE_ROTATE_HANDLE_LENGTH / 2);
            constexpr double pi = 3.14159265358979323846;
            const double mix1 = angle + pi / 8, mix0 = angle + 3 * pi / 8;
            const double mixAngle = mix * (mix1 - mix0) + mix0;
            const QPointF handle = center + QPointF(std::cos(mixAngle), -std::sin(mixAngle)) * WIPE_MIX_HANDLE_LENGTH;
            painter.drawLine(center, handle);
            painter.setBrush(QColor(200, 200, 200));
            painter.drawEllipse(center, 2.5, 2.5);
            painter.setBrush(_imp->hs == eHoverStateWipeMix ? QColor(0, 255, 0) : QColor(200, 200, 200));
            painter.drawEllipse(handle, 2.5, 2.5);
            painter.setBrush(Qt::NoBrush);
            QPainterPath arc;

            for (int i = 0; i <= 24; ++i) {
                const double current = mix1 + (mix0 - mix1) * i / 24;
                const QPointF point = center + QPointF(std::cos(current), -std::sin(current)) * WIPE_MIX_HANDLE_LENGTH;

                if (!i) {
                    arc.moveTo(point);
                } else {
                    arc.lineTo(point);
                }
            }

            painter.drawPath(arc);
        }

        {
            QScopedValueRollback<bool> drawing(_imp->drawingNodeOverlays, true);
            _imp->viewerTab->drawOverlays(getCurrentlyDisplayedTime(), RenderScale::fromMipmapLevel(getCurrentMipmapLevel()));
        }

        rasterizeOverlayCommands(_imp->nodeOverlayCommands, *this, painter);

        painter.setPen(QPen(QColor(230, 180, 0), 1.5));

        if (_imp->pickerState == ePickerStateRectangle) {
            const QPointF topLeft = toWidgetCoordinates(_imp->pickerRect.topLeft());
            const QPointF bottomRight = toWidgetCoordinates(_imp->pickerRect.bottomRight());
            painter.drawRect(QRectF(topLeft, bottomRight).normalized());
        } else if (_imp->pickerState == ePickerStatePoint) {
            // lastPickerPos is already canonical, including proxy/mipmap scale.
            const QPointF point = toWidgetCoordinates(_imp->lastPickerPos);
            const double radius = std::max(0.5 / devicePixelRatioF(), getZoomFactor() / 2);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(230, 180, 0));
            painter.drawEllipse(point, radius, radius);
        }
    }

    if (_imp->ms == eMouseStateSelecting) {
        const QRectF& selection = _imp->selectionRectangle;
        const QRectF rect(toWidgetCoordinates(selection.topLeft()), toWidgetCoordinates(selection.bottomRight()));
        painter.setPen(QPen(QColor(128, 204, 255, 102), 1.5));
        painter.setBrush(QColor(128, 204, 255, 102));
        painter.drawRect(rect.normalized());
    }

    if (_imp->displayPersistentMessage) {
        const QFontMetrics metrics(*_imp->_textFont);
        const int lineHeight = metrics.height() + 10;
        painter.fillRect(QRect(0, 0, width(), lineHeight * _imp->persistentMessages.size()),
                         _imp->persistentMessageType == 1 ? QColor(128, 0, 0) : QColor(166, 166, 0));
        painter.setPen(_imp->textRenderingColor);

        for (int i = 0; i < _imp->persistentMessages.size(); ++i) {
            painter.drawText(QPoint(20, i * lineHeight + metrics.ascent() + 5), _imp->persistentMessages[i]);
        }
    }
}

NATRON_NAMESPACE_EXIT
