"""Exercise the persistent USD/hdNVP backend in Natron's 3D Viewer."""
import json
import time
import traceback
from pathlib import Path
from PySide6.QtCore import QMetaObject, QTimer, Qt, QEventLoop
from PySide6.QtGui import QGuiApplication
from PySide6.QtWidgets import QApplication, QMainWindow, QDialog, QLabel, QGraphicsView
import NatronGui

directory = Path.cwd() / 'build/scene3d-hdnvp-demo'
report = {'ok': False}
retained = {}
test_application = None

def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values

def finish(error=None):
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
    report['ok'] = error is None
    (directory / 'gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if test_application:
        test_application.saveProjectAs((directory / 'hdnvp.ntp').as_posix())
    # Exercise the normal window-close path for both test instances.
    loaded_id = test_application.getAppID() if test_application else None
    if loaded_id != app.getAppID():
        app.saveProjectAs((directory / 'empty-test.ntp').as_posix())
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.close()

def setup():
    global test_application
    try:
        loaded = app.loadProject((directory / 'hdnvp.ntp').as_posix())
        assert loaded
        application = NatronGui.PyGuiApplication().getGuiInstance(loaded.getAppID())
        test_application = application
        viewer = application.getNode('HdNvpViewer') or application.createNode('fr.inria.built-in.Viewer')
        viewer.setScriptName('HdNvpViewer')
        if not viewer.getInput(0):
            assert viewer.connectInput(0, application.getNode('HdNvpRender3D'))
        for name, position in {'UsdStage': (0, 0), 'UsdCamera': (-240, 150), 'HdNvpRender3D': (0, 150), 'HdNvpViewer': (0, 310), 'ColorWriter': (200, 310)}.items():
            application.getNode(name).setPosition(*position)
        for window in widgets():
            if isinstance(window, QMainWindow):
                window.showNormal()
                # Saved workspace coordinates can put the Viewer above the
                # current monitor. Vulkan correctly skips unexposed surfaces.
                available = QGuiApplication.primaryScreen().availableGeometry()
                window.resize(min(1450, available.width()), min(1000, available.height()))
                window.move(available.center() - window.rect().center())
        def check():
            try:
                next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(True)
                next(w for w in widgets() if w.objectName() == 'Viewer3DCamera' and w.isVisible()).setChecked(True)
                plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.isVisible())
                # The empty launcher window must not occlude the native Vulkan
                # surface: an unexposed QWindow does not present queued frames.
                for window in widgets():
                    if isinstance(window, QMainWindow) and window != plot.window():
                        window.hide()
                plot.window().raise_()
                plot.window().activateWindow()
                def refresh():
                    assert QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
                    if plot.property('rendering'):
                        loop = QEventLoop()
                        poll = QTimer()
                        poll.timeout.connect(lambda: loop.quit() if not plot.property('rendering') else None)
                        poll.start(30)
                        QTimer.singleShot(60000, loop.quit)
                        loop.exec()
                        poll.stop()
                        assert not plot.property('rendering'), 'Preview timed out'
                    assert not plot.property('renderError'), plot.property('renderError')
                    assert plot.property('rendererBackend') == 'USD / hdNVP'
                refresh()
                renderer = application.getNode('HdNvpRender3D')
                denoiser = renderer.getParam('pathDenoiser')
                denoiser.setValue('Auto')
                refresh()
                automatic = json.loads(plot.property('hdNvpReport'))
                assert automatic['stats']['Denoiser'].startswith(('DLSS', 'OIDN')), automatic
                assert automatic['renderMode'] == 'interactive'
                denoiser.setValue('None')
                refresh()
                disabled = json.loads(plot.property('hdNvpReport'))
                assert disabled['stats']['Denoiser'] == 'None', disabled
                denoiser.setValue('OIDN')
                refresh()
                oidn = json.loads(plot.property('hdNvpReport'))
                assert oidn['stats']['Denoiser'].startswith('OIDN') and oidn['stats']['Denoise (ms)'] > 0, oidn
                denoiser.setValue('Auto')
                refresh()
                first = bytes(plot.property('capturedFrame').constBits())
                camera = application.getNode('UsdCamera')
                camera.getParam('translate').set(2., 0., 0.)
                refresh()
                assert bytes(plot.property('capturedFrame').constBits()) != first
                camera.getParam('translate').set(0., 0., 0.)
                refresh()
                assert plot.grab().save(str(directory / 'viewer.png'))
                application.setSelection([camera])
                assert QMetaObject.invokeMethod(plot, 'frameSelection', Qt.DirectConnection)
                refresh()
                assert len(plot.property('gizmoHandles')) == 3
                assert plot.grab().save(str(directory / 'camera-gizmo.png'))
                next(w for w in widgets() if w.objectName() == 'Viewer3DCamera' and w.isVisible()).setChecked(True)
                refresh()
                toggle = next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible())
                display = next(w for w in widgets() if w.metaObject().className().endswith('ViewerGL') and w.parentWidget() == plot.parentWidget())
                report['display_viewer'] = display.objectName()
                controller = application.getViewer(viewer.getScriptName())
                application.getProjectParam('frameRange').set(1, 4)
                def wait_until(predicate, description):
                    deadline = time.monotonic() + 60
                    loop = QEventLoop()
                    poll = QTimer()
                    poll.timeout.connect(lambda: loop.quit() if predicate() or time.monotonic() >= deadline else None)
                    poll.start(30)
                    loop.exec()
                    poll.stop()
                    report['native_transfers'] = display.property('nativeImageTransfers')
                    report['native_windows'] = [{'name': w.objectName(), 'visible': w.isVisible(), 'exposed': w.isExposed(), 'geometry': str(w.geometry())} for w in QGuiApplication.allWindows()]
                    assert predicate(), (description, display.property('lastPresentation'), errors())
                def errors():
                    return sorted({item.toolTip() for w in widgets() if isinstance(w, QGraphicsView) and w.scene()
                                   for item in w.scene().items() if 'HdNvpRender3D' in item.toolTip() and 'error:' in item.toolTip().lower()})
                for number in (2, 3, 4):
                    if number != 2:
                        assert QMetaObject.invokeMethod(plot, 'resetView', Qt.DirectConnection)
                        assert plot.property('rendering'), 'No active preview to cancel'
                    toggle.setChecked(False)
                    assert display.isVisible() and not plot.isVisible()
                    controller.seek(number)
                    controller.renderCurrentFrame(False)
                    wait_until(lambda: not plot.property('rendering') and display.property('lastPresentation').get('time0') == number,
                               '2D frame did not arrive after switching')
                    report['switch_errors'] = errors()
                    assert not report['switch_errors'], report['switch_errors']
                    assert not display.property('graphicsError'), display.property('graphicsError')
                    toggle.setChecked(True)
                    refresh()
                controller.seek(1)
                refresh()
                report['switch_3d_2d'] = True
                assert application.saveProjectAs((directory / 'hdnvp.ntp').as_posix())
                report.update(backend='USD / hdNVP', camera_update=True, camera_gizmo=True, saved=True,
                              denoiser=automatic['stats']['Denoiser'], denoiser_off=True, oidn_ms=oidn['stats']['Denoise (ms)'])
                finish()
            except Exception as error:
                finish(error)
        QTimer.singleShot(2000, check)
    except Exception as error:
        finish(error)

QTimer.singleShot(1000, setup)

def diagnostics():
    dialogs = [{'title': w.windowTitle(), 'labels': [label.text() for label in w.findChildren(QLabel)]} for w in widgets() if isinstance(w, QDialog) and w.isVisible()]
    (directory / 'gui-progress.json').write_text(json.dumps({'dialogs': dialogs}), encoding='utf-8')

progress_timer = QTimer()
progress_timer.timeout.connect(diagnostics)
progress_timer.start(3000)
