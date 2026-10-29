"""Exercise raster histogram rendering, async data, menus and native input."""
import json
import os
from pathlib import Path
import time
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Histogram smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, QTimer, Qt
from PySide6.QtGui import QAction, QEnterEvent, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication
import NatronEngine
from shiboken6 import isValid

report = {'ok': False, 'stages': {}}
finished = False
action_wrappers = []
known_actions = {}
widget_wrappers = {}


def all_widgets():
    widgets = QApplication.allWidgets()
    # Keep native menus and their actions alive while inspecting the widget tree.
    widget_wrappers.update((id(widget), widget) for widget in widgets)
    return widgets


def guarded(function):
    def run():
        if finished:
            return
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    if error:
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
        report['action_lifetimes'] = {text: isValid(item) for text, item in known_actions.items() if text}
    viewers = [w for w in all_widgets() if w.property('graphicsBackend') == 'Vulkan']
    report['graphics_errors'] = [w.property('graphicsError') for w in viewers]
    report['validation_errors'] = [w.property('validationErrors') for w in viewers]
    report['devices'] = [w.property('gpuDeviceName') for w in viewers]
    report['native_viewer_copies'] = [w.property('nativeImageTransfers') for w in viewers]
    report['shared_device_ids'] = [w.property('sharedDeviceId') for w in viewers]
    report['engine_device_ids'] = [w.property('engineDeviceId') for w in viewers]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['ok'] = (not error and bool(viewers) and not any(report['graphics_errors']) and not any(report['validation_errors'])
                    and not report['unexpected_dialogs'] and report['shared_device_ids'] == report['engine_device_ids'])
    Path('build/histogram-gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('HISTOGRAM_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def histogram():
    widgets = [w for w in all_widgets() if w.isVisible() and w.metaObject().className().endswith('Histogram')]
    assert len(widgets) == 1, f'Expected one visible histogram, got {len(widgets)}'
    widget = widgets[0]
    assert not widget.inherits('QOpenGLWidget') and widget.property('graphicsBackend') == 'Raster'
    return widget


def capture():
    widget = histogram()
    image = widget.grab().toImage()
    assert not image.isNull() and widget.property('histogramCommandCount') > 3
    assert abs(image.width()-widget.width()*widget.devicePixelRatioF()) <= 1
    assert abs(image.height()-widget.height()*widget.devicePixelRatioF()) <= 1
    return image


def action(text):
    if text in known_actions and isValid(known_actions[text]):
        return known_actions[text]
    actions = histogram().findChildren(QAction)
    action_wrappers.extend(actions)
    for item in actions:
        if isValid(item):
            known_actions[item.text()] = item
    choices = [item for item in actions if isValid(item) and item.text() == text]
    assert len(choices) == 1, f'Expected histogram action {text}, got {len(choices)}'
    return choices[0]


def wait_data(previous, mode, continuation):
    deadline = time.monotonic()+5
    def inspect():
        capture()
        widget = histogram()
        if widget.property('histogramHasImage') and widget.property('histogramGeneration') > previous and widget.property('histogramComputedMode') == mode:
            continuation()
        else:
            assert time.monotonic() < deadline, f'Histogram data did not arrive: mode {mode}, generation {widget.property("histogramGeneration")}, image {widget.property("histogramHasImage")}'
            QTimer.singleShot(100, guarded(inspect))
    QTimer.singleShot(100, guarded(inspect))


def histogram_point(value, y=None):
    widget = histogram()
    viewport = widget.property('histogramViewport')
    return QPointF((value-viewport.x())/viewport.width()*widget.width(), widget.height()/2 if y is None else y)


def inspect_bar(image, value, expected):
    widget = histogram()
    center = round(histogram_point(value).x()*image.width()/widget.width())
    matches = []
    for x in range(max(0, center-5), min(image.width(), center+6)):
        count = 0
        for y in range(int(image.height()*0.2), int(image.height()*0.8)):
            color = image.pixelColor(x, y)
            rgb = (color.red(), color.green(), color.blue())
            if max(abs(a-b) for a, b in zip(rgb, expected)) <= 4:
                count += 1
        if count >= 4:
            matches.append((x, count))
    assert matches, f'Histogram bar missing at {value}, expected {expected}, column {center}'
    return {'value': value, 'rgb': expected, 'physical_columns': matches}


def send_mouse(widget, kind, point, button=Qt.NoButton, buttons=Qt.NoButton):
    global_point = QPointF(widget.mapToGlobal(point.toPoint()))
    QApplication.sendEvent(widget, QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier))


def wheel(delta):
    widget = histogram()
    point = QPointF(widget.width()/2, widget.height()/2)
    event = QWheelEvent(point, QPointF(widget.mapToGlobal(point.toPoint())), QPoint(), QPoint(0, delta), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
    QApplication.sendEvent(widget, event)


def inspect_initial():
    widget = histogram()
    image = capture()
    peaks = widget.property('histogramPeaks')
    assert len(peaks) == 3 and all(abs(value-0.25) < 3/widget.property('histogramBinCount') for value in peaks), f'Unexpected RGB peaks: {peaks}'
    assert widget.property('histogramPixelCount') == 64*48
    image.save('build/histogram-before.png')
    report['stages']['initial'] = {'peaks': peaks, 'viewer_picker': widget.property('histogramViewerPicker'),
        'samples': widget.property('histogramPeakSamples'),
        'pixel_count': widget.property('histogramPixelCount'), 'device_pixel_ratio': widget.devicePixelRatioF(),
        'font_point_size': widget.property('histogramFontPointSize')}
    report['stages']['initial']['rgb_overlap'] = inspect_bar(image, 0.25, (255, 255, 255))
    previous = widget.property('histogramGeneration')
    before = widget.property('histogramViewport')
    wheel(-120)
    capture()
    after = widget.property('histogramViewport')
    assert after.width() > before.width() and after.x() < 0 and after.x()+after.width() > 1
    report['stages']['wheel'] = {'before': before.width(), 'after': after.width()}
    wait_data(previous, 0, lambda: inspect_mode(1))


mode_specs = {1: ('A', 1.0, (102, 102, 102)), 2: ('Y', 0.25, (102, 102, 102)),
              3: ('R', 0.25, (217, 50, 50)), 4: ('G', 0.25, (0, 167, 0)),
              5: ('B', 0.25, (88, 88, 255))}


def inspect_mode(mode):
    text, value, color = mode_specs[mode]
    previous = histogram().property('histogramGeneration')
    action(text).trigger()
    def check():
        widget = histogram()
        peaks = widget.property('histogramPeaks')
        assert len(peaks) == 1 and abs(peaks[0]-value) < 3/widget.property('histogramBinCount'), f'{text} data mismatch: {peaks}'
        report['stages'][text] = inspect_bar(capture(), value, color)
        if mode < 5:
            inspect_mode(mode+1)
        else:
            inspect_rapid_modes()
    wait_data(previous, mode, check)


def inspect_rapid_modes():
    previous = histogram().property('histogramGeneration')
    for _ in range(4):
        for text in ('RGB', 'A', 'R', 'G', 'B', 'Y'):
            action(text).trigger()
    action('RGB').trigger()
    wait_data(previous, 0, inspect_filter)


def inspect_filter():
    widget = histogram()
    assert len(widget.property('histogramPeaks')) == 3
    report['stages']['rapid_modes'] = {'computed_mode': widget.property('histogramComputedMode'), 'generation': widget.property('histogramGeneration')}
    previous = widget.property('histogramGeneration')
    action('High').trigger()
    def check():
        assert histogram().property('histogramFilterSize') == 5
        inspect_bar(capture(), 0.25, (255, 255, 255))
        report['stages']['filter'] = {'size': 5}
        inspect_viewer_picker()
    wait_data(previous, 0, check)


def inspect_viewer_picker():
    viewers = [w for w in all_widgets() if w.isVisible() and w.property('graphicsBackend') == 'Vulkan']
    assert len(viewers) == 1
    viewer = viewers[0]
    viewport = viewer.property('canonicalViewport')
    point = QPointF((32-viewport.x())/viewport.width()*viewer.width(), (viewport.y()+viewport.height()-24)/viewport.height()*viewer.height())
    windows = [w for w in QApplication.allWindows() if w.objectName() == viewer.objectName()+'/surface']
    assert len(windows) == 1
    send_mouse(windows[0], QEvent.MouseMove, point)
    image = capture()
    assert histogram().property('histogramViewerPicker'), 'Native Viewer hover did not reach the histogram'
    report['stages']['viewer_alpha_picker'] = inspect_bar(image, 1.0, (102, 102, 102))
    image.save('build/histogram-picker.png')
    send_mouse(windows[0], QEvent.MouseButtonPress, point, Qt.LeftButton, Qt.LeftButton)
    send_mouse(windows[0], QEvent.MouseButtonRelease, point, Qt.LeftButton)
    previous = histogram().property('histogramGeneration')
    action('Current Viewer').trigger()
    wait_data(previous, 0, change_source)


def change_source():
    report['stages']['current_viewer'] = {'pixel_count': histogram().property('histogramPixelCount')}
    assert histogram().property('histogramPixelCount') == 64*48
    previous = histogram().property('histogramGeneration')
    source.getParam('color').set(0.1, 0.4, 0.8, 1)
    wait_data(previous, 0, inspect_changed_source)


def inspect_changed_source():
    widget = histogram()
    peaks = widget.property('histogramPeaks')
    assert len(peaks) == 3 and all(abs(actual-expected) < 7/widget.property('histogramBinCount') for actual, expected in zip(peaks, (0.1, 0.4, 0.8))), f'Changed source did not update RGB data: {peaks}'
    report['stages']['source_change'] = {'peaks': peaks}
    QApplication.sendEvent(widget, QEnterEvent(QPointF(), QPointF(), QPointF()))
    send_mouse(widget, QEvent.MouseMove, QPointF(-widget.width()*2, widget.height()/2))
    capture()
    labels = widget.property('histogramLabels')
    assert all(text in labels for text in ('r=0', 'g=0', 'b=0')), f'Out-of-range picker is unsafe: {labels}'
    report['stages']['outside_picker'] = {'values': ['r=0', 'g=0', 'b=0']}
    before = widget.property('histogramViewport')
    start = QPointF(widget.width()/2, widget.height()/2)
    previous = widget.property('histogramGeneration')
    send_mouse(widget, QEvent.MouseButtonPress, start, Qt.MiddleButton, Qt.MiddleButton)
    send_mouse(widget, QEvent.MouseMove, start+QPointF(30, 10), Qt.NoButton, Qt.MiddleButton)
    send_mouse(widget, QEvent.MouseButtonRelease, start+QPointF(30, 10), Qt.MiddleButton)
    capture()
    after = widget.property('histogramViewport')
    assert after.x() < before.x() and abs(after.width()-before.width()) < 1e-8
    report['stages']['pan'] = {'left_before': before.x(), 'left_after': after.x()}
    wait_data(previous, 0, inspect_input_b)


def inspect_input_b():
    action('Input B').trigger()
    def check():
        image = capture()
        assert not histogram().property('histogramHasImage')
        assert 'Missing image' in histogram().property('histogramLabels')
        image.save('build/histogram-missing.png')
        report['stages']['missing_input'] = {'missing_image_label': True}
        previous = histogram().property('histogramGeneration')
        action('Input A').trigger()
        wait_data(previous, 0, inspect_fit)
    QTimer.singleShot(300, guarded(check))


def inspect_fit():
    widget = histogram()
    previous = widget.property('histogramGeneration')
    QApplication.sendEvent(widget, QKeyEvent(QEvent.KeyPress, Qt.Key_F, Qt.NoModifier))
    capture()
    viewport = widget.property('histogramViewport')
    assert abs(viewport.x()) < 1e-8 and abs(viewport.width()-1) < 1e-8
    report['stages']['fit'] = {'left': viewport.x(), 'width': viewport.width()}
    wait_data(previous, 0, complete)


def complete():
    capture().save('build/histogram-gui-frame.png')
    finish()


def create_histogram():
    graphs = [w for w in all_widgets() if w.isVisible() and w.metaObject().className().endswith('NodeGraph')]
    assert len(graphs) == 1
    panel = graphs[0].parentWidget()
    while panel and not panel.metaObject().className().endswith('TabWidget'):
        panel = panel.parentWidget()
    assert panel and QMetaObject.invokeMethod(panel, 'newHistogramHere', Qt.DirectConnection)
    widget = histogram()
    previous = widget.property('histogramGeneration') or 0
    action(viewer_node.getScriptName()).trigger()
    full = action('Full image')
    if not full.isChecked():
        full.trigger()
    wait_data(previous, 0, inspect_initial)


def setup():
    global source, viewer_node
    app.addFormat('HistogramSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('HistogramSmoke')
    source = app.createNode('net.sf.openfx.ConstantPlugin')
    assert source
    source.getParam('color').set(0.25, 0.25, 0.25, 1)
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    assert viewer_node.connectInput(0, source)
    QTimer.singleShot(6000, guarded(create_histogram))


QTimer.singleShot(0, guarded(setup))
