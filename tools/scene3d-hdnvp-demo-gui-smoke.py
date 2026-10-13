"""Exercise the persistent USD/hdNVP backend in Natron's 3D Viewer."""
import json
import traceback
from pathlib import Path
from PySide6.QtCore import QMetaObject, QTimer, Qt
from PySide6.QtWidgets import QApplication, QMainWindow, QDialog, QLabel
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
                window.resize(1450, 1000)
        def check():
            try:
                next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(True)
                next(w for w in widgets() if w.objectName() == 'Viewer3DCamera' and w.isVisible()).setChecked(True)
                plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.isVisible())
                def refresh():
                    assert QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
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
