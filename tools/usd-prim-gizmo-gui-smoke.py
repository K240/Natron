"""Owned GUI regression for composed USD Prim transform gizmos.

Uses the portable USD fixture; no external scene or tracking solve is needed.
Run through run-windows-owned-gui-test.ps1 with a fresh profile.
"""
import hashlib
import json
import math
import os
import time
import traceback
from pathlib import Path

from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPointF, QTimer, Qt
from PySide6.QtGui import QAction, QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QPlainTextEdit
import NatronGui

output = Path.cwd() / 'build/usd-prim-gizmos'
output.mkdir(parents=True, exist_ok=True)
fixture = Path.cwd() / 'Tests/fixtures/scene3d/usd/scene.usda'
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE']}
retained = {}
application = renderer = source = viewer = outline = plot = edit = None
finished = False
targets = os.environ.get('NATRON_PRIM_GIZMO_TARGETS', '/World/Animated/Panel').split(',')
target = '\n'.join(targets)
authored_target = target
material_path = '/World/Animated/Surface'
parent_mode = os.environ.get('NATRON_PRIM_GIZMO_PARENT') == '1'


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
        if plot:
            report['viewer_state'] = {key: plot.property(key) for key in ('selectedNode', 'selectedPrimPaths', 'renderError', 'sceneObjects')}
        if outline:
            report['outline_selection'] = outline.property('selectedPrimPaths')
    report['dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['ok'] = error is None and not report['dialogs']
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2, default=str), encoding='utf-8')
    for w in widgets():
        if isinstance(w, QMainWindow) and w.isVisible():
            w.grab().save(str(output / ('error.png' if error else 'usd-prim-gizmos.png')))
    QApplication.exit(0)


def guarded(function):
    def run():
        if finished:
            return
        try:
            (output / 'progress.txt').write_text(function.__name__, encoding='utf-8')
            function()
        except Exception as error:
            finish(error)
    return run


def wait(predicate, function, seconds=60):
    deadline = time.monotonic() + seconds
    def poll():
        assert time.monotonic() < deadline, (function.__name__, plot.property('renderError') if plot else '',
                                            outline.property('stageStatus') if outline else '')
        if predicate():
            function()
        else:
            QTimer.singleShot(100, guarded(poll))
    QTimer.singleShot(100, guarded(poll))


def invoke(widget, name):
    assert QMetaObject.invokeMethod(widget, name, Qt.DirectConnection)


def choose(paths):
    assert QMetaObject.invokeMethod(outline, 'selectPrimPaths', Qt.DirectConnection, Q_ARG('QStringList', paths))


def values(name):
    return [edit.getParam(name).getValue(i) for i in range(3)]


def ready():
    return (not plot.property('rendering') and not outline.property('composing')
            and not plot.property('renderError') and plot.property('selectedNode') == edit.getScriptName()
            and len(plot.property('gizmoHandles')) == 3)


def mouse(kind, point, button, buttons):
    global_point = QPointF(plot.mapToGlobal(point.toPoint()))
    QApplication.sendEvent(plot, QMouseEvent(kind, point, point, global_point, button, buttons, Qt.NoModifier))


def drag(start, end):
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    for step in range(1, 5):
        mouse(QEvent.MouseMove, start + (end - start) * (step / 4), Qt.NoButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)


def axis_drag(axis=0, distance=22):
    center = plot.property('gizmoCenter')
    start = plot.property('gizmoHandles')[axis]
    direction = start - center
    length = math.hypot(direction.x(), direction.y())
    assert length > 2, ('Edge-on handle', axis)
    drag(start, start + direction * (distance / length))


def undo(redo=False):
    name = next(w for w in widgets() if isinstance(w, QLineEdit) and w.text() == edit.getScriptName())
    panel = name.parentWidget()
    while panel and panel.metaObject().indexOfSlot('onUndoClicked()') < 0:
        panel = panel.parentWidget()
    assert panel, 'Missing edit Undo stack'
    invoke(panel, 'onRedoPressed' if redo else 'onUndoClicked')


def check_undo(name, before, after):
    assert math.dist(before, after) > 1e-5, (name, before, after)
    undo()
    assert math.dist(values(name), before) < 1e-8, ('Undo', name)
    undo(True)
    assert math.dist(values(name), after) < 1e-8, ('Redo', name)


def setup():
    global application, source, renderer, viewer, targets, target, material_path
    application = NatronGui.PyGuiApplication().getGuiInstance(app.getAppID())
    source = application.createNode('fr.inria.built-in.ReadUSD')
    source.setScriptName('SourceUSD')
    source.setPosition(100, -180)
    source.getParam('file').setValue(fixture.as_posix())
    if parent_mode:
        source.getParam('usdAssembly').setValue(1)
        source.getParam('readAnimation').setValue(False)
        root = '/Natron/Assets/n_' + source.getParam('usdIdentity').getValue() + '/Source'
        targets = [path.replace('/World', root, 1) for path in targets]
        target = '\n'.join(targets)
        material_path = root + '/Animated/Surface'
    camera = application.createNode('fr.inria.built-in.Camera')
    camera.setPosition(400, 200)
    renderer = application.createNode('fr.inria.built-in.Render3D')
    renderer.setScriptName('PrimEditRender')
    renderer.setPosition(100, 200)
    renderer.getParam('renderer').setValue('hdNVP')
    renderer.getParam('pathDenoiser').setValue('None')
    renderer.getParam('pathSamples').setValue(1)
    renderer.getParam('pathBounces').setValue(1)
    assert renderer.connectInput(0, source) and renderer.connectInput(1, camera)
    viewer = application.getNode('Viewer1') or application.createNode('fr.inria.built-in.Viewer')
    viewer.setPosition(100, 350)
    assert viewer.connectInput(0, renderer)
    application.getViewer(viewer.getScriptName()).seek(1)
    report['source_hash'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    for w in widgets():
        if isinstance(w, QMainWindow):
            w.showNormal()
            w.resize(1500, 1000)
    QTimer.singleShot(500, guarded(open_outline))


def open_outline():
    global outline, plot
    next(w for w in widgets() if w.objectName() == 'Viewer3DOutline' and w.isVisible()).click()
    outline = next(w for w in widgets() if w.objectName() == 'StageOutline' and w.isVisible())
    plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.window() == outline.window())
    wait(lambda: not outline.property('composing') and 'revision' in outline.property('stageStatus'), select_target)


def select_target():
    choose(targets)
    wait(lambda: not outline.property('composing') and outline.property('selectedPrimPaths') == sorted(targets)
         and all(path in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText() for path in targets), insert_edit)


def insert_edit():
    global edit, targets, target, authored_target, material_path
    invoke(outline, 'addTransformEdit')
    edit = renderer.getInput(0)
    assert edit.getPluginID() == 'fr.inria.built-in.USDTransform', (edit.getPluginID(), edit.getScriptName(),
        outline.property('sourceNode'), outline.property('selectedPrimPaths'), outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText())
    assert edit.getParam('primPaths').getValue() == target
    expected_x = sum(1 if '/Instance/' in path else -1 for path in targets) / len(targets)
    assert math.dist(values('pivot'), [expected_x, 0, 0]) < 1e-6, values('pivot')
    report['targets'] = targets
    report['pivot'] = values('pivot')
    edit.setPosition(100, -30)
    authored_target = target
    if parent_mode:
        parent = application.createNode('fr.inria.built-in.TransformGeo')
        parent.setScriptName('ParentFrame')
        parent.setPosition(100, 85)
        parent.getParam('translate').set(.3, .25, 0.)
        parent.getParam('rotate').set(7., 19., 13.)
        parent.getParam('scale').set(1.2, .9, 1.1)
        assert parent.connectInput(0, edit)
        renderer.disconnectInput(0)
        assert renderer.connectInput(0, parent)
        mount = '/Natron/Assets/n_' + source.getParam('usdIdentity').getValue()
        remounted = mount + '_t_' + parent.getParam('usdIdentity').getValue()
        targets = [path.replace(mount, remounted, 1) for path in targets]
        target = '\n'.join(targets)
        material_path = material_path.replace(mount, remounted, 1)
        invoke(outline, 'refreshSource')
    wait(lambda: not outline.property('composing'), show_gizmos)


def show_gizmos():
    next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(True)
    plot.setProperty('lookThroughCamera', True)
    wait(lambda: not plot.property('rendering') and not outline.property('composing') and not plot.property('renderError'), select_gizmo_targets)


def select_gizmo_targets():
    choose(targets)
    wait(ready, move_prim)


def move_prim():
    before = values('translate')
    plot.setProperty('gizmoMode', 0)
    axis_drag()
    check_undo('translate', before, values('translate'))
    report['move_undo_redo'] = True
    wait(ready, rotate_prim)


def rotate_prim():
    before = values('rotate')
    plot.setProperty('gizmoMode', 1)
    center = plot.property('gizmoCenter')
    start = plot.property('gizmoHandles')[2]
    delta = start - center
    angle = math.radians(18)
    end = center + QPointF(delta.x() * math.cos(angle) + delta.y() * math.sin(angle),
                          -delta.x() * math.sin(angle) + delta.y() * math.cos(angle))
    expected_degrees = 18
    if parent_mode:
        # A tilted parent foreshortens the circle into an ellipse. Follow its
        # displayed arc instead of treating screen angles as world angles.
        ring = plot.property('gizmoRings')[2]
        index = (len(ring) - 1) // 8
        start, end = ring[index], ring[index + 8]
        expected_degrees = 360 * 8 / (len(ring) - 1)
    drag(start, end)
    check_undo('rotate', before, values('rotate'))
    assert abs(values('rotate')[2] - expected_degrees) < .05, values('rotate')
    report['rotation_around_selection_undo_redo'] = True
    wait(ready, scale_prim)


def scale_prim():
    before = values('scale')
    plot.setProperty('gizmoMode', 2)
    center = plot.property('gizmoCenter')
    drag(center, center + QPointF(16, -12))
    after = values('scale')
    check_undo('scale', before, after)
    assert max(after) - min(after) < 1e-8
    report['uniform_scale_undo_redo'] = True
    wait(ready, cancel_drag)


def cancel_drag():
    before = values('scale')
    center = plot.property('gizmoCenter')
    mouse(QEvent.MouseButtonPress, center, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseMove, center + QPointF(12, 0), Qt.NoButton, Qt.LeftButton)
    assert math.dist(values('scale'), before) > .01
    QApplication.sendEvent(plot, QKeyEvent(QEvent.KeyPress, Qt.Key_Escape, Qt.NoModifier))
    mouse(QEvent.MouseButtonRelease, center, Qt.LeftButton, Qt.NoButton)
    assert math.dist(values('scale'), before) < 1e-8
    report['escape_restores_transform'] = True
    # An explicit graph selection and a Prim selection must resume the same edit.
    application.setSelection([source])
    invoke(plot, 'refreshScene')
    choose(targets)
    wait(ready, prepare_animation)


def prepare_animation():
    param = edit.getParam('translate')
    param.setValueAtTime(param.getValue(0), 1., 0)
    param.setValueAtTime(param.getValue(0) + .1, 10., 0)
    application.getViewer(viewer.getScriptName()).seek(5)
    plot.setProperty('gizmoMode', 0)
    wait(ready, animate_prim)


def animate_prim():
    param = edit.getParam('translate')
    count = param.getNumKeys(0)
    tenth = param.getValueAtTime(10., 0)
    axis_drag()
    fifth = param.getValueAtTime(5., 0)
    assert param.getNumKeys(0) == count + 1, 'Prim gizmo did not auto-key'
    application.getViewer(viewer.getScriptName()).seek(10)
    undo()
    assert param.getNumKeys(0) == count
    undo(True)
    assert param.getNumKeys(0) == count + 1 and abs(param.getValueAtTime(5., 0) - fifth) < 1e-8
    assert abs(param.getValueAtTime(10., 0) - tenth) < 1e-8
    report['autokey_undo_after_seek'] = True
    wait(ready, reject_material_transform)


def reject_material_transform():
    choose([material_path])
    wait(lambda: not outline.property('composing') and 'Type: Material' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText(), check_material_transform)


def check_material_transform():
    previous = renderer.getInput(0).getScriptName()
    invoke(outline, 'addTransformEdit')
    assert renderer.getInput(0).getScriptName() == previous, 'Material selection created an invalid transform'
    assert 'transformable' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    report['non_transformable_rejected'] = True
    choose(targets)
    wait(ready, save_reload)


def save_reload():
    assert values('pivot') == report['pivot']
    assert all(source.getParam('translate').getValue(i) == 0 for i in range(3)), 'Prim editing moved its reader'
    assert hashlib.sha256(fixture.read_bytes()).hexdigest() == report['source_hash']
    report['source_unchanged'] = True
    if parent_mode:
        origin = next(o['worldOrigin'] for o in plot.property('sceneObjects') if o['node'] == edit.getScriptName())
        invoke(plot, 'frameSelection')
        assert math.dist(plot.property('inspectionPivot'), origin) < 1e-5, (plot.property('inspectionPivot'), origin)
        report['parent_frame_alignment'] = True
    project = output / (Path(report['profile']).name + '.ntp')
    expected = {key: values(key) for key in ('pivot', 'translate', 'rotate', 'scale')}
    assert application.saveProjectAs(project.as_posix())
    copy = project.with_name(project.stem + '-reload.ntp')
    copy.write_bytes(project.read_bytes())
    loaded = app.loadProject(copy.as_posix())
    assert loaded
    restored = loaded.getNode(edit.getScriptName())
    assert restored and restored.getParam('primPaths').getValue() == authored_target
    for name, expected_values in expected.items():
        assert math.dist([restored.getParam(name).getValue(i) for i in range(3)], expected_values) < 1e-8, name
    loaded.closeProject()
    report.update(project=project.as_posix(), project_roundtrip=True)
    finish()


QTimer.singleShot(500, guarded(setup))
