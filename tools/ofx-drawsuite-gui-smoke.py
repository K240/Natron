"""Exercise the independent OFX DrawSuite plugin in the native Vulkan Viewer."""
import ctypes
import json
import os
from pathlib import Path
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('OFX DrawSuite smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QMetaObject, QPointF, Q_RETURN_ARG, QTimer, Qt
from PySide6.QtGui import QGuiApplication, QImage, QMouseEvent, QKeyEvent, QFocusEvent
from PySide6.QtWidgets import QApplication, QLabel
import NatronEngine

report = {'ok': False, 'stages': {}}
effect = library = None


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
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets()
                                    if w.isVisible() and w.isModal()]
    report['devices'] = [w.property('gpuDeviceName') for w in viewers]
    report['ok'] = not error and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs']
    Path('build/ofx-drawsuite-gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('OFX_DRAW_SUITE_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def viewer():
    viewers = [w for w in QApplication.allWidgets()
               if w.property('graphicsBackend') == 'Vulkan' and w.isVisible()]
    assert len(viewers) == 1, f'Expected one visible Vulkan Viewer, got {len(viewers)}'
    return viewers[0]


def capture(widget):
    frame = QMetaObject.invokeMethod(widget, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
    assert not frame.isNull()
    assert widget.property('nodeOverlaysAvailable')
    assert widget.property('nodeOverlayCommandCount') >= 7, 'OFX DrawSuite commands are missing'
    return frame


def widget_point(widget, x, y):
    viewport = widget.property('canonicalViewport')
    return QPointF((x-viewport.x())/viewport.width()*widget.width(),
                   (viewport.y()+viewport.height()-y)/viewport.height()*widget.height())


def frame_point(widget, frame, x, y):
    point = widget_point(widget, x, y)
    return round(point.x()*frame.width()/widget.width()), round(point.y()*frame.height()/widget.height())


def sample(widget, frame, x, y, expected):
    px, py = frame_point(widget, frame, x, y)
    color = frame.pixelColor(px, py)
    rgb = (color.red(), color.green(), color.blue())
    assert max(abs(a-b) for a, b in zip(rgb, expected)) <= 2, f'{x},{y}: expected {expected}, got {rgb}'
    return rgb


def counters():
    return dict(zip(('legacy_calls', 'native_draws', 'expired_context_rejections', 'invalid_context_acceptances', 'pen_motions',
                     'focus_gains', 'focus_losses', 'focus_callbacks_inside_modal', 'control_held', 'dialogs_opened', 'active_drag'),
                    (library.OfxDrawSuiteSmokeCounter(i) for i in range(11))))


def drag(widget, start, end):
    surfaces = [w for w in QGuiApplication.allWindows() if w.objectName() == widget.objectName()+'/surface']
    assert len(surfaces) == 1
    def send(kind, point, button, buttons):
        global_point = QPointF(widget.mapToGlobal(point.toPoint()))
        QGuiApplication.sendEvent(surfaces[0], QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier))
    send(QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
    send(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    send(QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton)
    send(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)


def inspect_drawing():
    widget = viewer()
    frame = capture(widget)
    frame.save('build/ofx-drawsuite-before.png')
    center = effect.getParam('center').get()
    x, y = center[0], center[1]
    report['stages']['drawing'] = {
        'alpha_rectangle_rgb': sample(widget, frame, x+4, y+2, (128, 20, 20)),
        'alpha_polygon_rgb': sample(widget, frame, x-10, y+12, (0, 201, 10)),
        'device_pixel_ratio': widget.devicePixelRatioF(),
        'commands': widget.property('nodeOverlayCommandCount'),
    }
    px, py = frame_point(widget, frame, x, y-9)
    yellow_rows = [row for row in range(py-4, py+5)
                   if frame.pixelColor(px, row).red() > 230 and frame.pixelColor(px, row).green() > 230
                   and frame.pixelColor(px, row).blue() < 25]
    assert len(yellow_rows) == 1, f'Width-zero line must cover one physical pixel, got {yellow_rows}'
    report['stages']['drawing']['hairline_physical_rows'] = len(yellow_rows)
    values = counters()
    assert values['legacy_calls'] == 0 and values['native_draws'] > 0, values
    assert values['invalid_context_acceptances'] == 0, values
    report['center_before'] = (x, y)
    drag(widget, widget_point(widget, x, y), widget_point(widget, x+3, y+2))
    QTimer.singleShot(1200, guarded(inspect_editing))


def inspect_editing():
    widget = viewer()
    before = report['center_before']
    center = effect.getParam('center').get()
    after = (center[0], center[1])
    viewport = widget.property('canonicalViewport')
    tolerance = max(viewport.width()/widget.width(), viewport.height()/widget.height(), 0.1)
    assert abs(after[0]-before[0]-3) <= tolerance and abs(after[1]-before[1]-2) <= tolerance, f'OFX pen motion did not change the parameter: {before} -> {after}'
    values = counters()
    assert values['legacy_calls'] == 0 and values['invalid_context_acceptances'] == 0, values
    assert values['expired_context_rejections'] > 0 and values['pen_motions'] > 0, values
    frame = capture(widget)
    frame.save('build/ofx-drawsuite-gui-frame.png')
    sample(widget, frame, after[0]+4, after[1]+2, (128, 20, 20))
    report['stages']['editing'] = {'center_before': before, 'center_after': tuple(after), 'counters': values}
    QTimer.singleShot(500, guarded(inspect_focus))


def inspect_focus():
    widget = viewer()
    surfaces = [w for w in QGuiApplication.allWindows() if w.objectName() == widget.objectName()+'/surface']
    assert len(surfaces) == 1
    surface = surfaces[0]
    before = counters()
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
    after = counters()
    assert after['focus_losses'] > before['focus_losses'] and after['focus_gains'] > before['focus_gains'], f'Native focus notifications were lost: {before} -> {after}'
    report['stages']['focus'] = {'native_gains': after['focus_gains']-before['focus_gains'],
                               'native_losses': after['focus_losses']-before['focus_losses']}
    # Native-window processing may restore Qt's logical widget focus. Test
    # duplicate notifications directly at the widget without inventing another
    # native transition whose platform focus state was never changed.
    before = counters()
    for kind in (QEvent.FocusOut, QEvent.FocusOut, QEvent.FocusIn, QEvent.FocusIn):
        QApplication.sendEvent(widget, QFocusEvent(kind, Qt.OtherFocusReason))
    after = counters()
    assert after['focus_losses'] == before['focus_losses']+1 and after['focus_gains'] == before['focus_gains']+1, f'Widget focus notifications were duplicated: {before} -> {after}'
    report['stages']['focus']['duplicate_widget_notifications_coalesced'] = True
    effect.getParam('showModal').set(True)
    QGuiApplication.sendEvent(surface, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    assert counters()['control_held'] == 1, 'OFX did not receive the native Control key'
    def dismiss():
        dialogs = [w for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()
                   and any('Automated OFX focus interruption.' in label.text() for label in w.findChildren(QLabel))]
        assert len(dialogs) == 1, f'Expected one controlled OFX message dialog, got {len(dialogs)}'
        report['expected_dialogs'] = [dialogs[0].windowTitle()]
        before = counters()
        assert before['active_drag'] == 1, 'The OFX modal action did not interrupt an active drag'
        QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
        QGuiApplication.sendEvent(surface, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
        values = counters()
        assert values['dialogs_opened'] == 1 and values['focus_callbacks_inside_modal'] == 0, values
        assert values['focus_losses'] == before['focus_losses'], 'Focus callback reentered the OFX modal action'
        report['focus_losses_in_modal'] = values['focus_losses']
        dialogs[0].accept()
    QTimer.singleShot(200, guarded(dismiss))
    center = effect.getParam('center').get()
    point = widget_point(widget, center[0], center[1])
    def send(kind, button, buttons):
        global_point = QPointF(widget.mapToGlobal(point.toPoint()))
        QGuiApplication.sendEvent(surface, QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier))
    send(QEvent.MouseMove, Qt.NoButton, Qt.NoButton)
    send(QEvent.MouseButtonPress, Qt.LeftButton, Qt.LeftButton)
    send(QEvent.MouseMove, Qt.NoButton, Qt.LeftButton)
    values = counters()
    assert values['active_drag'] == 0, f'OFX modal focus loss did not cancel the drag before following input: {values}'
    send(QEvent.MouseButtonRelease, Qt.LeftButton, Qt.NoButton)
    effect.getParam('showModal').set(False)
    values = counters()
    assert values['control_held'] == 0 and values['focus_callbacks_inside_modal'] == 0, f'OFX modal focus transition was not safely delivered: {values}'
    assert values['focus_losses'] > report['focus_losses_in_modal'], values
    report['stages']['modal_focus'] = values
    QTimer.singleShot(500, guarded(finish_focus))


def finish_focus():
    widget = viewer()
    frame = capture(widget)
    frame.save('build/ofx-drawsuite-gui-frame.png')
    finish()


def setup():
    global effect, library
    app.addFormat('VulkanDrawSuiteSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('VulkanDrawSuiteSmoke')
    app.getProjectParam('frameRange').set(1, 1)
    source = Path('build/qt6-smoke/drawsuite-source.ppm')
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_bytes(b'P6\n64 48\n255\n'+bytes((0, 40, 40))*(64*48))
    reader = app.createReader(source.resolve().as_posix())
    reader.getParam('ocioInputSpace').set('sRGB')
    reader.getParam('ocioOutputSpace').set('Linear')
    effect = app.createNode('org.natron.test.DrawSuite', -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert effect is not None, 'Native OFX DrawSuite plugin was not loaded'
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    assert effect.connectInput(0, reader) and viewer_node.connectInput(0, effect)
    binary = Path('build/ofx-tests/DrawSuiteSmoke.ofx.bundle/Contents/Win64/DrawSuiteSmoke.ofx').resolve()
    library = ctypes.WinDLL(str(binary))
    library.OfxDrawSuiteSmokeCounter.argtypes = [ctypes.c_int]
    library.OfxDrawSuiteSmokeCounter.restype = ctypes.c_ulonglong
    QTimer.singleShot(6000, guarded(inspect_drawing))


QTimer.singleShot(0, guarded(setup))
