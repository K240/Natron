"""Owned Natron GUI test: landmark clicks, scene setup, placement and Undo.

Requires the RGB-only project from camera-tracker-smoke.py.
"""
import json
import math
import os
import traceback
from pathlib import Path

from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QModelIndex, QPointF, QTimer, Qt
from PySide6.QtGui import QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QPlainTextEdit, QPushButton, QTreeWidget
import NatronGui

directory = Path.cwd() / 'build/camera-tracker-real'
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE']}
retained = {}
application = setup = renderer = plot = viewer = None
finished = False
outline_attempts = 0
selected = ['33', '49', '75']


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    report['dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['ok'] = error is None and not report['dialogs']
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
        report['buttons'] = [(w.text(), w.isVisible()) for w in widgets() if isinstance(w, QPushButton)]
        report['ancestors'] = []
        for w in widgets():
            if isinstance(w, QPushButton) and w.text() == 'Set ground from selection':
                while w:
                    report['ancestors'].append((w.metaObject().className(), w.objectName(), w.isHidden(), str(w.size())))
                    w = w.parentWidget()
        for w in widgets():
            if isinstance(w, QMainWindow) and w.isVisible():
                w.grab().save(str(directory / 'layout-gui-error.png'))
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2, default=str), encoding='utf-8')
    QApplication.exit(0)


def later(function, ms=300):
    def run():
        if finished:
            return
        try:
            (directory / 'layout-gui-progress.txt').write_text(function.__name__, encoding='utf-8')
            function()
        except Exception as error:
            finish(error)
    QTimer.singleShot(ms, run)


def invoke(name):
    assert QMetaObject.invokeMethod(plot, name, Qt.DirectConnection)


def refresh():
    invoke('refreshScene')
    assert not plot.property('renderError'), plot.property('renderError')


def button(text):
    return next(w for w in widgets() if isinstance(w, QPushButton) and w.isVisible() and w.text().replace('&', '') == text)


def click(text):
    w = button(text)
    w.click()
    return w


def undo_redo(w, redo=False):
    panel = w.parentWidget()
    while panel and not panel.metaObject().indexOfSlot('onUndoClicked()') >= 0:
        panel = panel.parentWidget()
    assert panel, 'Missing native settings Undo stack'
    assert QMetaObject.invokeMethod(panel, 'onRedoPressed' if redo else 'onUndoClicked', Qt.DirectConnection)


def selection():
    return setup.getParam('selectedLandmarks').getValue().split()


def landmark(identifier, shift=False):
    refresh()
    value = next(p for p in plot.property('guideLandmarks') if p['id'] == identifier)
    assert value['visible'], value
    p = value['pixel']
    global_p = QPointF(plot.mapToGlobal(p.toPoint()))
    modifiers = Qt.ShiftModifier if shift else Qt.NoModifier
    for kind, buttons in [(QEvent.MouseButtonPress, Qt.LeftButton), (QEvent.MouseButtonRelease, Qt.NoButton)]:
        QApplication.sendEvent(plot, QMouseEvent(kind, p, p, global_p, Qt.LeftButton, buttons, modifiers))


def start():
    global application, setup, renderer, viewer
    loaded = app.loadProject((directory / 'camera-tracker.ntp').as_posix())
    assert loaded
    application = NatronGui.PyGuiApplication().getGuiInstance(loaded.getAppID())
    setup = application.getNode('SceneSetup1')
    renderer = application.getNode('Render3D1')
    assert setup and renderer
    for name, x, y in [('TrackingPlate', 100, -150), ('PlateCameraTracker', 100, 0),
                       ('ReconstructionScene1', 100, 150), ('SceneSetup1', 100, 450),
                       ('SceneCamera1', 330, 450), ('Render3D1', 100, 620)]:
        application.getNode(name).setPosition(x, y)
    # Each owned GUI run saves a unique project to avoid another process's lock.
    assert application.saveProjectAs((directory / (Path(report['profile']).name + '.ntp')).as_posix())
    viewer = application.getNode('Viewer1') or application.createNode('fr.inria.built-in.Viewer')
    viewer.disconnectInput(0)
    assert viewer.connectInput(0, renderer)
    viewer.setPosition(100, 780)
    controller = application.getViewer(viewer.getScriptName())
    controller.setAInput(0)
    controller.setBInput(-1)
    controller.seek(1)
    controller.renderCurrentFrame(False)
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.hide()
            window.showNormal()
            window.resize(1650, 1120)
            window.raise_()
            window.activateWindow()
    later(open_points, 700)


def open_points():
    global plot
    next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(True)
    plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.isVisible())
    plot.setProperty('lookThroughCamera', True)
    next(w for w in widgets() if w.objectName() == 'Viewer3DPoints' and w.isVisible()).click()
    assert plot.property('pointSelectionMode')
    later(select_points, 400)


def select_points():
    landmark(selected[0])
    assert selection() == selected[:1], selection()
    later(align_points)


def align_points():
    # Invalid selection is reported without altering the frame or graph.
    click('Set ground from selection')
    assert 'at least three' in setup.getParam('layoutStatus').getValue(), setup.getParam('layoutStatus').getValue()
    assert not setup.getParam('sceneFrame').getValue()
    landmark(selected[1], True)
    landmark(selected[2], True)
    assert selection() == selected, selection()
    landmark(selected[2], True)
    assert selection() == selected[:2]
    landmark(selected[2], True)
    w = click('Set ground from selection')
    frame = setup.getParam('sceneFrame').getValue()
    assert json.loads(frame)['ground']
    undo_redo(w)
    assert not setup.getParam('sceneFrame').getValue(), 'Ground alignment did not undo'
    undo_redo(w, True)
    assert setup.getParam('sceneFrame').getValue() == frame
    landmark(selected[1])
    click('Set origin from selection')
    landmark(selected[0])
    landmark(selected[1], True)
    setup.getParam('knownDistance').setValue(.8)
    click('Set scale from two landmarks')
    refresh()
    plane = [p['position'] for p in plot.property('guideLandmarks') if p['id'] in selected]
    assert max(abs(p[1]) for p in plane) < 1e-6, plane
    assert abs(math.dist(plane[0], plane[1]) - .8) < 1e-6, plane
    report['landmark_selection'] = selected
    report['frame'] = json.loads(setup.getParam('sceneFrame').getValue())
    assert plot.grab().save(str(directory / 'layout-selected-points.png'))
    later(place_geometry)


def place_geometry():
    landmark(selected[1])
    before = setup.getInput(0).getScriptName()
    w = click('Create axis at selection')
    after = setup.getInput(0).getScriptName()
    assert before != after and application.getNode('Axis1')
    undo_redo(w)
    assert setup.getInput(0).getScriptName() == before and not application.getNode('Axis1')
    undo_redo(w, True)
    assert setup.getInput(0).getScriptName() == after and application.getNode('Axis1')
    click('Create card at selection')
    application.getNode('Card1').getParam('opacity').setValue(0.)
    setup.getParam('placementSize').setValue(1.65)
    w = click('Create projected ground')
    projection = application.getNode('Project3D1')
    assert projection and application.getNode('Card2')
    camera_input = projection.getInput(1).getInput(0).getScriptName()
    assert camera_input != setup.getScriptName()
    after = setup.getInput(0).getScriptName()
    undo_redo(w)
    assert not application.getNode('Project3D1') and not application.getNode('Card2')
    undo_redo(w, True)
    assert setup.getInput(0).getScriptName() == after
    projection = application.getNode('Project3D1')
    assert projection.getInput(1) and application.getNode('Card2').getInput(0)
    setup.getParam('assetFile').setValue((Path.cwd() / 'Tests/fixtures/scene3d/usd/layout-asset.usda').as_posix())
    setup.getParam('assetScale').setValue(.16)
    before = setup.getInput(0).getScriptName()
    w = click('Place USD asset on ground')
    assert application.getNode('ReadUSD1'), setup.getParam('layoutStatus').getValue()
    after = setup.getInput(0).getScriptName()
    undo_redo(w)
    assert setup.getInput(0).getScriptName() == before and not application.getNode('ReadUSD1')
    undo_redo(w, True)
    assert setup.getInput(0).getScriptName() == after and application.getNode('ReadUSD1')
    # Add the native environment light to the same upstream scene frame.
    env = application.createNode('fr.inria.built-in.Light')
    env.getParam('lightType').setValue('Environment')
    env.getParam('intensity').setValue(.65)
    merge = setup.getInput(0)
    assert merge.connectInput(2, env)
    report['frame_and_placement_undo_redo'] = True
    report['projection_without_feedback'] = True
    # Keyboard mode switching keeps the toolbar in sync.
    QApplication.sendEvent(plot, QKeyEvent(QEvent.KeyPress, Qt.Key_P, Qt.NoModifier))
    assert not plot.property('pointSelectionMode')
    assert not next(w for w in widgets() if w.objectName() == 'Viewer3DPoints' and w.isVisible()).isChecked()
    plot.setProperty('lookThroughCamera', False)
    application.setSelection([application.getNode('ReadUSD1')])
    invoke('frameSelection')
    later(edit_asset, 600)


def edit_asset():
    refresh()
    handles = plot.property('gizmoHandles')
    assert len(handles) == 3, 'Placed USD asset did not expose its transform gizmo'
    asset = application.getNode('ReadUSD1')
    before = [asset.getParam('translate').getValue(i) for i in range(3)]
    before_points = [p['position'] for p in plot.property('guideLandmarks')]
    center, start = plot.property('gizmoCenter'), handles[0]
    delta = start - center
    length = math.hypot(delta.x(), delta.y())
    assert length > 2
    end = start + delta * (25 / length)
    for kind, pos, button_value, buttons in [(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton),
                                            (QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton),
                                            (QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)]:
        global_pos = QPointF(plot.mapToGlobal(pos.toPoint()))
        QApplication.sendEvent(plot, QMouseEvent(kind, pos, pos, global_pos, button_value, buttons, Qt.NoModifier))
    after = [asset.getParam('translate').getValue(i) for i in range(3)]
    assert math.dist(before, after) > 1e-5, (before, after)
    refresh()
    assert before_points == [p['position'] for p in plot.property('guideLandmarks')], 'Asset gizmo moved the reconstruction'
    name = next(w for w in widgets() if isinstance(w, QLineEdit) and w.text() == 'ReadUSD1')
    undo_redo(name)
    assert math.dist(before, [asset.getParam('translate').getValue(i) for i in range(3)]) < 1e-8
    undo_redo(name, True)
    assert math.dist(after, [asset.getParam('translate').getValue(i) for i in range(3)]) < 1e-8
    undo_redo(name)
    report['asset_gizmo_with_shared_frame'] = True
    application.setSelection([])
    invoke('frameSelection')
    later(inspect, 500)


def inspect():
    refresh()
    assert len(plot.property('guideLandmarks')) >= 100
    assert plot.property('capturedFrame') and not plot.property('capturedFrame').isNull()
    report['objects'] = plot.property('sceneObjects')
    assert any(o['kind'] == 'ReadUSD' for o in report['objects'])
    assert any(o['kind'] == 'Axis' for o in report['objects'])
    # Reveal SceneSetup for the review screenshot, then retain the free view.
    plot.setProperty('pointSelectionMode', True)
    landmark(selected[1])
    plot.setProperty('pointSelectionMode', False)
    w = next(w for w in widgets() if isinstance(w, QPushButton) and w.text() == 'Set ground from selection')
    panel = w.parentWidget()
    while panel and panel.metaObject().indexOfSlot('setClosed(bool)') < 0:
        panel = panel.parentWidget()
    assert panel
    QMetaObject.invokeMethod(panel, 'setClosed', Qt.DirectConnection, Q_ARG(bool, True))
    QMetaObject.invokeMethod(panel, 'setClosed', Qt.DirectConnection, Q_ARG(bool, False))
    application.setSelection([])
    assert plot.grab().save(str(directory / 'layout-3d-view.png'))
    for graph in widgets():
        if graph.metaObject().className() == 'Natron::NodeGraph' and graph.isVisible():
            QMetaObject.invokeMethod(graph, 'centerOnAllNodes', Qt.DirectConnection)
    for window in widgets():
        if isinstance(window, QMainWindow) and window.isVisible():
            window.grab().save(str(directory / 'layout-controls.png'))
    project = directory / (Path(report['profile']).name + '.ntp')
    assert application.saveProjectAs(project.as_posix())
    report['project'] = project.as_posix()
    next(w for w in widgets() if w.objectName() == 'Viewer3DOutline' and w.isVisible()).click()
    later(inspect_outline, 400)


def inspect_outline():
    global outline_attempts
    outline_attempts += 1
    outline = next(w for w in widgets() if w.objectName() == 'StageOutline' and w.isVisible())
    outline.setProperty('pinnedSource', renderer.getParam('usdIdentity').getValue())
    tree = outline.findChild(QTreeWidget, 'StageOutlineTree')
    tree.expandAll()

    def paths(parent=QModelIndex()):
        result = []
        for row in range(tree.model().rowCount(parent)):
            index = tree.model().index(row, 0, parent)
            result.append(index.data(Qt.UserRole))
            result.extend(paths(index))
        return result

    values = [p for p in paths() if isinstance(p, str)]
    assert outline_attempts < 25, (outline.property('stageStatus'), values)
    if not any(p.endswith('/Source/Box') for p in values) or not any(p.endswith('/Landmarks') for p in values):
        later(inspect_outline, 200)
        return
    box = '/Natron/Assets/n_' + application.getNode('ReadUSD1').getParam('usdIdentity').getValue() + '/Source/Box'
    assert box in values and any(p.endswith('/Source/Camera') for p in values)
    assert QMetaObject.invokeMethod(outline, 'selectPrimPaths', Qt.DirectConnection, Q_ARG('QStringList', [box]))
    report['usd_outline_paths'] = values
    later(check_outline, 400)


def check_outline():
    outline = next(w for w in widgets() if w.objectName() == 'StageOutline' and w.isVisible())
    details = outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    assert 'Type: Mesh' in details and 'points: 8 elements' in details and '/Source/Material' in details, details
    report['usd_asset_material_composed'] = True
    assert application.saveProjectAs(report['project'])
    finish()


later(start, 600)
