"""Exercise native overlay drawing and editing through the Vulkan window."""
import json
import os
from pathlib import Path
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Python overlay smoke started', encoding='utf-8')

from PySide6.QtCore import QTimer, QMetaObject, Q_RETURN_ARG, Qt, QEvent, QPointF
from PySide6.QtGui import QImage, QGuiApplication, QMouseEvent, QKeyEvent, QFocusEvent
from PySide6.QtWidgets import QApplication, QTabWidget, QAbstractButton
import NatronEngine
import NatronGui

report = {'ok': False, 'stages': {}}
widgets = []
roto = curve = viewer_node = None
tracker_colors = ((160, 32, 16), (16, 160, 32), (32, 16, 160), (192, 192, 192))


def finish(error=None):
    if error:
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
    viewers = [w for w in QApplication.allWidgets() if w.property('graphicsBackend') == 'Vulkan']
    report['graphics_errors'] = [w.property('graphicsError') for w in viewers]
    report['validation_errors'] = [w.property('validationErrors') for w in viewers]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets()
                                     if w.isVisible() and w.isModal()]
    report['devices'] = [w.property('gpuDeviceName') for w in viewers]
    report['ok'] = not error and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs']
    Path('build/overlay-gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('OVERLAY_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def visible_viewer():
    viewers = [w for w in QApplication.allWidgets()
               if w.property('graphicsBackend') == 'Vulkan' and w.isVisible()]
    assert len(viewers) == 1, f'Expected one visible Vulkan Viewer, got {len(viewers)}'
    return viewers[0]


def capture(viewer):
    frame = QMetaObject.invokeMethod(viewer, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
    assert not frame.isNull()
    assert viewer.property('nodeOverlaysAvailable'), 'Native overlays are disabled'
    return frame


def xy(value):
    return (value[0], value[1])


def inspect_handle_pixels(viewer, frame, x, y):
    position = widget_point(viewer, x, y)
    px = round(position.x() * frame.width() / viewer.width())
    py = round(position.y() * frame.height() / viewer.height())
    radius = max(3, round(3 * viewer.devicePixelRatioF()))
    colors = [frame.pixelColor(cx, cy)
              for cy in range(max(0, py-radius), min(frame.height(), py+radius+1))
              for cx in range(max(0, px-radius), min(frame.width(), px+radius+1))]
    assert any(min(c.red(), c.green(), c.blue()) > 140 for c in colors), 'Native handle pixels are missing'
    assert viewer.property('nodeOverlayCommandCount') > 0, 'Native overlay commands were not produced'


def select_transform_page():
    tabs = [w for w in QApplication.allWidgets() if isinstance(w, QTabWidget) and w.isVisible()]
    report['parameter_tabs'] = []
    selected = False
    for tab in tabs:
        names = [tab.tabText(i) for i in range(tab.count())]
        report['parameter_tabs'].append(names)
        for index, name in enumerate(names):
            if name == 'Transform':
                if index != 0:
                    tab.setCurrentIndex(0)
                tab.setCurrentIndex(index)
                selected = True
    assert selected, 'Tracker Transform parameter page was not found'


def widget_point(viewer, x, y):
    viewport = viewer.property('canonicalViewport')
    return QPointF((x - viewport.x()) / viewport.width() * viewer.width(),
                   (viewport.y() + viewport.height() - y) / viewport.height() * viewer.height())


def drag(viewer, start, end):
    surfaces = [w for w in QGuiApplication.allWindows() if w.objectName() == viewer.objectName() + '/surface']
    assert len(surfaces) == 1
    def send(kind, point, button, buttons):
        global_point = QPointF(viewer.mapToGlobal(point.toPoint()))
        event = QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier)
        QGuiApplication.sendEvent(surfaces[0], event)
    send(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
    send(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    send(QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton)
    send(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)


def inspect_roto():
    viewer = visible_viewer()
    frame = capture(viewer)
    frame.save('build/overlay-gui-frame.png')
    frame.save('build/overlay-roto-frame.png')
    viewport = viewer.property('canonicalViewport')
    report['viewport'] = [viewport.x(), viewport.y(), viewport.width(), viewport.height()]
    # The magenta contour is away from the source pixels and tests raster
    # composition, canonical placement, and the native window's row direction.
    target = widget_point(viewer, 32, 36)
    px, py = round(target.x() * frame.width() / viewer.width()), round(target.y() * frame.height() / viewer.height())
    pixels = [frame.pixelColor(x, y) for y in range(max(0, py-3), min(frame.height(), py+4))
              for x in range(max(0, px-3), min(frame.width(), px+4))]
    assert any(c.red() > 180 and c.blue() > 180 and c.green() < 90 for c in pixels), 'Roto contour is missing from the Vulkan frame'
    before = curve.getControlPointPosition(0, 1)
    drag(viewer, widget_point(viewer, before[0], before[1]), widget_point(viewer, before[0] + 3, before[1] + 2))
    after = curve.getControlPointPosition(0, 1)
    tolerance = max(viewport.width() / viewer.width(), viewport.height() / viewer.height())
    assert abs(after[0] - before[0] - 3) <= tolerance and abs(after[1] - before[1] - 2) <= tolerance, f'Roto point did not follow the native input: {before} -> {after}'
    report['stages']['roto'] = {'contour': True, 'drag': True, 'point_before': list(before[:2]), 'point_after': list(after[:2])}
    frame = capture(viewer)
    frame.save('build/overlay-gui-frame.png')
    transform = app.createNode('fr.inria.built-in.Tracker', -1, app,
                               {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert transform is not None
    transform.getParam('motionType').set(2)
    transform.getParam('transformType').set(0)
    assert transform.connectInput(0, reader), 'Could not connect the Transform input'
    viewer_node.disconnectInput(0)
    assert viewer_node.connectInput(0, transform), 'Could not reconnect the Viewer to Transform'
    widgets.append(transform)
    select_transform_page()
    QTimer.singleShot(2000, guarded(inspect_transform))


def inspect_transform():
    select_transform_page()
    viewer = visible_viewer()
    transform = widgets[-1]
    center = xy(transform.getParam('center').get())
    translate = transform.getParam('translate')
    before = xy(translate.get())
    start = widget_point(viewer, center[0] + before[0], center[1] + before[1])
    end = widget_point(viewer, center[0] + before[0] + 4, center[1] + before[1] + 3)
    frame = capture(viewer)
    frame.save('build/overlay-transform-before.png')
    inspect_handle_pixels(viewer, frame, center[0] + before[0], center[1] + before[1])
    report['transform_center'] = list(center)
    report['native_overlays'] = list(viewer.property('nodeOverlayStatus'))
    report['native_overlay_commands'] = viewer.property('nodeOverlayCommandCount')
    report['transform_parameters'] = {name: {'visible': transform.getParam(name).getIsVisible(),
                                            'enabled': transform.getParam(name).getIsEnabled()}
                                      for name in ('translate', 'center', 'motionType', 'transformType', 'Transform')}
    report['transform_viewport'] = [viewer.property('canonicalViewport').x(), viewer.property('canonicalViewport').y()]
    drag(viewer, start, end)
    after = xy(translate.get())
    report['stages']['transform'] = {'drag': True, 'translation_before': list(before), 'translation_after': list(after)}
    report['roto_point_after_transform'] = list(curve.getControlPointPosition(0, 1)[:2])
    viewport = viewer.property('canonicalViewport')
    tolerance = max(viewport.width() / viewer.width(), viewport.height() / viewer.height(), 0.1)
    assert abs(after[0] - before[0] - 4) <= tolerance and abs(after[1] - before[1] - 3) <= tolerance, f'Transform handle did not follow native input: {list(before)} -> {list(after)}'
    frame = capture(viewer)
    frame.save('build/overlay-gui-frame.png')
    transform.getParam('transformType').set(1)
    transform.getParam('to1').set(16, 12)
    QTimer.singleShot(1000, guarded(inspect_corner_pin))


def inspect_corner_pin():
    viewer = visible_viewer()
    corner = widgets[-1].getParam('to1')
    before = xy(corner.get())
    frame = capture(viewer)
    inspect_handle_pixels(viewer, frame, *before)
    drag(viewer, widget_point(viewer, *before), widget_point(viewer, before[0] + 2, before[1] + 3))
    after = xy(corner.get())
    report['stages']['corner_pin'] = {'drag': True, 'point_before': list(before), 'point_after': list(after)}
    viewport = viewer.property('canonicalViewport')
    tolerance = max(viewport.width() / viewer.width(), viewport.height() / viewer.height(), 0.1)
    assert abs(after[0] - before[0] - 2) <= tolerance and abs(after[1] - before[1] - 3) <= tolerance, f'Corner Pin handle did not follow native input: {before} -> {after}'
    frame = capture(viewer)
    frame.save('build/overlay-gui-frame.png')
    report['framebuffer_size'] = [frame.width(), frame.height()]
    report['device_pixel_ratio'] = viewer.devicePixelRatioF()
    QTimer.singleShot(0, guarded(setup_tracker))


def select_tracking_page():
    for tab in QApplication.allWidgets():
        if isinstance(tab, QTabWidget) and tab.isVisible():
            for index in range(tab.count()):
                if tab.tabText(index) == 'Tracking':
                    tab.setCurrentIndex(index)


def setup_tracker():
    global tracker, track
    tracker = widgets[-1]
    tracker.getParam('motionType').set(0)
    tracker.getParam('magWindowSize').set(120)
    source = Path('build/qt6-smoke/tracker-source.ppm')
    source.write_bytes(b'P6\n64 48\n255\n' + b''.join(bytes(tracker_colors[(y >= 24)*2 + (x >= 32)])
                                                          for y in range(48) for x in range(64)))
    tracker_reader = app.createReader(source.resolve().as_posix())
    tracker_reader.getParam('ocioInputSpace').set('sRGB')
    tracker_reader.getParam('ocioOutputSpace').set('Linear')
    report['tracker_read_colorspaces'] = {name: tracker_reader.getParam(name).get()
                                          for name in ('ocioInputSpace', 'ocioOutputSpace')}
    widgets.append(tracker_reader)
    tracker.disconnectInput(0)
    assert tracker.connectInput(0, tracker_reader)
    track = tracker.getTrackerContext().createTrack()
    assert track
    track.getParam('centerPoint').set(32, 24)
    for name, point in {'searchWndBtmLeft': (-12, -12), 'searchWndTopRight': (12, 12),
                        'patternTopLeft': (-4, 4), 'patternTopRight': (4, 4),
                        'patternBtmRight': (4, -4), 'patternBtmLeft': (-4, -4)}.items():
        track.getParam(name).set(*point)
    select_tracking_page()
    QTimer.singleShot(1500, guarded(select_tracker))


def select_tracker():
    select_tracking_page()
    viewer = visible_viewer()
    point = widget_point(viewer, 32, 24)
    drag(viewer, point, point)
    selected = tracker.getTrackerContext().getSelectedTracks()
    assert len(selected) == 1, 'Native click did not select the tracking marker'
    tracker.getParam('setPatternKey').trigger()
    QTimer.singleShot(2500, guarded(inspect_tracker))


def inspect_tracker():
    viewer = visible_viewer()
    frame = capture(viewer)
    frame.save('build/overlay-tracker-before.png')
    # The hovered center is green; the unhovered pattern corner is neutral.
    inspect_handle_pixels(viewer, frame, 36, 28)
    # Both images are above/outside the fitted source. Sample within each
    # quadrant, away from marker handles and labels, using logical coordinates.
    scale_x, scale_y = frame.width()/viewer.width(), frame.height()/viewer.height()
    preview_samples = []
    for width, offset in ((120, 0), (75, 120)):
        actual = []
        for y in (0.3, 0.8):
            for x in (0.2, 0.8):
                color = frame.pixelColor(round((offset+x*width)*scale_x), round(y*width*scale_y))
                actual.append([color.red(), color.green(), color.blue()])
        preview_samples.append(actual)
        assert all(max(abs(a-b) for a, b in zip(pixel, expected)) <= 2
                   for pixel, expected in zip(actual, tracker_colors)), f'Tracker preview rows/channels or sRGB conversion differ: {actual}'
        edge = []
        for x in (width/2-2, width/2+2):
            color = frame.pixelColor(round((offset+x)*scale_x), round(width*0.3*scale_y))
            edge.append([color.red(), color.green(), color.blue()])
        assert all(max(abs(a-b) for a, b in zip(pixel, expected)) <= 2
                   for pixel, expected in zip(edge, tracker_colors[:2])), f'Magnified source pixels were blurred: {edge}'
    before = xy(track.getParam('centerPoint').get())
    drag(viewer, widget_point(viewer, *before), widget_point(viewer, before[0]+2, before[1]+3))
    after = xy(track.getParam('centerPoint').get())
    viewport = viewer.property('canonicalViewport')
    tolerance = max(viewport.width()/viewer.width(), viewport.height()/viewer.height(), 0.1)
    assert abs(after[0]-before[0]-2) <= tolerance and abs(after[1]-before[1]-3) <= tolerance, f'Tracker marker did not follow native input: {before} -> {after}'
    report['stages']['tracker'] = {'drag': True, 'center_before': list(before), 'center_after': list(after),
                                    'magnifier_and_keyframe_rgb': preview_samples,
                                    'native_overlay_commands': viewer.property('nodeOverlayCommandCount')}
    QTimer.singleShot(500, guarded(inspect_tracker_magnifier))


def inspect_tracker_magnifier():
    viewer = visible_viewer()
    size = tracker.getParam('magWindowSize')
    before_size = size.get()
    drag(viewer, QPointF(before_size, before_size), QPointF(before_size+16, before_size))
    after_size = size.get()
    assert abs(after_size-before_size-16) <= 1, f'Magnifier resize handle did not respond: {before_size} -> {after_size}'
    before = xy(track.getParam('centerPoint').get())
    start = QPointF(after_size*0.75, after_size*0.65)
    drag(viewer, start, start+QPointF(8, 6))
    after = xy(track.getParam('centerPoint').get())
    viewport = viewer.property('canonicalViewport')
    dx, dy = viewport.width()/viewer.width(), viewport.height()/viewer.height()
    assert abs(after[0]-before[0]-8*dx) <= dx and abs(after[1]-before[1]+6*dy) <= dy, f'Magnifier drag did not move the marker: {before} -> {after}'
    report['stages']['tracker_magnifier'] = {'resize': True, 'width_before': before_size, 'width_after': after_size,
                                             'drag': True, 'center_before': list(before), 'center_after': list(after)}
    QTimer.singleShot(500, guarded(inspect_tracker_focus))


def inspect_tracker_focus():
    viewer = visible_viewer()
    surfaces = [w for w in QGuiApplication.allWindows() if w.objectName() == viewer.objectName()+'/surface']
    assert len(surfaces) == 1
    surface = surfaces[0]
    center = track.getParam('centerPoint')
    offset = track.getParam('offset')
    before = xy(center.get())
    offset.set(0, 0)
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    drag(viewer, widget_point(viewer, *before), widget_point(viewer, before[0]+1, before[1]+1))
    viewport = viewer.property('canonicalViewport')
    tolerance = max(viewport.width()/viewer.width(), viewport.height()/viewer.height(), 0.1)
    control_offset = xy(offset.get())
    unchanged = xy(center.get())
    assert all(abs(a-b) < 0.001 for a, b in zip(unchanged, before)), f'Control drag moved the center: {before} -> {unchanged}'
    assert all(abs(value-1) <= tolerance for value in control_offset), f'Native Control key did not enable offset drag: {control_offset}'
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
    offset.set(0, 0)
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
    # No key release arrives when another window interrupts a held modifier.
    drag(viewer, widget_point(viewer, *before), widget_point(viewer, before[0]+2, before[1]+1))
    after = xy(center.get())
    final_offset = xy(offset.get())
    assert abs(after[0]-before[0]-2) <= tolerance and abs(after[1]-before[1]-1) <= tolerance, f'Focus loss left Control active: {before} -> {after}, offset={final_offset}'
    assert all(abs(value) < 0.001 for value in final_offset), f'Focus loss left offset drag active: {final_offset}'
    report['stages']['tracker_focus'] = {'native_control_offset': control_offset, 'center_before': before,
                                       'center_after': after, 'offset_after_focus_loss': final_offset}
    context = tracker.getTrackerContext()
    count_before = len(context.getAllTracks())
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Alt, Qt.ControlModifier | Qt.AltModifier))
    point = widget_point(viewer, 6, 6)
    drag(viewer, point, point)
    count_with_modifiers = len(context.getAllTracks())
    assert count_with_modifiers == count_before+1, 'Native Ctrl+Alt did not enable temporary track addition'
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyRelease, Qt.Key_Alt, Qt.ControlModifier))
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Alt, Qt.ControlModifier | Qt.AltModifier))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
    point = widget_point(viewer, 56, 6)
    drag(viewer, point, point)
    count_after_focus_loss = len(context.getAllTracks())
    assert count_after_focus_loss == count_with_modifiers, f'Focus loss left temporary track addition active: {count_with_modifiers} -> {count_after_focus_loss}'
    report['stages']['tracker_focus']['temporary_add_counts'] = (count_before, count_with_modifiers, count_after_focus_loss)
    add_buttons = [w for w in QApplication.allWidgets() if isinstance(w, QAbstractButton)
                   and w.isVisible() and w.isCheckable() and 'add new tracks' in w.toolTip().lower()]
    assert len(add_buttons) == 1, f'Expected one visible Add Track button, got {len(add_buttons)}'
    add_button = add_buttons[0]
    assert not add_button.isChecked(), 'Temporary Add Track button remained checked'
    add_button.click()
    assert add_button.isChecked(), 'Manual Add Track mode did not activate'
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.NoModifier))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
    assert add_button.isChecked(), 'Modifier release or focus loss cleared the manually selected Add Track mode'
    point = widget_point(viewer, 6, 42)
    drag(viewer, point, point)
    assert len(context.getAllTracks()) == count_with_modifiers+1, 'Manual Add Track mode no longer added a marker'
    add_button.click()
    assert not add_button.isChecked()
    report['stages']['tracker_focus']['manual_add_preserved'] = True
    QTimer.singleShot(500, guarded(inspect_tracker_drag_interruption))


def inspect_tracker_drag_interruption():
    viewer = visible_viewer()
    surfaces = [w for w in QGuiApplication.allWindows() if w.objectName() == viewer.objectName()+'/surface']
    assert len(surfaces) == 1
    surface = surfaces[0]
    def send(kind, point, button, buttons):
        global_point = QPointF(viewer.mapToGlobal(point.toPoint()))
        QGuiApplication.sendEvent(surface, QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier))
    center = track.getParam('centerPoint')
    before = xy(center.get())
    start = widget_point(viewer, *before)
    send(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
    send(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    send(QEvent.MouseMove, widget_point(viewer, before[0]+2, before[1]+1), Qt.NoButton, Qt.LeftButton)
    at_interruption = xy(center.get())
    assert abs(at_interruption[0]-before[0]) > 1, 'The interrupted drag never started'
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
    end = widget_point(viewer, before[0]+5, before[1]+3)
    send(QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton)
    send(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
    after = xy(center.get())
    assert all(abs(a-b) < 0.001 for a, b in zip(after, at_interruption)), f'Focus loss did not cancel the active Tracker drag: {at_interruption} -> {after}'
    report['stages']['tracker_focus']['drag_cancelled'] = {'before': before, 'at_interruption': at_interruption, 'after': after}
    QTimer.singleShot(500, guarded(inspect_tracker_modal_interruption))


def inspect_tracker_modal_interruption():
    viewer = visible_viewer()
    surfaces = [w for w in QGuiApplication.allWindows() if w.objectName() == viewer.objectName()+'/surface']
    assert len(surfaces) == 1
    surface = surfaces[0]
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    report['modal_center_before'] = xy(track.getParam('centerPoint').get())
    def dismiss():
        dialogs = [w for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()
                   and w.windowTitle() == 'Vulkan Focus Smoke']
        assert len(dialogs) == 1, f'Expected one controlled focus-test dialog, got {len(dialogs)}'
        report['expected_dialogs'] = [dialogs[0].windowTitle()]
        # These notifications arrive while the host suppresses plugin callbacks
        # during its modal loop. They must be delivered after that loop returns.
        QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
        QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
        dialogs[0].accept()
    QTimer.singleShot(200, guarded(dismiss))
    NatronGui.natron.informationDialog('Vulkan Focus Smoke', 'Automatically dismissed test of interrupted Viewer input.')
    QTimer.singleShot(500, guarded(inspect_tracker_modal_return))


def inspect_tracker_modal_return():
    viewer = visible_viewer()
    before = report['modal_center_before']
    drag(viewer, widget_point(viewer, *before), widget_point(viewer, before[0]+1, before[1]+1))
    after = xy(track.getParam('centerPoint').get())
    offset = xy(track.getParam('offset').get())
    viewport = viewer.property('canonicalViewport')
    tolerance = max(viewport.width()/viewer.width(), viewport.height()/viewer.height(), 0.1)
    assert all(abs(a-b-1) <= tolerance for a, b in zip(after, before)) and all(abs(value) < 0.001 for value in offset), f'Modal interruption left a modifier active: {before} -> {after}, offset={offset}'
    report['stages']['tracker_focus']['modal_interruption'] = {'center_before': before, 'center_after': after,
                                                            'offset_after': offset, 'dialog_dismissed': True}
    QTimer.singleShot(500, guarded(finish_tracker))


def finish_tracker():
    viewer = visible_viewer()
    frame = capture(viewer)
    frame.save('build/overlay-gui-frame.png')
    finish()


def setup():
    global roto, curve, viewer_node, reader
    app.addFormat('VulkanOverlaySmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('VulkanOverlaySmoke')
    app.getProjectParam('frameRange').set(1, 1)
    source = Path('build/qt6-smoke/overlay-source.ppm')
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b'P6\n64 48\n255\n' + bytes((0, 40, 40)) * (64*48))
    reader = app.createReader(source.resolve().as_posix())
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    roto = app.createNode('fr.inria.built-in.Roto', -1, app,
                          {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert roto and roto.connectInput(0, reader) and viewer_node.connectInput(0, roto)
    curve = roto.getRotoContext().createRectangle(16, 36, 24, 1)
    curve.setOverlayColor(1, 0, 1)
    roto.getParam('SelectAllTool').trigger()
    QTimer.singleShot(6000, guarded(inspect_roto))


QTimer.singleShot(0, guarded(setup))
