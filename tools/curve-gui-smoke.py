"""Verify native curve rendering, editing and OFX parametric backgrounds."""
import colorsys
import ctypes
import json
import os
from pathlib import Path
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Curve smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QMetaObject, QPointF, QRectF, QTimer, Qt
from PySide6.QtGui import QAction, QColor, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication, QTreeWidget
import NatronEngine

report = {'ok': False, 'stages': {}}
effect = library = plot = source = None
background_nodes = []
background_cases = iter((('color_lookup', 'net.sf.openfx.ColorLookupPlugin', 1),
                         ('hue_correct', 'net.sf.openfx.HueCorrect', 2),
                         ('hue_correct_v1', 'net.sf.openfx.HueCorrect', 1),
                         ('hue_keyer', 'net.sf.openfx.HueKeyer', 2)))
retained = {}
retained_actions = {}


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values


def plain(value):
    if isinstance(value, QPointF):
        return [value.x(), value.y()]
    if isinstance(value, QRectF):
        return [value.x(), value.y(), value.width(), value.height()]
    if isinstance(value, QColor):
        return list(value.getRgb())
    if isinstance(value, dict):
        return {key: plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(item) for item in value]
    return value


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
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['ok'] = not error and bool(viewers) and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs']
    Path('build/curve-gui-result.json').write_text(json.dumps(plain(report), indent=2), encoding='utf-8')
    print('CURVE_GUI_SMOKE', json.dumps(plain(report)))
    QApplication.exit(0)


def counters():
    return dict(zip(('legacy_calls', 'native_draws', 'expired_context_rejections', 'invalid_context_acceptances', 'input_callbacks'),
                    (library.OfxCurveDrawSuiteSmokeCounter(i) for i in range(5))))


def point(x, y):
    viewport = plot.property('curveEditorViewport')
    return QPointF((x-viewport.x())/viewport.width()*plot.width(),
                   (viewport.y()+viewport.height()-y)/viewport.height()*plot.height())


def capture(name='frame'):
    frame = plot.grab().toImage()
    assert not frame.isNull() and frame.save(f'build/curve-gui-{name}.png')
    return frame


def sample(frame, x, y, expected, tolerance=3):
    position = point(x, y)
    px = round(position.x()*frame.width()/plot.width())
    py = round(position.y()*frame.height()/plot.height())
    color = frame.pixelColor(px, py)
    rgb = (color.red(), color.green(), color.blue())
    assert max(abs(a-b) for a, b in zip(rgb, expected)) <= tolerance, (x, y, rgb, expected)
    return rgb


def key_points():
    curves = plot.property('curveEditorCurves')
    assert len(curves) == 1
    return [key['position'] for key in curves[0]['keys']]


def pen(kind, position, button, buttons, modifiers=Qt.NoModifier):
    event = QMouseEvent(kind, position, QPointF(plot.mapToGlobal(position.toPoint())), button, buttons, modifiers)
    QApplication.sendEvent(plot, event)


def drag(start, end):
    plot.setFocus(Qt.OtherFocusReason)
    pen(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    pen(QEvent.MouseMove, start+(end-start)*.5, Qt.NoButton, Qt.LeftButton)
    pen(QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton)
    pen(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)


def action(prefix):
    for widget in widgets():
        for item in widget.actions()+widget.findChildren(QAction):
            retained_actions[id(item)] = item
    found = [item for item in retained_actions.values() if item.text().replace('&', '').startswith(prefix) and item.isEnabled()]
    assert found, f'Missing enabled {prefix} action'
    # Main-window undo actions may also be listed in their menu containers.
    return found[0]


def inspect_editing():
    after = key_points()[1]
    before = report['key_before']
    tolerance = 2*max(plot.property('curveEditorViewport').width()/plot.width(),
                    plot.property('curveEditorViewport').height()/plot.height())
    assert abs(after.x()-before[0]-.055) <= tolerance and abs(after.y()-before[1]+.052) <= tolerance, (before, after)
    selected = plot.property('curveEditorSelectedKeys')
    assert len(selected) == 1
    frame = capture('tangents')
    sample(frame, after.x(), after.y(), (255, 255, 255))
    assert any('x:' in text for text in plot.property('curveEditorLabels'))
    report['stages']['editing'] = {'before': before, 'after': after, 'selected': selected}
    report['key_after'] = [after.x(), after.y()]
    action('Undo').trigger()
    QTimer.singleShot(500, guarded(inspect_undo))


def inspect_undo():
    restored = key_points()[1]
    assert abs(restored.x()-report['key_before'][0]) < 1e-9 and abs(restored.y()-report['key_before'][1]) < 1e-9, restored
    report['stages']['undo'] = {'position': restored}
    action('Redo').trigger()
    QTimer.singleShot(500, guarded(inspect_redo))


def inspect_redo():
    restored = key_points()[1]
    assert abs(restored.x()-report['key_after'][0]) < 1e-9 and abs(restored.y()-report['key_after'][1]) < 1e-9, restored
    report['stages']['redo'] = {'position': restored}
    assert QMetaObject.invokeMethod(plot, 'breakDerivativesForSelectedKeyFrames', Qt.DirectConnection)
    QTimer.singleShot(500, guarded(inspect_tangent))


def inspect_tangent():
    curve = plot.property('curveEditorCurves')[0]
    report['derivative_before'] = curve['keys'][1]['rightDerivative']
    selected = plot.property('curveEditorSelectedKeys')
    assert len(selected) == 1
    tangent = selected[0]['rightTangent']
    origin = point(tangent.x(), tangent.y())
    assert plot.rect().contains(origin.toPoint())
    drag(origin, origin+QPointF(0, -24))
    QTimer.singleShot(500, guarded(inspect_tangent_edit))


def inspect_tangent_edit():
    after = plot.property('curveEditorCurves')[0]['keys'][1]['rightDerivative']
    assert abs(after-report['derivative_before']) > .1, (after, report['derivative_before'])
    report['stages']['tangent'] = {'before': report['derivative_before'], 'after': after}
    before = plot.property('curveEditorViewport')
    position = QPointF(plot.width()*.47, plot.height()*.53)
    wheel = QWheelEvent(position, QPointF(plot.mapToGlobal(position.toPoint())), QPointF().toPoint(),
                        QPointF(0, 120).toPoint(), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
    QApplication.sendEvent(plot, wheel)
    report['viewport_before_zoom'] = before
    QTimer.singleShot(500, guarded(inspect_zoom))


def inspect_zoom():
    before = report['viewport_before_zoom']
    after = plot.property('curveEditorViewport')
    assert after.width() < before.width() and after.height() < before.height(), (before, after)
    report['stages']['zoom'] = {'before': before, 'after': after}
    start = QPointF(plot.width()*.47, plot.height()*.53)
    pen(QEvent.MouseButtonPress, start, Qt.MiddleButton, Qt.MiddleButton)
    pen(QEvent.MouseMove, start+QPointF(25, 12), Qt.NoButton, Qt.MiddleButton)
    pen(QEvent.MouseButtonRelease, start+QPointF(25, 12), Qt.MiddleButton, Qt.NoButton)
    report['viewport_before_pan'] = after
    QTimer.singleShot(500, guarded(inspect_pan))


def inspect_pan():
    before = report['viewport_before_pan']
    after = plot.property('curveEditorViewport')
    assert after.x() < before.x() and after.y() > before.y() and abs(after.width()-before.width()) < 1e-9, (before, after)
    report['stages']['pan'] = {'before': before, 'after': after}
    assert QMetaObject.invokeMethod(plot, 'frameAll', Qt.DirectConnection)
    QTimer.singleShot(500, guarded(finish_editing))


def finish_editing():
    values = counters()
    assert values['legacy_calls'] == values['invalid_context_acceptances'] == values['input_callbacks'] == 0, values
    assert values['expired_context_rejections'] > 0
    report['stages']['final'] = {'counters': values, 'viewport': plot.property('curveEditorViewport')}
    capture()
    next_background()


def next_background():
    global plot, background_case
    case = next(background_cases, None)
    if case is None:
        setup_ordinary_curve()
        return
    name, plugin, version = case
    previous = {id(w) for w in widgets() if w.property('graphicsRole') == 'CurveEditor'}
    node = app.createNode(plugin, version, app,
                          {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert node is not None, case
    assert node.connectInput(0, source)
    background_nodes.append(node)
    candidates = [w for w in widgets() if w.property('graphicsRole') == 'CurveEditor' and id(w) not in previous]
    assert len(candidates) == 1, (case, len(candidates))
    plot = candidates[0]
    background_case = (name, node)
    plot.update()
    QTimer.singleShot(900, guarded(inspect_background))


def inspect_background():
    name, node = background_case
    assert plot.isVisible() and plot.width() > 200 and plot.height() > 100
    assert not plot.inherits('QOpenGLWidget') and plot.property('curveEditorHasBackground')
    assert plot.property('curveEditorBackgroundStatus') == 0, (name, plot.property('curveEditorBackgroundStatus'))
    frame = capture(name)
    if name == 'color_lookup':
        rgb = sample(frame, .313, .271, (80, 80, 80))
    else:
        rgb = sample(frame, 2.313, .271,
                     tuple(round(value*255) for value in colorsys.hsv_to_rgb((2.313-1)/6, 1, .3)), 5)
    report['stages'][name] = {'rgb': rgb, 'viewport': plot.property('curveEditorViewport'),
                            'command_count': plot.property('curveEditorCommandCount'),
                            'background_status': plot.property('curveEditorBackgroundStatus')}
    if name == 'hue_correct':
        node.getParam('huevshue').setValue(True)
        QTimer.singleShot(600, guarded(inspect_sheared_background))
    elif name == 'color_lookup':
        node.getParam('backgroundDisplay').setValue(2)
        node.getParam('updateHistogram').trigger()
        QTimer.singleShot(900, guarded(inspect_lookup_histogram))
    else:
        next_background()


def inspect_sheared_background():
    frame = capture('hue_sheared')
    samples = []
    for x, y in ((2.313, .271), (2.713, 1.271)):
        expected = tuple(round(value*255) for value in colorsys.hsv_to_rgb(((x+3*y-4)/6) % 1, 1, .3))
        samples.append(sample(frame, x, y, expected, 6))
    report['stages']['hue_sheared'] = {'rgb': samples, 'background_status': plot.property('curveEditorBackgroundStatus')}
    next_background()


def inspect_lookup_histogram():
    frame = capture('lookup_histogram')
    # Choose a bin away from the .2 grid line, which is drawn over the histogram.
    start, end = point(54/256, .431), point(55/256, .431)
    scale = frame.width()/plot.width()
    py = round(start.y()*frame.height()/plot.height())
    colors = [frame.pixelColor(x, py).getRgb()[:3]
              for x in range(int(start.x()*scale), int(end.x()*scale)+1)]
    assert any(max(abs(channel-128) for channel in rgb) <= 3 for rgb in colors), colors
    report['stages']['lookup_histogram'] = {'rgb': colors, 'background_status': plot.property('curveEditorBackgroundStatus')}
    next_background()


def setup_ordinary_curve():
    global plot, ordinary_tree
    color = source.getParam('color')
    for time, value in ((1, .2), (5, .7), (10, .35)):
        color.setValueAtTime(value, time, 0)
    candidates = [w for w in widgets() if w.property('graphicsRole') == 'CurveEditor' and w.property('curveEditorHasTimeline')]
    assert len(candidates) == 1
    plot = candidates[0]
    container, pane = plot.parentWidget(), None
    while container and pane is None:
        pane = app.getTabWidget(container.objectName())
        container = container.parentWidget()
    assert pane is not None
    labels = [pane.getTabLabel(i) for i in range(pane.count())]
    report['ordinary_pane_labels'] = labels
    index = next(i for i, label in enumerate(labels) if label == 'Curve Editor')
    pane.setCurrentIndex(index)
    container = plot.parentWidget()
    while container and not container.findChildren(QTreeWidget):
        container = container.parentWidget()
    assert container
    trees = container.findChildren(QTreeWidget)
    assert len(trees) == 1
    ordinary_tree = trees[0]
    QTimer.singleShot(600, guarded(select_ordinary_curve))


def select_ordinary_curve():
    tree = ordinary_tree
    report['ordinary_tree'] = [{'name': tree.topLevelItem(i).text(0), 'hidden': tree.topLevelItem(i).isHidden(),
                                'children': tree.topLevelItem(i).childCount()} for i in range(tree.topLevelItemCount())]
    for i in range(tree.topLevelItemCount()):
        item = tree.topLevelItem(i)
        if not item.isHidden():
            item.setSelected(True)
    assert QMetaObject.invokeMethod(plot, 'frameAll', Qt.DirectConnection)
    QTimer.singleShot(900, guarded(inspect_ordinary_curve))


def inspect_ordinary_curve():
    assert plot.isVisible() and plot.width() > 200 and not plot.inherits('QOpenGLWidget')
    curves = plot.property('curveEditorCurves')
    assert curves and any(len(curve['keys']) == 3 for curve in curves), curves
    viewport = plot.property('curveEditorViewport')
    assert viewport.x() < 1 and viewport.right() > 10, viewport
    report['stages']['ordinary'] = {'viewport': viewport, 'curves': curves,
                                  'font_point_size': plot.property('curveEditorFontPointSize')}
    drag(QPointF(point(app.timelineGetTime(), 0).x(), 3), QPointF(point(5, 0).x(), 3))
    QTimer.singleShot(900, guarded(inspect_ordinary_scrub))


def inspect_ordinary_scrub():
    assert app.timelineGetTime() == 5, app.timelineGetTime()
    frame = capture('ordinary')
    position = point(5, .431)
    px = round(position.x()*frame.width()/plot.width())
    py = round(position.y()*frame.height()/plot.height())
    # The playhead is orange, separate from the colored animation curves.
    colors = [frame.pixelColor(px+dx, py) for dx in range(-2, 3)]
    assert any(c.red() > 150 and c.red() > c.green()*1.2 and c.blue() < 90 for c in colors), [c.getRgb() for c in colors]
    report['stages']['ordinary']['playhead_rgb'] = [c.getRgb() for c in colors]
    report['stages']['ordinary']['scrub_frame'] = app.timelineGetTime()
    finish()


def inspect():
    global plot
    plots = [w for w in widgets() if w.property('graphicsRole') == 'CurveEditor']
    report['plots'] = [{'visible': w.isVisible(), 'timeline': w.property('curveEditorHasTimeline'),
                        'size': [w.width(), w.height()], 'viewport': w.property('curveEditorViewport'),
                        'curves': w.property('curveEditorCurves'), 'background': w.property('curveEditorHasBackground'),
                        'background_status': w.property('curveEditorBackgroundStatus')} for w in plots]
    visible = [w for w in plots if w.isVisible() and not w.property('curveEditorHasTimeline')]
    assert len(visible) == 1, report['plots']
    plot = visible[0]
    assert not plot.inherits('QOpenGLWidget')
    assert plot.property('curveEditorHasBackground') and plot.property('curveEditorBackgroundStatus') == 0
    frame = capture('before')
    values = counters()
    assert values['legacy_calls'] == 0 and values['native_draws'] > 0 and values['invalid_context_acceptances'] == 0, values
    report['stages']['initial'] = {'counters': values, 'font_point_size': plot.property('curveEditorFontPointSize'),
                                   'device_pixel_ratio': plot.devicePixelRatioF(),
                                   'gray_ramp_rgb': sample(frame, .313, .271, (80, 80, 80)),
                                   'additive_bar_rgb': sample(frame, .113, .131, (157, 157, 157)),
                                   'skew_gradient_rgb': sample(frame, .7613, .1305, (49, 0, 28))}
    keys = key_points()
    assert len(keys) == 3
    middle = keys[1]
    report['key_before'] = [middle.x(), middle.y()]
    drag(point(middle.x(), middle.y()), point(middle.x()+.055, middle.y()-.052))
    QTimer.singleShot(700, guarded(inspect_editing))


def setup():
    global effect, library, source
    app.addFormat('VulkanCurveSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('VulkanCurveSmoke')
    app.getProjectParam('frameRange').set(1, 10)
    source = app.createNode('net.sf.openfx.ConstantPlugin', -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    source.getParam('color').set(.213, .213, .213, 1.)
    effect = app.createNode('org.natron.test.CurveDrawSuite', -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert effect is not None, 'Native curve C ABI plugin was not loaded'
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert effect.connectInput(0, source) and viewer.connectInput(0, effect)
    binary = Path('build/ofx-tests/CurveDrawSuiteSmoke.ofx.bundle/Contents/Win64/CurveDrawSuiteSmoke.ofx').resolve()
    library = ctypes.WinDLL(str(binary))
    library.OfxCurveDrawSuiteSmokeCounter.argtypes = [ctypes.c_int]
    library.OfxCurveDrawSuiteSmokeCounter.restype = ctypes.c_ulonglong
    QTimer.singleShot(6000, guarded(inspect))


QTimer.singleShot(0, guarded(setup))
