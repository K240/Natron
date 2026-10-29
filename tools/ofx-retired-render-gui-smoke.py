"""Exercise retired-render declarations through Natron's real GUI OFX host."""
import ctypes
import json
import os
from pathlib import Path
import time
import traceback
import NatronEngine
from PySide6.QtCore import QMetaObject, QTimer, Qt, Q_RETURN_ARG
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication, QMessageBox

output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True, exist_ok=True)
Path(os.environ['NATRON_TEST_START_MARKER']).write_text('Retired render ABI test started', encoding='utf-8')
backend = os.environ.get('NATRON_TEST_VIEWER_BACKEND', 'Vulkan')
disabled = os.environ.get('NATRON_VULKAN_DISABLED') == '1'
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE'], 'backend': backend,
          'vulkan_disabled': disabled, 'cases': []}
ids = ['org.natron.test.RequiredOpenGL', 'org.natron.test.OptionalOpenGL',
       'org.natron.test.ExplicitCPU', 'org.natron.test.ContextOpenGL',
       'org.natron.test.DualRenderAPI', 'org.natron.test.DefaultCPU']
library = effect = source = viewer_node = writer = None
position, finished = 0, False
current = None


def counters():
    return [[library.OfxRetiredRenderCounter(kind, index) for index in range(12)] for kind in range(6)]


def display():
    widgets = [w for w in QApplication.allWidgets() if w.objectName().startswith('NatronVulkanViewer')
               and w.property('graphicsBackend') == backend and w.isVisible()]
    assert len(widgets) == 1, len(widgets)
    return widgets[0]


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    if error:
        report.update(error=str(error), traceback=traceback.format_exc(), pending_case=current)
    widgets = [w for w in QApplication.allWidgets() if w.objectName().startswith('NatronVulkanViewer')]
    report['validation_errors'] = [w.property('validationErrors') for w in widgets]
    report['graphics_errors'] = [w.property('graphicsError') for w in widgets] if backend == 'Vulkan' else []
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['counters'] = counters() if library else []
    report['ok'] = not error and len(report['cases']) == 6 and not any(report['validation_errors']) \
        and not any(report['graphics_errors']) and not report['unexpected_dialogs'] \
        and 'expected_error_dialog' in report
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    os.write(1, b'NATRON_RETIRED_RENDER_EXIT_REQUESTED\n')
    QTimer.singleShot(0, lambda: QApplication.exit(0))


def guarded(function):
    def invoke():
        try:
            function()
        except Exception as error:
            finish(error)
    return invoke


def inspect_expected_dialog():
    # Only close the error produced by this fixture creation in this owned PID.
    dialogs = [w for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    assert len(dialogs) == 1 and isinstance(dialogs[0], QMessageBox), dialogs
    dialog = dialogs[0]
    text = dialog.text()
    assert dialog.windowTitle() == 'Error while creating node' and ids[0] in text, text
    assert 'requires OpenGL rendering' in text and 'CPU-compatible or Vulkan-compatible' in text, text
    assert dialog.grab().save(str(output.parent / 'required-render-error.png'))
    report['expected_error_dialog'] = {'title': dialog.windowTitle(), 'text': text}
    dialog.accept()


def create_case():
    global current, effect, position
    if position == len(ids):
        finish()
        return
    current = {'id': ids[position], 'before_counters': counters()}
    properties = {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)}
    if position == 0:
        properties['CreateNodeArgsPropSilent'] = NatronEngine.BoolNodeCreationProperty(False)
        QTimer.singleShot(100, guarded(inspect_expected_dialog))
    effect = app.createNode(ids[position], -1, app, properties)
    if position in (0, 3):
        assert effect is None, 'OpenGL-only effect was instantiated'
        values = counters()[position]
        assert values[0] > 0 and values[1] > 0 and not any(values[2:10]), values
        current.update(rejected_before_create=True, after_counters=counters())
        report['cases'].append(current)
        position += 1
        QTimer.singleShot(200, guarded(create_case))
        return
    assert effect is not None, 'A CPU/Vulkan-compatible effect was rejected'
    assert effect.getParam('enableGPURendering') is None, 'Legacy metadata exposed an inactive GPU control'
    assert effect.getParam('ofxDrawSuiteWarning') is None, 'A node without an interact requires DrawSuite'
    assert effect.connectInput(0, source) and viewer_node.connectInput(0, effect)
    app.selectNode(effect, True)
    QTimer.singleShot(1500, guarded(render_case))


def render_case():
    global writer
    destination = output.parent / f'{position}-output.png'
    writer = app.createWriter(destination.as_posix())
    assert writer is not None and writer.connectInput(0, effect)
    app.render(writer, 1, 1)
    current['render_started'] = time.monotonic()
    QTimer.singleShot(100, guarded(inspect_render))


def inspect_render():
    image = QImage(str(output.parent / f'{position}-output.png'))
    if disabled and position == 4:
        if time.monotonic() - current['render_started'] < 3:
            QTimer.singleShot(100, guarded(inspect_render))
            return
        assert image.isNull() and not counters()[4][4], 'A GPU-only effect was retried through CPU'
        current['native_unavailable_failure'] = True
    else:
        if image.isNull() and time.monotonic() - current['render_started'] < 15:
            QTimer.singleShot(100, guarded(inspect_render))
            return
        assert not image.isNull() and (image.width(), image.height()) == (64, 48)
        for y in range(48):
            for x in range(64):
                expected = (255, 0, 0, 255) if x < 32 else (0, 255, 0, 255)
                assert image.pixelColor(x, y).getRgb() == expected, (x, y, image.pixelColor(x, y).getRgb())
        values = counters()[position]
        assert values[4] > 0 and values[5] > 0 and values[6] > 0 and not any(values[7:9]), values
        assert values[9] == (values[4] if position == 4 else 0), values
        if position == 4:
            assert values[10] in (0x10de, 0x1002) and values[11] > 0, values
        current.update(pixels_verified=64*48, native_render=position == 4)
        widget = display()
        frame = QMetaObject.invokeMethod(widget, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
        assert not frame.isNull() and frame.save(str(output.parent / f'{position}-viewer.png'))
    writer.destroy(False)
    viewer_node.disconnectInput(0)
    effect.destroy(False)
    current['destroy_started'] = time.monotonic()
    QTimer.singleShot(200, guarded(inspect_destroy))


def inspect_destroy():
    global position, effect
    values = counters()[position]
    if values[3] <= current['before_counters'][position][3] and time.monotonic() - current['destroy_started'] < 5:
        QTimer.singleShot(100, guarded(inspect_destroy))
        return
    assert values[2] > current['before_counters'][position][2] and values[3] > current['before_counters'][position][3], values
    assert not any(values[7:9]), values
    current['after_counters'] = counters()
    report['cases'].append(current)
    effect = None
    position += 1
    QTimer.singleShot(300, guarded(create_case))


def setup():
    global library, source, viewer_node
    app.addFormat('RetiredRenderSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('RetiredRenderSmoke')
    app.getProjectParam('frameRange').set(1, 1)
    app.getProjectParam('autoPreviews').set(False)
    source_file = output.parent / 'source.ppm'
    source_file.write_bytes(b'P6\n64 48\n255\n' + (bytes((255, 0, 0))*32 + bytes((0, 255, 0))*32)*48)
    source = app.createReader(source_file.as_posix())
    assert source is not None
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    binary = Path('build/ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/Win64/RetiredRenderSmoke.ofx').resolve()
    library = ctypes.CDLL(str(binary))
    library.OfxRetiredRenderCounter.argtypes = [ctypes.c_int, ctypes.c_int]
    library.OfxRetiredRenderCounter.restype = ctypes.c_ulonglong
    window = display().window()
    window.hide()
    window.showNormal()
    window.move(0, 0)
    window.raise_()
    window.activateWindow()
    QTimer.singleShot(500, guarded(create_case))


QTimer.singleShot(1000, guarded(setup))
