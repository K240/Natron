"""Exercise the dockable composed-stage panel, 2D inspection and selection."""
import json
import time
import traceback
import os
import hashlib
from pathlib import Path
from PySide6.QtCore import QMetaObject, QTimer, Qt, QModelIndex, QItemSelectionModel, Q_ARG, Q_RETURN_ARG, QPointF, QEvent
from PySide6.QtGui import QAction, QMouseEvent
from PySide6.QtWidgets import QApplication, QMainWindow, QTreeWidget, QPlainTextEdit, QLineEdit, QComboBox, QPushButton
import NatronGui
import NatronEngine

user_scene = os.environ.get('NATRON_STAGE_OUTLINE_SCENE')
directory = Path.cwd() / ('build/scene3d-stage-outline-user' if user_scene else 'build/scene3d-stage-outline')
directory.mkdir(parents=True, exist_ok=True)
report = {'ok': False}
retained = {}
application = None
plot = None
outline = None
tree = None
heartbeat = 0
main_button = None
main_toggle = None


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values


def finish(error=None):
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
    report['ok'] = error is None
    report['heartbeat'] = heartbeat
    (directory / 'gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    if application:
        application.saveProjectAs((directory / 'outline.ntp').as_posix())
        application.closeProject()


def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def wait(predicate, function, seconds=60):
    deadline = time.monotonic() + seconds
    def poll():
        assert time.monotonic() < deadline, ('Timed out', outline.property('stageStatus') if outline else '', plot.property('renderError') if plot else '')
        if predicate():
            function()
        else:
            QTimer.singleShot(100, guarded(poll))
    QTimer.singleShot(100, guarded(poll))


def find(path):
    model = tree.model()
    def search(parent):
        for row in range(model.rowCount(parent)):
            index = model.index(row, 0, parent)
            if index.data(Qt.UserRole) == path:
                return index
            if result := search(index):
                return result
        return None
    return search(QModelIndex())


def choose(path):
    tree.selectionModel().select(find(path), QItemSelectionModel.ClearAndSelect | QItemSelectionModel.Rows)


def node(kind, name):
    effect = application.createNode('fr.inria.built-in.' + kind)
    assert effect
    effect.setScriptName(name)
    return effect


def setup():
    global application, plot, outline, tree
    application = NatronGui.PyGuiApplication().getGuiInstance(app.getAppID())
    NatronEngine.natron.getSettings().getParam('loadProjectWorkspace').setValue(True)
    source = node('ReadUSD', 'OutlineStage')
    source.getParam('file').setValue(Path(user_scene).as_posix() if user_scene else (Path.cwd() / 'Tests/fixtures/scene3d/usd/scene.usda').as_posix())
    card = node('Card', 'OutlineCard')
    card.getParam('translate').set(0., 1.5, 0.)
    scene = node('Scene', 'OutlineScene')
    assert scene.connectInput(0, source) and scene.connectInput(1, card)
    camera = node('ReadUSDCamera' if user_scene else 'Camera', 'OutlineCamera')
    if user_scene:
        camera.getParam('file').setValue(Path(user_scene).as_posix())
        report['source_hash_before'] = hashlib.sha256(Path(user_scene).read_bytes()).hexdigest()
    renderer = node('Render3D', 'OutlineRender')
    renderer.getParam('renderer').setValue('hdNVP')
    renderer.getParam('pathDenoiser').setValue('None')
    renderer.getParam('pathSamples').setValue(1)
    renderer.getParam('pathBounces').setValue(1)
    assert renderer.connectInput(0, scene) and renderer.connectInput(1, camera)
    viewer = application.getNode('Viewer1') or node('Viewer', 'OutlineViewer')
    assert viewer.connectInput(0, renderer)
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.showNormal()
            window.resize(1500, 1000)
    def start():
        global plot, outline, tree, main_button, main_toggle
        main_toggle = next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible())
        main_button = next(w for w in widgets() if w.objectName() == 'Viewer3DOutline' and w.isVisible())
        main_button.click()
        outline = next(w for w in widgets() if w.objectName() == 'StageOutline' and w.isVisible())
        plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.window() == outline.window())
        tree = outline.findChild(QTreeWidget, 'StageOutlineTree')
        assert not main_toggle.isChecked() and not plot.isVisible()
        assert not plot.property('rendering')
        report.update(outline_available_in_2d=True, preview_idle_in_2d=True)
        if user_scene:
            wait(lambda: tree.model().rowCount(QModelIndex()) > 1, select_user_geometry)
        else:
            wait(lambda: find('/World') is not None, expand_world)
    QTimer.singleShot(800, guarded(start))


def expand_world():
    report['outline_available_during_render'] = bool(plot.property('rendering'))
    tree.setExpanded(find('/World'), True)
    if os.environ.get('NATRON_STAGE_INTERACTION_TEST'):
        wait(lambda: find('/World/Camera') is not None, interaction_camera)
        return
    wait(lambda: find('/World/Animated') is not None, expand_asset)


def undo_action(name):
    actions = []
    for widget in widgets():
        actions += widget.actions() + widget.findChildren(QAction)
    next(a for a in actions if a.text().replace('&', '').startswith(name) and a.isEnabled()).trigger()


def interaction_camera():
    choose('/World/Camera')
    wait(lambda: outline.findChild(QPushButton, 'StageOutlineLookThrough').isEnabled(), interaction_look)


def interaction_look():
    renderer = application.getNode('OutlineRender')
    outline.findChild(QPushButton, 'StageOutlineLookThrough').click()
    assert plot.property('lookThroughPrimPath') == '/World/Camera'
    assert renderer.getParam('usdCamera').getValue() == ''
    wait(lambda: not plot.property('rendering') and not plot.property('renderError'), interaction_render_camera)


def interaction_render_camera():
    renderer = application.getNode('OutlineRender')
    outline.findChild(QPushButton, 'StageOutlineRenderCamera').click()
    assert renderer.getParam('usdCamera').getValue() == '/World/Camera'
    undo_action('Undo')
    assert renderer.getParam('usdCamera').getValue() == ''
    undo_action('Redo')
    assert renderer.getParam('usdCamera').getValue() == '/World/Camera'
    report['independent_cameras_and_undo'] = True
    wait(lambda: not plot.property('rendering') and not plot.property('renderError'), interaction_pick)


def interaction_pick():
    # Search actual GPU IDs rather than assuming window dimensions or framing.
    hit = None
    paths = {}
    for y in range(20, plot.height()-20, 15):
        for x in range(20, plot.width()-20, 15):
            point = QPointF(x, y)
            path = QMetaObject.invokeMethod(plot, 'primAt', Qt.DirectConnection, Q_RETURN_ARG(str), Q_ARG(QPointF, point))
            key = path or '(background)'
            paths[key] = paths.get(key, 0) + 1
            if path == '/World/Instance/Panel':
                hit = point
                break
        if hit is not None:
            break
    report['gpu_pick_paths'] = paths
    report['gpu_pick_report'] = json.loads(plot.property('hdNvpReport'))
    assert hit is not None, 'No instance proxy resolved by the GPU pick buffer'
    global_point = QPointF(plot.mapToGlobal(hit.toPoint()))
    for kind, buttons in [(QEvent.MouseButtonPress, Qt.LeftButton), (QEvent.MouseButtonRelease, Qt.NoButton)]:
        QApplication.sendEvent(plot, QMouseEvent(kind, hit, hit, global_point, Qt.LeftButton, buttons, Qt.NoModifier))
    wait(lambda: outline.property('selectedPrimPaths') == ['/World/Instance/Panel'], interaction_edit)


def interaction_edit():
    report['gpu_instance_picking_and_outline'] = True
    main_toggle.setChecked(False)
    wait(lambda: not plot.property('rendering') and not outline.property('composing'), interaction_insert)


def interaction_insert():
    renderer = application.getNode('OutlineRender')
    previous = renderer.getInput(0).getScriptName()
    assert QMetaObject.invokeMethod(outline, 'addTransformEdit', Qt.DirectConnection)
    edit = renderer.getInput(0)
    assert edit.getParam('primPaths').getValue() == '/World/Instance/Panel'
    retained['transform_node'] = edit
    undo_action('Undo')
    assert renderer.getInput(0).getScriptName() == previous
    undo_action('Redo')
    assert renderer.getInput(0).getScriptName() == edit.getScriptName()
    edit.getParam('translate').set(0., 0., 1.)
    report['edit_node_insertion_undo_redo'] = True
    wait(lambda: not outline.property('composing') and 'natron_' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText(), interaction_save)


def interaction_save():
    assert outline.grab().save(str(directory / 'interaction-outline.png'))
    saved = directory / 'interaction.ntp'
    assert application.saveProjectAs(saved.as_posix())
    loaded = app.loadProject(saved.as_posix())
    # Copy to a distinct filename so this exercises project deserialization.
    reload_file = directory / 'interaction-reload.ntp'
    reload_file.write_bytes(saved.read_bytes())
    loaded = app.loadProject(reload_file.as_posix())
    assert loaded
    edit = loaded.getNode(retained['transform_node'].getScriptName())
    assert edit and edit.getParam('primPaths').getValue() == '/World/Instance/Panel'
    assert edit.getParam('translate').getValue(2) == 1.
    assert loaded.getNode('OutlineRender').getParam('usdCamera').getValue() == '/World/Camera'
    loaded.closeProject()
    report['usd_edit_project_roundtrip'] = True
    finish()


def expand_asset():
    tree.setExpanded(find('/World/Animated'), True)
    wait(lambda: find('/World/Animated/Panel') is not None, select_mesh)


def select_mesh():
    tree.clearSelection()
    choose('/World/Animated/Panel')
    wait(lambda: plot.property('selectedPrimPaths') == ['/World/Animated/Panel'], check_selection)


def check_selection():
    details = outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    assert 'Material: /World/Animated/Surface' in details, details
    assert 'points: 4 elements' in details
    assert outline.property('selectedPrimPaths') == ['/World/Animated/Panel']
    report['prim_selection'] = True
    outline.findChild(QPushButton, 'StageOutlineFrame').click()
    assert main_toggle.isChecked() and plot.isVisible()
    assert abs(plot.property('inspectionPivot')[0] + 1) < .01, plot.property('inspectionPivot')
    report['frame_selection'] = True
    wait(lambda: not plot.property('rendering'), after_frame)


def after_frame():
    assert not plot.property('renderError'), plot.property('renderError')
    assert plot.property('selectedPrimPaths') == ['/World/Animated/Panel']
    tree.setExpanded(find('/World/Instance'), True)
    wait(lambda: find('/World/Instance/Panel') is not None, inspect_proxy)


def inspect_proxy():
    assert 'proxy' in find('/World/Instance/Panel').siblingAtColumn(2).data()
    choose('/World/Animated/Panel')
    tree.selectionModel().select(find('/World/Instance/Panel'), QItemSelectionModel.Select | QItemSelectionModel.Rows)
    paths = ['/World/Animated/Panel', '/World/Instance/Panel']
    wait(lambda: plot.property('selectedPrimPaths') == paths, check_multiselection)


def check_multiselection():
    details = outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    assert '/World/Animated/Panel' in details and '/World/Instance/Panel' in details
    query = outline.findChild(QLineEdit, 'StageOutlineFilter')
    query.setText('Panel')
    surface = find('/World/Animated/Surface')
    assert surface is not None and tree.isRowHidden(surface.row(), surface.parent())
    panel = find('/World/Animated/Panel')
    assert not tree.isRowHidden(panel.row(), panel.parent())
    query.clear()
    report.update(multiselection=True, filtering=True)
    card = application.getNode('OutlineCard')
    identity = card.getParam('usdIdentity').getValue()
    path = '/Natron/Geometry/n_' + identity
    wait(lambda: find(path) is not None, lambda: select_card(path))


def select_card(path):
    tree.clearSelection()
    choose(path)
    wait(lambda: plot.property('selectedPrimPaths') == [path], lambda: check_card(path))


def check_card(path):
    assert plot.property('selectedNode') == 'OutlineCard'
    assert len(plot.property('gizmoHandles')) == 3
    card = application.getNode('OutlineCard')
    card.setScriptName('RenamedOutlineCard')
    card.setLabel('Renamed card')
    wait(lambda: find(path) is not None and find(path).data() == 'Renamed card' and not plot.property('rendering') and plot.property('selectedPrimPaths') == [path], lambda: finish_checks(path))


def finish_checks(path):
    assert not plot.property('renderError'), plot.property('renderError')
    assert plot.property('selectedPrimPaths') == [path]
    report.update(proxy_inspection=True, native_selection=True, stable_after_rename=True)
    main_toggle.setChecked(False)
    assert outline.isVisible() and not plot.isVisible()
    card = application.getNode('RenamedOutlineCard')
    card.getParam('translate').set(3.5, 1.5, 0.)
    application.getViewer('Viewer1' if application.getNode('Viewer1') else 'OutlineViewer').seek(2)
    wait(lambda: 'frame 2' in outline.property('stageStatus') and outline.property('selectedPrimPaths') == [path]
         and '3.5' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
         and not plot.property('rendering'), lambda: independent_checks(path))


def pane_of(widget):
    while widget and widget.metaObject().className() != 'Natron::TabWidget':
        widget = widget.parentWidget()
    assert widget is not None
    return widget


def independent_checks(path):
    assert not plot.isVisible() and not plot.property('rendering')
    report['updated_in_2d'] = True
    other_card = node('Card', 'OtherOutlineCard')
    other_camera = node('Camera', 'OtherOutlineCamera')
    other_render = node('Render3D', 'OtherOutlineRender')
    other_render.getParam('renderer').setValue('hdNVP')
    other_render.getParam('pathSamples').setValue(1)
    assert other_render.connectInput(0, other_card) and other_render.connectInput(1, other_camera)
    source = outline.findChild(QComboBox, 'StageOutlineSource')
    identity = application.getNode('OutlineRender').getParam('usdIdentity').getValue()
    other_identity = other_render.getParam('usdIdentity').getValue()
    wait(lambda: source.findData(other_identity) >= 0, lambda: standalone_source(path, identity, other_identity))


def standalone_source(path, identity, other_identity):
    source = outline.findChild(QComboBox, 'StageOutlineSource')
    source.setCurrentIndex(source.findData(other_identity))
    other_path = '/Natron/Geometry/n_' + application.getNode('OtherOutlineCard').getParam('usdIdentity').getValue()
    def check():
        choose(other_path)
        def selected():
            assert not outline.findChild(QPushButton, 'StageOutlineFrame').isEnabled()
            report['inspection_without_viewer'] = True
            pin_source(path, identity)
        wait(lambda: 'points: 4 elements' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText(), selected)
    wait(lambda: outline.property('sourceNode') == 'OtherOutlineRender' and find(other_path) is not None, check)


def pin_source(path, identity):
    source = outline.findChild(QComboBox, 'StageOutlineSource')
    source.setCurrentIndex(source.findData(identity))
    other_viewer = node('Viewer', 'OtherOutlineViewer')
    assert other_viewer.connectInput(0, application.getNode('OtherOutlineRender'))
    def activate():
        buttons = [w for w in widgets() if w.objectName() == 'Viewer3DOutline' and w != main_button and w.isVisible()]
        assert len(buttons) == 1, buttons
        buttons[0].click()
        wait(lambda: not outline.property('composing') and outline.property('sourceNode') == 'OutlineRender'
             and find('/World') is not None, lambda: follow_source(path, identity))
    QTimer.singleShot(800, guarded(activate))


def follow_source(path, identity):
    assert outline.property('pinnedSource') == identity
    assert find('/World') is not None
    report['pinned_across_viewers'] = True
    outline.findChild(QComboBox, 'StageOutlineSource').setCurrentIndex(0)
    other_path = '/Natron/Geometry/n_' + application.getNode('OtherOutlineCard').getParam('usdIdentity').getValue()
    wait(lambda: outline.property('sourceNode') == 'OtherOutlineRender' and find(other_path) is not None,
         lambda: follow_input(path, identity))


def follow_input(path, identity):
    other = application.getNode('OtherOutlineViewer')
    assert other.connectInput(1, application.getNode('OutlineRender'))
    py_viewer = application.getViewer('OtherOutlineViewer')
    def switch():
        py_viewer.setAInput(1)
        wait(lambda: outline.property('sourceNode') == 'OutlineRender' and find(path) is not None, back)
    def back():
        py_viewer.setAInput(0)
        wait(lambda: outline.property('sourceNode') == 'OtherOutlineRender' and not outline.property('composing'), complete)
    def complete():
        report['follow_active_input'] = True
        return_source(path, identity)
    wait(lambda: other.getInput(1) is not None and other.getInput(1).getScriptName() == 'OutlineRender', switch)


def return_source(path, identity):
    assert not outline.property('selectedPrimPaths'), 'Selection leaked between stage branches'
    report['follow_active_viewer'] = True
    source = outline.findChild(QComboBox, 'StageOutlineSource')
    source.setCurrentIndex(source.findData(identity))
    wait(lambda: outline.property('sourceNode') == 'OutlineRender' and find(path) is not None,
         lambda: dock_checks(path))


def dock_checks(path):
    choose(path)
    wait(lambda: outline.property('selectedPrimPaths') == [path]
         and 'points: 4 elements' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText(), lambda: frame_pinned_source(path))


def frame_pinned_source(path):
    outline.findChild(QPushButton, 'StageOutlineFrame').click()
    assert main_toggle.isChecked() and plot.isVisible(), 'Frame selection did not activate the matching Viewer'
    assert abs(plot.property('inspectionPivot')[0] - 3.5) < .01, plot.property('inspectionPivot')
    main_toggle.setChecked(False)
    report['frame_pinned_source'] = True
    wait(lambda: not plot.property('rendering'), save_docked_layout)


def save_docked_layout():
    original_pane = pane_of(outline)
    # Sharing a pane with the Viewer makes the Outline invisible while framing.
    viewer_pane = application.getTabWidget(pane_of(main_toggle).objectName())
    assert application.moveTab('stageOutline', viewer_pane)
    outline.findChild(QPushButton, 'StageOutlineFrame').click()
    assert not outline.isVisible() and plot.isVisible()
    assert abs(plot.property('inspectionPivot')[0] - 3.5) < .01
    main_button.click()
    assert outline.isVisible()
    assert application.moveTab('stageOutline', application.getTabWidget(original_pane.objectName()))
    main_toggle.setChecked(False)
    report['frame_shared_pane'] = True
    wait(lambda: not plot.property('rendering'), save_panel_layout)


def save_panel_layout():
    original_pane = pane_of(outline)
    original_name = original_pane.objectName()
    pane = application.getTabWidget(original_name).splitHorizontally()
    assert application.moveTab('stageOutline', pane)
    assert pane.currentWidget().objectName() == 'StageOutline'
    report['docking'] = True
    pane.floatCurrentTab()
    assert not isinstance(outline.window(), QMainWindow)
    report['floating'] = True
    pane.closeCurrentTab()
    assert not outline.isVisible()
    camera = application.getNode('OutlineCamera')
    application.selectNode(camera, True)
    camera.getParam('translate').set(.25, 0., 0.)
    main_toggle.setChecked(True)
    wait(lambda: plot.property('selectedNode') == 'OutlineCamera' and not plot.property('rendering'),
         lambda: reopen_and_save(original_pane, original_name))


def reopen_and_save(original_pane, original_name):
    assert not plot.property('selectedPrimPaths'), 'Closed Outline restored an old prim selection'
    report['closed_panel_does_not_override_selection'] = True
    main_toggle.setChecked(False)
    # A closed singleton can be reopened; no new Outline instance is created.
    main_button.click()
    assert outline.isVisible()
    assert sum(w.objectName() == 'StageOutline' for w in widgets()) == 1
    assert QMetaObject.invokeMethod(original_pane, 'moveStageOutlineHere', Qt.DirectConnection)
    assert pane_of(outline) == original_pane
    report['close_reopen'] = True
    # Save a floating layout and its pinned UUID to verify actual restoration.
    saved_pane = application.getTabWidget(original_name)
    saved_pane.floatCurrentTab()
    assert not isinstance(outline.window(), QMainWindow)
    assert outline.grab().save(str(directory / 'outline.png'))
    saved = directory / 'outline.ntp'
    identity = application.getNode('RenamedOutlineCard').getParam('usdIdentity').getValue()
    assert application.saveProjectAs(saved.as_posix())
    # Natron focuses a project which is already open. Use a second filename
    # so this exercises deserialization in a fresh window.
    reload_file = directory / 'outline-reload.ntp'
    reload_file.write_bytes(saved.read_bytes())
    loaded = app.loadProject(reload_file.as_posix())
    assert loaded
    assert loaded.getNode('RenamedOutlineCard').getParam('usdIdentity').getValue() == identity
    report['identity_after_reload'] = True
    loaded_outline = next(w for w in widgets() if w.objectName() == 'StageOutline' and w != outline)
    assert loaded_outline.isVisible()
    assert not isinstance(loaded_outline.window(), QMainWindow)
    assert loaded_outline.property('pinnedSource') == outline.property('pinnedSource')
    report.update(layout_after_reload=True, source_after_reload=True)
    def complete():
        assert loaded_outline.property('sourceNode') == 'OutlineRender'
        loaded.closeProject()
        QTimer.singleShot(0, guarded(finish))
    wait(lambda: not loaded_outline.property('composing') and 'frame 2' in loaded_outline.property('stageStatus'), complete)


def select_user_geometry():
    roots = []
    for row in range(tree.model().rowCount(QModelIndex())):
        index = tree.model().index(row, 0, QModelIndex())
        path = index.data(Qt.UserRole)
        if path and path != '/Natron' and index.siblingAtColumn(1).data() in ('Xform', 'Mesh'):
            roots.append(path)
    assert roots, 'No imported geometry root'
    path = roots[0]
    report['geometry_root'] = path
    report['outline_available_during_render'] = bool(plot.property('rendering'))
    tree.setExpanded(find(path), True)
    choose(path)
    wait(lambda: plot.property('selectedPrimPaths') == [path], lambda: frame_user_geometry(path), 90)


def frame_user_geometry(path):
    outline.findChild(QPushButton, 'StageOutlineFrame').click()
    wait(lambda: not plot.property('rendering'), lambda: finish_user_scene(path), 90)


def finish_user_scene(path):
    assert not plot.property('renderError'), plot.property('renderError')
    assert plot.property('selectedPrimPaths') == [path]
    report['source_hash_after'] = hashlib.sha256(Path(user_scene).read_bytes()).hexdigest()
    assert report['source_hash_after'] == report['source_hash_before']
    assert plot.parentWidget().grab().save(str(directory / 'outline.png'))
    report.update(prim_selection=True, frame_selection=True, source_unchanged=True)
    finish()


def tick():
    global heartbeat
    heartbeat += 1


timer = QTimer()
timer.timeout.connect(tick)
timer.start(50)
QTimer.singleShot(500, guarded(setup))
