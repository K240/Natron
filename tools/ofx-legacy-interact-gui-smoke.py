"""Verify legacy UI suppression through the actual Natron OFX host."""
import ctypes
import json
import os
from pathlib import Path
import time
import traceback
import NatronEngine
from PySide6.QtCore import QMetaObject, QTimer, Qt, Q_RETURN_ARG
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication, QLabel

output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True, exist_ok=True)
Path(os.environ['NATRON_TEST_START_MARKER']).write_text('Legacy OFX UI test started', encoding='utf-8')
backend = os.environ.get('NATRON_TEST_VIEWER_BACKEND', 'Vulkan')
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE'], 'backend': backend, 'cases': []}
ids = ['org.natron.test.LegacyOverlay', 'org.natron.test.LegacyCustomUI', 'org.natron.test.NoOverlay']
library = effect = source = viewer_node = writer = None
position, finished = 0, False
current = None


def counters():
    return [[library.OfxLegacyInteractCounter(kind, index) for index in range(8)] for kind in range(3)]


def display():
    widgets = [w for w in QApplication.allWidgets() if w.objectName().startswith('NatronVulkanViewer')
               and w.property('graphicsBackend') == backend and w.isVisible()]
    assert len(widgets) == 1
    return widgets[0]


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    if error:
        report.update(error=str(error), traceback=traceback.format_exc(), pending_case=current)
    widgets = [w for w in QApplication.allWidgets() if w.objectName().startswith('NatronVulkanViewer')
               and w.property('graphicsBackend') == backend]
    report['validation_errors'] = [w.property('validationErrors') for w in widgets]
    report['graphics_errors'] = [w.property('graphicsError') for w in widgets] if backend == 'Vulkan' else []
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['counters'] = counters() if library else []
    report['devices'] = [w.property('gpuDeviceName') for w in widgets]
    report['ok'] = not error and len(report['cases']) == 3 and not any(report['validation_errors']) \
        and not any(report['graphics_errors']) and not report['unexpected_dialogs']
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    os.write(1, b'NATRON_LEGACY_INTERACT_EXIT_REQUESTED\n')
    QTimer.singleShot(0, lambda: QApplication.exit(0))


def guarded(function):
    def invoke():
        try:
            function()
        except Exception as error:
            finish(error)
    return invoke


def create_case():
    global effect, current
    if position == len(ids):
        finish()
        return
    current = {'id': ids[position], 'before_counters': counters()}
    effect = app.createNode(ids[position], -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    assert effect is not None, 'CPU-compatible legacy OFX was excluded'
    assert effect.connectInput(0, source) and viewer_node.connectInput(0, effect)
    app.selectNode(effect, True)
    effect.getParam('gain').setValue(2.5)
    assert effect.getParam('gain').getValue() == 2.5, 'Ordinary parameter control is unavailable'
    QTimer.singleShot(3500 if position == 0 else 1500, guarded(inspect))


def inspect():
    global writer
    widget = display()
    frame = QMetaObject.invokeMethod(widget, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
    assert not frame.isNull() and widget.property('presentedFrames') > 0
    assert widget.property('nodeOverlayCommandCount') == 0, 'A legacy overlay reached the native renderer'
    assert frame.save(str(output.parent / f'{position}-viewer.png'))
    warning = effect.getParam('ofxDrawSuiteWarning')
    labels = [w for w in QApplication.allWidgets() if isinstance(w, QLabel) and w.isVisible()
              and 'Some plug-in editing controls require DrawSuite' in w.text()]
    current['warning_present'] = warning is not None
    if position < 2:
        assert warning is not None and labels, 'Nonmodal compatibility explanation is missing'
        assert labels[0].grab().save(str(output.parent / f'{position}-warning.png'))
        effect.getParam('refreshButton').trigger()
        detail = effect.getParam('nodeInfos').getValue()
        expected = 'Viewer overlay' if position == 0 else 'custom control'
        assert expected in detail and 'DrawSuite' in detail, detail
        if position == 1:
            assert 'gain' in detail, detail
        current['compatibility_detail'] = detail
    else:
        assert warning is None and not labels, 'A node without an interact incorrectly requires DrawSuite'
    assert not any(w.objectName() == 'gain/customInteract' for w in QApplication.allWidgets()), 'Legacy custom UI was created'
    assert all(not any(values[:6]) for values in counters()), 'A legacy callback ran during Describe/Create/drawing'
    destination = output.parent / f'{position}-identity.png'
    writer = app.createWriter(destination.as_posix())
    assert writer is not None and writer.connectInput(0, effect)
    app.render(writer, 1, 1)
    current['render_started'] = time.monotonic()
    QTimer.singleShot(100, guarded(inspect_render))


def inspect_render():
    destination = output.parent / f'{position}-identity.png'
    image = QImage(str(destination))
    if image.isNull() and time.monotonic() - current['render_started'] < 15:
        QTimer.singleShot(100, guarded(inspect_render))
        return
    assert not image.isNull() and (image.width(), image.height()) == (64, 48)
    for y in range(48):
        for x in range(64):
            pixel = image.pixelColor(x, y)
            expected = (255, 0, 0, 255) if x < 32 else (0, 255, 0, 255)
            assert pixel.getRgb() == expected, (x, y, pixel.getRgb(), expected)
    current['cpu_pixels_verified'] = 64 * 48
    writer.destroy(False)
    viewer_node.disconnectInput(0)
    effect.destroy(False)
    current['destroy_started'] = time.monotonic()
    QTimer.singleShot(200, guarded(inspect_destroy))


def inspect_destroy():
    global position, effect
    values = counters()
    before = current['before_counters'][position]
    if values[position][7] <= before[7] and time.monotonic() - current['destroy_started'] < 5:
        QTimer.singleShot(100, guarded(inspect_destroy))
        return
    assert values[position][6] > before[6] and values[position][7] > before[7], 'Effect creation/destruction was not exercised'
    assert all(not any(row[:6]) for row in values), 'A legacy interact callback ran during destruction'
    current['after_counters'] = values
    report['cases'].append(current)
    effect = None
    position += 1
    QTimer.singleShot(300, guarded(create_case))


def setup():
    global library, source, viewer_node
    app.addFormat('LegacyInteractSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('LegacyInteractSmoke')
    app.getProjectParam('frameRange').set(1, 1)
    app.getProjectParam('autoPreviews').set(False)
    source_file = output.parent / 'source.ppm'
    source_file.write_bytes(b'P6\n64 48\n255\n' + (bytes((255, 0, 0)) * 32 + bytes((0, 255, 0)) * 32) * 48)
    source = app.createReader(source_file.as_posix())
    assert source is not None
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    binary = Path('build/ofx-tests/LegacyInteractSmoke.ofx.bundle/Contents/Win64/LegacyInteractSmoke.ofx').resolve()
    library = ctypes.CDLL(str(binary))
    library.OfxLegacyInteractCounter.argtypes = [ctypes.c_int, ctypes.c_int]
    library.OfxLegacyInteractCounter.restype = ctypes.c_ulonglong
    window = display().window()
    window.hide()
    window.showNormal()
    window.move(0, 0)
    window.raise_()
    window.activateWindow()
    display().setFixedSize(640, 480)
    create_case()


QTimer.singleShot(0, guarded(setup))
