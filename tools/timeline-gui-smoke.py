"""Verify timeline raster drawing and real timeline/Viewer interactions."""
import json
import os
from pathlib import Path
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Timeline smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QPoint, QPointF, QTimer, Qt, QMetaObject, Q_ARG
from PySide6.QtGui import QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication, QLineEdit, QMenu
import NatronEngine

report = {'ok': False, 'stages': {}}


def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def finish(error=None):
    if error:
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
    viewers = [w for w in QApplication.allWidgets() if w.property('graphicsBackend') == 'Vulkan']
    report['graphics_errors'] = [w.property('graphicsError') for w in viewers]
    report['validation_errors'] = [w.property('validationErrors') for w in viewers]
    report['devices'] = [w.property('gpuDeviceName') for w in viewers]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets()
                                    if w.isVisible() and w.isModal()]
    report['ok'] = not error and bool(viewers) and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs']
    Path('build/timeline-gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('TIMELINE_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def timeline():
    widgets = [w for w in QApplication.allWidgets() if w.isVisible() and w.metaObject().className().endswith('TimeLineGui')]
    assert len(widgets) == 1, f'Expected one visible timeline, got {len(widgets)}'
    widget = widgets[0]
    assert not widget.inherits('QOpenGLWidget'), 'Timeline still requires a GL context'
    assert widget.property('graphicsBackend') == 'Raster'
    return widget


def capture(widget):
    image = widget.grab().toImage()
    assert not image.isNull()
    assert widget.property('timelineCommandCount') > 3, 'Timeline drawing commands are missing'
    assert abs(image.width()-widget.width()*widget.devicePixelRatioF()) <= 1
    assert abs(image.height()-widget.height()*widget.devicePixelRatioF()) <= 1
    return image


def frame_point(widget, value, y=None):
    viewport = widget.property('timelineViewport')
    return QPointF((value-viewport.x())/viewport.width()*widget.width(), widget.height()/2 if y is None else y)


def marker_rows(widget, image, value, setting):
    color = NatronEngine.natron.getSettings().getParam(setting).get()
    expected = tuple(round(color[i]*255) for i in range(3))
    px = round(frame_point(widget, value).x()*image.width()/widget.width())
    assert 0 <= px < image.width()
    rows = []
    for y in range(image.height()):
        sample = image.pixelColor(px, y)
        rgb = (sample.red(), sample.green(), sample.blue())
        if max(abs(a-b) for a, b in zip(rgb, expected)) <= 2:
            rows.append(y)
    return expected, rows


def inspect_marker_color(widget, image, value, setting):
    expected, rows = marker_rows(widget, image, value, setting)
    assert rows, f'Timeline {setting} marker is missing at {value}, expected {expected}'
    return {'rgb': expected, 'physical_rows': rows}


def drag(widget, start, end, button=Qt.LeftButton, modifiers=Qt.NoModifier):
    def send(kind, point, changed, held):
        global_point = QPointF(widget.mapToGlobal(point.toPoint()))
        QApplication.sendEvent(widget, QMouseEvent(kind, point, point, global_point, changed, held, modifiers))
    send(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
    send(QEvent.MouseButtonPress, start, button, button)
    send(QEvent.MouseMove, end, Qt.NoButton, button)
    send(QEvent.MouseButtonRelease, end, button, Qt.NoButton)


def inspect_initial():
    widget = timeline()
    frame = capture(widget)
    frame.save('build/timeline-before.png')
    def rgb(value):
        position = frame_point(widget, value, widget.height()*0.2)
        px, py = round(position.x()*frame.width()/widget.width()), round(position.y()*frame.height()/widget.height())
        assert 0 <= px < frame.width() and 0 <= py < frame.height(), f'Timeline sample is outside the frame: {value}, {px},{py}'
        color = frame.pixelColor(px, py)
        return (color.red(), color.green(), color.blue())
    viewport = widget.property('timelineViewport')
    outside_value = viewport.x()+(1-viewport.x())*0.1 if viewport.x() < 1 else 20+(viewport.x()+viewport.width()-20)*0.9
    assert outside_value < 1 or outside_value > 20
    inside, outside = rgb(5), rgb(outside_value)
    assert inside != outside, f'Timeline frame-range background is missing: {inside} / {outside}'
    colors = {frame.pixelColor(x, y).rgba() for y in range(frame.height()) for x in range(frame.width())}
    assert len(colors) > 10, f'Timeline ticks, text and handles are missing: {len(colors)} colors'
    # Sample inside each one-frame bar; fractional DPI can partially cover its edge pixels.
    report['stages']['markers'] = {'cache_frame_1': inspect_marker_color(widget, frame, 1.5, 'cachedFrame'),
                                  'key_frame_5': inspect_marker_color(widget, frame, 5.5, 'keyframe'),
                                  'key_frame_10': inspect_marker_color(widget, frame, 10.5, 'keyframe')}
    before = app.timelineGetTime()
    drag(widget, frame_point(widget, 7), frame_point(widget, 11))
    after = app.timelineGetTime()
    assert after == 11, f'Timeline scrub did not reach frame 11: {before} -> {after}'
    report['stages']['initial'] = {'inside_rgb': inside, 'outside_rgb': outside, 'colors': len(colors),
                                 'physical_size': (frame.width(), frame.height()), 'device_pixel_ratio': widget.devicePixelRatioF(),
                                 'font_point_size': widget.property('timelineFontPointSize')}
    report['stages']['scrub'] = {'before': before, 'after': after}
    QTimer.singleShot(800, guarded(inspect_range))


def inspect_range():
    widget = timeline()
    image = capture(widget)
    assert widget.property('currentFrame') == app.timelineGetTime() == 11
    report['stages']['markers']['cache_frame_11'] = inspect_marker_color(widget, image, 11.5, 'cachedFrame')
    bounds = widget.property('timelineBounds')
    before = (bounds.x(), bounds.y())
    drag(widget, frame_point(widget, 20), frame_point(widget, 17), modifiers=Qt.ControlModifier)
    capture(widget)
    bounds = widget.property('timelineBounds')
    after = (bounds.x(), bounds.y())
    assert after == (before[0], 17), f'Timeline right-bound drag failed: {before} -> {after}'
    outputs = [w for w in QApplication.allWidgets() if isinstance(w, QLineEdit) and w.isVisible()
               and w.toolTip() == 'The playback out point']
    assert len(outputs) == 1 and float(outputs[0].text()) == 17, 'Timeline edit did not update the playback out control'
    project_bounds = (app.timelineGetLeftBound(), app.timelineGetRightBound())
    assert project_bounds == (1, 20), f'Playback edit changed the project frame range: {project_bounds}'
    report['stages']['bounds'] = {'before': before, 'after': after, 'playback_out_control': outputs[0].text(),
                                'project_bounds': project_bounds}
    QTimer.singleShot(500, guarded(inspect_zoom))


def inspect_zoom():
    widget = timeline()
    capture(widget)
    before = widget.property('timelineViewport')
    point = QPointF(widget.width()/2, widget.height()/2)
    event = QWheelEvent(point, QPointF(widget.mapToGlobal(point.toPoint())), QPoint(), QPoint(0, 120),
                        Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
    QApplication.sendEvent(widget, event)
    capture(widget)
    after = widget.property('timelineViewport')
    assert after.width() < before.width(), f'Timeline wheel did not zoom: {before} -> {after}'
    report['stages']['wheel'] = {'width_before': before.width(), 'width_after': after.width()}
    left_before = after.x()
    drag(widget, point, point+QPointF(30, 0), button=Qt.MiddleButton)
    capture(widget)
    left_after = widget.property('timelineViewport').x()
    assert left_after < left_before, f'Timeline middle pan did not move the viewport: {left_before} -> {left_after}'
    report['stages']['pan'] = {'left_before': left_before, 'left_after': left_after}
    # Fit a concrete selected range using the existing right-button gesture.
    drag(widget, frame_point(widget, 4), frame_point(widget, 14), button=Qt.RightButton)
    QTimer.singleShot(500, guarded(inspect_selected_range))


def inspect_selected_range():
    widget = timeline()
    frame = capture(widget)
    viewport = widget.property('timelineViewport')
    assert viewport.x() <= 4 and viewport.x()+viewport.width() >= 14 and viewport.width() < 25, f'Right-button range fit failed: {viewport}'
    bounds = widget.property('timelineBounds')
    assert (bounds.x(), bounds.y()) == (1, 17)
    frame.save('build/timeline-gui-frame.png')
    report['stages']['range_fit'] = {'left': viewport.x(), 'right': viewport.x()+viewport.width()}
    assert '11' in widget.property('timelineLabels'), 'Current-frame label was not drawn'
    assert QMetaObject.invokeMethod(widget, 'onTimeFormatChanged', Qt.DirectConnection, Q_ARG(int, 0))
    timecode = capture(widget)
    labels = widget.property('timelineLabels')
    assert '00:00:00:11' in labels and timecode != frame, f'Timecode text did not change: {labels}'
    timecode.save('build/timeline-timecode.png')
    report['stages']['timecode'] = {'current_label': '00:00:00:11', 'image_changed': True}
    viewers = [w for w in QApplication.allWidgets() if w.property('graphicsBackend') == 'Vulkan' and w.isVisible()]
    assert len(viewers) == 1 and viewers[0].property('presentedFrames') > 0
    report['presented_frames'] = viewers[0].property('presentedFrames')
    report['stages']['cache_clear'] = {'before': widget.property('timelineCachedFrames')}
    menus = [w for w in QApplication.allWidgets() if isinstance(w, QMenu) and w.title().replace('&', '') == 'Cache']
    assert len(menus) == 1, f'Expected one cache menu, got {len(menus)}'
    actions = [action for action in menus[0].actions() if action.text().replace('&', '') == 'Clear Per-Node Cache']
    assert len(actions) == 1, f'Expected one node-cache action, got {len(actions)}'
    actions[0].trigger()
    QTimer.singleShot(500, guarded(inspect_cache_clear))


def inspect_cache_clear():
    widget = timeline()
    frame = capture(widget)
    after = widget.property('timelineCachedFrames')
    assert after == [], f'Cleared cache still reports retained frames: {after}'
    assert not marker_rows(widget, frame, 11.5, 'cachedFrame')[1], 'Cleared cache still has a green marker'
    report['stages']['cache_clear']['after'] = after
    drag(widget, frame_point(widget, 11), frame_point(widget, 12))
    assert app.timelineGetTime() == 12
    QTimer.singleShot(1000, guarded(inspect_cache_refill))


def inspect_cache_refill():
    widget = timeline()
    frame = capture(widget)
    frames = widget.property('timelineCachedFrames')
    assert 12 in frames and 1 not in frames and 11 not in frames, f'Unexpected refilled cache: {frames}'
    report['stages']['cache_refill'] = {'frames': frames, 'marker': inspect_marker_color(widget, frame, 12.5, 'cachedFrame')}
    drag(widget, frame_point(widget, 12), frame_point(widget, 13))
    assert app.timelineGetTime() == 13
    global replacement
    replacement = app.createNode('net.sf.openfx.ConstantPlugin')
    replacement.getParam('color').set(0.7, 0.1, 0.2, 1)
    replacement.getParam('color').setValueAtTime(0.7, 5, 0)
    replacement.getParam('color').setValueAtTime(0.8, 10, 0)
    viewer_node.disconnectInput(0)
    assert viewer_node.connectInput(0, replacement)
    QTimer.singleShot(1500, guarded(inspect_input_change))


def inspect_input_change():
    widget = timeline()
    frame = capture(widget)
    frames = widget.property('timelineCachedFrames')
    assert 13 in frames and 12 not in frames, f'Previous input cache is still marked: {frames}'
    report['stages']['input_change'] = {'frames': frames, 'marker': inspect_marker_color(widget, frame, 13.5, 'cachedFrame')}
    finish()


def setup():
    global source, viewer_node
    app.addFormat('TimelineSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('TimelineSmoke')
    app.getProjectParam('frameRange').set(1, 20)
    source = app.createNode('net.sf.openfx.ConstantPlugin', -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert source is not None
    source.getParam('color').set(0.2, 0.3, 0.4, 1)
    source.getParam('color').setValueAtTime(0.2, 5, 0)
    source.getParam('color').setValueAtTime(0.4, 10, 0)
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    assert viewer_node.connectInput(0, source)
    QTimer.singleShot(6000, guarded(inspect_initial))


QTimer.singleShot(0, guarded(setup))
