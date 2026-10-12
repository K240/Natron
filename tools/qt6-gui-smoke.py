"""Inspect a launched Natron GUI, then quit the test instance."""
import json
import os
import sys
import time
from pathlib import Path

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Python GUI smoke started', encoding='utf-8')

from PySide6.QtCore import QTimer, qVersion, QMetaObject, Q_RETURN_ARG, Qt, QEvent, QPoint, QPointF
from PySide6.QtGui import QImage, QGuiApplication, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication, QMainWindow, QTextBrowser
import NatronEngine

_smoke_connected = False
_retained = {}


def widgets():
    values = QApplication.allWidgets()
    _retained.update((id(value), value) for value in values)
    return values


def native_windows():
    values = QGuiApplication.allWindows()
    _retained.update((id(value), value) for value in values)
    return values


def inspect_native_input(viewer):
    surfaces = [window for window in native_windows()
                if window.objectName() == viewer.objectName() + '/surface']
    assert len(surfaces) == 1, 'Native Vulkan surface was not found'
    surface = surfaces[0]
    center = QPointF(viewer.width() / 2, viewer.height() / 2)
    global_point = QPointF(viewer.mapToGlobal(center.toPoint()))

    def mouse(kind, point, button, buttons):
        event = QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier)
        QGuiApplication.sendEvent(surface, event)

    before = viewer.property('canonicalViewport')
    mouse(QEvent.MouseButtonPress, center, Qt.MiddleButton, Qt.MiddleButton)
    mouse(QEvent.MouseMove, center + QPointF(17, 9), Qt.NoButton, Qt.MiddleButton)
    mouse(QEvent.MouseButtonRelease, center + QPointF(17, 9), Qt.MiddleButton, Qt.NoButton)
    panned = viewer.property('canonicalViewport')
    assert panned != before, 'Middle-button pan did not reach the Viewer widget'
    mouse(QEvent.MouseButtonPress, center + QPointF(17, 9), Qt.MiddleButton, Qt.MiddleButton)
    mouse(QEvent.MouseMove, center, Qt.NoButton, Qt.MiddleButton)
    mouse(QEvent.MouseButtonRelease, center, Qt.MiddleButton, Qt.NoButton)

    original_zoom = viewer.property('zoomFactor')
    for delta in (120, -120):
        event = QWheelEvent(center, global_point, QPoint(), QPoint(0, delta), Qt.NoButton,
                            Qt.NoModifier, Qt.NoScrollPhase, False)
        QGuiApplication.sendEvent(surface, event)
        if delta > 0:
            zoomed = viewer.property('zoomFactor')
    assert zoomed > original_zoom, 'Wheel zoom did not reach the Viewer widget'
    restored = viewer.property('canonicalViewport')
    assert abs(viewer.property('zoomFactor') - original_zoom) < 1e-8
    assert abs(restored.x() - before.x()) < 1e-8 and abs(restored.y() - before.y()) < 1e-8
    return {'pan': True, 'wheel_zoom': True, 'zoom_before': original_zoom, 'zoom_after': zoomed,
            'surface': surface.objectName()}


def quadrant_pixels(viewer, frame):
    if frame.isNull():
        return {}, False
    viewport = viewer.property('canonicalViewport')
    corners = {}
    valid = True
    for name, x, y, expected in (('top_left', 16, 36, (255, 0, 0)),
                                ('top_right', 48, 36, (0, 255, 0)),
                                ('bottom_left', 16, 12, (0, 0, 255)),
                                ('bottom_right', 48, 12, (255, 255, 255))):
        px = int((x-viewport.x())/viewport.width()*frame.width())
        py = int((viewport.y()+viewport.height()-y)/viewport.height()*frame.height())
        if not (0 <= px < frame.width() and 0 <= py < frame.height()):
            corners[name] = None
            valid = False
            continue
        actual = frame.pixelColor(px, py)
        corners[name] = actual.name()
        valid = valid and all(abs(a-b) <= 2 for a, b in
                              zip((actual.red(), actual.green(), actual.blue()), expected))
    return corners, valid


def inspect_gui():
    all_widgets = widgets()
    windows = [w for w in all_widgets if isinstance(w, QMainWindow) and w.isVisible()]
    viewers = [w for w in all_widgets if w.property('graphicsBackend') == 'Vulkan' and w.isVisible()]
    all_viewers = [w for w in all_widgets if w.property('graphicsBackend') == 'Vulkan']
    libraries = [w.toPlainText() for w in all_widgets
                 if isinstance(w, QTextBrowser) and w.objectName() == 'libraryVersions']
    qt_gl_modules = None
    system_gl_loaded = None
    if sys.platform == 'win32':
        import ctypes
        get_module = ctypes.WinDLL('kernel32', use_last_error=True).GetModuleHandleW
        get_module.argtypes = [ctypes.c_wchar_p]
        get_module.restype = ctypes.c_void_p
        qt_gl_modules = [name for name in ('Qt6OpenGL.dll', 'Qt6OpenGLWidgets.dll') if get_module(name)]
        system_gl_loaded = bool(get_module('opengl32.dll'))
    report = {
        'qt': qVersion(),
        'platform': QGuiApplication.platformName(),
        'screens': [{'geometry': [s.geometry().x(), s.geometry().y(), s.geometry().width(), s.geometry().height()],
                     'dpr': s.devicePixelRatio()} for s in QGuiApplication.screens()],
        'presentation_wait_ms': round((time.monotonic()-_presentation_started)*1000),
        'presentation_attempts': _presentation_attempts,
        'scripting_core_module': QtCore.__name__,
        'scripting_gui_module': QtGui.__name__,
        'scripting_widgets_module': QtWidgets.__name__,
        'imported_qt_modules': sorted(name for name in sys.modules if name.startswith(('PySide6.', 'qtpy.'))),
        'windows': [w.windowTitle() for w in windows],
        'main_window_states': [{'minimized': w.isMinimized(), 'active': w.isActiveWindow(),
            'size': [w.width(), w.height()]} for w in windows],
        'native_windows': [{'name': w.objectName(), 'visible': w.isVisible(), 'exposed': w.isExposed(),
            'state': str(w.windowState()), 'size': [w.width(), w.height()]} for w in native_windows()],
        'unexpected_dialogs': [w.windowTitle() for w in QApplication.topLevelWidgets()
                               if w.isVisible() and w.isModal()],
        'vulkan_viewers': len(viewers),
        'legacy_gl_widgets': sum(w.inherits('QOpenGLWidget') for w in all_widgets),
        'qt_gl_modules': qt_gl_modules,
        'system_gl_loaded': system_gl_loaded,
        'graphics_errors': [w.property('graphicsError') for w in all_viewers],
        'devices': [w.property('gpuDeviceName') for w in all_viewers],
        'shared_device_ids': [w.property('sharedDeviceId') for w in all_viewers],
        'engine_device_ids': [w.property('engineDeviceId') for w in all_viewers],
        'presented_frames': [w.property('presentedFrames') for w in viewers],
        'validation_errors': [w.property('validationErrors') for w in all_viewers],
        'texture_depth_choice': NatronEngine.natron.getSettings().getParam('texturesBitDepth').get(),
        'library_versions': libraries,
    }
    if package := os.environ.get('NATRON_PORTABLE_ROOT'):
        import runpy
        modules = runpy.run_path(os.environ['NATRON_PORTABLE_MODULE_TEST'])['loaded_modules']()
        permitted = [Path(package).resolve(), Path(os.environ['WINDIR']).resolve()]
        if driver := os.environ.get('NATRON_PORTABLE_SOFTWARE_DRIVER'):
            permitted.append(Path(driver).resolve())
        report['loaded_modules'] = modules
        report['unexpected_modules'] = [p for p in modules if not any(Path(p).resolve().is_relative_to(base) for base in permitted)]
    report['ok'] = (bool(windows) and bool(viewers)
                    and report['legacy_gl_widgets'] == 0
                    and not qt_gl_modules
                    and QtCore.__name__ == 'PySide6.QtCore'
                    and QtGui.__name__ == 'PySide6.QtGui'
                    and QtWidgets.__name__ == 'PySide6.QtWidgets'
                    and not any(name == 'qtpy' or name.startswith('qtpy.') for name in sys.modules)
                    and not any(report['graphics_errors'])
                    and not any(report['validation_errors'])
                    and all(report['presented_frames'])
                    and len(all_viewers) >= 2
                    and len(set(report['shared_device_ids'])) == 1
                    and report['engine_device_ids'] == report['shared_device_ids']
                    and len(libraries) == 1 and 'Vulkan version ' in libraries[0]
                    and 'OpenGL renderers:' not in libraries[0]
                    and all(device and device in libraries[0] for device in report['devices'])
                    and not report['unexpected_dialogs'])
    report['ok'] = report['ok'] and not report.get('unexpected_modules', [])
    image_viewers = viewers
    if image_viewers:
        report['native_input'] = inspect_native_input(image_viewers[0])
        frame = QMetaObject.invokeMethod(image_viewers[0], 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
        report['framebuffer_size'] = [frame.width(), frame.height()]
        report['ok'] = report['ok'] and not frame.isNull()
        if _smoke_connected and not frame.isNull():
            pixel = frame.pixelColor(frame.width() // 2, frame.height() // 2)
            report['center_pixel'] = pixel.name()
            report['quadrant_pixels'], valid = quadrant_pixels(image_viewers[0], frame)
            report['ok'] = report['ok'] and valid
        frame.save('build/gui-qt6-frame.png')
        report['graphics_errors'] = [w.property('graphicsError') for w in all_viewers]
        report['validation_errors'] = [w.property('validationErrors') for w in all_viewers]
        report['ok'] = report['ok'] and not any(report['graphics_errors']) and not any(report['validation_errors'])
    Path('build/gui-qt6-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('GUI_QT6_SMOKE', json.dumps(report))
    QApplication.exit(0)


def wait_for_presentation():
    global _presentation_attempts
    _presentation_attempts += 1
    all_widgets = widgets()
    all_viewers = [w for w in all_widgets if w.property('graphicsBackend') == 'Vulkan']
    viewers = [w for w in all_viewers if w.isVisible()]
    terminal = (any(w.isVisible() and w.isModal() for w in QApplication.topLevelWidgets())
                or any(w.property('graphicsError') or w.property('validationErrors') for w in all_viewers))
    ready = bool(viewers) and all(w.property('presentedFrames') for w in viewers)
    if ready and _smoke_connected:
        frame = QMetaObject.invokeMethod(viewers[0], 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
        _, ready = quadrant_pixels(viewers[0], frame)
    if _presentation_attempts % 10 == 1:
        Path('build/gui-qt6-readiness.json').write_text(json.dumps({
            'wait_ms': round((time.monotonic()-_presentation_started)*1000),
            'frames': [w.property('presentedFrames') for w in viewers],
            'main': [{'minimized': w.isMinimized(), 'size': [w.width(), w.height()]} for w in all_widgets if isinstance(w, QMainWindow)],
            'native': [{'name': w.objectName(), 'visible': w.isVisible(), 'exposed': w.isExposed(),
                'size': [w.width(), w.height()]} for w in native_windows()]}, indent=2), encoding='utf-8')
    if ready or terminal or time.monotonic()-_presentation_started >= 45:
        inspect_gui()
    else:
        QTimer.singleShot(250, wait_for_presentation)

viewer = app.createNode('fr.inria.built-in.Viewer')
assert viewer is not None, 'Could not create a viewer'
source = Path.cwd() / 'build' / 'qt6-smoke' / '出力.png'
if source.is_file():
    reader = app.createReader(source.as_posix())
    assert reader is not None and viewer.connectInput(0, reader)
    _smoke_connected = True
second_viewer = app.createNode('fr.inria.built-in.Viewer')
assert second_viewer is not None
# The four corners catch channel swaps and row-direction errors in the actual
# Engine cache-to-Vulkan upload, which uniform red/cyan frames cannot detect.
quad_source = Path.cwd() / 'build' / 'qt6-smoke' / 'vulkan-quadrants.ppm'
quad_source.parent.mkdir(parents=True, exist_ok=True)
colors = ((255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255))
quad_source.write_bytes(b'P6\n64 48\n255\n' + b''.join(bytes(colors[(y >= 24)*2 + (x >= 32)])
                                                        for y in range(48) for x in range(64)))
quad_reader = app.createReader(quad_source.as_posix())
assert quad_reader is not None and second_viewer.connectInput(0, quad_reader)
_smoke_connected = True
# Windows may initially hide a process started by a test runner. Expose this
# owned window explicitly so presentation is tested on a real native surface.
for window in [w for w in widgets() if isinstance(w, QMainWindow)]:
    window.showNormal()
    window.resize(1280, 900)
    window.move(60, 60)
    window.raise_()
    window.activateWindow()
_presentation_started = time.monotonic()
_presentation_attempts = 0
QTimer.singleShot(0, wait_for_presentation)
