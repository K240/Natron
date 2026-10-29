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

#ifndef Gui_ViewerGLPrivate_h
#define Gui_ViewerGLPrivate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"
#include <atomic>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QMutex>
#include <QSize>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Image.h"

#include "Engine/TextureRect.h"
#include "Engine/OverlayDrawList.h"
#include "Gpu/ViewerFrame.h"
#include "Gpu/ViewerRenderer.h"
#include "Gpu/ViewerDisplayConversion.h"
#include "Gpu/Qt/VulkanCanvas.h"
class QPainter;
#include "Gui/ViewerGL.h"
#include "Gui/ZoomContext.h"
#include "Gui/GuiFwd.h"

#define WIPE_MIX_HANDLE_LENGTH 50.
#define WIPE_ROTATE_HANDLE_LENGTH 100.
#define WIPE_ROTATE_OFFSET 30

#define MAX_MIP_MAP_LEVELS 20

NATRON_NAMESPACE_ENTER

// Viewer state, Vulkan images, and canonical interaction coordinates.


/**
 *@enum MouseStateEnum
 *@brief basic state switching for mouse events
 **/
enum MouseStateEnum {
    eMouseStateSelecting = 0,
    eMouseStateDraggingImage,
    eMouseStateDraggingRoiLeftEdge,
    eMouseStateDraggingRoiRightEdge,
    eMouseStateDraggingRoiTopEdge,
    eMouseStateDraggingRoiBottomEdge,
    eMouseStateDraggingRoiTopLeft,
    eMouseStateDraggingRoiTopRight,
    eMouseStateDraggingRoiBottomRight,
    eMouseStateDraggingRoiBottomLeft,
    eMouseStateDraggingRoiCross,
    eMouseStateBuildingUserRoI,
    eMouseStatePickingColor,
    eMouseStatePickingInputColor,
    eMouseStateBuildingPickerRectangle,
    eMouseStateDraggingWipeCenter,
    eMouseStateDraggingWipeMixHandle,
    eMouseStateRotatingWipeHandle,
    eMouseStateZoomingImage,
    eMouseStateUndefined
};

enum HoverStateEnum {
    eHoverStateNothing = 0,
    eHoverStateWipeMix,
    eHoverStateWipeRotateHandle
};

enum PickerStateEnum {
    ePickerStateInactive = 0,
    ePickerStatePoint,
    ePickerStateRectangle
};

class ViewerGpuTexture : public ViewerFrameTexture
{
public:
    ViewerGpuTexture(const std::shared_ptr<Gpu::VulkanDevice> &device, const TextureRect& rect, Gpu::ViewerStorage storage)
        : rectangle(rect), frame(device,
    {
        rect.x1, rect.y1, rect.x2, rect.y2
    }, storage) {}
    TextureRect rectangle;
    Gpu::ViewerFrame frame;
};

struct TextureInfo {
    TextureInfo()
        : texture()
        , roiNotRoundedToTileSize()
        , gain(1.)
        , gamma(1.)
        , offset(0.)
        , mipmapLevel(0)
        , premult(eImagePremultiplicationOpaque)
        , time(0)
        , rod()
        , format()
        , pixelAspectRatio(1.)
        , lastRenderedTiles(MAX_MIP_MAP_LEVELS)
        , memoryHeldByLastRenderedImages(0)
        , isPartialImage(false)
        , isVisible(false)
    {
    }

    std::shared_ptr<ViewerGpuTexture> texture;
    TextureRect roiNotRoundedToTileSize;
    double gain;
    double gamma;
    double offset;
    unsigned int mipmapLevel;
    ImagePremultiplicationEnum premult;
    SequenceTime time;
    RectD rod;

    RectD format;

    double pixelAspectRatio;

    // Hold shared pointers here because some images might not be held by the cache
    std::vector<ImagePtr> lastRenderedTiles;
    U64 memoryHeldByLastRenderedImages;
    bool isPartialImage;

    // false if this input is disconnected for the viewer
    bool isVisible;
};

struct ViewerGL::Implementation {
    Implementation(ViewerGL* this_,
                   ViewerTab* parent);

    ~Implementation();

    ViewerGL *_this; // link to parent, for access to public methods

    /////////////////////////////////////////////////////////
    // The following are only accessed from the main thread:
    TextureInfo displayTextures[2]; /*!< A pointer to the textures that would be used if A and B are displayed*/
    std::vector<TextureInfo> partialUpdateTextures; /*!< Pointer to the partial rectangle textures overlayed onto the displayed texture when tracking*/
    QVariantMap lastPresentation;
    InfoViewerWidget *infoViewer[2]; /*!< Pointer to the info bar below the viewer holding pixel/mouse/format related info*/
    ViewerTab *const viewerTab; /*!< Pointer to the viewer tab GUI*/
    bool zoomOrPannedSinceLastFit; //< true if the user zoomed or panned the image since the last call to fitToRoD
    QPoint oldClick;
    ViewerColorSpaceEnum displayingImageLut;
    MouseStateEnum ms; /*!< Holds the mouse state*/
    HoverStateEnum hs;
    const QColor textRenderingColor;
    const QColor displayWindowOverlayColor;
    const QColor rodOverlayColor;
    std::unique_ptr<QFont> _textFont;
    bool overlay; /*!< True if the user enabled overlay display*/
    bool updatingTexture;
    QColor clearColor;
    QMenu *menu;
    QStringList persistentMessages;
    int persistentMessageType;
    bool displayPersistentMessage;
    QPoint lastMousePosition; //< in widget coordinates
    QPointF lastDragStartPos; //< in zoom coordinates
    bool hasMovedSincePress;
    bool setColorPickerCursor; //< was the color picker cursor set?

    /////// currentViewerInfo
    QString currentViewerInfo_btmLeftBBOXoverlay[2]; /*!< The string holding the bottom left corner coordinates of the dataWindow*/
    QString currentViewerInfo_topRightBBOXoverlay[2]; /*!< The string holding the top right corner coordinates of the dataWindow*/
    QString currentViewerInfo_resolutionOverlay[2]; /*!< The string holding the resolution overlay, e.g: "1920x1080"*/

    ///////Picker info, used only by the main-thread
    PickerStateEnum pickerState;
    QPointF lastPickerPos;
    QRectF pickerRect;

    // projection info, only used by the main thread

    //////////////////////////////////////////////////////////
    // The following are accessed from various threads
    QMutex userRoIMutex;
    bool userRoIEnabled;
    RectD userRoI; //< in canonical coords
    bool buildUserRoIOnNextPress;
    RectD draggedUserRoI;
    ZoomContext zoomCtx; /*!< All zoom related variables are packed into this object. */
    mutable QMutex zoomCtxMutex; /// protectx zoomCtx*
    QMutex clipToDisplayWindowMutex;
    bool clipToDisplayWindow;
    mutable QMutex wipeControlsMutex;
    double mixAmount; /// the amount of the second input to blend, by default 1.
    double wipeAngle; /// the angle to the X axis
    QPointF wipeCenter; /// the center of the wipe control
    bool wipeInitialized;
    QRectF selectionRectangle;
    mutable QMutex lastRenderedImageMutex; //protects lastRenderedImage & memoryHeldByLastRenderedImages
    QSize sizeH;
    PenType pointerTypeOnPress;
    double pressureOnPress, pressureOnRelease;
    int wheelDeltaSeekFrame; // accumulated wheel delta for frame seeking (crtl+wheel)
    bool isUpdatingTexture;
    bool renderOnPenUp;

    std::shared_ptr<Gpu::QtVulkanInstance> vulkanInstance;
    std::shared_ptr<VulkanImageContext> vulkanContext;
    qulonglong nativeImageTransfers = 0;
    QString graphicsFailure;
    QImage rasterFrame;
    qulonglong rasterFrames = 0;
    Gpu::VulkanCanvas *canvas = nullptr; // Owned by windowContainer.
    // Read by render workers; the native window remains GUI-thread-only.
    std::atomic<bool> nativeImageTransferAvailable{false};
    bool deviceRecoveryPending = false;
    unsigned consecutiveDeviceRecoveries = 0;
    QWidget *windowContainer = nullptr;
    std::unique_ptr<Gpu::ViewerRenderer> renderer;
    std::unique_ptr<Gpu::ViewerDisplayConversion> byteDisplayConversion;
    std::unique_ptr<Gpu::ViewerDisplayTables> byteDisplayTables;
    ViewerColorSpaceEnum byteDisplayLut = eViewerColorSpaceLinear;
    double byteDisplayGamma = 1;
    std::shared_ptr<Gpu::VulkanImage> rasterOverlay;
    QPainter *overlayPainter = nullptr; // Valid only during raster overlay rendering.
    OverlayDrawList nodeOverlayCommands;
    bool drawingNodeOverlays = false;
    bool overlaysHaveFocus = false;
    bool overlayFocusLossPending = false;
    bool overlayFocusGainPending = false;

public:
    bool isNearbyWipeCenter(const QPointF& pos, double zoomScreenPixelWidth, double zoomScreenPixelHeight) const;
    bool isNearbyWipeRotateBar(const QPointF& pos, double zoomScreenPixelWidth, double zoomScreenPixelHeight) const;
    bool isNearbyWipeMixHandle(const QPointF& pos, double zoomScreenPixelWidth, double zoomScreenPixelHeight) const;
    void refreshSelectionRectangle(const QPointF& pos);
};

NATRON_NAMESPACE_EXIT

#endif
