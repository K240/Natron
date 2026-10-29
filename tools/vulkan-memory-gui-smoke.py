"""Force real Viewer allocations past a bounded budget after healthy presentation."""
import hashlib
import ctypes
import json
import os
from pathlib import Path
import time
import traceback

from PySide6.QtCore import QMetaObject, Q_ARG, Q_RETURN_ARG, QTimer, Qt
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / 'build/vulkan-memory-gui-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Memory GUI smoke started', encoding='utf-8')
mode = os.environ.get('NATRON_TEST_MEMORY_GUI_MODE', 'transfer')
report = {'ok': False, 'mode': mode, 'profile': os.environ.get('NATRON_TEST_PROFILE')}
retained = {}
started = time.monotonic()
stage = 'initial'
native_module = None

def finish():
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    # Fixtures quit the Qt event loop directly, without the project save dialog.
    QApplication.exit(0)

def guarded(call):
    def run():
        try:
            call()
        except Exception:
            report['exception'] = traceback.format_exc()
            finish()
    return run

def widget():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    viewers = [value for value in values if value.objectName().startswith('NatronVulkanViewer')
               and value.property('graphicsBackend') in ('Vulkan', 'Raster') and value.isVisible()]
    assert len(viewers) == 1, len(viewers)
    return viewers[0]

def frame(w):
    return QMetaObject.invokeMethod(w, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))

def valid(image):
    if image.isNull():
        return False
    pixel = image.pixelColor(image.width() // 2, image.height() // 2)
    return max(abs(a-b) for a, b in zip((pixel.red(), pixel.green(), pixel.blue()), (0, 255, 0))) <= 2

def source(name, width, height):
    path = output / name
    path.write_bytes(f'P6\n{width} {height}\n255\n'.encode() + bytes((0, 255, 0)) * (width*height))
    return path

def wait():
    global stage, started
    dialogs = [v.windowTitle() for v in QApplication.topLevelWidgets() if v.isVisible() and v.isModal()]
    assert not dialogs, dialogs
    w = widget()
    backend = w.property('graphicsBackend')
    report['display_scale'] = w.devicePixelRatioF()
    report['last_backend'] = backend
    report['last_error'] = w.property('graphicsError')
    if stage == 'initial':
        assert backend == 'Vulkan', 'The Viewer must present successfully before pressure is applied'
        image = frame(w)
        report['initial_frames_seen'] = w.property('presentedFrames')
        report['initial_center'] = image.pixelColor(image.width()//2, image.height()//2).name() if not image.isNull() else None
        if w.property('presentedFrames') > 1 and valid(image):
            report['initial_frames'] = w.property('presentedFrames')
            report['initial_native_transfers'] = w.property('nativeImageTransfers')
            assert not w.property('graphicsError') and not w.property('validationErrors')
            stage, started = 'pressure', time.monotonic()
            if mode == 'canvas':
                # RGBA32F surface target exceeds the 64 MiB managed heap cap.
                w.setFixedSize(2200, 2100)
            else:
                assert mode in ('transfer', 'native-transfer')
                parent = w.parent()
                while parent and parent.metaObject().indexOfMethod('onFullFrameButtonToggle(bool)') < 0:
                    parent = parent.parent()
                assert parent, 'Could not find the Viewer full-frame processing slot'
                assert QMetaObject.invokeMethod(parent, 'onFullFrameButtonToggle', Qt.DirectConnection, Q_ARG(bool, True))
                if mode == 'native-transfer':
                    large = app.createNode('org.natron.test.VulkanMemorySuccess')
                else:
                    large = app.createReader(source('large.ppm', 3072, 2051).as_posix())
                viewer.disconnectInput(0)
                assert large and viewer.connectInput(0, large)
                if mode == 'native-transfer':
                    fmt = large.getOutputFormat()
                    report['native_format'] = [fmt.x1, fmt.y1, fmt.x2, fmt.y2]
                app.clearSelection()
                QTimer.singleShot(500, guarded(lambda: app.getViewer(viewer.getScriptName()).renderCurrentFrame(True)))
    elif stage == 'pressure':
        report['pressure_native_transfers'] = w.property('nativeImageTransfers')
        if mode == 'native-transfer':
            report['pressure_native_renders'] = native_module.OfxMemoryCounter(10)
            report['pressure_counters'] = [native_module.OfxMemoryCounter(i) for i in range(7, 11)]
        if backend == 'Raster':
            report['fallback_reason'] = w.property('graphicsError')
            assert 'VkResult -2' in report['fallback_reason'], report['fallback_reason']
            assert not w.property('sharedDeviceId') and not w.property('engineDeviceId')
            if mode == 'native-transfer':
                report['native_clip_renders'] = native_module.OfxMemoryCounter(10)
                assert report['initial_native_transfers'] > 0
                assert report['fallback_reason'].startswith('Native Viewer image transfer:'), report['fallback_reason']
                assert native_module.OfxMemoryCounter(9) == 0, 'GPU-only generator attempted a CPU render'
                # A GPU-only node has no CPU implementation. Reconnect the
                # retained reader to verify the fallback display remains usable.
                viewer.disconnectInput(0)
                assert viewer.connectInput(0, small_reader)
            w.setFixedSize(640, 480)
            stage, started = 'raster', time.monotonic()
    else:
        assert backend == 'Raster'
        image = frame(w)
        if w.property('presentedFrames') > 1 and valid(image):
            path = output / 'recovered.png'
            assert image.save(str(path))
            report.update(ok=True, raster_frames=w.property('presentedFrames'),
                          screenshot_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                          validation_errors=w.property('validationErrors'), unexpected_dialogs=dialogs)
            assert not report['validation_errors']
            finish()
            return
    if time.monotonic()-started > 55:
        raise RuntimeError(f'Viewer memory test timed out at {stage}: {report}')
    QTimer.singleShot(200, guarded(wait))

def setup():
    global viewer, small_reader, native_module
    assert os.environ.get('NATRON_VULKAN_MEMORY_LIMIT_MB') == '64'
    app.addFormat('MemoryGui 64x48 1')
    app.getProjectParam('outputFormat').setValue('MemoryGui')
    app.getProjectParam('autoPreviews').setValue(False)
    small = output / 'small.png'
    image = QImage(64, 48, QImage.Format_RGBA8888)
    image.fill(0xff00ff00)
    assert image.save(str(small))
    reader = app.createReader(small.as_posix())
    small_reader = reader
    if mode == 'native-transfer':
        native_module = ctypes.CDLL(str(root / 'build/ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/Win64/VulkanMemoryFailureSmoke.ofx'))
        native_module.OfxMemoryCounter.argtypes = [ctypes.c_int]
        native_module.OfxMemoryCounter.restype = ctypes.c_ulonglong
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert reader and viewer and viewer.connectInput(0, reader)
    app.clearSelection()
    w = widget()
    w.window().showNormal()
    w.setFixedSize(640, 480)
    QTimer.singleShot(750, guarded(wait))

QTimer.singleShot(0, guarded(setup))
