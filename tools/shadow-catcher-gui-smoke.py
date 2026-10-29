"""Owned GUI coverage for shadow/reflection ground creation and plate occluder editing."""
import json
import math
import os
import traceback
from pathlib import Path
from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QModelIndex, QPointF, QTimer, Qt
from PySide6.QtGui import QAction, QImage, QMouseEvent
from PySide6.QtWidgets import QApplication, QLineEdit, QMainWindow, QPlainTextEdit, QPushButton, QTreeWidget
import NatronGui

directory = Path.cwd() / 'build/camera-tracker-real'
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE']}
retained = {}
application = setup = render = viewer = plot = proxy_card = catcher = None
finished = False
outline_attempts = 0
role_attempts = 0
material_attempts = 0
occluder_previous = None


def widgets():
    result = QApplication.allWidgets()
    retained.update((id(w), w) for w in result)
    return result


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    report['dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['ok'] = error is None and not report['dialogs']
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
        if plot:
            plot.grab().save(str(directory / 'catcher-gui-error.png'))
        if application and viewer:
            application.getViewer(viewer.getScriptName()).pause()
            viewer.disconnectInput(0)
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2, default=str), encoding='utf-8')
    QTimer.singleShot(150, lambda: QApplication.exit(0))


def later(function, ms=350):
    def run():
        if finished:
            return
        try:
            (directory / 'catcher-gui-progress.txt').write_text(function.__name__, encoding='utf-8')
            function()
        except Exception as error:
            finish(error)
    QTimer.singleShot(ms, run)


def refresh():
    assert QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
    assert not plot.property('renderError'), plot.property('renderError')


def button(text):
    return next(w for w in widgets() if isinstance(w, QPushButton) and w.isVisible() and w.text().replace('&', '') == text)


def undo(widget, redo=False):
    panel = widget.parentWidget()
    while panel and panel.metaObject().indexOfSlot('onUndoClicked()') < 0:
        panel = panel.parentWidget()
    assert panel
    assert QMetaObject.invokeMethod(panel, 'onRedoPressed' if redo else 'onUndoClicked', Qt.DirectConnection)


def mouse(kind, point, button_value, buttons):
    global_point = QPointF(plot.mapToGlobal(point.toPoint()))
    QApplication.sendEvent(plot, QMouseEvent(kind, point, point, global_point, button_value, buttons, Qt.NoModifier))


def bytes_image(image):
    rgba = image.convertToFormat(QImage.Format_RGBA8888)
    return bytes(rgba.constBits())


def mask_max():
    return max(bytes_image(plot.property('capturedFrame'))[0::4])


def start():
    global application, setup, render, viewer
    loaded = app.loadProject((directory / 'reconstruction-layout.ntp').as_posix())
    assert loaded
    application = NatronGui.PyGuiApplication().getGuiInstance(loaded.getAppID())
    setup, render = application.getNode('SceneSetup1'), application.getNode('Render3D1')
    assert application.saveProjectAs((directory / (Path(report['profile']).name + '.ntp')).as_posix())
    render.getParam('renderer').setValue('Vulkan')
    for node in application.getChildren():
        if node.getPluginID() == 'fr.inria.built-in.Light' and node.getParam('lightType').getValue() == 3:
            node.getParam('shadows').setValue(False)
        if node.getPluginID() == 'fr.inria.built-in.Card' and node.getInput(0) and node.getInput(0).getPluginID() == 'fr.inria.built-in.Project3D':
            node.getParam('opacity').setValue(0.)
    viewer = application.getNode('Viewer1') or application.createNode('fr.inria.built-in.Viewer')
    viewer.disconnectInput(0)
    assert viewer.connectInput(0, render)
    controller = application.getViewer(viewer.getScriptName())
    controller.setAInput(0)
    controller.setBInput(-1)
    controller.seek(1)
    controller.renderCurrentFrame(False)
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.hide()
            window.showNormal()
            window.resize(1650, 1100)
            window.raise_()
            window.activateWindow()
    later(open_scene, 600)


def open_scene():
    global plot
    next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(True)
    plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.isVisible())
    plot.setProperty('lookThroughCamera', True)
    plot.setProperty('pointSelectionMode', True)
    refresh()
    point = next(p for p in plot.property('guideLandmarks') if p['id'] == '49')
    assert point['visible']
    mouse(QEvent.MouseButtonPress, point['pixel'], Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, point['pixel'], Qt.LeftButton, Qt.NoButton)
    plot.setProperty('pointSelectionMode', False)
    later(create_ground)


def create_ground():
    global catcher
    setup.getParam('placementSize').setValue(1.65)
    before = setup.getInput(0).getScriptName()
    w = button('Create shadow catcher ground')
    w.click()
    catcher = application.getNode('ShadowCatcher1')
    assert catcher, setup.getParam('layoutStatus').getValue()
    after = setup.getInput(0).getScriptName()
    undo(w)
    assert not application.getNode('ShadowCatcher1') and setup.getInput(0).getScriptName() == before
    undo(w, True)
    catcher = application.getNode('ShadowCatcher1')
    assert catcher and setup.getInput(0).getScriptName() == after
    button('Create key light').click()
    render.getParam('output').setValue('ShadowPreview')
    refresh()
    assert mask_max() > 30
    report['shadow_mask_max'] = mask_max()
    catcher.getParam('shadowDensity').setValue(0.)
    refresh()
    assert mask_max() == 0, 'Density zero left visible shadows'
    catcher.getParam('shadowDensity').setValue(.8)
    catcher.getParam('shadowTint').set(.02, .01, 0., 1.)
    render.getParam('output').setValue('Color')
    setup.getParam('placementSize').setValue(.1)
    w = button('Create occluder card')
    w.click()
    proxy = application.getNode('Occluder1')
    assert proxy
    undo(w)
    assert not application.getNode('Occluder1')
    undo(w, True)
    assert application.getNode('Occluder1')
    report['creation_undo_redo'] = True
    later(check_reflections)


def check_reflections():
    # Dedicated ground creation remains one graph Undo operation.
    before = setup.getInput(0).getScriptName()
    control = button('Create reflection catcher ground')
    control.click()
    reflected = application.getNode('ReflectionCatcher1')
    assert reflected and reflected.getPluginID() == 'fr.inria.built-in.ReflectionCatcher'
    assert reflected.getParam('reflectionStrength').getIsVisible()
    assert reflected.getParam('reflectionRoughness').getIsVisible()
    assert not reflected.getParam('shadowDensity').getIsVisible()
    reflected.getParam('catchShadows').setValue(True)
    assert reflected.getParam('shadowDensity').getIsVisible()
    undo(control)
    assert not application.getNode('ReflectionCatcher1') and setup.getInput(0).getScriptName() == before
    undo(control, True)
    assert application.getNode('ReflectionCatcher1')
    undo(control)
    # Add reflections on the existing shadow ground instead of a coplanar card.
    catcher.getParam('catchReflections').setValue(True)
    assert catcher.getParam('reflectionStrength').getIsVisible()
    catcher.getParam('reflectionRoughness').setValue(.2)
    catcher.getParam('reflectionStrength').setValue(0.)
    render.getParam('output').setValue('ReflectionPreview')
    refresh()
    off = plot.property('capturedFrame')
    catcher.getParam('reflectionStrength').setValue(.7)
    refresh()
    on = plot.property('capturedFrame')
    assert on != off, 'Reflection strength left the Viewer unchanged'
    catcher.getParam('reflectionRoughness').setValue(.65)
    refresh()
    assert plot.property('capturedFrame') != on, 'Roughness left the Viewer unchanged'
    assert plot.grab().save(str(directory / 'reflection-preview-ui.png'))
    catcher.getParam('catchReflections').setValue(False)
    render.getParam('output').setValue('Color')
    report['reflection_creation_controls_preview'] = True
    later(place_proxy)


def place_proxy():
    global proxy_card
    proxy = application.getNode('Occluder1')
    proxy.getParam('shadowVisible').setValue(False)
    proxy_card = proxy.getInput(0)
    proxy_card.getParam('size').set(.09, .15)
    proxy_card.getParam('translate').set((317-320.6)/535.4*35, -(224-248.1)/539.2*35, -35.)
    application.setSelection([proxy_card])
    refresh()
    assert any(o.get('compositingRole') == 2 for o in plot.property('sceneObjects'))
    assert any(o.get('compositingRole') == 1 for o in plot.property('sceneObjects'))
    render.getParam('output').setValue('OcclusionPreview')
    refresh()
    assert mask_max() > 200
    proxy_card.getParam('translate').set((317-320.6)/535.4*35, -(224-248.1)/539.2*35, -50.)
    refresh()
    assert mask_max() == 0
    proxy_card.getParam('translate').set((317-320.6)/535.4*35, -(224-248.1)/539.2*35, -35.)
    render.getParam('output').setValue('Color')
    refresh()
    report['depth_order_and_mask_preview'] = True
    later(edit_proxy)


def edit_proxy():
    refresh()
    handles = plot.property('gizmoHandles')
    assert len(handles) == 3
    before = [proxy_card.getParam('translate').getValue(i) for i in range(3)]
    points = [p['position'] for p in plot.property('guideLandmarks')]
    center, start = plot.property('gizmoCenter'), handles[0]
    vector = start-center
    length = math.hypot(vector.x(), vector.y())
    assert length > 2
    end = start+vector*(28/length)
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)
    after = [proxy_card.getParam('translate').getValue(i) for i in range(3)]
    assert math.dist(before, after) > 1e-5
    refresh()
    assert points == [p['position'] for p in plot.property('guideLandmarks')]
    field = next(w for w in widgets() if isinstance(w, QLineEdit) and w.text() == proxy_card.getScriptName())
    undo(field)
    assert math.dist(before, [proxy_card.getParam('translate').getValue(i) for i in range(3)]) < 1e-8
    undo(field, True)
    assert math.dist(after, [proxy_card.getParam('translate').getValue(i) for i in range(3)]) < 1e-8
    undo(field)
    report['proxy_gizmo_undo_redo'] = True
    refresh()
    assert plot.grab().save(str(directory / 'catcher-camera-ui.png'))
    plot.setProperty('lookThroughCamera', False)
    application.setSelection([])
    assert QMetaObject.invokeMethod(plot, 'frameSelection', Qt.DirectConnection)
    later(save)


def save():
    refresh()
    assert plot.grab().save(str(directory / 'catcher-proxy-ui.png'))
    for graph in widgets():
        if graph.metaObject().className() == 'Natron::NodeGraph' and graph.isVisible():
            QMetaObject.invokeMethod(graph, 'centerOnAllNodes', Qt.DirectConnection)
    project = directory / (Path(report['profile']).name + '.ntp')
    assert application.saveProjectAs(project.as_posix())
    report.update(project=project.as_posix(), backend=plot.property('rendererBackend'))
    next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(False)
    # Outline edits do not require an attached image Viewer. Stop automatic
    # image renders while checking asynchronous stage composition/graph Undo.
    application.getViewer(viewer.getScriptName()).pause()
    viewer.disconnectInput(0)
    render.getParam('renderer').setValue('hdNVP')
    render.getParam('pathSamples').setValue(4)
    render.getParam('pathDenoiser').setValue('None')
    next(w for w in widgets() if w.objectName() == 'Viewer3DOutline' and w.isVisible()).click()
    later(outline_ready, 500)


def outline_widget():
    return next(w for w in widgets() if w.objectName() == 'StageOutline' and w.isVisible())


def outline_ready():
    global outline_attempts
    outline_attempts += 1
    outline = outline_widget()
    outline.setProperty('pinnedSource', render.getParam('usdIdentity').getValue())
    tree = outline.findChild(QTreeWidget, 'StageOutlineTree')
    tree.expandAll()

    def paths(parent=QModelIndex()):
        result = []
        for row in range(tree.model().rowCount(parent)):
            index = tree.model().index(row, 0, parent)
            result.append(index.data(Qt.UserRole))
            result.extend(paths(index))
        return result

    box = '/Natron/Assets/n_' + application.getNode('ReadUSD1').getParam('usdIdentity').getValue() + '/Source/Box'
    assert outline_attempts < 35, outline.property('stageStatus')
    if box not in paths():
        later(outline_ready, 200)
        return
    assert QMetaObject.invokeMethod(outline, 'selectPrimPaths', Qt.DirectConnection, Q_ARG('QStringList', [box]))
    later(add_usd_roles, 400)


def graph_action(name):
    graph = next(w for w in widgets() if w.metaObject().className() == 'Natron::NodeGraph' and w.isVisible())
    graph.setFocus(Qt.OtherFocusReason)
    actions = []
    for w in widgets():
        actions += w.actions() + w.findChildren(QAction)
    next(a for a in actions if a.text().replace('&', '').startswith(name) and a.isEnabled()).trigger()


def add_usd_roles():
    outline = outline_widget()
    details = outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    assert 'Type: Mesh' in details, details
    original = render.getInput(0).getScriptName()
    assert QMetaObject.invokeMethod(outline, 'addReflectionCatcherEdit', Qt.DirectConnection)
    catcher_edit = render.getInput(0)
    assert catcher_edit.getPluginID() == 'fr.inria.built-in.USDCompositing'
    assert catcher_edit.getParam('compositingRole').getValue() == 3
    graph_action('Undo')
    assert render.getInput(0).getScriptName() == original
    graph_action('Redo')
    assert render.getInput(0).getParam('compositingRole').getValue() == 3
    later(add_usd_occluder, 500)


def add_usd_occluder():
    global role_attempts, occluder_previous
    role_attempts += 1
    outline = outline_widget()
    box = '/Natron/Assets/n_' + application.getNode('ReadUSD1').getParam('usdIdentity').getValue() + '/Source/Box'
    details = outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    assert role_attempts < 60, (outline.property('stageStatus'), details, outline.property('selectedPrimPaths'))
    if outline.property('selectedPrimPaths') != [box]:
        QMetaObject.invokeMethod(outline, 'selectPrimPaths', Qt.DirectConnection, Q_ARG('QStringList', [box]))
        later(add_usd_occluder, 200)
        return
    if outline.property('composing') or not next(w for w in widgets() if w.objectName() == 'StageOutlineEdit').isEnabled() or 'Type: Mesh' not in details or box not in details:
        later(add_usd_occluder, 200)
        return
    occluder_previous = render.getInput(0).getScriptName()
    assert QMetaObject.invokeMethod(outline, 'addOccluderEdit', Qt.DirectConnection)
    later(check_usd_occluder, 200)


def check_usd_occluder():
    global role_attempts
    role_attempts += 1
    assert role_attempts < 60, (render.getInput(0).getScriptName(), occluder_previous)
    if render.getInput(0).getScriptName() == occluder_previous:
        later(check_usd_occluder, 200)
        return
    edit = render.getInput(0)
    assert edit.getPluginID() == 'fr.inria.built-in.USDCompositing'
    assert edit.getParam('compositingRole').getValue() == 2, edit.getScriptName()
    report['usd_role_actions_undo_redo'] = True
    material = '/Natron/Assets/n_' + application.getNode('ReadUSD1').getParam('usdIdentity').getValue() + '/Source/Material'
    outline = outline_widget()
    assert QMetaObject.invokeMethod(outline, 'selectPrimPaths', Qt.DirectConnection, Q_ARG('QStringList', [material]))
    later(reject_material, 500)


def reject_material():
    global material_attempts
    material_attempts += 1
    outline = outline_widget()
    material = '/Natron/Assets/n_' + application.getNode('ReadUSD1').getParam('usdIdentity').getValue() + '/Source/Material'
    details = outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    assert material_attempts < 60, (outline.property('stageStatus'), details)
    if outline.property('selectedPrimPaths') != [material]:
        QMetaObject.invokeMethod(outline, 'selectPrimPaths', Qt.DirectConnection, Q_ARG('QStringList', [material]))
        later(reject_material, 200)
        return
    if outline.property('composing') or 'Type: Material' not in details or material not in details:
        later(reject_material, 200)
        return
    before = render.getInput(0).getScriptName()
    assert QMetaObject.invokeMethod(outline, 'addOccluderEdit', Qt.DirectConnection)
    assert render.getInput(0).getScriptName() == before
    assert 'Select Mesh' in outline.findChild(QPlainTextEdit, 'StageOutlineProperties').toPlainText()
    report['non_geometry_role_rejected'] = True
    graph_action('Undo')
    later(undo_usd_reflection, 200)


def undo_usd_reflection():
    edit = render.getInput(0)
    assert edit.getPluginID() == 'fr.inria.built-in.USDCompositing' and edit.getParam('compositingRole').getValue() == 3
    graph_action('Undo')
    later(finish_usd_edits, 200)


def finish_usd_edits():
    assert render.getInput(0).getScriptName() == setup.getScriptName(), render.getInput(0).getScriptName()
    render.getParam('renderer').setValue('Vulkan')
    assert viewer.connectInput(0, render)
    next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible()).setChecked(True)
    assert application.saveProjectAs(report['project'])
    finish()


later(start, 600)
