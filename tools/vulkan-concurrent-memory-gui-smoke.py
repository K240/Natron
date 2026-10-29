"""Overlap two Viewer CPU retries while a real shared-device heap stays full."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import time
import traceback

from PySide6.QtCore import QMetaObject, QTimer, Qt, Q_RETURN_ARG
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / 'build/vulkan-concurrent-memory-gui-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Concurrent memory GUI smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'stages': []}
retained = {}
viewers, displays = [], []
pressure = False
module = None
started = time.monotonic()
stage = 'initial'
quiet_activity = None
quiet_since = time.monotonic()
artifacts = []


def counters():
    return [module.OfxPressureCounter(i) for i in range(18)]


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    return values


def finish():
    global pressure
    if module:
        if pressure:
            report['pressure_release_status'] = module.OfxPressureReleaseHeap()
            pressure = False
        report['counters'] = counters()
    report['artifact_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    QApplication.exit(0)


def guarded(call):
    def run():
        try:
            call()
        except Exception:
            report['exception'] = traceback.format_exc()
            finish()
    return run


def frame(display):
    return QMetaObject.invokeMethod(display, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))


def valid(image, rgb):
    if image.isNull():
        return False
    image = image.convertToFormat(QImage.Format_RGBA8888)
    pixels = bytes(image.constBits())
    x1, x2 = image.width()//5, 4*image.width()//5
    y1, y2 = image.height()//5, 4*image.height()//5
    expected = bytes((*rgb, 255)) * (x2-x1)
    return all(pixels[y*image.bytesPerLine()+4*x1:y*image.bytesPerLine()+4*x2] == expected
               for y in range(y1, y2))


def capture(display, name, rgb):
    image = frame(display)
    assert valid(image, rgb), f'{name}: display interior differs from the reference color'
    path = output / (name + '.png')
    assert image.save(str(path))
    artifacts.append(path)
    pixels = (4*image.width()//5-image.width()//5) * (4*image.height()//5-image.height()//5)
    report.setdefault('capture_pixels', {})[name] = pixels
    report['pixels_compared'] = report.get('pixels_compared', 0) + pixels


def state():
    return [{'name': w.objectName(), 'visible': w.isVisible(), 'backend': w.property('graphicsBackend'),
             'device': w.property('gpuDeviceName'), 'device_id': w.property('sharedDeviceId'),
             'engine_device_id': w.property('engineDeviceId'), 'error': w.property('graphicsError'),
             'frames': w.property('presentedFrames'), 'native_transfers': w.property('nativeImageTransfers'),
             'validation_errors': w.property('validationErrors'), 'display_scale': w.devicePixelRatioF(),
             'surface_pixels': [round(w.width()*w.devicePixelRatioF()), round(w.height()*w.devicePixelRatioF())]}
            for w in displays]


def node(kind, source=None):
    effect = app.createNode('org.natron.test.VulkanPressure' + kind)
    assert effect, kind
    if source:
        assert effect.connectInput(0, source)
    return effect


def pane_for(display):
    parent = display.parentWidget()
    while parent:
        if pane := app.getTabWidget(parent.objectName()):
            return pane
        parent = parent.parentWidget()
    raise AssertionError('Could not locate the Viewer pane')


def add_viewer(effect, pane=None):
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert viewer and viewer.connectInput(0, effect)
    viewers.append(viewer)
    if pane:
        assert app.moveTab(viewer.getScriptName(), pane)
    candidates = [w for w in widgets() if w.objectName().startswith('NatronVulkanViewer')
                  and w.property('graphicsBackend') in ('Vulkan', 'Raster') and w not in displays]
    assert len(candidates) == 1, len(candidates)
    display = candidates[0]
    displays.append(display)
    scale = display.devicePixelRatioF()
    display.setFixedSize(round(480/scale), round(360/scale))
    # Fresh profiles on a multi-monitor desktop can start outside the visible
    # screen when QT_SCALE_FACTOR changes. Place only these owned test windows.
    window = display.window()
    bounds = QApplication.primaryScreen().availableGeometry()
    index = len(displays)-1
    window.setGeometry(bounds.x()+30+index*360, bounds.y()+30,
                       max(round(600/scale), display.width()+100), display.height()+250)
    window.showNormal()
    return display


def apply_pressure():
    global pressure, stage, started
    assert module.OfxPressureFillHeap() == 8, counters()
    pressure = True
    assert counters()[8] > 0 and counters()[9] == 1, counters()
    report['pressure_counters'] = counters()
    for display in displays:
        # Each surface fits the cap by itself. Under the retained heap pressure,
        # both requests compete for the shared remaining device-local memory.
        scale = display.devicePixelRatioF()
        display.setFixedSize(round(1200/scale), round(900/scale))
    app.clearSelection()
    stage, started = 'display-pressure', time.monotonic()


def wait():
    global stage, started, quiet_activity, quiet_since, pressure
    dialogs = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    assert not dialogs, dialogs
    current = state()
    report['last_state'] = current
    native_windows = QApplication.allWindows()
    retained.update((id(value), value) for value in native_windows)
    report['native_windows'] = [{'name': w.objectName(), 'visible': w.isVisible(), 'exposed': w.isExposed(),
                                 'state': str(w.windowState()), 'size': [w.width(), w.height()],
                                 'position': [w.x(), w.y()]} for w in native_windows
                                if w.objectName().endswith('/surface') or w.isTopLevel() and w.isVisible()]
    (output / 'readiness.json').write_text(json.dumps({'stage': stage, 'viewers': current,
                                                     'windows': report['native_windows']}, indent=2))
    assert all(w['visible'] and not w['validation_errors'] for w in current), current
    if stage == 'initial':
        assert all(w['backend'] == 'Vulkan' and not w['error'] for w in current), current
        ready = all(w['frames'] > 1 and w['native_transfers'] > 0 for w in current)
        if ready and all(valid(frame(w), (0, 255, 0)) for w in displays):
            ids = [w['device_id'] for w in current]
            assert ids[0] and len(set(ids)) == 1 and all(w['engine_device_id'] == ids[0] for w in current)
            report['initial_device_id'] = ids[0]
            report['stages'].append({'name': 'initial', 'viewers': current, 'counters': counters()})
            for i, display in enumerate(displays):
                capture(display, f'initial-{i}', (0, 255, 0))
                app.getViewer(viewers[i].getScriptName()).pause()
            stage, started = 'settling', time.monotonic()
    elif stage == 'settling':
        activity = counters()
        if activity != quiet_activity:
            quiet_activity, quiet_since = activity, time.monotonic()
        elif time.monotonic()-quiet_since >= .6:
            assert activity[0] >= 1 and activity[1] == 0 and activity[10] >= 2, activity
            apply_pressure()
    elif stage == 'display-pressure':
        if all(w['backend'] == 'Raster' for w in current):
            assert all('VkResult -2' in w['error'] for w in current), current
            activity = counters()
            if activity != quiet_activity:
                quiet_activity, quiet_since = activity, time.monotonic()
            elif time.monotonic()-quiet_since >= .6 and all(valid(frame(w), (0, 255, 0)) for w in displays):
                report['stages'].append({'name': 'display-pressure', 'viewers': current, 'counters': counters()})
                # Refill the memory released by the display fallback before
                # starting independent effect requests, with no resize in flight.
                assert module.OfxPressureFillHeap() == 8, counters()
                assert module.OfxPressureSetCpuBarrier(2) == 0
                filters = [node('ConcurrentFilter', source) for _ in viewers]
                for i, (viewer, effect) in enumerate(zip(viewers, filters)):
                    effect.getParam('requestToken').setValue(i)
                    viewer.disconnectInput(0)
                    assert viewer.connectInput(0, effect)
                app.clearSelection()
                stage, started = 'pressure', time.monotonic()
                # Graph edits can supersede an automatic connection render.
                # Request both independent viewers after those edits have
                # reached the event loop, while keeping the overlap barrier.
                def request_both():
                    for viewer in viewers:
                        app.getViewer(viewer.getScriptName()).renderCurrentFrame()
                QTimer.singleShot(0, guarded(request_both))
    elif stage == 'pressure':
        counts = counters()
        assert counts[15] == 0, f'Viewer CPU retries did not overlap: {counts}'
        raster = [w for w in displays if w.property('graphicsBackend') == 'Raster']
        if counts[3] >= 2 and counts[13] >= 2 and counts[14] >= 2 and counts[16] and counts[17] and raster:
            reasons = [w.property('graphicsError') for w in raster]
            assert all('VkResult -2' in value for value in reasons), reasons
            # Capture actual CPU output while the pressure images are retained.
            if all(valid(frame(w), (0, 255, 255)) for w in raster):
                for i, display in enumerate(raster):
                    capture(display, f'pressure-raster-{i}', (0, 255, 255))
                report['stages'].append({'name': 'pressure', 'viewers': current, 'counters': counts})
                report['raster_count_under_pressure'] = len(raster)
                report['overlapping_cpu_actions'] = counts[14]
                assert module.OfxPressureReleaseHeap() == 0
                pressure = False
                assert module.OfxPressureSetCpuBarrier(0) == 0
                stage, started = 'released', time.monotonic()
    elif stage == 'released':
        if all(valid(frame(w), (0, 255, 255)) for w in displays):
            for i, display in enumerate(displays):
                capture(display, f'recovered-{i}', (0, 255, 255))
            report['stages'].append({'name': 'released', 'viewers': current, 'counters': counters()})
            new_pane = pane_for(displays[0]).splitHorizontally()
            new_pane.floatPane()
            add_viewer(node('Inspect', node('Source')), new_pane)
            app.clearSelection()
            stage, started = 'native-recovery', time.monotonic()
    else:
        w = displays[-1]
        assert w.property('graphicsBackend') == 'Vulkan' and not w.property('graphicsError'), state()
        if w.property('presentedFrames') > 1 and w.property('nativeImageTransfers') > 0 and valid(frame(w), (0, 255, 0)):
            assert w.property('sharedDeviceId') == w.property('engineDeviceId') == report['initial_device_id']
            capture(w, 'native-recovery', (0, 255, 0))
            info = DeviceInfo()
            assert module.OfxPressureGetDeviceInfo(ctypes.byref(info)) == 0
            assert info.validation_enabled == 1 and info.validation_errors == 0
            kernel = ctypes.WinDLL('kernel32', use_last_error=True)
            kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
            kernel.GetModuleHandleW.restype = ctypes.c_void_p
            loaded = [name for name in ('Qt6OpenGL.dll', 'Qt6OpenGLWidgets.dll') if kernel.GetModuleHandleW(name)]
            assert not loaded, loaded
            report.update(ok=True, device=info.name.decode('utf-8'), validation_errors=info.validation_errors,
                          qt_opengl_loaded=loaded, system_gl_loaded=bool(kernel.GetModuleHandleW('opengl32.dll')),
                          unexpected_dialogs=dialogs, native_recovered=True,
                          display_scale=w.devicePixelRatioF(), final_state=state())
            finish()
            return
    if time.monotonic()-started > 55:
        raise RuntimeError(f'Concurrent Viewer memory test timed out at {stage}: {report}')
    QTimer.singleShot(200, guarded(wait))


class DeviceInfo(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in (
        'vendor_id', 'device_id', 'api_version', 'validation_enabled', 'validation_errors')]


def setup():
    global module, source
    assert os.environ.get('NATRON_VULKAN_MEMORY_LIMIT_MB') == '64'
    module = ctypes.CDLL(str(root / 'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx'))
    module.OfxPressureCounter.argtypes = [ctypes.c_int]
    module.OfxPressureCounter.restype = ctypes.c_uint64
    module.OfxPressureSetCpuBarrier.argtypes = [ctypes.c_uint]
    module.OfxPressureSetCpuBarrier.restype = ctypes.c_int
    module.OfxPressureFillHeap.restype = ctypes.c_int
    module.OfxPressureReleaseHeap.restype = ctypes.c_int
    module.OfxPressureGetDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
    module.OfxPressureGetDeviceInfo.restype = ctypes.c_int
    app.addFormat('ConcurrentMemory 64x48 1')
    app.getProjectParam('outputFormat').setValue('ConcurrentMemory')
    app.getProjectParam('autoPreviews').setValue(False)
    source = node('Source')
    source.getParam('forceCaching').setValue(True)
    first = add_viewer(node('Inspect', source))
    first_pane = pane_for(first)
    second_pane = first_pane.splitHorizontally()
    second_pane.floatPane()
    add_viewer(node('Inspect', source), second_pane)
    app.clearSelection()
    QTimer.singleShot(750, guarded(wait))


QTimer.singleShot(0, guarded(setup))
