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


#include "TrackerNodeInteract.h"

#include <limits>

#include "Engine/Image.h"
#include "Engine/Lut.h"
#include "Engine/Node.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/TimeLine.h"
#include "Engine/TrackerNode.h"
#include "Engine/TrackerContext.h"
#include "Engine/TrackMarker.h"
#include "Engine/ViewerInstance.h"

NATRON_NAMESPACE_ENTER

TrackerNodePrivate::TrackerNodePrivate(TrackerNode* publicInterface)
    : publicInterface(publicInterface)
    , ui(new TrackerNodeInteract(this))
{
}

TrackerNodePrivate::~TrackerNodePrivate()
{
}

TrackerNodeInteract::TrackerNodeInteract(TrackerNodePrivate* p)
    : _p(p)
    , addTrackButton()
    , trackRangeButton()
    , trackBwButton()
    , trackPrevButton()
    , trackNextButton()
    , trackFwButton()
    , trackAllKeyframesButton()
    , trackCurrentKeyframeButton()
    , clearAllAnimationButton()
    , clearBwAnimationButton()
    , clearFwAnimationButton()
    , updateViewerButton()
    , centerViewerButton()
    , createKeyOnMoveButton()
    , setKeyFrameButton()
    , removeKeyFrameButton()
    , resetOffsetButton()
    , resetTrackButton()
    , showCorrelationButton()
    , clickToAddTrackEnabled(false)
    , modifierAddTrackActive(false)
    , lastMousePos()
    , selectionRectangle()
    , controlDown(0)
    , shiftDown(0)
    , altDown(0)
    , eventState(eMouseStateIdle)
    , hoverState(eDrawStateInactive)
    , interactMarker()
    , trackPreviews()
    , trackRequestsMap()
    , selectedMarkerPreview()
    , selectedMarkerPreviewTime(0)
    , selectedMarkerPreviewRoI()
    , selectedMarker()
    , imageGetterWatcher()
    , showMarkerPreview(false)
    , selectedMarkerScale(1.)
    , selectedMarkerImg()
    , isTracking(false)
{
}

TrackerNodeInteract::~TrackerNodeInteract()
{}

TrackerContextPtr
TrackerNodeInteract::getContext() const
{
    return _p->publicInterface->getNode()->getTrackerContext();
}

void
TrackerNodeInteract::onTrackRangeClicked()
{
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);
    ViewerInstance* viewer = overlay->getInternalViewerNode();
    assert(viewer);
    int viewerFirst, viewerLast;
    viewer->getUiContext()->getViewerFrameRange(&viewerFirst, &viewerLast);

    int first = trackRangeDialogFirstFrame.lock()->getValue();
    int last = trackRangeDialogLastFrame.lock()->getValue();
    int step = trackRangeDialogStep.lock()->getValue();

    if (first == std::numeric_limits<int>::min()) {
        trackRangeDialogFirstFrame.lock()->setValue(viewerFirst);
    }

    if (last == std::numeric_limits<int>::min()) {
        trackRangeDialogLastFrame.lock()->setValue(viewerLast);
    }

    if (step == std::numeric_limits<int>::min()) {
        trackRangeDialogStep.lock()->setValue(1);
    }

    KnobGroupPtr k = trackRangeDialogGroup.lock();

    if (k->getValue()) {
        k->setValue(false);
    } else {
        k->setValue(true);
    }
}

void
TrackerNodeInteract::onTrackAllKeyframesClicked()
{
    TrackerContextPtr ctx = getContext();
    std::list<TrackMarkerPtr> selectedMarkers;

    ctx->getSelectedMarkers(&selectedMarkers);

    std::set<int> userKeys;

    for (std::list<TrackMarkerPtr>::iterator it = selectedMarkers.begin(); it != selectedMarkers.end(); ++it) {
        std::set<int> trackUserKeys;
        (*it)->getUserKeyframes(&trackUserKeys);
        userKeys.insert(trackUserKeys.begin(), trackUserKeys.end());
    }

    if (userKeys.empty()) {
        return;
    }

    int first = *userKeys.begin();
    int last = *userKeys.rbegin() + 1;
    ctx->trackSelectedMarkers(first, last, 1,  _p->publicInterface->getCurrentViewportForOverlays());
}

void
TrackerNodeInteract::onTrackCurrentKeyframeClicked()
{
    TrackerContextPtr ctx = getContext();
    SequenceTime currentFrame = _p->publicInterface->getCurrentTime();
    std::list<TrackMarkerPtr> selectedMarkers;

    ctx->getSelectedMarkers(&selectedMarkers);

    std::set<int> userKeys;

    for (std::list<TrackMarkerPtr>::iterator it = selectedMarkers.begin(); it != selectedMarkers.end(); ++it) {
        std::set<int> trackUserKeys;
        (*it)->getUserKeyframes(&trackUserKeys);
        userKeys.insert(trackUserKeys.begin(), trackUserKeys.end());
    }

    if (userKeys.empty()) {
        return;
    }

    std::set<int>::iterator it = userKeys.lower_bound(currentFrame);

    if (it == userKeys.end()) {
        return;
    }

    int last = *it + 1;
    int first;

    if (it == userKeys.begin()) {
        first = *it;
    } else {
        --it;
        first = *it;
    }

    ctx->trackSelectedMarkers(first, last, 1,  _p->publicInterface->getCurrentViewportForOverlays());
}

void
TrackerNodeInteract::onTrackBwClicked()
{
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);
    ViewerInstance* viewer = overlay->getInternalViewerNode();
    assert(viewer);
    int first, last;
    viewer->getUiContext()->getViewerFrameRange(&first, &last);

    int startFrame = viewer->getTimeline()->currentFrame();
    TrackerContextPtr ctx = getContext();

    if (ctx->isCurrentlyTracking()) {
        ctx->abortTracking();
    } else {
        ctx->trackSelectedMarkers(startFrame, first - 1, -1,  overlay);
    }
}

void
TrackerNodeInteract::onTrackPrevClicked()
{
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);
    ViewerInstance* viewer = overlay->getInternalViewerNode();
    assert(viewer);
    int startFrame = viewer->getTimeline()->currentFrame();
    TrackerContextPtr ctx = getContext();
    ctx->trackSelectedMarkers(startFrame, startFrame - 2, -1, overlay);
}

void
TrackerNodeInteract::onStopButtonClicked()
{
    getContext()->abortTracking();
}

void
TrackerNodeInteract::onTrackNextClicked()
{
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);
    ViewerInstance* viewer = overlay->getInternalViewerNode();
    assert(viewer);
    int startFrame = viewer->getTimeline()->currentFrame();
    TrackerContextPtr ctx = getContext();
    ctx->trackSelectedMarkers(startFrame, startFrame + 2, true, overlay);
}

void
TrackerNodeInteract::onTrackFwClicked()
{
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);
    ViewerInstance* viewer = overlay->getInternalViewerNode();
    assert(viewer);
    int first, last;
    viewer->getUiContext()->getViewerFrameRange(&first, &last);

    int startFrame = viewer->getTimeline()->currentFrame();
    TrackerContextPtr ctx = getContext();

    if (ctx->isCurrentlyTracking()) {
        ctx->abortTracking();
    } else {
        ctx->trackSelectedMarkers(startFrame, last + 1, true,  overlay);
    }
}

void
TrackerNodeInteract::onUpdateViewerClicked(bool clicked)
{
    getContext()->setUpdateViewer(clicked);
}

void
TrackerNodeInteract::onAddTrackClicked(bool clicked)
{
    clickToAddTrackEnabled = clicked;
}

bool
TrackerNodeInteract::updateModifierAddTrackMode(bool enabled)
{
    // A manually selected add mode remains enabled across modifier releases.
    // Only the mode entered by the temporary shortcut belongs to that shortcut.
    if (enabled && !clickToAddTrackEnabled) {
        modifierAddTrackActive = true;
        clickToAddTrackEnabled = true;
    } else if (!enabled && modifierAddTrackActive) {
        modifierAddTrackActive = false;
        clickToAddTrackEnabled = false;
    } else {
        return false;
    }

    if (auto button = addTrackButton.lock()) {
        button->setValue(clickToAddTrackEnabled);
    }

    return true;
}

void
TrackerNodeInteract::onClearAllAnimationClicked()
{
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        (*it)->clearAnimation();
    }
}

void
TrackerNodeInteract::onClearBwAnimationClicked()
{
    int time = _p->publicInterface->getCurrentTime();
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        (*it)->clearAnimationBeforeTime(time);
    }
}

void
TrackerNodeInteract::onClearFwAnimationClicked()
{
    int time = _p->publicInterface->getCurrentTime();
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        (*it)->clearAnimationAfterTime(time);
    }
}

void
TrackerNodeInteract::onCenterViewerButtonClicked(bool clicked)
{
    getContext()->setCenterOnTrack(clicked);
}

void
TrackerNodeInteract::onSetKeyframeButtonClicked()
{
    int time = _p->publicInterface->getCurrentTime();
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        (*it)->setUserKeyframe(time);
    }
}

void
TrackerNodeInteract::onRemoveKeyframeButtonClicked()
{
    int time = _p->publicInterface->getCurrentTime();
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        (*it)->removeUserKeyframe(time);
    }
}

void
TrackerNodeInteract::onResetOffsetButtonClicked()
{
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        KnobDoublePtr offsetKnob = (*it)->getOffsetKnob();
        assert(offsetKnob);

        for (int i = 0; i < offsetKnob->getDimension(); ++i) {
            offsetKnob->resetToDefaultValue(i);
        }
    }
}

void
TrackerNodeInteract::onResetTrackButtonClicked()
{
    TrackerContextPtr context = getContext();

    assert(context);
    std::list<TrackMarkerPtr> markers;
    context->getSelectedMarkers(&markers);
    context->clearSelection(TrackerContext::eTrackSelectionInternal);
    context->endEditSelection(TrackerContext::eTrackSelectionInternal);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        (*it)->resetTrack();
    }

    context->beginEditSelection(TrackerContext::eTrackSelectionInternal);
    context->addTracksToSelection(markers, TrackerContext::eTrackSelectionInternal);
    context->endEditSelection(TrackerContext::eTrackSelectionInternal);
}

QPointF
TrackerNodeInteract::computeMidPointExtent(const QPointF& prev,
        const QPointF& next,
        const QPointF& point,
        const QPointF& handleSize)
{
    Point leftDeriv, rightDeriv;

    leftDeriv.x = prev.x() - point.x();
    leftDeriv.y = prev.y() - point.y();

    rightDeriv.x = next.x() - point.x();
    rightDeriv.y = next.y() - point.y();
    double derivNorm = std::sqrt((rightDeriv.x - leftDeriv.x) * (rightDeriv.x - leftDeriv.x) + (rightDeriv.y - leftDeriv.y) * (rightDeriv.y - leftDeriv.y));
    QPointF ret;

    if (derivNorm == 0) {
        double norm = std::sqrt((leftDeriv.x - point.x()) * (leftDeriv.x - point.x()) + (leftDeriv.y - point.y()) * (leftDeriv.y  - point.y()));

        if (norm != 0) {
            ret.rx() = point.x() + ((leftDeriv.y - point.y()) / norm) * handleSize.x();
            ret.ry() = point.y() - ((leftDeriv.x - point.x()) / norm) * handleSize.y();

            return ret;
        } else {
            return QPointF(0, 0);
        }
    } else {
        ret.rx() = point.x() + ((rightDeriv.y - leftDeriv.y) / derivNorm) * handleSize.x();
        ret.ry() = point.y() - ((rightDeriv.x - leftDeriv.x) / derivNorm) * handleSize.y();
    }

    return ret;
}

int
TrackerNodeInteract::isInsideKeyFramePreview(double currentTime,
        const QPointF& pos,
        const QPointF& viewportPos) const
{
    if (!showMarkerPreview) {
        return std::numeric_limits<int>::max();
    }


    RectD textureRectCanonical;

    if (selectedMarkerPreview) {
        computeSelectedMarkerCanonicalRect(&textureRectCanonical);
    }

    if ((pos.y() < textureRectCanonical.y1) || (pos.y() > textureRectCanonical.y2)) {
        return std::numeric_limits<int>::max();
    }

    if (pos.x() < textureRectCanonical.x2) {
        return std::numeric_limits<int>::max();
    }

    TrackMarkerPtr marker = selectedMarker.lock();

    if (!marker) {
        return std::numeric_limits<int>::max();
    }

    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();
    assert(overlay);

    //Find out which keyframe it is by counting keyframe portions
    int xRightMainTexture = overlay->toWidgetCoordinates(QPointF(textureRectCanonical.x2, 0)).x();
    const double keyWidthpx = TO_DPIX(SELECTED_MARKER_KEYFRAME_WIDTH_SCREEN_PX);
    double indexF = (viewportPos.x() - xRightMainTexture) / keyWidthpx;
    int texIndex = (int)std::floor(indexF);

    for (TrackKeysMap::const_iterator it = trackPreviews.begin(); it != trackPreviews.end(); ++it) {
        if (it->first.lock() == marker) {
            if (it->second.empty()) {
                break;
            }

            ///Render at most MAX_TRACK_KEYS_TO_DISPLAY keyframes
            KeyFrameImages keysToRender = getKeysToRenderForMarker(currentTime, it->second);

            if ((texIndex < 0) || (texIndex >= (int)keysToRender.size())) {
                return std::numeric_limits<int>::max();
            }

            KeyFrameImages::iterator found = keysToRender.begin();
            std::advance(found, texIndex);
            RectD texCanonicalRect;
            computePreviewCanonicalRect(*found->second, indexF * keyWidthpx + xRightMainTexture,
                                        keyWidthpx, &texCanonicalRect);

            if ((pos.y() >= texCanonicalRect.y1) && (pos.y() < texCanonicalRect.y2)) {
                return found->first;
            }

            break;
        }
    }

    return std::numeric_limits<int>::max();
} // isInsideKeyFramePreview

bool
TrackerNodeInteract::isNearbySelectedMarkerPreviewResizeAnchor(const QPointF& pos) const
{
    RectD textureRectCanonical;

    computeSelectedMarkerCanonicalRect(&textureRectCanonical);

    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();
    assert(overlay);
    QPointF clickWidget = overlay->toWidgetCoordinates(pos);
    QPointF btmRightWidget = overlay->toWidgetCoordinates(QPointF(textureRectCanonical.x2, textureRectCanonical.y1));
    double tolerance = TO_DPIX(POINT_TOLERANCE);

    if ((clickWidget.x() >= (btmRightWidget.x() - tolerance)) && (clickWidget.x() <= (btmRightWidget.x() + tolerance)) &&
        (clickWidget.y() >= (btmRightWidget.y() - tolerance)) && (clickWidget.y() <= (btmRightWidget.y() + tolerance))) {
        return true;
    }

    return false;
}

bool
TrackerNodeInteract::isInsideSelectedMarkerPreview(const QPointF& pos) const
{
    RectD textureRectCanonical;

    computeSelectedMarkerCanonicalRect(&textureRectCanonical);

    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();
    assert(overlay);
    QPointF clickWidget = overlay->toWidgetCoordinates(pos);
    QPointF btmRightWidget = overlay->toWidgetCoordinates(QPointF(textureRectCanonical.x2, textureRectCanonical.y1));
    QPointF topLeftWidget = overlay->toWidgetCoordinates(QPointF(textureRectCanonical.x1, textureRectCanonical.y2));
    RectD rect;
    rect.x1 = topLeftWidget.x();
    rect.y1 = topLeftWidget.y();
    rect.x2 = btmRightWidget.x();
    rect.y2 = btmRightWidget.y();

    return rect.contains(clickWidget.x(), clickWidget.y());
}

void
TrackerNodeInteract::computePreviewCanonicalRect(const MarkerPreview& tex,
        int xOffset,
        int texWidthPx,
        RectD* rect) const
{
    ///Preserve width
    if (tex.h() == 0 || tex.w() == 0) {
        return;
    }

    double par = tex.w() / (double)tex.h();
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);
    rect->x2 = overlay->toCanonicalCoordinates(QPointF(xOffset + texWidthPx, 0.)).x();
    QPointF topLeft = overlay->toCanonicalCoordinates(QPointF(xOffset, 0.));
    rect->x1 = topLeft.x();
    rect->y2 = topLeft.y();
    double height = rect->width() / par;
    rect->y1 = rect->y2 - height;
}

void
TrackerNodeInteract::computeSelectedMarkerCanonicalRect(RectD* rect) const
{
    assert(selectedMarkerPreview);

    if (!selectedMarkerPreview) {
        rect->clear();
        return;
    }

    int selectedMarkerWidth = magWindowPxSizeKnob.lock()->getValue();
    computePreviewCanonicalRect(*selectedMarkerPreview, 0, selectedMarkerWidth, rect);
}

Point
TrackerNodeInteract::toMagWindowPoint(const Point& ptnPoint,
                                      const RectD& canonicalSearchWindow,
                                      const RectD& textureRectCanonical)
{
    Point ret;
    double xCenterPercent = (ptnPoint.x - canonicalSearchWindow.x1) / (canonicalSearchWindow.x2 - canonicalSearchWindow.x1);
    double yCenterPercent = (ptnPoint.y - canonicalSearchWindow.y1) / (canonicalSearchWindow.y2 - canonicalSearchWindow.y1);

    ret.y = textureRectCanonical.y1 + yCenterPercent * (textureRectCanonical.y2 - textureRectCanonical.y1);
    ret.x = textureRectCanonical.x1 + xCenterPercent * (textureRectCanonical.x2 - textureRectCanonical.x1);

    return ret;
}

void
TrackerNodeInteract::drawEllipse(OverlayDrawList& overlay, double x,
                                 double y,
                                 double radiusX,
                                 double radiusY,
                                 int l,
                                 double r,
                                 double g,
                                 double b,
                                 double a)
{
    overlay.color(r * l * a, g * l * a, b * l * a);

    overlay.pushTransform();
    //  center the oval at x_center, y_center
    overlay.translate((float)x, (float)y);
    //  draw the oval using line segments
    overlay.begin(OverlayPrimitive::LineLoop);
    // we don't need to be pixel-perfect here, it's just an interact!
    // 40 segments is enough.
    double m = 2 * 3.14159265358979323846264338327950288419717 / 40.;

    for (int i = 0; i < 40; ++i) {
        double theta = i * m;
        overlay.vertex(radiusX * std::cos(theta), radiusY * std::sin(theta));
    }

    overlay.end();

    overlay.popTransform();
}

TrackerNodeInteract::KeyFrameImages
TrackerNodeInteract::getKeysToRenderForMarker(double currentTime,
        const KeyFrameImages& allKeys)
{
    KeyFrameImages keysToRender;
    ///Find the first key equivalent to currentTime or after
    KeyFrameImages::const_iterator lower = allKeys.lower_bound(currentTime);
    KeyFrameImages::const_iterator prev = lower;

    if (lower != allKeys.begin()) {
        --prev;
    } else {
        prev = allKeys.end();
    }

    for (int i = 0; i < MAX_TRACK_KEYS_TO_DISPLAY; ++i) {
        if (lower != allKeys.end()) {
            keysToRender.insert(*lower);
            ++lower;
        }

        if (prev != allKeys.end()) {
            keysToRender.insert(*prev);

            if (prev != allKeys.begin()) {
                --prev;
            } else {
                prev = allKeys.end();
            }
        } else {
            if (lower == allKeys.end()) {
                ///No more keyframes
                break;
            }
        }
    }

    return keysToRender;
}

void
TrackerNodeInteract::drawSelectedMarkerKeyframes(const std::pair<double, double> &pixelScale,
        int currentTime)
{
    TrackMarkerPtr marker = selectedMarker.lock();

    assert(marker);

    if (!marker) {
        return;
    }

    if (!marker->isEnabled(currentTime)) {
        return;
    }

    OverlaySupport* viewport = _p->publicInterface->getCurrentViewportForOverlays();
    assert(viewport);
    auto* commands = viewport->overlayDrawList();

    if (!commands) {
        return;
    }

    OverlayDrawList& overlay = *commands;
    OverlayDrawScope scope(overlay);

    double overlayColor[3];

    if (!_p->publicInterface->getNode()->getOverlayColor(&overlayColor[0], &overlayColor[1], &overlayColor[2])) {
        overlayColor[0] = overlayColor[1] = overlayColor[2] = 0.8;
    }


    KnobDoublePtr centerKnob = marker->getCenterKnob();
    KnobDoublePtr offsetKnob = marker->getOffsetKnob();
    KnobDoublePtr errorKnob = marker->getErrorKnob();
    KnobDoublePtr ptnTopLeft = marker->getPatternTopLeftKnob();
    KnobDoublePtr ptnTopRight = marker->getPatternTopRightKnob();
    KnobDoublePtr ptnBtmRight = marker->getPatternBtmRightKnob();
    KnobDoublePtr ptnBtmLeft = marker->getPatternBtmLeftKnob();
    KnobDoublePtr searchWndBtmLeft = marker->getSearchWindowBottomLeftKnob();
    KnobDoublePtr searchWndTopRight = marker->getSearchWindowTopRightKnob();
    int fontHeight = viewport->getWidgetFontHeight();

    int selectedMarkerWidth = magWindowPxSizeKnob.lock()->getValue();
    double xOffsetPixels = selectedMarkerWidth;
    QPointF viewerTopLeftCanonical = viewport->toCanonicalCoordinates(QPointF(0, 0.));


    for (TrackKeysMap::iterator it = trackPreviews.begin(); it != trackPreviews.end(); ++it) {
        if (it->first.lock() == marker) {
            if (it->second.empty()) {
                break;
            }

            ///Render at most MAX_TRACK_KEYS_TO_DISPLAY keyframes
            KeyFrameImages keysToRender = getKeysToRenderForMarker(currentTime, it->second);

            for (KeyFrameImages::const_iterator it2 = keysToRender.begin(); it2 != keysToRender.end(); ++it2) {
                double time = (double)it2->first;
                Point offset, center, topLeft, topRight, btmRight, btmLeft;

                center.x = centerKnob->getValueAtTime(time, 0);
                center.y = centerKnob->getValueAtTime(time, 1);
                offset.x = offsetKnob->getValueAtTime(time, 0);
                offset.y = offsetKnob->getValueAtTime(time, 1);

                topLeft.x = ptnTopLeft->getValueAtTime(time, 0) + offset.x + center.x;
                topLeft.y = ptnTopLeft->getValueAtTime(time, 1) + offset.y + center.y;

                topRight.x = ptnTopRight->getValueAtTime(time, 0) + offset.x + center.x;
                topRight.y = ptnTopRight->getValueAtTime(time, 1) + offset.y + center.y;

                btmRight.x = ptnBtmRight->getValueAtTime(time, 0) + offset.x + center.x;
                btmRight.y = ptnBtmRight->getValueAtTime(time, 1) + offset.y + center.y;

                btmLeft.x = ptnBtmLeft->getValueAtTime(time, 0) + offset.x + center.x;
                btmLeft.y = ptnBtmLeft->getValueAtTime(time, 1) + offset.y + center.y;

                //const double searchLeft   = searchWndBtmLeft->getValueAtTime(time, 0) + offset.x + center.x;
                //const double searchRight  = searchWndTopRight->getValueAtTime(time, 0) + offset.x + center.x;
                //const double searchBottom = searchWndBtmLeft->getValueAtTime(time, 1) + offset.y + center.y;
                //const double searchTop    = searchWndTopRight->getValueAtTime(time, 1) + offset.y + center.y;

                const RectI& texRect = it2->second->bounds;

                if (texRect.height() <= 0) {
                    continue;
                }

                double par = texRect.width() / (double)texRect.height();
                RectD textureRectCanonical;

                textureRectCanonical.x2 = viewport->toCanonicalCoordinates(QPointF(TO_DPIX(SELECTED_MARKER_KEYFRAME_WIDTH_SCREEN_PX) + xOffsetPixels, 0.)).x();
                textureRectCanonical.x1 = viewport->toCanonicalCoordinates(QPointF(xOffsetPixels, 0.)).x();
                textureRectCanonical.y2 = viewerTopLeftCanonical.y();
                double height = textureRectCanonical.width() / par;
                textureRectCanonical.y1 = textureRectCanonical.y2 - height;


                const RectD canonicalSearchWindow = texRect.toCanonical_noClipping(0, it2->second->par);

                //Remove any offset to the center to see the marker in the magnification window
                double xCenterPercent = (center.x - canonicalSearchWindow.x1 + offset.x) / (canonicalSearchWindow.x2 - canonicalSearchWindow.x1);
                double yCenterPercent = (center.y - canonicalSearchWindow.y1 + offset.y) / (canonicalSearchWindow.y2 - canonicalSearchWindow.y1);
                Point centerPointCanonical;
                centerPointCanonical.y = textureRectCanonical.y1 + yCenterPercent * (textureRectCanonical.y2 - textureRectCanonical.y1);
                centerPointCanonical.x = textureRectCanonical.x1 + xCenterPercent * (textureRectCanonical.x2 - textureRectCanonical.x1);


                Point innerTopLeft = toMagWindowPoint(topLeft, canonicalSearchWindow, textureRectCanonical);
                Point innerTopRight = toMagWindowPoint(topRight, canonicalSearchWindow, textureRectCanonical);
                Point innerBtmLeft = toMagWindowPoint(btmLeft, canonicalSearchWindow, textureRectCanonical);
                Point innerBtmRight = toMagWindowPoint(btmRight, canonicalSearchWindow, textureRectCanonical);

                overlay.image(it2->second->pixels, textureRectCanonical.x1, textureRectCanonical.y1, textureRectCanonical.x2, textureRectCanonical.y2);

                QPointF textPos = viewport->toCanonicalCoordinates(QPointF(xOffsetPixels + 5, fontHeight + 5));
                viewport->renderText(textPos.x(), textPos.y(), marker->getLabel(), overlayColor[0], overlayColor[1], overlayColor[2]);

                QPointF framePos = viewport->toCanonicalCoordinates(QPointF(xOffsetPixels + 5, viewport->toWidgetCoordinates(QPointF(textureRectCanonical.x1, textureRectCanonical.y1)).y()));
                QString frameText = tr("Frame");
                frameText.append(QString::fromUtf8(" ") + QString::number(it2->first));
                viewport->renderText(framePos.x(), framePos.y(), frameText.toStdString(), overlayColor[0], overlayColor[1], overlayColor[2]);

                //Draw contour



                if (time == currentTime) {
                    overlay.color(0.93, 0.54, 0, 1);
                } else {
                    KeyFrameImages::const_iterator next = it2;
                    ++next;
                    KeyFrameImages::const_iterator prev = it2;

                    if (prev != keysToRender.begin()) {
                        --prev;
                    } else {
                        prev = keysToRender.end();
                    }

                    if ((next == keysToRender.end()) && (time < currentTime)) {
                        //Beyond the last keyframe
                        overlay.color(0.93, 0.54, 0, 1);
                    } else if ((prev == keysToRender.end()) && (time > currentTime)) {
                        //Before the first keyframe
                        overlay.color(0.93, 0.54, 0, 1);
                    } else {
                        if (time < currentTime) {
                            assert(next != keysToRender.end());

                            if (next->first > currentTime) {
                                overlay.color(1, 0.75, 0.47, 1);
                            } else {
                                overlay.color(1., 1., 1., 0.5);
                            }
                        } else {
                            //time > currentTime
                            assert(prev != keysToRender.end());

                            if (prev->first < currentTime) {
                                overlay.color(1, 0.75, 0.47, 1);
                            } else {
                                overlay.color(1., 1., 1., 0.5);
                            }
                        }
                    }
                }

                overlay.lineWidth(1.5);
                overlay.begin(OverlayPrimitive::LineLoop);
                overlay.vertex(textureRectCanonical.x1, textureRectCanonical.y1);
                overlay.vertex(textureRectCanonical.x1, textureRectCanonical.y2);
                overlay.vertex(textureRectCanonical.x2, textureRectCanonical.y2);
                overlay.vertex(textureRectCanonical.x2, textureRectCanonical.y1);
                overlay.end();



                //Draw internal marker
                for (int l = 0; l < 2; ++l) {


                    int direction = (l == 0) ? 1 : -1;
                    // translate (1,-1) pixels
                    overlay.translateOutput(direction * pixelScale.first, -direction * pixelScale.second);


                    overlay.color(0.8 * l, 0.8 * l, 0.8 * l, 1);

                    overlay.lineWidth(1.5);
                    overlay.begin(OverlayPrimitive::LineLoop);
                    overlay.vertex(innerTopLeft.x, innerTopLeft.y);
                    overlay.vertex(innerTopRight.x, innerTopRight.y);
                    overlay.vertex(innerBtmRight.x, innerBtmRight.y);
                    overlay.vertex(innerBtmLeft.x, innerBtmLeft.y);
                    overlay.end();

                    overlay.pointSize(POINT_SIZE);
                    overlay.begin(OverlayPrimitive::Points);
                    overlay.vertex(centerPointCanonical.x, centerPointCanonical.y);
                    overlay.end();
                }

                xOffsetPixels += TO_DPIX(SELECTED_MARKER_KEYFRAME_WIDTH_SCREEN_PX);
            }

            break;
        }
    }
} // TrackerNodeInteract::drawSelectedMarkerKeyframes

void
TrackerNodeInteract::drawSelectedMarkerPreview(const std::pair<double, double> &pixelScale,
        int currentTime,
        const Point& ptnCenter,
        const Point& offset,
        const Point& ptnTopLeft,
        const Point& ptnTopRight,
        const Point& ptnBtmRight,
        const Point& ptnBtmLeft,
        const Point & /*selectedSearchWndBtmLeft*/,
        const Point & /*selectedSearchWndTopRight*/)
{
    TrackMarkerPtr marker = selectedMarker.lock();
    OverlaySupport* viewport = _p->publicInterface->getCurrentViewportForOverlays();


    assert(viewport);
    auto* commands = viewport->overlayDrawList();

    if (!commands) {
        return;
    }

    OverlayDrawList& overlay = *commands;
    OverlayDrawScope scope(overlay);
    ViewerInstance* viewer = viewport->getInternalViewerNode();
    assert(viewer);

    if (isTracking || !selectedMarkerPreview || !marker || !marker->isEnabled(currentTime) || viewer->getRenderEngine()->isDoingSequentialRender()) {
        return;
    }

    RectD textureRectCanonical;
    computeSelectedMarkerCanonicalRect(&textureRectCanonical);


    double overlayColor[3];

    if (!_p->publicInterface->getNode()->getOverlayColor(&overlayColor[0], &overlayColor[1], &overlayColor[2])) {
        overlayColor[0] = overlayColor[1] = overlayColor[2] = 0.8;
    }

    const RectI& texRect = selectedMarkerPreview->bounds;
    RectD texCoords;

    texCoords.x1 = texCoords.y1 = 0.;
    texCoords.x2 = texCoords.y2 = 1.;

    const RectD canonicalSearchWindow = texRect.toCanonical_noClipping(0, selectedMarkerPreview->par);

    Point centerPoint, innerTopLeft, innerTopRight, innerBtmLeft, innerBtmRight;

    //Remove any offset to the center to see the marker in the magnification window
    double xCenterPercent = (ptnCenter.x - canonicalSearchWindow.x1 + offset.x) / (canonicalSearchWindow.x2 - canonicalSearchWindow.x1);
    double yCenterPercent = (ptnCenter.y - canonicalSearchWindow.y1 + offset.y) / (canonicalSearchWindow.y2 - canonicalSearchWindow.y1);
    centerPoint.y = textureRectCanonical.y1 + yCenterPercent * (textureRectCanonical.y2 - textureRectCanonical.y1);
    centerPoint.x = textureRectCanonical.x1 + xCenterPercent * (textureRectCanonical.x2 - textureRectCanonical.x1);


    innerTopLeft = toMagWindowPoint(ptnTopLeft, canonicalSearchWindow, textureRectCanonical);
    innerTopRight = toMagWindowPoint(ptnTopRight, canonicalSearchWindow, textureRectCanonical);
    innerBtmLeft = toMagWindowPoint(ptnBtmLeft, canonicalSearchWindow, textureRectCanonical);
    innerBtmRight = toMagWindowPoint(ptnBtmRight, canonicalSearchWindow, textureRectCanonical);

    Transform::Point3D btmLeftTex, topLeftTex, topRightTex, btmRightTex;
    btmLeftTex.z = topLeftTex.z = topRightTex.z = btmRightTex.z = 1.;
    btmLeftTex.x = texCoords.x1;
    btmLeftTex.y = texCoords.y1;
    topLeftTex.x = texCoords.x1;
    topLeftTex.y = texCoords.y2;
    topRightTex.x = texCoords.x2;
    topRightTex.y = texCoords.y2;
    btmRightTex.x = texCoords.x2;
    btmRightTex.y = texCoords.y1;
    Transform::Matrix3x3 m = Transform::matTransformCanonical(0, 0, selectedMarkerScale, selectedMarkerScale, 0, 0, false, 0, xCenterPercent, yCenterPercent);
    btmLeftTex = Transform::matApply(m, btmLeftTex);
    topLeftTex = Transform::matApply(m, topLeftTex);
    btmRightTex = Transform::matApply(m, btmRightTex);
    topRightTex = Transform::matApply(m, topRightTex);

    overlay.image(selectedMarkerPreview->pixels, textureRectCanonical.x1, textureRectCanonical.y1, textureRectCanonical.x2, textureRectCanonical.y2, topLeftTex.x, 1 - topLeftTex.y, btmRightTex.x, 1 - btmRightTex.y);

    //Draw contour


    overlay.color(1., 1., 1., 0.5);
    overlay.lineWidth(1.5);
    overlay.begin(OverlayPrimitive::LineLoop);
    overlay.vertex(textureRectCanonical.x1, textureRectCanonical.y1);
    overlay.vertex(textureRectCanonical.x1, textureRectCanonical.y2);
    overlay.vertex(textureRectCanonical.x2, textureRectCanonical.y2);
    overlay.vertex(textureRectCanonical.x2, textureRectCanonical.y1);
    overlay.end();

    overlay.color(0.8, 0.8, 0.8, 1.);
    overlay.pointSize(POINT_SIZE);
    overlay.begin(OverlayPrimitive::Points);
    overlay.vertex(textureRectCanonical.x2, textureRectCanonical.y1);
    overlay.end();


    int fontHeight = viewport->getWidgetFontHeight();
    QPointF textPos = viewport->toCanonicalCoordinates(QPointF(5, fontHeight + 5));
    viewport->renderText(textPos.x(), textPos.y(), marker->getLabel(), overlayColor[0], overlayColor[1], overlayColor[2]);

    //Draw internal marker

    for (int l = 0; l < 2; ++l) {


        int direction = (l == 0) ? 1 : -1;
        // translate (1,-1) pixels
        overlay.translateOutput(direction * pixelScale.first, -direction * pixelScale.second);


        overlay.color(0.8 * l, 0.8 * l, 0.8 * l, 1);
        overlay.lineWidth(1.5);
        overlay.begin(OverlayPrimitive::LineLoop);
        overlay.vertex(innerTopLeft.x, innerTopLeft.y);
        overlay.vertex(innerTopRight.x, innerTopRight.y);
        overlay.vertex(innerBtmRight.x, innerBtmRight.y);
        overlay.vertex(innerBtmLeft.x, innerBtmLeft.y);
        overlay.end();

        overlay.pointSize(POINT_SIZE);
        overlay.begin(OverlayPrimitive::Points);
        overlay.vertex(centerPoint.x, centerPoint.y);
        overlay.end();

        ///Draw ellipse if scaling
        if ((eventState == eMouseStateScalingSelectedMarker) || (hoverState == eDrawStateShowScalingHint)) {
            double ellipseColor[3];

            if (eventState == eMouseStateScalingSelectedMarker) {
                ellipseColor[0] = 0.8;
                ellipseColor[1] = 0.8;
                ellipseColor[2] = 0.;
            } else {
                ellipseColor[0] = 0.8;
                ellipseColor[1] = 0.8;
                ellipseColor[2] = 0.8;
            }

            double rx = std::sqrt((lastMousePos.x() - centerPoint.x) * (lastMousePos.x() - centerPoint.x) + (lastMousePos.y() - centerPoint.y) * (lastMousePos.y() - centerPoint.y));
            double ry = rx;
            drawEllipse(overlay, centerPoint.x, centerPoint.y, rx, ry, l, ellipseColor[0], ellipseColor[1], ellipseColor[2], 1.);
        }
    }

    ///Now draw keyframes
    drawSelectedMarkerKeyframes(pixelScale, currentTime);
} // TrackerNodeInteract::drawSelectedMarkerPreview

bool
TrackerNodeInteract::isNearbyPoint(const KnobDoublePtr& knob,
                                   double xWidget,
                                   double yWidget,
                                   double toleranceWidget,
                                   double time)
{
    QPointF p;
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);

    p.rx() = knob->getValueAtTime(time, 0);
    p.ry() = knob->getValueAtTime(time, 1);
    p = overlay->toWidgetCoordinates(p);

    if ((p.x() <= (xWidget + toleranceWidget)) && (p.x() >= (xWidget - toleranceWidget)) &&
        (p.y() <= (yWidget + toleranceWidget)) && (p.y() >= (yWidget - toleranceWidget))) {
        return true;
    }

    return false;
}

bool
TrackerNodeInteract::isNearbyPoint(const QPointF& p,
                                   double xWidget,
                                   double yWidget,
                                   double toleranceWidget)
{
    OverlaySupport* overlay = _p->publicInterface->getCurrentViewportForOverlays();

    assert(overlay);


    QPointF pw = overlay->toWidgetCoordinates(p);

    if ((pw.x() <= (xWidget + toleranceWidget)) && (pw.x() >= (xWidget - toleranceWidget)) &&
        (pw.y() <= (yWidget + toleranceWidget)) && (pw.y() >= (yWidget - toleranceWidget))) {
        return true;
    }

    return false;
}

void
TrackerNodeInteract::findLineIntersection(const Point& p,
        const Point& l1,
        const Point& l2,
        Point* inter)
{
    Point h, u;
    double a;

    h.x = p.x - l1.x;
    h.y = p.y - l1.y;

    u.x = l2.x - l1.x;
    u.y = l2.y - l1.y;

    a = (u.x * h.x + u.y * h.y) / (u.x * u.x + u.y * u.y);
    inter->x = l1.x + u.x * a;
    inter->y = l1.y + u.y * a;
}

void
TrackerNodeInteract::refreshSelectedMarkerPreview()
{
    assert(QThread::currentThread() == qApp->thread());

    if (isTracking) {
        return;
    }

    TrackMarkerPtr marker = selectedMarker.lock();

    if (!marker) {
        return;
    }

    int time = _p->publicInterface->getCurrentTime();
    RectI roi = marker->getMarkerImageRoI(time);

    if (roi.isNull()) {
        return;
    }

    ImagePtr existingMarkerImg = selectedMarkerImg.lock();

    if (existingMarkerImg && (existingMarkerImg->getTime() == time) && (roi == selectedMarkerPreviewRoI)) {
        return;
    }

    selectedMarkerImg.reset();

    imageGetterWatcher = std::make_shared<TrackWatcher>();
    QObject::connect(imageGetterWatcher.get(), SIGNAL(finished()), this, SLOT(onTrackImageRenderingFinished()));
    imageGetterWatcher->setFuture(QtConcurrent::run([marker, time, roi]() {
        return marker->getMarkerImage(time, roi);
    }));
}

void
TrackerNodeInteract::makeMarkerKeyPreview(int time,
        const TrackMarkerPtr& track)
{
    assert(QThread::currentThread() == qApp->thread());
    TrackRequestKey k;
    k.time = time;
    k.track = track;
    k.roi = track->getMarkerImageRoI(time);

    TrackKeysMap::iterator foundTrack = trackPreviews.find(track);

    if (foundTrack != trackPreviews.end()) {
        KeyFrameImages::iterator foundKey = foundTrack->second.find(k.time);

        if (foundKey != foundTrack->second.end()) {
            const RectI& texRect = foundKey->second->bounds;

            if ((k.roi.x1 == texRect.x1) &&
                (k.roi.x2 == texRect.x2) &&
                (k.roi.y1 == texRect.y1) &&
                (k.roi.y2 == texRect.y2)) {
                return;
            }
        }
    }

    if (!k.roi.isNull()) {
        TrackWatcherPtr watcher(new TrackWatcher());
        QObject::connect(watcher.get(), SIGNAL(finished()), this, SLOT(onKeyFrameImageRenderingFinished()));
        trackRequestsMap[k] = watcher;
        watcher->setFuture(QtConcurrent::run([track, time, roi = k.roi]() {
            return track->getMarkerImage(time, roi);
        }));
    }
}

TrackerNodeInteract::MarkerPreviewPtr
TrackerNodeInteract::makeMarkerPreview(const ImagePtr& image, const RectI& renderWindow)
{
    const RectI bounds = image ? image->getBounds() : renderWindow;
    const RectI roi = renderWindow.intersect(bounds);

    if (roi.isNull()) {
        return {};
    }

    auto preview = std::make_shared<MarkerPreview>();
    preview->bounds = roi;
    preview->par = image ? image->getPixelAspectRatio() : 1;
    auto pixels = std::make_shared<OverlayImage>();
    pixels->width = roi.width();
    pixels->height = roi.height();
    pixels->pixels.resize(size_t(roi.width()) * roi.height() * 4);
    preview->pixels = pixels;

    if (!image) {
        for (size_t i = 3; i < pixels->pixels.size(); i += 4) {
            pixels->pixels[i] = 255;
        }

        return preview;
    }

    const int components = image->getComponentsCount();

    if (image->getBitDepth() != eImageBitDepthFloat || components < 3) {
        return {};
    }

    Image::ReadAccess access(image.get());
    const Color::Lut* lut = Color::LutManager::sRGBLut();
    lut->validate();

    for (int y = roi.y1; y < roi.y2; ++y) {
        const float *source = reinterpret_cast<const float *>(access.pixelAt(roi.x1, y));

        if (!source) {
            return {};
        }

        // Preserve the existing sRGB error-diffusion conversion. The command
        // buffer stores top-down rows and explicit RGBA byte order.
        uint8_t *destination = pixels->pixels.data() + size_t(roi.y2 - 1 - y) * roi.width() * 4;
        const int start = rand() % roi.width();

        for (int backward = 0; backward < 2; ++backward) {
            unsigned errors[3] = {0x80, 0x80, 0x80};

            for (int x = backward ? start - 1 : start; x >= 0 && x < roi.width(); x += backward ? -1 : 1) {
                for (int channel = 0; channel < 3; ++channel) {
                    errors[channel] = (errors[channel] & 0xff) + lut->toColorSpaceUint8xxFromLinearFloatFast(source[x * components + channel]);
                    destination[x * 4 + channel] = uint8_t(errors[channel] >> 8);
                }

                destination[x * 4 + 3] = 255;
            }
        }
    }

    return preview;
} // TrackerNodeInteract::makeMarkerPreview

void
TrackerNodeInteract::onTrackingStarted(int step)
{
    isTracking = true;

    if (step > 0) {
        trackFwButton.lock()->setValue(true);
    } else {
        trackBwButton.lock()->setValue(true);
    }
}

void
TrackerNodeInteract::onTrackingEnded()
{
    trackBwButton.lock()->setValue(false);
    trackFwButton.lock()->setValue(false);
    isTracking = false;
    _p->publicInterface->redrawOverlayInteract();
}

void
TrackerNodeInteract::onContextSelectionChanged(int reason)
{
    std::list<TrackMarkerPtr> selection;

    getContext()->getSelectedMarkers(&selection);

    if (selection.empty() || (selection.size() > 1)) {
        showMarkerPreview = false;
    } else {
        assert(selection.size() == 1);

        const TrackMarkerPtr& selectionFront = selection.front();
        TrackMarkerPtr oldMarker = selectedMarker.lock();

        if (oldMarker != selectionFront) {
            selectedMarker = selectionFront;
            refreshSelectedMarkerPreview();


            std::set<int> keys;
            selectionFront->getUserKeyframes(&keys);

            for (std::set<int>::iterator it2 = keys.begin(); it2 != keys.end(); ++it2) {
                makeMarkerKeyPreview(*it2, selectionFront);
            }
        } else {
            if (selectionFront) {
                showMarkerPreview = true;
            }
        }
    }

    if ((TrackerContext::TrackSelectionReason)reason == TrackerContext::eTrackSelectionViewer) {
        return;
    }

    _p->publicInterface->redrawOverlayInteract();
}

void
TrackerNodeInteract::onTrackImageRenderingFinished()
{
    assert(QThread::currentThread() == qApp->thread());
    QFutureWatcher<std::pair<ImagePtr, RectI> > *future = dynamic_cast<QFutureWatcher<std::pair<ImagePtr, RectI> >*>(sender());
    assert(future);
    std::pair<ImagePtr, RectI> ret = future->result();

    if (future != imageGetterWatcher.get() || !ret.first) {
        return;
    }

    auto preview = makeMarkerPreview(ret.first, ret.second);

    if (!preview) {
        return;
    }

    selectedMarkerPreview = preview;
    showMarkerPreview = true;
    selectedMarkerPreviewTime = (int)ret.first->getTime();
    selectedMarkerPreviewRoI = ret.second;


    _p->publicInterface->redrawOverlayInteract();
}

void
TrackerNodeInteract::onKeyFrameImageRenderingFinished()
{
    assert(QThread::currentThread() == qApp->thread());
    TrackWatcher* future = dynamic_cast<TrackWatcher *>(sender());
    assert(future);
    std::pair<ImagePtr, RectI> ret = future->result();

    if (!ret.first || ret.second.isNull()) {
        return;
    }

    for (TrackKeyframeRequests::iterator it = trackRequestsMap.begin(); it != trackRequestsMap.end(); ++it) {
        if (it->second.get() == future) {
            TrackMarkerPtr track = it->first.track.lock();

            if (!track) {
                return;
            }

            TrackerNodeInteract::KeyFrameImages& keyTextures = trackPreviews[track];
            auto preview = makeMarkerPreview(ret.first, ret.second);

            if (preview) {
                keyTextures[it->first.time] = preview;
            }

            trackRequestsMap.erase(it);

            _p->publicInterface->redrawOverlayInteract();

            return;
        }
    }

    assert(false);
}

void
TrackerNodeInteract::rebuildMarkerPreviews()
{
    ///Refresh textures for all markers
    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {
        std::set<int> keys;
        (*it)->getUserKeyframes(&keys);

        for (std::set<int>::iterator it2 = keys.begin(); it2 != keys.end(); ++it2) {
            makeMarkerKeyPreview(*it2, *it);
        }
    }

    onContextSelectionChanged(TrackerContext::eTrackSelectionInternal);
}

/**
 *@brief Moves of the given pixel the selected tracks.
 * This takes into account the zoom factor.
 **/
bool
TrackerNodeInteract::nudgeSelectedTracks(int x,
        int y)
{
    if (!isInsideSelectedMarkerPreview(lastMousePos)) {
        return false;
    }

    std::list<TrackMarkerPtr> markers;

    getContext()->getSelectedMarkers(&markers);

    if (!markers.empty()) {
        std::pair<double, double> pixelScale;
        _p->publicInterface->getCurrentViewportForOverlays()->getPixelScale(pixelScale.first, pixelScale.second);
        double time = _p->publicInterface->getCurrentTime();
        bool createkey = createKeyOnMoveButton.lock()->getValue();

        int hasMovedMarker = false;

        for (std::list<TrackMarkerPtr>::iterator it = markers.begin(); it != markers.end(); ++it) {

            if (!(*it)->isEnabled(time)) {
                continue;
            }

            KnobDoublePtr centerKnob = (*it)->getCenterKnob();
            KnobDoublePtr patternCorners[4];
            patternCorners[0] = (*it)->getPatternBtmLeftKnob();
            patternCorners[1] = (*it)->getPatternTopLeftKnob();
            patternCorners[2] = (*it)->getPatternTopRightKnob();
            patternCorners[3] = (*it)->getPatternBtmRightKnob();

            centerKnob->setValuesAtTime(time, centerKnob->getValueAtTime(time, 0) + x,
                                        centerKnob->getValueAtTime(time, 1) + y,
                                        ViewSpec::all(),
                                        eValueChangedReasonPluginEdited);

            for (int i = 0; i < 4; ++i) {
                for (int d = 0; d < patternCorners[i]->getDimension(); ++d) {
                    patternCorners[i]->setValueAtTime(time, patternCorners[i]->getValueAtTime(time, d), ViewSpec::all(), d);
                }
            }

            if (createkey) {
                (*it)->setUserKeyframe(time);
            }

            hasMovedMarker = true;
        }

        refreshSelectedMarkerPreview();

        return hasMovedMarker;
    }

    return false;
}

void
TrackerNodeInteract::transformPattern(double time,
                                      TrackerMouseStateEnum state,
                                      const Point& delta)
{
    KnobDoublePtr searchWndTopRight, searchWndBtmLeft;
    KnobDoublePtr patternCorners[4];
    TrackerContextPtr context = getContext();
    KnobDoublePtr centerKnob = interactMarker->getCenterKnob();
    KnobDoublePtr offsetKnob = interactMarker->getOffsetKnob();
    bool transformPatternCorners = state != eMouseStateDraggingOuterBtmLeft &&
                                   state != eMouseStateDraggingOuterBtmRight &&
                                   state != eMouseStateDraggingOuterTopLeft &&
                                   state != eMouseStateDraggingOuterTopRight &&
                                   state != eMouseStateDraggingOuterMidLeft &&
                                   state != eMouseStateDraggingOuterMidRight &&
                                   state != eMouseStateDraggingOuterTopMid &&
                                   state != eMouseStateDraggingOuterBtmMid;

    if (transformPatternCorners) {
        patternCorners[0] = interactMarker->getPatternTopLeftKnob();
        patternCorners[1] = interactMarker->getPatternBtmLeftKnob();
        patternCorners[2] = interactMarker->getPatternBtmRightKnob();
        patternCorners[3] = interactMarker->getPatternTopRightKnob();
    }

    searchWndTopRight = interactMarker->getSearchWindowTopRightKnob();
    searchWndBtmLeft = interactMarker->getSearchWindowBottomLeftKnob();

    QPointF centerPoint;
    centerPoint.rx() = centerKnob->getValueAtTime(time, 0);
    centerPoint.ry() = centerKnob->getValueAtTime(time, 1);

    QPointF offset;
    offset.rx() = offsetKnob->getValueAtTime(time, 0);
    offset.ry() = offsetKnob->getValueAtTime(time, 1);

    QPointF patternPoints[4];
    QPointF searchPoints[4];

    if (transformPatternCorners) {
        for (int i = 0; i < 4; ++i) {
            patternPoints[i].rx() = patternCorners[i]->getValueAtTime(time, 0) + centerPoint.x() + offset.x();
            patternPoints[i].ry() = patternCorners[i]->getValueAtTime(time, 1) + centerPoint.y() + offset.y();
        }
    }

    searchPoints[1].rx() = searchWndBtmLeft->getValueAtTime(time, 0) + centerPoint.x() + offset.x();
    searchPoints[1].ry() = searchWndBtmLeft->getValueAtTime(time, 1) + centerPoint.y() + offset.y();

    searchPoints[3].rx() = searchWndTopRight->getValueAtTime(time, 0) + centerPoint.x() + offset.x();
    searchPoints[3].ry() = searchWndTopRight->getValueAtTime(time, 1) + centerPoint.y() + offset.y();

    searchPoints[0].rx() = searchPoints[1].x();
    searchPoints[0].ry() = searchPoints[3].y();

    searchPoints[2].rx() = searchPoints[3].x();
    searchPoints[2].ry() = searchPoints[1].y();

    if ((state == eMouseStateDraggingInnerBtmLeft) ||
        (state == eMouseStateDraggingOuterBtmLeft)) {
        if (transformPatternCorners) {
            patternPoints[1].rx() += delta.x;
            patternPoints[1].ry() += delta.y;

            if (controlDown == 0) {
                patternPoints[0].rx() += delta.x;
                patternPoints[0].ry() -= delta.y;

                patternPoints[2].rx() -= delta.x;
                patternPoints[2].ry() += delta.y;

                patternPoints[3].rx() -= delta.x;
                patternPoints[3].ry() -= delta.y;
            }
        }

        searchPoints[1].rx() += delta.x;
        searchPoints[1].ry() += delta.y;

        if (controlDown == 0) {
            searchPoints[0].rx() += delta.x;
            searchPoints[0].ry() -= delta.y;

            searchPoints[2].rx() -= delta.x;
            searchPoints[2].ry() += delta.y;

            searchPoints[3].rx() -= delta.x;
            searchPoints[3].ry() -= delta.y;
        }
    } else if ((state == eMouseStateDraggingInnerBtmRight) ||
               (state == eMouseStateDraggingOuterBtmRight)) {
        if (transformPatternCorners) {


            patternPoints[2].rx() += delta.x;
            patternPoints[2].ry() += delta.y;

            if (controlDown == 0) {
                patternPoints[1].rx() -= delta.x;
                patternPoints[1].ry() += delta.y;

                patternPoints[0].rx() -= delta.x;
                patternPoints[0].ry() -= delta.y;

                patternPoints[3].rx() += delta.x;
                patternPoints[3].ry() -= delta.y;
            }
        }


        searchPoints[2].rx() += delta.x;
        searchPoints[2].ry() += delta.y;

        if (controlDown == 0) {
            searchPoints[1].rx() -= delta.x;
            searchPoints[1].ry() += delta.y;

            searchPoints[0].rx() -= delta.x;
            searchPoints[0].ry() -= delta.y;

            searchPoints[3].rx() += delta.x;
            searchPoints[3].ry() -= delta.y;
        }
    } else if ((state == eMouseStateDraggingInnerTopRight) ||
               (state == eMouseStateDraggingOuterTopRight)) {
        if (transformPatternCorners) {

            if (controlDown == 0) {
                patternPoints[1].rx() -= delta.x;
                patternPoints[1].ry() -= delta.y;

                patternPoints[0].rx() -= delta.x;
                patternPoints[0].ry() += delta.y;

                patternPoints[2].rx() += delta.x;
                patternPoints[2].ry() -= delta.y;
            }

            patternPoints[3].rx() += delta.x;
            patternPoints[3].ry() += delta.y;
        }

        if (controlDown == 0) {
            searchPoints[1].rx() -= delta.x;
            searchPoints[1].ry() -= delta.y;

            searchPoints[0].rx() -= delta.x;
            searchPoints[0].ry() += delta.y;

            searchPoints[2].rx() += delta.x;
            searchPoints[2].ry() -= delta.y;
        }

        searchPoints[3].rx() += delta.x;
        searchPoints[3].ry() += delta.y;
    } else if ((state == eMouseStateDraggingInnerTopLeft) ||
               (state == eMouseStateDraggingOuterTopLeft)) {
        if (transformPatternCorners) {
            patternPoints[0].rx() += delta.x;
            patternPoints[0].ry() += delta.y;

            if (controlDown == 0) {
                patternPoints[1].rx() += delta.x;
                patternPoints[1].ry() -= delta.y;

                patternPoints[2].rx() -= delta.x;
                patternPoints[2].ry() -= delta.y;

                patternPoints[3].rx() -= delta.x;
                patternPoints[3].ry() += delta.y;
            }
        }

        searchPoints[0].rx() += delta.x;
        searchPoints[0].ry() += delta.y;


        if (controlDown == 0) {
            searchPoints[1].rx() += delta.x;
            searchPoints[1].ry() -= delta.y;

            searchPoints[2].rx() -= delta.x;
            searchPoints[2].ry() -= delta.y;

            searchPoints[3].rx() -= delta.x;
            searchPoints[3].ry() += delta.y;
        }
    } else if ((state == eMouseStateDraggingInnerBtmMid) ||
               (state == eMouseStateDraggingOuterBtmMid)) {
        if (transformPatternCorners) {
            patternPoints[1].ry() += delta.y;
            patternPoints[2].ry() += delta.y;

            if (controlDown == 0) {
                patternPoints[0].ry() -= delta.y;
                patternPoints[3].ry() -= delta.y;
            }
        }

        searchPoints[1].ry() += delta.y;
        searchPoints[2].ry() += delta.y;

        if (controlDown == 0) {
            searchPoints[0].ry() -= delta.y;
            searchPoints[3].ry() -= delta.y;
        }
    } else if ((state == eMouseStateDraggingInnerTopMid) ||
               (state == eMouseStateDraggingOuterTopMid)) {
        if (transformPatternCorners) {
            if (controlDown == 0) {
                patternPoints[1].ry() -= delta.y;
                patternPoints[2].ry() -= delta.y;
            }

            patternPoints[0].ry() += delta.y;
            patternPoints[3].ry() += delta.y;
        }

        if (controlDown == 0) {
            searchPoints[1].ry() -= delta.y;
            searchPoints[2].ry() -= delta.y;
        }

        searchPoints[0].ry() += delta.y;
        searchPoints[3].ry() += delta.y;
    } else if ((state == eMouseStateDraggingInnerMidLeft) ||
               (state == eMouseStateDraggingOuterMidLeft)) {
        if (transformPatternCorners) {
            patternPoints[0].rx() += delta.x;
            patternPoints[1].rx() += delta.x;

            if (controlDown == 0) {
                patternPoints[2].rx() -= delta.x;
                patternPoints[3].rx() -= delta.x;
            }
        }

        searchPoints[0].rx() += delta.x;
        searchPoints[1].rx() += delta.x;

        if (controlDown == 0) {
            searchPoints[2].rx() -= delta.x;
            searchPoints[3].rx() -= delta.x;
        }
    } else if ((state == eMouseStateDraggingInnerMidRight) ||
               (state == eMouseStateDraggingOuterMidRight)) {
        if (transformPatternCorners) {
            if (controlDown == 0) {
                patternPoints[0].rx() -= delta.x;
                patternPoints[1].rx() -= delta.x;
            }

            patternPoints[2].rx() += delta.x;
            patternPoints[3].rx() += delta.x;
        }

        if (controlDown == 0) {
            searchPoints[0].rx() -= delta.x;
            searchPoints[1].rx() -= delta.x;
        }

        searchPoints[2].rx() += delta.x;
        searchPoints[3].rx() += delta.x;
    }

    EffectInstancePtr effect = context->getNode()->getEffectInstance();
    effect->beginChanges();

    if (transformPatternCorners) {
        for (int i = 0; i < 4; ++i) {
            patternPoints[i].rx() -= (centerPoint.x() + offset.x());
            patternPoints[i].ry() -= (centerPoint.y() + offset.y());


            if (patternCorners[i]->hasAnimation()) {
                patternCorners[i]->setValuesAtTime(time, patternPoints[i].x(), patternPoints[i].y(), ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
            } else {
                patternCorners[i]->setValues(patternPoints[i].x(), patternPoints[i].y(), ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
            }
        }
    }

    searchPoints[1].rx() -= (centerPoint.x() + offset.x());
    searchPoints[1].ry() -= (centerPoint.y() + offset.y());

    searchPoints[3].rx() -= (centerPoint.x() + offset.x());
    searchPoints[3].ry() -= (centerPoint.y() + offset.y());

    if (searchWndBtmLeft->hasAnimation()) {
        searchWndBtmLeft->setValuesAtTime(time, searchPoints[1].x(), searchPoints[1].y(), ViewSpec::current(),  eValueChangedReasonNatronInternalEdited);
    } else {
        searchWndBtmLeft->setValues(searchPoints[1].x(), searchPoints[1].y(), ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
    }

    if (searchWndTopRight->hasAnimation()) {
        searchWndTopRight->setValuesAtTime(time, searchPoints[3].x(), searchPoints[3].y(), ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
    } else {
        searchWndTopRight->setValues(searchPoints[3].x(), searchPoints[3].y(), ViewSpec::current(), eValueChangedReasonNatronInternalEdited);
    }

    effect->endChanges();

    refreshSelectedMarkerPreview();

    if (createKeyOnMoveButton.lock()->getValue()) {
        interactMarker->setUserKeyframe(time);
    }
} // TrackerNodeInteract::transformPattern

void
TrackerNodeInteract::onKeyframeSetOnTrack(const TrackMarkerPtr& marker,
        int key)
{
    makeMarkerKeyPreview(key, marker);
}

void
TrackerNodeInteract::onKeyframeRemovedOnTrack(const TrackMarkerPtr& marker,
        int key)
{
    for (TrackerNodeInteract::TrackKeysMap::iterator it = trackPreviews.begin(); it != trackPreviews.end(); ++it) {
        if (it->first.lock() == marker) {
            KeyFrameImages::iterator found = it->second.find(key);

            if (found != it->second.end()) {
                it->second.erase(found);
            }

            break;
        }
    }

    _p->publicInterface->redrawOverlayInteract();
}

void
TrackerNodeInteract::onAllKeyframesRemovedOnTrack(const TrackMarkerPtr& marker)
{
    for (TrackerNodeInteract::TrackKeysMap::iterator it = trackPreviews.begin(); it != trackPreviews.end(); ++it) {
        if (it->first.lock() == marker) {
            it->second.clear();
            break;
        }
    }

    _p->publicInterface->redrawOverlayInteract();
}

NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_TrackerNodeInteract.cpp"
