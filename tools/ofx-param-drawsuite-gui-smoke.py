"""Exercise Natron's native custom-parameter OFX extension without OpenGL."""
import ctypes
import json
import os
from pathlib import Path
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('OFX parameter DrawSuite smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QPointF, QTimer, Qt
from PySide6.QtGui import QFocusEvent, QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QLabel
from shiboken6 import isValid
import NatronEngine

report = {'ok': False, 'stages': {}}
effect = library = control = None
retained_widgets = {}


def widgets():
    # Retain Qt-owned wrappers, including menu parents, for the entire test.
    result = QApplication.allWidgets()
    retained_widgets.update((id(w), w) for w in result)
    return result


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
    viewers = [w for w in widgets() if w.property('graphicsBackend') == 'Vulkan']
    report['graphics_errors'] = [w.property('graphicsError') for w in viewers]
    report['validation_errors'] = [w.property('validationErrors') for w in viewers]
    report['devices'] = [w.property('gpuDeviceName') for w in viewers]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets()
                                    if w.isVisible() and w.isModal()]
    report['ok'] = not error and bool(viewers) and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs']
    Path('build/ofx-param-drawsuite-gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('OFX_PARAM_DRAW_SUITE_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def counters():
    names = ('legacy_calls', 'native_draws', 'expired_context_rejections', 'invalid_context_acceptances',
             'pen_motions', 'focus_gains', 'focus_losses', 'focus_callbacks_inside_modal', 'control_held',
             'dialogs_opened', 'active_drag', 'utf8_key_strings', 'key_downs', 'key_repeats', 'key_ups',
             'viewport_width', 'viewport_height', 'screen_ratio_percent')
    return dict(zip(names, (library.OfxParamDrawSuiteSmokeCounter(i) for i in range(len(names)))))


def point(x, y):
    return QPointF(x, control.height()-1-y)


def pen(kind, position, button, buttons):
    global_position = QPointF(control.mapToGlobal(position.toPoint()))
    event = QMouseEvent(kind, position, global_position, button, buttons, Qt.NoModifier)
    QApplication.sendEvent(control, event)


def key(kind, code, text='', repeat=False, modifiers=Qt.NoModifier):
    QApplication.sendEvent(control, QKeyEvent(kind, code, modifiers, text, repeat))


def capture(path):
    assert isValid(control) and control.isVisible()
    assert control.property('graphicsBackend') == 'Raster'
    assert control.property('customInteractDrawStatus') == 0
    assert control.property('customInteractCommandCount') >= 7
    frame = control.grab().toImage()
    assert not frame.isNull() and frame.save(path)
    return frame


def sample(frame, x, y, expected):
    position = point(x, y)
    px = round(position.x()*frame.width()/control.width())
    py = round(position.y()*frame.height()/control.height())
    color = frame.pixelColor(px, py)
    rgb = (color.red(), color.green(), color.blue())
    assert max(abs(a-b) for a, b in zip(rgb, expected)) <= 2, (position, rgb, expected)
    return rgb


def inspect_drawing():
    global control
    controls = [w for w in widgets() if w.property('graphicsRole') == 'OfxCustomParamInteract' and w.isVisible()]
    assert len(controls) == 1, f'Expected one native custom control, got {len(controls)}'
    control = controls[0]
    assert control.objectName() == 'center/customInteract', control.objectName()
    # legacyOnly must retain its ordinary parameter control and never invoke V1.
    assert not any(w.objectName() == 'legacyOnly/customInteract' for w in widgets())
    frame = capture('build/ofx-param-drawsuite-before.png')
    center = effect.getParam('center').get()
    x, y = center[0], center[1]
    values = counters()
    assert values['legacy_calls'] == 0 and values['native_draws'] > 0, values
    assert values['invalid_context_acceptances'] == 0, values
    assert (values['viewport_width'], values['viewport_height']) == (control.width(), control.height()), values
    assert values['screen_ratio_percent'] == round(control.devicePixelRatioF()*100), values
    assert not control.inherits('QOpenGLWidget')
    report['stages']['drawing'] = {
        'alpha_rectangle_rgb': sample(frame, x+4, y+2, (128, 0, 0)),
        'alpha_polygon_rgb': sample(frame, x-10, y+14, (0, 191, 0)),
        'device_pixel_ratio': control.devicePixelRatioF(),
        'viewport': [control.width(), control.height()],
        'commands': control.property('customInteractCommandCount'),
    }
    position = point(x, y-9)
    px, py = round(position.x()*frame.width()/control.width()), round(position.y()*frame.height()/control.height())
    # The dashed rectangle is 3 logical pixels below the hairline. Restrict the
    # sample to the hairline's neighbourhood so DPR rounding cannot include it.
    yellow_rows = [r for r in range(py-2, py+3) if frame.pixelColor(px, r).red() > 230
                   and frame.pixelColor(px, r).green() > 230 and frame.pixelColor(px, r).blue() < 25]
    assert len(yellow_rows) == 1, yellow_rows
    report['stages']['drawing']['hairline_physical_rows'] = len(yellow_rows)
    report['center_before'] = (x, y)
    control.setFocus(Qt.OtherFocusReason)
    pen(QEvent.MouseButtonPress, point(x, y), Qt.LeftButton, Qt.LeftButton)
    pen(QEvent.MouseMove, point(x+12.5, y+7.25), Qt.NoButton, Qt.LeftButton)
    pen(QEvent.MouseButtonRelease, point(x+12.5, y+7.25), Qt.LeftButton, Qt.NoButton)
    QTimer.singleShot(700, guarded(inspect_editing))


def inspect_editing():
    center = effect.getParam('center').get()
    before = report['center_before']
    after = (center[0], center[1])
    assert abs(after[0]-before[0]-12.5) < 1e-6 and abs(after[1]-before[1]-7.25) < 1e-6, (before, after)
    values = counters()
    assert values['pen_motions'] > 0 and values['expired_context_rejections'] > 0, values
    assert values['invalid_context_acceptances'] == 0 and values['legacy_calls'] == 0, values
    report['stages']['editing'] = {'before': before, 'after': after, 'counters': values}
    key(QEvent.KeyPress, Qt.Key_E, 'é')
    key(QEvent.KeyPress, Qt.Key_E, 'é', repeat=True)
    key(QEvent.KeyRelease, Qt.Key_E, 'é')
    values = counters()
    assert values['utf8_key_strings'] == 3 and values['key_repeats'] == 1 and values['key_ups'] == 1, values
    report['stages']['keyboard'] = values
    control.setMinimumSize(320, 192)
    QTimer.singleShot(700, guarded(inspect_resize))


def inspect_resize():
    frame = capture('build/ofx-param-drawsuite-resize.png')
    values = counters()
    assert control.width() >= 320 and control.height() >= 192
    assert (values['viewport_width'], values['viewport_height']) == (control.width(), control.height()), values
    center = effect.getParam('center').get()
    sample(frame, center[0]+4, center[1]+2, (128, 0, 0))
    report['stages']['resize'] = {'viewport': [control.width(), control.height()], 'counters': values}
    control.setFocus(Qt.OtherFocusReason)
    before = counters()
    for kind in (QEvent.FocusOut, QEvent.FocusOut, QEvent.FocusIn, QEvent.FocusIn):
        QApplication.sendEvent(control, QFocusEvent(kind, Qt.OtherFocusReason))
    after = counters()
    assert after['focus_losses'] == before['focus_losses']+1 and after['focus_gains'] == before['focus_gains']+1, (before, after)
    report['stages']['focus'] = {'duplicate_notifications_coalesced': True}
    effect.getParam('showModal').set(True)
    key(QEvent.KeyPress, Qt.Key_Control, modifiers=Qt.ControlModifier)
    assert counters()['control_held'] == 1
    def dismiss():
        dialogs = [w for w in widgets() if w.isVisible() and w.isModal()
                   and any('Automated OFX focus interruption.' in label.text() for label in w.findChildren(QLabel))]
        assert len(dialogs) == 1, [w.windowTitle() for w in dialogs]
        report['expected_dialogs'] = [dialogs[0].windowTitle()]
        before = counters()
        assert before['active_drag'] == 1
        for kind in (QEvent.FocusOut, QEvent.FocusIn):
            QApplication.sendEvent(control, QFocusEvent(kind, Qt.OtherFocusReason))
        # Neither input nor repaint may reenter the callback's modal loop.
        key(QEvent.KeyPress, Qt.Key_E, 'é')
        pen(QEvent.MouseMove, point(center[0]+5, center[1]+3), Qt.NoButton, Qt.LeftButton)
        control.repaint()
        after = counters()
        for name in ('native_draws', 'key_downs', 'pen_motions', 'focus_losses', 'focus_gains'):
            assert after[name] == before[name], (name, before, after)
        assert after['focus_callbacks_inside_modal'] == 0
        report['modal_snapshot'] = after
        dialogs[0].accept()
    QTimer.singleShot(200, guarded(dismiss))
    pen(QEvent.MouseButtonPress, point(center[0], center[1]), Qt.LeftButton, Qt.LeftButton)
    pen(QEvent.MouseButtonRelease, point(center[0], center[1]), Qt.LeftButton, Qt.NoButton)
    effect.getParam('showModal').set(False)
    values = counters()
    assert values['control_held'] == 0 and values['active_drag'] == 0 and values['focus_callbacks_inside_modal'] == 0, values
    assert values['focus_losses'] > report['modal_snapshot']['focus_losses'], values
    report['stages']['modal_focus'] = values
    QTimer.singleShot(500, guarded(finish_checks))


def finish_checks():
    frame = capture('build/ofx-param-drawsuite-gui-frame.png')
    center = effect.getParam('center').get()
    sample(frame, center[0]+4, center[1]+2, (128, 0, 0))
    values = counters()
    assert values['legacy_calls'] == 0 and values['invalid_context_acceptances'] == 0, values
    finish()


def setup():
    global effect, library
    app.addFormat('VulkanParamDrawSuiteSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('VulkanParamDrawSuiteSmoke')
    app.getProjectParam('frameRange').set(1, 1)
    source = app.createNode('net.sf.openfx.ConstantPlugin')
    source.getParam('color').set(.0, .15, .15, 1.)
    effect = app.createNode('org.natron.test.ParamDrawSuite', -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert effect is not None, 'Native OFX parameter plugin was not loaded'
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert effect.connectInput(0, source) and viewer.connectInput(0, effect)
    binary = Path('build/ofx-tests/ParamDrawSuiteSmoke.ofx.bundle/Contents/Win64/ParamDrawSuiteSmoke.ofx').resolve()
    library = ctypes.WinDLL(str(binary))
    library.OfxParamDrawSuiteSmokeCounter.argtypes = [ctypes.c_int]
    library.OfxParamDrawSuiteSmokeCounter.restype = ctypes.c_ulonglong
    QTimer.singleShot(6000, guarded(inspect_drawing))


QTimer.singleShot(0, guarded(setup))
