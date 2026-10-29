"""Owned-profile GUI regression for real-plate CameraTracker controls and USD."""
import base64
import json
import os
import time
import traceback
import zlib
from pathlib import Path

from PySide6.QtCore import QTimer, QMetaObject, Qt, QModelIndex, Q_RETURN_ARG
from PySide6.QtGui import QAction, QImage
from PySide6.QtWidgets import QApplication, QMainWindow, QPushButton, QTreeWidget
import NatronEngine
import NatronGui

directory = Path.cwd() / 'build/camera-tracker-real'
result_path = Path(os.environ['NATRON_GUI_TEST_RESULT'])
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE']}
auto_lens = os.environ.get('NATRON_CAMERA_AUTO') == '1'
refine_principal = os.environ.get('NATRON_CAMERA_PRINCIPAL') == '1'
last_frame = int(os.environ.get('NATRON_CAMERA_FRAMES', '45'))
retained = {}
application = tracker = viewer = plot = None
heartbeat = 0
track_start = 0
accepted_tracks = accepted_solve = ''
finished = False
outline_attempts = 0


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    report.update(ok=error is None, heartbeat=heartbeat)
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
    result_path.write_text(json.dumps(report, indent=2, default=str), encoding='utf-8')
    QApplication.exit(0)


def guarded(function):
    def run():
        if finished:
            return
        try:
            (directory / ('gui-progress-' + Path(report['profile']).name + '.txt')).write_text(function.__name__, encoding='utf-8')
            function()
        except Exception as error:
            finish(error)
    return run


def later(function, ms=150):
    QTimer.singleShot(ms, guarded(function))


def wait(predicate, function, timeout=90):
    deadline = time.monotonic() + timeout
    def poll():
        assert time.monotonic() < deadline, tracker.getParam('analysisStatus').getValue()
        if predicate():
            function()
        else:
            later(poll, 50)
    later(poll, 50)


def button(text):
    return next(w for w in widgets() if isinstance(w, QPushButton) and w.text().replace('&', '') == text and w.isVisible())


def action(text):
    actions = []
    for w in widgets():
        actions += w.actions() + w.findChildren(QAction)
    next(a for a in actions if a.text().replace('&', '').startswith(text) and a.isEnabled()).trigger()


def data(name):
    value = tracker.getParam(name).getValue()
    return json.loads(zlib.decompress(base64.b64decode(value)[4:])) if value else None


def setup():
    global application, tracker, viewer
    application = NatronGui.PyGuiApplication().getGuiInstance(app.getAppID())
    application.addFormat('CameraTrackerGui 640x480 1')
    application.getProjectParam('outputFormat').setValue('CameraTrackerGui')
    application.getProjectParam('frameRange').set(1, last_frame)
    reader = application.createReader((directory / 'plate/plate.####.png').as_posix())
    properties = {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)}
    tracker = application.createNode('fr.inria.built-in.CameraTracker', -1, application, properties)
    tracker.setScriptName('PlateCameraTracker')
    reader.setPosition(-300, 0)
    tracker.setPosition(-300, 150)
    assert tracker.connectInput(0, reader)
    for key, value in [('firstFrame', 1), ('lastFrame', last_frame), ('maximumFeatures', 180),
                       ('minimumTrackLength', 8)]:
        tracker.getParam(key).setValue(value)
    if auto_lens:
        tracker.getParam('calibrationMode').setValue('AutoLens')
        tracker.getParam('refinePrincipalPoint').setValue(refine_principal)
    else:
        for key, value in [('pixelIntrinsics', True), ('focalPixelsX', 535.4), ('focalPixelsY', 539.2),
                           ('principalPointX', 320.6), ('principalPointY', 248.1)]:
            tracker.getParam(key).setValue(value)
    viewer = application.getNode('Viewer1') or application.createNode('fr.inria.built-in.Viewer')
    viewer.disconnectInput(0)
    assert viewer.connectInput(0, tracker)
    viewer.setPosition(-300, 300)
    application.getViewer(viewer.getScriptName()).setAInput(0)
    application.getViewer(viewer.getScriptName()).setBInput(-1)
    application.getViewer(viewer.getScriptName()).renderCurrentFrame(False)
    application.setSelection([tracker])
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.hide()
            window.showNormal()
            window.resize(1600, 1050)
            window.raise_()
            window.activateWindow()
    later(cancel_track, 700)


def cancel_track():
    button('Track features').click()
    assert tracker.getParam('analysisRunning').getValue()
    later(lambda: button('Cancel analysis').click(), 80)
    wait(lambda: not tracker.getParam('analysisRunning').getValue(), after_cancel)


def after_cancel():
    assert not data('trackingData'), 'Cancelled job replaced accepted tracks'
    report['cancel_status'] = tracker.getParam('analysisStatus').getValue()
    later(start_track)


def start_track():
    global track_start
    track_start = heartbeat
    button('Track features').click()
    wait(lambda: not tracker.getParam('analysisRunning').getValue(), tracked)


def tracked():
    global accepted_tracks
    accepted_tracks = tracker.getParam('trackingData').getValue()
    assert len(data('trackingData')['observations']) > 500
    assert heartbeat - track_start >= 10, 'GUI event loop was blocked during tracking'
    report['tracking_heartbeats'] = heartbeat - track_start
    action('Undo')
    assert not data('trackingData'), 'Track result did not undo'
    action('Redo')
    assert tracker.getParam('trackingData').getValue() == accepted_tracks
    tracker.getParam('maximumFeatures').setValue(181)
    button('Solve camera').click()
    assert not tracker.getParam('analysisRunning').getValue()
    assert 'Track the current' in tracker.getParam('analysisStatus').getValue()
    tracker.getParam('maximumFeatures').setValue(180)
    button('Solve camera').click()
    wait(lambda: not tracker.getParam('analysisRunning').getValue(), solved)


def solved():
    global accepted_solve
    accepted_solve = tracker.getParam('reconstructionData').getValue()
    solution = data('reconstructionData')
    assert len(solution['cameras']) == last_frame and len(solution['points']) > 30
    report.update(camera_count=last_frame, point_count=len(solution['points']), rms=solution['rms'], calibration=solution['calibration'], ambiguous=solution.get('calibrationAmbiguous'))
    if auto_lens:
        assert solution['estimatedIntrinsics']
    report['estimated_principal_point'] = solution.get('estimatedPrincipalPoint', False)
    report['principal_validation_gain'] = solution.get('principalPointValidationGain', 0)
    action('Undo')
    assert not data('reconstructionData'), 'Solve result did not undo'
    action('Redo')
    assert tracker.getParam('reconstructionData').getValue() == accepted_solve
    application.getViewer(viewer.getScriptName()).seek(12)
    application.getViewer(viewer.getScriptName()).renderCurrentFrame(False)
    later(inspect_plate, 700)


def inspect_plate():
    for graph in widgets():
        if graph.metaObject().className() == 'Natron::NodeGraph' and graph.isVisible():
            QMetaObject.invokeMethod(graph, 'centerOnAllNodes', Qt.DirectConnection)
    report['viewer_widgets'] = [{'class': w.metaObject().className(), 'name': w.objectName(),
                                 'backend': w.property('graphicsBackend'), 'overlay': w.property('nodeOverlayCommandCount')}
                                for w in widgets() if w.property('graphicsBackend') and w.isVisible()]
    display = next(w for w in widgets() if w.property('nodeOverlayCommandCount') is not None and w.isVisible())
    frame = QMetaObject.invokeMethod(display, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
    report['plate_presentation'] = display.property('lastPresentation')
    report['presented_frames'] = display.property('presentedFrames')
    report['device'] = display.property('gpuDeviceName')
    report['viewport'] = str(display.property('canonicalViewport'))
    report['overlay_commands'] = display.property('nodeOverlayCommandCount')
    assert not display.property('graphicsError'), display.property('graphicsError')
    assert report['overlay_commands'] > 0, display.property('nodeOverlayStatus')
    assert not frame.isNull()
    frame.save(str(directory / 'tracked-plate.png'))
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.grab().save(str(directory / 'tracker-controls.png'))
    button('Create USD scene').click()
    assert application.getNode('SceneCamera1')
    if auto_lens:
        assert application.getNode('Render3D1').getInput(2).getPluginID() == 'fr.inria.built-in.CameraTrackerUndistort'
    application.getNode('Render3D1').getParam('renderer').setValue('CPU')
    later(open_scene, 700)


def open_scene():
    global plot
    toggle = next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible())
    toggle.setChecked(True)
    plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.isVisible())
    QMetaObject.invokeMethod(plot, 'frameSelection', Qt.DirectConnection)
    later(inspect_scene, 400)


def inspect_scene():
    assert not plot.property('renderError'), plot.property('renderError')
    assert not plot.property('capturedFrame').isNull()
    cameras = [o for o in plot.property('sceneObjects') if o['kind'] == 'SceneCamera']
    assert len(cameras) == 1 and not cameras[0]['editable'], cameras
    application.setSelection([application.getNode('SceneCamera1')])
    QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
    assert not plot.property('gizmoHandles'), 'Solved camera exposed unsupported edit handles'
    application.setSelection([])
    QMetaObject.invokeMethod(plot, 'frameSelection', Qt.DirectConnection)
    next(w for w in widgets() if w.objectName() == 'Viewer3DOutline' and w.isVisible()).click()
    later(inspect_outline, 600)


def inspect_outline():
    global outline_attempts
    outline_attempts += 1
    outline = next(w for w in widgets() if w.objectName() == 'StageOutline' and w.isVisible())
    outline.setProperty('pinnedSource', application.getNode('Render3D1').getParam('usdIdentity').getValue())
    assert 'failed' not in outline.property('stageStatus').lower(), outline.property('stageStatus')
    tree = outline.findChild(QTreeWidget, 'StageOutlineTree')
    tree.expandAll()
    def paths(parent=QModelIndex()):
        model = tree.model()
        result = []
        for row in range(model.rowCount(parent)):
            index = model.index(row, 0, parent)
            result.append(index.data(Qt.UserRole))
            result.extend(paths(index))
        return result
    values = [p for p in paths() if isinstance(p, str)]
    debug = {'source': outline.property('sourceNode'), 'status': outline.property('stageStatus'),
             'pinned': outline.property('pinnedSource'), 'paths': values}
    (directory / 'outline-debug.json').write_text(json.dumps(debug, indent=2), encoding='utf-8')
    assert outline_attempts < 20, debug
    if not any(p.endswith('/Landmarks') for p in values):
        later(inspect_outline, 300)
        return
    assert any('/Assets/' in p and p.endswith('/Camera') for p in values), values
    report['usd_paths'] = values
    assert plot.grab().save(str(directory / 'sparse-points.png'))
    for graph in widgets():
        if graph.metaObject().className() == 'Natron::NodeGraph' and graph.isVisible():
            QMetaObject.invokeMethod(graph, 'centerOnAllNodes', Qt.DirectConnection)
    project = directory / (Path(report['profile']).name + '.ntp')
    assert application.saveProjectAs(project.as_posix())
    report['project'] = project.as_posix()
    report.update(undo_redo=True, stale_solve_rejected=True, usd_outline=True)
    finish()


def tick():
    global heartbeat
    heartbeat += 1


timer = QTimer()
timer.timeout.connect(tick)
timer.start(50)
later(setup, 500)
