"""Exercise CPU fallback through the real Natron Viewer and input handlers."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import time
import traceback

from PySide6.QtCore import QEvent, QMetaObject, QPoint, QPointF, Q_RETURN_ARG, QTimer, Qt
from PySide6.QtGui import QImage, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / 'build/raster-gui-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Raster GUI smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'captures': []}
retained = {}
started = time.monotonic()

def finish():
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

def widget():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    viewers = [value for value in values if value.property('graphicsBackend') == 'Raster'
               and value.objectName().startswith('NatronVulkanViewer') and value.isVisible()]
    report['viewer_widgets'] = [{'name': value.objectName(), 'size': [value.width(), value.height()],
                                  'reason': value.property('graphicsError')} for value in viewers]
    assert len(viewers) == 1, f'Expected one raster Viewer: {len(viewers)}'
    return viewers[0]

def frame():
    return QMetaObject.invokeMethod(widget(), 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))

def samples(image):
    w = widget()
    viewport = w.property('canonicalViewport')
    values = []
    for x, y, expected in [(16,36,(255,0,0)), (48,36,(0,255,0)),
                            (16,12,(0,0,255)), (48,12,(255,255,255))]:
        px = int((x-viewport.x())/viewport.width()*image.width())
        py = int((viewport.y()+viewport.height()-y)/viewport.height()*image.height())
        if not (0 <= px < image.width() and 0 <= py < image.height()):
            return False, values
        pixel = image.pixelColor(px, py)
        actual = (pixel.red(), pixel.green(), pixel.blue())
        values.append(actual)
        if any(abs(a-b) > 2 for a,b in zip(actual,expected)):
            return False, values
    return True, values

def capture(name):
    image = frame()
    assert not image.isNull()
    valid, colors = samples(image)
    assert valid, (name, colors)
    path = output / (name+'.png')
    assert image.save(str(path))
    report['captures'].append({'file': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                               'size': [image.width(),image.height()], 'colors': colors})

def input_checks():
    w = widget()
    capture('initial')
    assert not w.property('sharedDeviceId') and not w.property('engineDeviceId')
    assert not w.property('nativeImageTransfers') and not w.property('validationErrors')
    assert w.property('graphicsError'), 'CPU fallback reason is missing'
    before = w.property('canonicalViewport')
    center = QPointF(w.width()/2,w.height()/2)
    global_point = QPointF(w.mapToGlobal(center.toPoint()))
    for kind, point, button, buttons in [
            (QEvent.MouseButtonPress, center, Qt.MiddleButton, Qt.MiddleButton),
            (QEvent.MouseMove, center+QPointF(17,9), Qt.NoButton, Qt.MiddleButton),
            (QEvent.MouseButtonRelease, center+QPointF(17,9), Qt.MiddleButton, Qt.NoButton)]:
        QApplication.sendEvent(w,QMouseEvent(kind,point,point,global_point,button,buttons,Qt.NoModifier))
    assert w.property('canonicalViewport') != before, 'CPU Viewer pan did not move the viewport'
    report['pan'] = True
    zoom = w.property('zoomFactor')
    QApplication.sendEvent(w,QWheelEvent(center,global_point,QPoint(),QPoint(0,120),Qt.NoButton,
                                         Qt.NoModifier,Qt.NoScrollPhase,False))
    assert w.property('zoomFactor') > zoom, 'CPU Viewer wheel zoom did not work'
    report['wheel_zoom'] = True
    report['fallback_reason'] = w.property('graphicsError')
    QTimer.singleShot(750,guarded(after_input))

def after_input():
    capture('after-pan-zoom')
    get_module = ctypes.WinDLL('kernel32').GetModuleHandleW
    get_module.argtypes = [ctypes.c_wchar_p]
    get_module.restype = ctypes.c_void_p
    report['loaded_graphics_modules'] = [name for name in ('vulkan-1.dll','opengl32.dll','Qt6OpenGL.dll',
                                           'Qt6OpenGLWidgets.dll','libEGL.dll','libGLESv2.dll') if get_module(name)]
    allowed = [] if os.environ.get('NATRON_VULKAN_DISABLED') == '1' else ['vulkan-1.dll']
    assert not set(report['loaded_graphics_modules']) - set(allowed)
    report['unexpected_dialogs'] = [value.windowTitle() for value in QApplication.topLevelWidgets()
                                    if value.isVisible() and value.isModal()]
    assert not report['unexpected_dialogs']
    report['ok'] = True
    finish()

def wait():
    dialogs = [value.windowTitle() for value in QApplication.topLevelWidgets() if value.isVisible() and value.isModal()]
    assert not dialogs, dialogs
    image = frame()
    ready = not image.isNull() and samples(image)[0]
    if ready:
        input_checks()
    elif time.monotonic()-started > 45:
        raise RuntimeError('CPU Viewer image did not become ready')
    else:
        QTimer.singleShot(200,guarded(wait))

def setup():
    app.addFormat('RasterSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('RasterSmoke')
    app.getProjectParam('autoPreviews').setValue(False)
    colors = [(255,0,0),(0,255,0),(0,0,255),(255,255,255)]
    source = output / 'quadrants.ppm'
    source.write_bytes(b'P6\n64 48\n255\n'+b''.join(bytes(colors[(y>=24)*2+(x>=32)]) for y in range(48) for x in range(64)))
    reader = app.createReader(source.as_posix())
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert reader and viewer and viewer.connectInput(0,reader)
    app.clearSelection()
    w = widget()
    w.window().showNormal()
    w.setFixedSize(640,480)
    QTimer.singleShot(750,guarded(wait))


QTimer.singleShot(0,guarded(setup))
