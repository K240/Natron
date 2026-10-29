"""Recreate two sharing Natron Viewers after submit/completion/present loss."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import time
import traceback
from PySide6.QtCore import QMetaObject, QTimer, Qt, Q_RETURN_ARG
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / 'build/vulkan-device-loss-gui-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Device loss GUI smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'cases': [],
          'physical_driver_reset': False, 'unexpected_dialogs': []}
retained, displays, viewers, artifacts = {}, [], [], []
abi, held = None, None
point = 0
started = time.monotonic()
before = None
old_ids = None


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    return values


def state():
    return [{'name': w.objectName(), 'backend': w.property('graphicsBackend'),
             'device_id': w.property('sharedDeviceId'), 'engine_device_id': w.property('engineDeviceId'),
             'error': w.property('graphicsError'), 'frames': w.property('presentedFrames'),
             'native_transfers': w.property('nativeImageTransfers'),
             'validation_errors': w.property('validationErrors')}
            for w in displays]


def frame(display):
    return QMetaObject.invokeMethod(display, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))


def valid(image):
    if image.isNull():
        return False
    image = image.convertToFormat(QImage.Format_RGBA8888)
    pixels = bytes(image.constBits())
    x1, x2 = image.width()//5, 4*image.width()//5
    y1, y2 = image.height()//5, 4*image.height()//5
    row = bytes((0, 255, 0, 255)) * (x2-x1)
    return all(pixels[y*image.bytesPerLine()+4*x1:y*image.bytesPerLine()+4*x2] == row
               for y in range(y1, y2))


def finish():
    if abi:
        abi.close()
    report['artifact_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    QApplication.exit(0)


def guarded(function):
    def invoke():
        try:
            function()
        except Exception:
            report['exception'] = traceback.format_exc()
            finish()
    return invoke


def node(kind, source=None):
    value = app.createNode('org.natron.test.VulkanPressure' + kind)
    assert value, kind
    if source:
        assert value.connectInput(0, source)
    return value


def add_viewer(effect, pane=None):
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert viewer and viewer.connectInput(0, effect)
    viewers.append(viewer)
    if pane:
        assert app.moveTab(viewer.getScriptName(), pane)
    candidates = [w for w in widgets() if w.objectName().startswith('NatronVulkanViewer')
                  and w.property('graphicsBackend') is not None and w not in displays]
    assert len(candidates) == 1
    display = candidates[0]
    displays.append(display)
    scale = display.devicePixelRatioF()
    display.setFixedSize(round(480/scale), round(360/scale))
    bounds = QApplication.primaryScreen().availableGeometry()
    display.window().setGeometry(bounds.x()+30+(len(displays)-1)*360, bounds.y()+30,
                                 max(round(600/scale), display.width()+100), display.height()+250)
    display.window().showNormal()
    return display


def wait():
    global held, point, before, old_ids, started
    dialogs = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['unexpected_dialogs'] = dialogs
    assert not dialogs, dialogs
    # Explicit capture causes both old windows to observe the shared loss. They
    # queue recreation outside the capture/render stack, including the second
    # Viewer which may otherwise have no spontaneous repaint pending.
    captured = [frame(w) for w in displays]
    current = state()
    report['last_state'] = current
    (output / 'readiness.json').write_text(json.dumps({'point': point, 'viewers': current}, indent=2))
    assert all(not w['validation_errors'] for w in current), current
    ready = all(w['backend'] == 'Vulkan' and not w['error'] and w['frames'] > 1
                and w['native_transfers'] > 0 for w in current) and all(valid(image) for image in captured)
    if old_ids:
        ready = ready and all(w['device_id'] not in old_ids for w in current)
    if ready:
        assert current[0]['device_id'] == current[1]['device_id']
        assert all(w['device_id'] == w['engine_device_id'] for w in current)
        for index, image in enumerate(captured):
            path = output / f'point-{point}-viewer-{index}.png'
            assert image.save(str(path))
            artifacts.append(path)
            report['pixels_compared'] = report.get('pixels_compared', 0) + (
                4*image.width()//5-image.width()//5) * (4*image.height()//5-image.height()//5)
        if held:
            statuses = abi.retired(held)
            fresh = abi.context()
            after = abi.counters()
            assert abi.generation(fresh) > held[3]
            assert after[0] > before[0] and after[1] == before[1], (before, after)
            report['cases'].append({'point': point, 'old_generation': held[3],
                                    'new_generation': abi.generation(fresh), 'retired_statuses': statuses,
                                    'before': before, 'after': after, 'viewers': current})
        if point == 3:
            info = abi.info(abi.context())
            assert info.validation_enabled == 1 and info.validation_errors == 0
            report.update(ok=True, device=info.name.decode('utf-8'), validation_errors=0,
                          native_recovered=True, final_state=current)
            finish()
            return
        point += 1
        held = abi.retain()
        before = abi.counters()
        old_ids = [w['device_id'] for w in current]
        assert abi.test.armDeviceLoss(held[0], point) == 0
        frame(displays[0])
        started = time.monotonic()
    if time.monotonic()-started > 35:
        raise RuntimeError(f'Device loss GUI timed out at point {point}: {report}')
    QTimer.singleShot(200, guarded(wait))


def setup():
    global abi, source
    abi = runpy.run_path(str(root / 'tools/vulkan-device-loss-abi.py'))['LossAbi'](root)
    app.addFormat('DeviceLoss 64x48 1')
    app.getProjectParam('outputFormat').setValue('DeviceLoss')
    app.getProjectParam('autoPreviews').setValue(False)
    source = node('Source')
    source.getParam('forceCaching').setValue(True)
    first = add_viewer(node('Inspect', source))
    parent = first.parentWidget()
    while parent and not (pane := app.getTabWidget(parent.objectName())):
        parent = parent.parentWidget()
    assert parent and pane
    second_pane = pane.splitHorizontally()
    second_pane.floatPane()
    add_viewer(node('Inspect', source), second_pane)
    app.clearSelection()
    QTimer.singleShot(750, guarded(wait))


QTimer.singleShot(0, guarded(setup))
