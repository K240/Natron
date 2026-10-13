"""Exercise the real 3D Viewer controls, picking, gizmos, keyframes and Undo/Redo."""
import json
import math
import os
import traceback
from pathlib import Path
from PySide6.QtCore import Q_ARG, QEvent, QMetaObject, QPoint, QPointF, QTimer, Qt
from PySide6.QtGui import QAction, QColor, QImage, QKeyEvent, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication, QMainWindow
import NatronEngine

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('3D Viewer smoke started', encoding='utf-8')
output = Path('build/scene3d-gui-smoke')
output.mkdir(parents=True, exist_ok=True)
report = {'ok': False, 'stages': {}}
retained = {}
plot = camera_button = card = camera = renderer = viewer = None
axis = transformed = projector = projection = None
multi_cards = multi_axis = multi_transform = multi_scene = None

def widgets():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    return values

def finish(error=None):
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['ok'] = error is None and not report['unexpected_dialogs']
    Path('build/scene3d-gui-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    QApplication.exit(0)

def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run

def later(function):
    QTimer.singleShot(600, guarded(function))

def mouse(kind, position, button, buttons, modifiers=Qt.NoModifier):
    global_position = QPointF(plot.mapToGlobal(position.toPoint()))
    QApplication.sendEvent(plot, QMouseEvent(kind, position, position, global_position, button, buttons, modifiers))

def drag(start, end, button=Qt.LeftButton, modifiers=Qt.NoModifier):
    mouse(QEvent.MouseButtonPress, start, button, button, modifiers)
    for step in range(1, 5):
        mouse(QEvent.MouseMove, start+(end-start)*(step/4), Qt.NoButton, button, modifiers)
    mouse(QEvent.MouseButtonRelease, end, button, Qt.NoButton, modifiers)

def key(value):
    QApplication.sendEvent(plot, QKeyEvent(QEvent.KeyPress, value, Qt.NoModifier))

def refresh():
    assert QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
    assert not plot.property('renderError'), plot.property('renderError')

def axis_drag(axis, distance):
    refresh()
    center = plot.property('gizmoCenter')
    start = plot.property('gizmoHandles')[axis]
    vector = start-center
    length = math.hypot(vector.x(), vector.y())
    assert length > 1
    drag(start, start+vector*(distance/length))

def rotate_drag(degrees):
    refresh()
    center = plot.property('gizmoCenter')
    start = plot.property('gizmoHandles')[2]
    v = start-center
    angle = math.radians(degrees)
    end = center+QPointF(v.x()*math.cos(angle)+v.y()*math.sin(angle), -v.x()*math.sin(angle)+v.y()*math.cos(angle))
    drag(start, end)

def capture(name):
    frame = plot.grab().toImage()
    assert frame.save(str(output / (name+'.png')))
    return frame

def action(name):
    actions = []
    for widget in widgets():
        actions += widget.actions()+widget.findChildren(QAction)
    found = next(a for a in actions if a.text().replace('&', '').startswith(name) and a.isEnabled())
    return found

def setup():
    global camera, card, renderer, viewer
    app.addFormat('Scene3DGui 96x64 1')
    app.getProjectParam('outputFormat').setValue('Scene3DGui')
    app.getProjectParam('frameRange').set(1, 10)
    source = app.createNode('net.sf.openfx.ConstantPlugin')
    source.getParam('color').set(.5, .1, .05, 1.)
    properties = {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)}
    card = app.createNode('fr.inria.built-in.Card', -1, app, properties)
    card.setScriptName('EditableCard')
    card.getParam('size').set(2., 2.)
    assert card.connectInput(0, source)
    camera = app.createNode('fr.inria.built-in.Camera')
    renderer = app.createNode('fr.inria.built-in.Render3D')
    assert renderer.connectInput(0, card) and renderer.connectInput(1, camera)
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert viewer.connectInput(0, renderer)
    for window in widgets():
        if isinstance(window, QMainWindow):
            window.showNormal(); window.resize(1400, 1000); window.raise_(); window.activateWindow()
    QTimer.singleShot(2500, guarded(open_three_d))

def open_three_d():
    global plot, camera_button
    toggle = next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible())
    toggle.setChecked(True)
    plot = next(w for w in widgets() if w.objectName() == 'Scene3DViewer' and w.isVisible())
    camera_button = next(w for w in widgets() if w.objectName() == 'Viewer3DCamera' and w.isVisible())
    later(inspect)

def inspect():
    assert not plot.property('renderError'), plot.property('renderError')
    assert not plot.property('capturedFrame').isNull()
    report['stages']['initial'] = {'backend': plot.property('rendererBackend'), 'size': [plot.width(), plot.height()]}
    first = bytes(plot.property('capturedFrame').constBits())
    center = QPointF(plot.width()/2, plot.height()/2)
    drag(center, center+QPointF(40, -20), modifiers=Qt.AltModifier)
    assert bytes(plot.property('capturedFrame').constBits()) != first, 'Orbit did not change the preview'
    report['stages']['orbit'] = True
    drag(center, center+QPointF(25, 10), button=Qt.MiddleButton)
    before = bytes(plot.property('capturedFrame').constBits())
    QApplication.sendEvent(plot, QWheelEvent(center, QPointF(plot.mapToGlobal(center.toPoint())), QPoint(), QPoint(0, 120), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False))
    assert bytes(plot.property('capturedFrame').constBits()) != before, 'Dolly did not change the preview'
    report['stages']['pan_dolly'] = True
    camera_button.setChecked(True)
    later(pick_card)

def pick_card():
    assert plot.property('lookThroughCamera')
    center = QPointF(plot.width()/2, plot.height()/2)
    mouse(QEvent.MouseButtonPress, center, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, center, Qt.LeftButton, Qt.NoButton)
    assert plot.property('selectedNode') == 'EditableCard', plot.property('selectedNode')
    capture('selected-card')
    # X handle is 65 device-independent pixels from the card origin in this camera.
    report['before'] = card.getParam('translate').getValue(0)
    drag(center+QPointF(45, 0), center+QPointF(75, 0))
    later(inspect_move)

def inspect_move():
    after = card.getParam('translate').getValue(0)
    assert after > report['before']+.01, (after, report['before'])
    report['after'] = after
    capture('moved-card')
    action('Undo').trigger()
    later(inspect_undo)

def inspect_undo():
    assert abs(card.getParam('translate').getValue(0)-report['before']) < 1e-8
    action('Redo').trigger()
    later(inspect_redo)

def inspect_redo():
    assert abs(card.getParam('translate').getValue(0)-report['after']) < 1e-8
    report['stages']['gizmo_undo_redo'] = True
    translate = card.getParam('translate')
    translate.setValueAtTime(report['after'], 1., 0)
    translate.setValueAtTime(report['after']+.1, 10., 0)
    app.getViewer(viewer.getScriptName()).seek(5)
    later(move_animated)

def move_animated():
    before_keys = card.getParam('translate').getNumKeys(0)
    value = card.getParam('translate').getValue(0)
    report['key_before'] = value
    # Re-select after the animated position changes, using the captured frame's card center.
    image = plot.property('capturedFrame')
    # The renderer's view maps world X to screen through a 50/36 lens at Z=5.
    image_width = min(plot.width(), plot.height()*1.5)
    center = QPointF(plot.width()/2 + value/1.8*image_width/2, plot.height()/2)
    mouse(QEvent.MouseButtonPress, center, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, center, Qt.LeftButton, Qt.NoButton)
    drag(center+QPointF(40, 0), center+QPointF(55, 0))
    assert card.getParam('translate').getNumKeys(0) == before_keys+1, 'Gizmo did not auto-key at the current frame'
    report['stages']['gizmo_autokey'] = True
    edited = card.getParam('translate').getValueAtTime(5., 0)
    frame_ten = card.getParam('translate').getValueAtTime(10., 0)
    app.getViewer(viewer.getScriptName()).seek(10)
    action('Undo').trigger()
    assert card.getParam('translate').getNumKeys(0) == before_keys, 'Undo did not remove the new key'
    action('Redo').trigger()
    assert card.getParam('translate').getNumKeys(0) == before_keys+1
    assert abs(card.getParam('translate').getValueAtTime(5., 0)-edited) < 1e-8, 'Redo followed the playhead instead of the edited frame'
    assert abs(card.getParam('translate').getValueAtTime(10., 0)-frame_ten) < 1e-8, 'Undo/Redo changed an unrelated frame'
    app.getViewer(viewer.getScriptName()).seek(5)
    report['stages']['animated_undo_redo_after_seek'] = True
    capture('animated-card')
    later(rotate_tools)

def rotate_tools():
    tool = next(w for w in widgets() if w.objectName() == 'Viewer3DGizmo' and w.isVisible())
    assert QMetaObject.invokeMethod(tool, 'setCurrentIndex', Qt.DirectConnection, Q_ARG(int, 2))
    assert plot.property('gizmoMode') == 2, 'Toolbar did not switch to Scale'
    key(Qt.Key_E)
    assert plot.property('gizmoMode') == 1
    rotate_drag(20)
    first = card.getParam('rotate').getValue(2)
    assert abs(first-20) < .01, first
    action('Undo').trigger()
    assert abs(card.getParam('rotate').getValue(2)) < 1e-8
    action('Redo').trigger()
    rotate_drag(6)
    second = card.getParam('rotate').getValue(2)
    assert abs(second-first-6) < .01, (first, second)
    action('Undo').trigger()
    assert abs(card.getParam('rotate').getValue(2)-first) < 1e-8, 'Separate drags merged into one Undo'
    action('Undo').trigger()
    assert abs(card.getParam('rotate').getValue(2)) < 1e-8
    action('Redo').trigger(); action('Redo').trigger()
    report['stages']['rotate_undo_redo_and_gestures'] = True
    # Align the lens with the selected origin so X/Y rings are exactly edge-on.
    camera.getParam('translate').set(card.getParam('translate').getValue(0), card.getParam('translate').getValue(1), 5.)
    for axis in (0, 1):
        # These rings are edge-on through the render camera; use the visible arc.
        axis_drag(axis, 14)
        assert abs(card.getParam('rotate').getValue(axis)) > 1, ('Edge-on rotation ring did not respond', axis)
        assert abs(card.getParam('rotate').getValue(2)-second) < 1e-8
        action('Undo').trigger()
        assert abs(card.getParam('rotate').getValue(axis)) < 1e-8
    report['stages']['rotate_edge_on_x_y'] = True
    camera.getParam('translate').set(0., 0., 5.)
    capture('rotation-rings')
    rotation = card.getParam('rotate')
    rotation.setValueAtTime(second, 1., 2)
    rotation.setValueAtTime(second+10, 10., 2)
    later(rotate_animated)

def rotate_animated():
    rotation = card.getParam('rotate')
    before = rotation.getNumKeys(2)
    rotate_drag(8)
    assert rotation.getNumKeys(2) == before+1
    action('Undo').trigger()
    assert rotation.getNumKeys(2) == before
    action('Redo').trigger()
    assert rotation.getNumKeys(2) == before+1
    report['stages']['rotate_autokey'] = True
    assert rotation.setInterpolationAtTime(5., NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeLinear, 2)
    original_curve = [rotation.getValueAtTime(time, 2) for time in (3., 5., 7.)]
    original_derivative = rotation.getDerivativeAtTime(5., 2)
    rotate_drag(4)
    action('Undo').trigger()
    assert all(abs(rotation.getValueAtTime(time, 2)-value) < 1e-8 for time, value in zip((3., 5., 7.), original_curve))
    assert abs(rotation.getDerivativeAtTime(5., 2)-original_derivative) < 1e-8, 'Undo lost the original interpolation'
    action('Redo').trigger(); action('Undo').trigger()
    assert abs(rotation.getValueAtTime(5., 2)-original_curve[1]) < 1e-8
    report['stages']['modified_key_restores_curve'] = True
    scale = card.getParam('scale')
    for axis in range(3):
        scale.setValueAtTime(1., 1., axis)
        scale.setValueAtTime(1., 10., axis)
    key(Qt.Key_R)
    later(scale_tools)

def scale_tools():
    scale = card.getParam('scale')
    refresh()
    center = plot.property('gizmoCenter')
    drag(center, center+QPointF(15, -10))
    expected = math.exp(25/65)
    assert all(abs(scale.getValue(axis)-expected) < 1e-8 and scale.getNumKeys(axis)==3 for axis in range(3))
    action('Undo').trigger()
    assert all(abs(scale.getValue(axis)-1) < 1e-8 and scale.getNumKeys(axis)==2 for axis in range(3))
    action('Redo').trigger()
    assert all(scale.getNumKeys(axis)==3 for axis in range(3))
    axis_drag(0, 18)
    assert abs(scale.getValue(0)-expected*math.exp(18/65)) < 1e-8
    assert abs(scale.getValue(1)-expected) < 1e-8 and abs(scale.getValue(2)-expected) < 1e-8
    action('Undo').trigger()
    assert abs(scale.getValue(0)-expected) < 1e-8
    report['stages']['scale_axes_uniform_autokey_undo_redo'] = True
    refresh()
    start = plot.property('gizmoHandles')[0]
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseMove, start+QPointF(15, 0), Qt.NoButton, Qt.LeftButton)
    assert abs(scale.getValue(0)-expected) > .01
    key(Qt.Key_Escape)
    mouse(QEvent.MouseButtonRelease, start, Qt.LeftButton, Qt.NoButton)
    assert abs(scale.getValue(0)-expected) < 1e-8, 'Escape did not cancel the active scale edit'
    report['stages']['escape_cancels_gesture'] = True
    refresh()
    start = plot.property('gizmoHandles')[0]
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseMove, start+QPointF(15, 0), Qt.NoButton, Qt.LeftButton)
    toggle = next(w for w in widgets() if w.objectName() == 'Viewer3DToggle' and w.isVisible())
    toggle.setChecked(False)
    action('Undo').trigger()
    assert abs(scale.getValue(0)-expected) < 1e-8, 'Hiding the Viewer left an open Undo gesture'
    toggle.setChecked(True)
    report['stages']['hide_closes_gesture'] = True
    capture('scale-handles')
    assert app.saveProjectAs(str((output/'editable-scene.ntp').resolve()))
    later(parented_tools)

def parented_tools():
    global card, axis, transformed
    source = card.getInput(0)
    axis = app.createNode('fr.inria.built-in.Axis')
    axis.getParam('rotate').set(0., 0., 35.)
    axis.getParam('scale').set(2., .75, 1.)
    properties = {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)}
    card = app.createNode('fr.inria.built-in.Card', -1, app, properties)
    card.setScriptName('ParentedCard')
    card.getParam('rotate').set(0., 0., 10.)
    assert card.connectInput(0, source) and card.connectInput(1, axis)
    transformed = app.createNode('fr.inria.built-in.TransformGeo')
    transformed.getParam('rotate').set(0., 0., -20.)
    transformed.getParam('scale').set(1.3, 1.1, 1.)
    assert transformed.connectInput(0, card)
    renderer.disconnectInput(0)
    assert renderer.connectInput(0, transformed)
    key(Qt.Key_W)
    later(edit_parented)

def edit_parented():
    refresh()
    center = QPointF(plot.width()/2, plot.height()/2)
    mouse(QEvent.MouseButtonPress, center, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, center, Qt.LeftButton, Qt.NoButton)
    assert plot.property('selectedNode') == 'ParentedCard'
    axis_drag(0, 15)
    assert card.getParam('translate').getValue(0) > .01
    assert abs(card.getParam('translate').getValue(1)) < 1e-8
    action('Undo').trigger()
    assert abs(card.getParam('translate').getValue(0)) < 1e-8
    key(Qt.Key_E)
    before = card.getParam('rotate').getValue(2)
    rotate_drag(12)
    assert abs(card.getParam('rotate').getValue(2)-before) > 1
    action('Undo').trigger()
    assert abs(card.getParam('rotate').getValue(2)-before) < 1e-8
    key(Qt.Key_R)
    axis_drag(0, 15)
    assert abs(card.getParam('scale').getValue(0)-math.exp(15/65)) < 1e-8
    assert abs(card.getParam('scale').getValue(1)-1) < 1e-8
    action('Undo').trigger()
    report['stages']['parent_axis_and_transformgeo_handles'] = True
    key(Qt.Key_H)
    axis_drag(2, 12)
    assert abs(card.getParam('scale').getValue(2)-math.exp(12/65)) < 1e-8
    assert abs(card.getParam('scale').getValue(0)-1) < 1e-8 and abs(card.getParam('scale').getValue(1)-1) < 1e-8
    action('Undo').trigger()
    key(Qt.Key_H)
    report['stages']['scale_z_in_perspective'] = True
    capture('parented-handles')
    assert app.saveProjectAs(str((output/'editable-scene.ntp').resolve()))
    report['stages']['saved'] = True
    later(camera_controls)

def objects(node):
    return [item for item in plot.property('sceneObjects') if item['node'] == node.getScriptName()]

def select_graph(node):
    app.setSelection([node]); refresh()
    assert plot.property('selectedNode') == node.getScriptName(), 'Graph selection did not reach the 3D Viewer'

def pick_object(item):
    assert item['visible'], item
    mouse(QEvent.MouseButtonPress, item['icon'], Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, item['icon'], Qt.LeftButton, Qt.NoButton)
    refresh()
    assert plot.property('selectedNode') == item['node'], (plot.property('selectedNode'), item, plot.property('sceneObjects'))

def camera_controls():
    assert camera.connectInput(0, axis)
    select_graph(camera)
    key(Qt.Key_F); refresh()
    assert not plot.property('lookThroughCamera'), 'Frame selected did not enter the inspection camera'
    info = objects(camera)[0]
    assert info['renderCamera'] and len(info['frustum']) == 4 and info['visible']
    inspection = bytes(plot.property('capturedFrame').constBits())
    camera.getParam('focal').setValue(80.); refresh()
    assert bytes(plot.property('capturedFrame').constBits()) == inspection, 'Render lens changed the inspection camera'
    assert objects(camera)[0]['frustum'] != info['frustum']
    camera.getParam('focal').setValue(50.); refresh()
    report['stages']['inspection_lens_is_independent'] = True
    select_graph(card); pick_object(info)
    key(Qt.Key_W); axis_drag(0, 12)
    assert camera.getParam('translate').getValue(0) > .01
    assert abs(camera.getParam('translate').getValue(1)) < 1e-8
    action('Undo').trigger()
    assert abs(camera.getParam('translate').getValue(0)) < 1e-8
    key(Qt.Key_E); rotate_drag(12)
    assert abs(camera.getParam('rotate').getValue(2)) > 1
    action('Undo').trigger()
    key(Qt.Key_R); axis_drag(0, 12)
    assert abs(camera.getParam('scale').getValue(0)-math.exp(12/65)) < 1e-8
    action('Undo').trigger()
    report['stages']['render_camera_frustum_pick_parented_controls'] = True
    capture('camera-frustum')
    later(transform_controls)

def transform_controls():
    select_graph(transformed); key(Qt.Key_W)
    original = objects(card)[0]['worldOrigin']
    axis_drag(0, 12)
    assert transformed.getParam('translate').getValue(0) > .01
    assert objects(card)[0]['worldOrigin'] != original
    assert abs(card.getParam('translate').getValue(0)) < 1e-8
    action('Undo').trigger(); refresh()
    assert objects(card)[0]['worldOrigin'] == original
    select_graph(axis)
    info = next(item for item in objects(axis) if item['time'] == app.getViewer(viewer.getScriptName()).getCurrentFrame() and item['visible'])
    select_graph(card); pick_object(info)
    axis_drag(0, 12)
    assert axis.getParam('translate').getValue(0) > .01
    assert objects(card)[0]['worldOrigin'] != original
    action('Undo').trigger(); refresh()
    assert objects(card)[0]['worldOrigin'] == original
    key(Qt.Key_E)
    angle = axis.getParam('rotate').getValue(2)
    rotate_drag(12)
    assert abs(axis.getParam('rotate').getValue(2)-angle) > 1
    action('Undo').trigger()
    key(Qt.Key_R); axis_drag(0, 12)
    assert abs(axis.getParam('scale').getValue(0)-2*math.exp(12/65)) < 1e-8
    action('Undo').trigger()
    report['stages']['axis_transformgeo_graph_pick_controls'] = True
    capture('transform-controls')
    later(projection_controls)

def projection_controls():
    global projector, projection
    source = card.getInput(0)
    projector = app.createNode('fr.inria.built-in.Camera')
    projector.setScriptName('ReferenceCamera')
    projector.getParam('translate').setValueAtTime(1., 1., 0)
    projector.getParam('translate').setValueAtTime(2., 10., 0)
    parent = app.createNode('fr.inria.built-in.Axis')
    parent.setScriptName('ReferenceAxis')
    parent.getParam('translate').set(1., 0., 0.)
    assert projector.connectInput(0, parent)
    projection = app.createNode('fr.inria.built-in.Project3D')
    assert projection.connectInput(0, source) and projection.connectInput(1, projector)
    card.disconnectInput(0); assert card.connectInput(0, projection)
    transformed.getParam('translate').set(1.25, .3, 0.)
    select_graph(projector); key(Qt.Key_F); refresh()
    info = objects(projector)[0]
    assert info['projectionCamera'] and info['time']==1 and not info['editable']
    assert info['worldOrigin'] == [2., 0., 5.], 'TransformGeo moved the world-space projection camera'
    assert len(info['frustum']) == 4
    assert len(plot.property('gizmoHandles')) == 0, 'Frozen projection exposed controls at the wrong frame'
    select_graph(card); pick_object(info)
    drag(info['icon'], info['icon']+QPointF(20, 0))
    assert projector.getParam('translate').getNumKeys(0) == 2
    assert projector.getParam('translate').getValueAtTime(1., 0) == 1.
    capture('frozen-projection-frustum')
    app.getViewer(viewer.getScriptName()).seek(1); refresh(); key(Qt.Key_W)
    assert objects(projector)[0]['editable']
    axis_drag(0, 12)
    assert projector.getParam('translate').getValueAtTime(1., 0) > 1.01
    action('Undo').trigger()
    assert projector.getParam('translate').getValueAtTime(1., 0) == 1.
    app.getViewer(viewer.getScriptName()).seek(5); refresh()
    assert len(plot.property('gizmoHandles')) == 0
    projection.getParam('freeze').setValue(False); refresh()
    assert objects(projector)[0]['time'] == 5 and objects(projector)[0]['editable']
    axis_drag(0, 12)
    assert projector.getParam('translate').getNumKeys(0) == 3
    action('Undo').trigger()
    assert projector.getParam('translate').getNumKeys(0) == 2
    projector.getParam('orthographic').setValue(True); refresh()
    rectangle = objects(projector)[0]['frustum']
    projector.getParam('focal').setValue(80.); refresh()
    assert objects(projector)[0]['frustum'] == rectangle, 'Orthographic frustum depended on focal length'
    projector.getParam('orthoWidth').setValue(6.); refresh()
    assert objects(projector)[0]['frustum'] != rectangle
    capture('orthographic-frustum')
    projector.getParam('orthographic').setValue(False)
    projector.getParam('focal').setValue(50.)
    projection.getParam('freeze').setValue(True); refresh()
    report['stages']['fixed_live_projection_frusta_reference_editing'] = True
    assert app.saveProjectAs(str((output/'editable-scene.ntp').resolve()))
    card.getParam('scale').set(0., 1., 1.)
    assert QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
    assert plot.property('renderError') and not plot.property('sceneObjects') and not plot.property('gizmoHandles')
    key(Qt.Key_F)
    assert plot.property('renderError'), 'Navigation hid an invalid scene instead of reporting its error'
    card.getParam('scale').set(1., 1., 1.); refresh()
    assert objects(projector) and not plot.property('capturedFrame').isNull()
    report['stages']['invalid_scene_clears_controls_and_recovers'] = True
    viewer.disconnectInput(0)
    assert QMetaObject.invokeMethod(plot, 'refreshScene', Qt.DirectConnection)
    assert plot.property('renderError') and not plot.property('sceneObjects')
    assert viewer.connectInput(0, renderer)
    refresh()
    assert objects(projector)
    report['stages']['viewer_disconnect_reconnect'] = True
    later(multi_setup)

def multi_setup():
    global multi_cards, multi_axis, multi_transform, multi_scene
    source = projection.getInput(0)
    multi_axis = app.createNode('fr.inria.built-in.Axis')
    multi_axis.getParam('translate').set(-.7, 0., 0.)
    multi_axis.getParam('rotate').set(0., 0., 35.)
    multi_axis.getParam('scale').set(1.7, .65, 1.)
    multi_cards = [app.createNode('fr.inria.built-in.Card') for _ in range(2)]
    for index, node in enumerate(multi_cards):
        node.setScriptName('MultiCard'+str(index+1))
        assert node.connectInput(0, source)
        node.getParam('size').set(.4, .6)
        node.getParam('rotate').set(0., 0., -10.+25*index)
        node.getParam('scale').set(.8+index*.5, 1.2-index*.3, 1.)
        # Animate both components so world X under a rotated parent must add
        # keys to X/Y but leave Z unkeyed.
        for dimension in (0, 1):
            node.getParam('translate').setValueAtTime(0., 1., dimension)
            node.getParam('translate').setValueAtTime(.1, 10., dimension)
    assert multi_cards[0].connectInput(1, multi_axis)
    multi_transform = app.createNode('fr.inria.built-in.TransformGeo')
    multi_transform.getParam('translate').set(.7, 0., 0.)
    multi_transform.getParam('rotate').set(0., 0., -25.)
    multi_transform.getParam('scale').set(.9, 1.4, 1.)
    assert multi_transform.connectInput(0, multi_cards[1])
    multi_scene = app.createNode('fr.inria.built-in.Scene')
    assert multi_scene.connectInput(0, multi_cards[0]) and multi_scene.connectInput(1, multi_transform)
    renderer.disconnectInput(0); assert renderer.connectInput(0, multi_scene)
    renderer.setScriptName('MultiRender3D')
    camera.disconnectInput(0)
    camera.getParam('translate').set(0., 0., 5.)
    camera_button.setChecked(True)
    app.setSelection([])
    key(Qt.Key_W)
    later(multi_select)

def selected_names():
    return set(plot.property('selectedNodes'))

def check_selection(nodes):
    expected = {node.getScriptName() for node in nodes}
    assert selected_names() == expected, (selected_names(), expected)
    assert {node.getScriptName() for node in app.getSelectedNodes()} == expected

def shift_click(position):
    mouse(QEvent.MouseButtonPress, position, Qt.LeftButton, Qt.LeftButton, Qt.ShiftModifier)
    mouse(QEvent.MouseButtonRelease, position, Qt.LeftButton, Qt.NoButton, Qt.ShiftModifier)
    refresh()

def world_origins():
    return [objects(node)[0]['worldOrigin'] for node in multi_cards]

def assert_origins(expected):
    assert all(abs(a-b)<1e-8 for actual, origin in zip(world_origins(), expected) for a, b in zip(actual, origin)), (world_origins(), expected)

def multi_select():
    refresh()
    first, second = [objects(node)[0]['icon'] for node in multi_cards]
    pick_object(objects(multi_cards[0])[0]); check_selection(multi_cards[:1])
    shift_click(second); check_selection(multi_cards)
    assert plot.property('selectedNode') == multi_cards[1].getScriptName()
    shift_click(first); check_selection(multi_cards[1:])
    shift_click(first); check_selection(multi_cards)
    report['stages']['shift_selection_graph_sync'] = True
    # Rectangle selection replaces the selection; Shift rectangle adds to it.
    app.setSelection([]); refresh()
    drag(first-QPointF(30, 35), first+QPointF(30, 35))
    check_selection(multi_cards[:1])
    drag(second-QPointF(30, 35), second+QPointF(30, 35), modifiers=Qt.ShiftModifier)
    check_selection(multi_cards)
    capture('multi-selection')
    # Escape must leave the existing selection unchanged.
    start = QPointF(10, 45)
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseMove, start+QPointF(30, 25), Qt.NoButton, Qt.LeftButton)
    key(Qt.Key_Escape)
    mouse(QEvent.MouseButtonRelease, start+QPointF(30, 25), Qt.LeftButton, Qt.NoButton)
    check_selection(multi_cards)
    report['stages']['marquee_replace_add_cancel'] = True
    app.setSelection(multi_cards); refresh(); check_selection(multi_cards)
    assert len(plot.property('gizmoHandles')) == 3
    later(multi_move)

def multi_move():
    original = world_origins()
    before = bytes(plot.property('capturedFrame').constBits())
    axis_drag(0, 12)
    after = world_origins()
    changes = [[a-b for a, b in zip(origin, old)] for origin, old in zip(after, original)]
    assert changes[0][0] > .01, (changes, selected_names(), plot.property('sceneObjects'))
    assert all(abs(a-b)<1e-8 for a, b in zip(*changes)), changes
    assert abs(changes[0][1])<1e-8 and abs(changes[0][2])<1e-8, changes
    assert bytes(plot.property('capturedFrame').constBits()) != before
    for node in multi_cards:
        param = node.getParam('translate')
        assert [param.getNumKeys(d) for d in range(3)] == [3, 3, 0]
        assert param.getValueAtTime(10., 0) == .1 and param.getValueAtTime(10., 1) == .1
    action('Undo').trigger(); refresh(); assert_origins(original)
    assert all(node.getParam('translate').getNumKeys(0)==2 and node.getParam('translate').getNumKeys(1)==2 for node in multi_cards)
    app.getViewer(viewer.getScriptName()).seek(10); refresh()
    action('Redo').trigger(); refresh()
    assert all(node.getParam('translate').getValueAtTime(10., 0)==.1 for node in multi_cards)
    action('Undo').trigger(); refresh()
    assert all(node.getParam('translate').getValueAtTime(10., d)==.1 for node in multi_cards for d in (0, 1))
    app.getViewer(viewer.getScriptName()).seek(5); refresh(); assert_origins(original)
    action('Redo').trigger(); refresh(); assert_origins(after)
    action('Undo').trigger(); refresh(); assert_origins(original)
    # A second gesture is independent, and Escape cancels all of its edits.
    axis_drag(1, 10)
    action('Undo').trigger(); refresh(); assert_origins(original)
    start = plot.property('gizmoHandles')[0]
    mouse(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseMove, start+QPointF(15, 0), Qt.NoButton, Qt.LeftButton)
    assert world_origins() != original
    key(Qt.Key_Escape); refresh(); assert_origins(original)
    mouse(QEvent.MouseButtonRelease, start+QPointF(15, 0), Qt.LeftButton, Qt.NoButton)
    report['stages']['multi_world_move_parent_transforms_animated_undo'] = True
    later(multi_rotate_scale)

def multi_rotate_scale():
    original = world_origins()
    angles = [node.getParam('rotate').getValue(2) for node in multi_cards]
    key(Qt.Key_E); rotate_drag(12)
    changes = [node.getParam('rotate').getValue(2)-angle for node, angle in zip(multi_cards, angles)]
    assert abs(changes[0]) > 1 and abs(changes[0]-changes[1]) < 1e-8, changes
    assert_origins(original)
    action('Undo').trigger()
    assert all(abs(node.getParam('rotate').getValue(2)-angle)<1e-8 for node, angle in zip(multi_cards, angles))
    scales = [[node.getParam('scale').getValue(d) for d in range(3)] for node in multi_cards]
    key(Qt.Key_R); axis_drag(0, 12)
    for node, initial in zip(multi_cards, scales):
        assert abs(node.getParam('scale').getValue(0)-initial[0]*math.exp(12/65))<1e-8
        assert node.getParam('scale').getValue(1)==initial[1]
    assert_origins(original); action('Undo').trigger(); refresh()
    center = plot.property('gizmoCenter')
    drag(center, center+QPointF(12, -6))
    for node, initial in zip(multi_cards, scales):
        assert all(abs(node.getParam('scale').getValue(d)-initial[d]*math.exp(18/65))<1e-8 for d in range(3))
    action('Undo').trigger(); refresh(); assert_origins(original)
    assert all(node.getParam('scale').getValue(d)==initial[d] for node, initial in zip(multi_cards, scales) for d in range(3))
    report['stages']['multi_individual_rotate_axis_uniform_scale_undo'] = True
    key(Qt.Key_F); refresh()
    assert not plot.property('lookThroughCamera')
    assert all(objects(node)[0]['visible'] for node in multi_cards)
    capture('multi-framed')
    report['stages']['frame_multiple_cards'] = True
    camera_button.setChecked(True)
    app.setSelection([multi_cards[0], multi_axis]); refresh()
    assert len(plot.property('gizmoHandles')) == 0, 'Parent and child selection exposed controls that would apply a transform twice'
    check_selection([multi_cards[0], multi_axis])
    report['stages']['mixed_selection_safety'] = True
    # Check picking and rectangle geometry after changing the viewport aspect.
    plot.setMinimumHeight(300)
    later(multi_resized)

def multi_resized():
    app.setSelection([]); refresh(); key(Qt.Key_W)
    pick_object(objects(multi_cards[0])[0])
    shift_click(objects(multi_cards[1])[0]['icon']); check_selection(multi_cards)
    original = world_origins()
    axis_drag(0, 12)
    changes = [[a-b for a, b in zip(origin, old)] for origin, old in zip(world_origins(), original)]
    assert changes[0][0] > .01 and all(abs(a-b)<1e-8 for a,b in zip(*changes)), changes
    action('Undo').trigger(); refresh(); assert_origins(original)
    select_graph(multi_cards[0]); pick_object(objects(multi_axis)[0])
    check_selection([multi_axis])
    report['stages']['multi_resized_viewport_pick_and_move'] = [plot.width(), plot.height()]
    plot.setMinimumHeight(150)
    later(multi_group_setup)

def multi_group_setup():
    global multi_group, grouped_bulk_card
    multi_group = app.createNode('fr.inria.built-in.Group')
    assert multi_group.connectInput(0, projection.getInput(0))
    grouped_bulk_card = app.createNode('fr.inria.built-in.Card', -1, multi_group)
    grouped_bulk_card.setScriptName('GroupedBulkCard')
    grouped_bulk_card.getParam('size').set(.4, .3)
    grouped_bulk_card.getParam('translate').set(.1, -.65, 0.)
    assert grouped_bulk_card.connectInput(0, multi_group.getNode('Input1'))
    group_output = multi_group.getNode('Output1')
    group_output.disconnectInput(0); assert group_output.connectInput(0, grouped_bulk_card)
    assert multi_scene.connectInput(3, multi_group)
    app.setSelection([grouped_bulk_card]); refresh()
    # The Python API's empty selection clears the root graph only. Clicking
    # empty 3D space clears the participating root and Group graphs together.
    mouse(QEvent.MouseButtonPress, QPointF(10, 45), Qt.LeftButton, Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease, QPointF(10, 45), Qt.LeftButton, Qt.NoButton)
    refresh(); assert not selected_names()
    app.setSelection(multi_cards[:1]); refresh()
    later(multi_group_edit)

def multi_group_edit():
    shift_click(objects(grouped_bulk_card)[0]['icon'])
    assert selected_names() == {multi_cards[0].getScriptName(), grouped_bulk_card.getScriptName()}, (selected_names(), plot.property('sceneObjects'))
    assert {node.getScriptName() for node in app.getSelectedNodes(multi_group)} == {grouped_bulk_card.getScriptName()}
    before = [objects(node)[0]['worldOrigin'] for node in (multi_cards[0], grouped_bulk_card)]
    key(Qt.Key_W); axis_drag(0, 12)
    after = [objects(node)[0]['worldOrigin'] for node in (multi_cards[0], grouped_bulk_card)]
    changes = [[a-b for a,b in zip(origin, old)] for origin,old in zip(after,before)]
    assert changes[0][0] > .01 and all(abs(a-b)<1e-8 for a,b in zip(*changes)), changes
    action('Undo').trigger(); refresh()
    assert all(abs(a-b)<1e-8 for node, old in zip((multi_cards[0],grouped_bulk_card),before) for a,b in zip(objects(node)[0]['worldOrigin'],old))
    multi_scene.disconnectInput(3); refresh()
    assert not objects(grouped_bulk_card) and grouped_bulk_card.getScriptName() not in selected_names()
    report['stages']['bulk_selection_edit_across_group_graphs'] = True
    later(multi_instances)

def multi_instances():
    # Selecting two placements of one Card edits its knob once.
    duplicate = app.createNode('fr.inria.built-in.TransformGeo')
    duplicate.getParam('translate').set(0., .6, 0.)
    assert duplicate.connectInput(0, multi_cards[0]) and multi_scene.connectInput(2, duplicate)
    app.setSelection([]); refresh(); key(Qt.Key_W)
    # A fixed margin around the icons can land on a Card after the viewport
    # grows. Start in the empty corner and enclose all three placements.
    drag(QPointF(1,1),QPointF(plot.width()-2,plot.height()-2))
    check_selection(multi_cards)
    assert len(plot.property('selectedNodes')) == 2
    key(Qt.Key_R)
    scale = multi_cards[0].getParam('scale').getValue(0)
    axis_drag(0, 12)
    assert abs(multi_cards[0].getParam('scale').getValue(0)-scale*math.exp(12/65))<1e-8
    action('Undo').trigger(); refresh()
    report['stages']['reused_card_edited_once'] = True
    # Persist a bulk animation edit for a subsequent headless reload test.
    app.setSelection(multi_cards); refresh(); key(Qt.Key_W); axis_drag(0, 12)
    report['multi_values'] = {node.getScriptName(): [node.getParam('translate').getValueAtTime(5., d) for d in range(3)] for node in multi_cards}
    path = (output/'multi-card-scene.ntp').resolve()
    assert app.saveProjectAs(str(path))
    report['multi_project'] = str(path)
    capture('multi-cards-final')
    report['stages']['multi_saved'] = True
    later(depth_preview)

def depth_preview():
    renderer.getParam('output').setValue('Color'); refresh()
    original = bytes(plot.property('capturedFrame').constBits())
    renderer.getParam('output').setValue('DepthPreview'); refresh()
    image = plot.property('capturedFrame')
    assert bytes(image.constBits())!=original
    assert all(image.pixelColor(x,y).red()==image.pixelColor(x,y).green()==image.pixelColor(x,y).blue() for y in range(image.height()) for x in range(image.width()))
    assert len(plot.property('gizmoHandles'))==3
    preview = bytes(image.constBits())
    renderer.getParam('depthRange').set(4., 8.); refresh()
    assert bytes(plot.property('capturedFrame').constBits())!=preview
    capture('depth-preview')
    # Tool manipulation remains active in depth mode and refreshes the result.
    key(Qt.Key_W); axis_drag(0, 12)
    action('Undo').trigger(); refresh()
    before = bytes(plot.property('capturedFrame').constBits())
    renderer.getParam('depthRange').set(8., 4.)
    assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
    assert 'Depth preview' in plot.property('renderError') and not plot.property('gizmoHandles')
    renderer.getParam('depthRange').set(4., 8.); refresh()
    assert bytes(plot.property('capturedFrame').constBits())==before
    renderer.getParam('output').setValue('Color'); refresh()
    assert bytes(plot.property('capturedFrame').constBits())==original
    report['stages']['depth_preview_range_editing_recovery_color_restore'] = True
    later(fbx_mesh)

def fbx_mesh():
    def values(effect,name):
        return [effect.getParam(name).getValue(d) for d in range(3)]
    properties = {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)}
    mesh=app.createNode('fr.inria.built-in.ReadGeo',-1,app,properties)
    mesh.setScriptName('EditableFbx')
    filename=Path('Tests/fixtures/scene3d/colored-cube.fbx').resolve().as_posix()
    mesh.getParam('file').setValue(filename)
    mesh.getParam('scale').set(.35,.35,.35)
    mesh.getParam('translate').set(-.5,0.,0.)
    mesh.getParam('rotate').set(15.,25.,0.)
    mesh_camera=app.createNode('fr.inria.built-in.Camera')
    scene=app.createNode('fr.inria.built-in.Scene')
    image=app.createNode('net.sf.openfx.ConstantPlugin');image.getParam('color').set(.1,.3,.7,1.)
    companion=app.createNode('fr.inria.built-in.Card',-1,app,properties)
    companion.setScriptName('FbxCompanionCard');companion.getParam('size').set(.6,.6)
    companion.getParam('translate').set(.5,0.,0.)
    assert companion.connectInput(0,image) and scene.connectInput(0,mesh) and scene.connectInput(1,companion)
    renderer.disconnectInput(0);renderer.disconnectInput(1)
    assert renderer.connectInput(0,scene) and renderer.connectInput(1,mesh_camera)
    renderer.getParam('output').setValue('Color');camera_button.setChecked(True)
    app.setSelection([mesh]);refresh();key(Qt.Key_W)
    assert plot.property('rendererBackend')==('CPU' if os.environ.get('NATRON_VULKAN_DISABLED')=='1' else 'Vulkan')
    assert len(plot.property('gizmoHandles'))==3 and objects(mesh)[0]['kind']=='ReadGeo'
    before=values(mesh,'translate');axis_drag(0,12)
    assert values(mesh,'translate')[0]>before[0]
    action('Undo').trigger();refresh();assert values(mesh,'translate')==before
    app.setSelection([]);refresh()
    center=objects(mesh)[0]['icon'];mouse(QEvent.MouseButtonPress,center,Qt.LeftButton,Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease,center,Qt.LeftButton,Qt.NoButton)
    check_selection([mesh])
    app.setSelection([mesh,companion]);refresh();key(Qt.Key_W)
    origins=[objects(n)[0]['worldOrigin'] for n in (mesh,companion)]
    axis_drag(0,12)
    deltas=[objects(n)[0]['worldOrigin'][0]-old[0] for n,old in zip((mesh,companion),origins)]
    assert deltas[0]>.01 and abs(deltas[0]-deltas[1])<1e-8
    action('Undo').trigger();refresh()
    renderer.getParam('output').setValue('DepthPreview');refresh()
    capture('fbx-mesh-depth')
    key(Qt.Key_F);refresh()
    assert not plot.property('lookThroughCamera')
    capture('fbx-mesh-inspection')
    mesh.getParam('file').setValue(filename+'.missing')
    assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
    assert 'Cannot open FBX' in plot.property('renderError') and not plot.property('gizmoHandles')
    mesh.getParam('file').setValue(filename);refresh()
    renderer.getParam('output').setValue('Color');refresh()
    capture('fbx-mesh-color')
    report['stages']['fbx_import_pick_edit_bulk_depth_frame_error_recovery']=True
    later(fbx_animation)

def fbx_animation():
    app.getProjectParam('frameRange').set(1,49)
    app.getProjectParam('frameRate').setValue(24.)
    mesh=app.createNode('fr.inria.built-in.ReadGeo')
    mesh.setScriptName('AnimatedFbxViewer')
    mesh.getParam('file').setValue(Path('Tests/fixtures/scene3d/animated-cube.fbx').resolve().as_posix())
    mesh.getParam('scale').set(.35,.35,.35)
    renderer.disconnectInput(0);assert renderer.connectInput(0,mesh)
    camera_button.setChecked(True);app.setSelection([mesh]);key(Qt.Key_W)
    # Connections made while the 2D Viewer is rendering are queued until that
    # render finishes. Yield to the event loop before testing the new scene.
    later(fbx_animation_frames)


def fbx_animation_frames():
    mesh=app.getNode('AnimatedFbxViewer')
    assert renderer.getInput(0).getScriptName()==mesh.getScriptName(), 'FBX Viewer connection has not been applied'
    assert all(name in mesh.getParam('animationInfo').getValue() for name in ('Move','Scale','Hide'))
    def frame(number):
        app.getViewer(viewer.getScriptName()).seek(number);refresh()
        return bytes(plot.property('capturedFrame').constBits())
    static=frame(1);assert frame(25)==static
    mesh.getParam('readAnimation').setValue(True)
    beginning=frame(1);middle=frame(13);ending=frame(25)
    repeated=frame(1)
    assert beginning!=middle and middle!=ending, 'FBX pose did not follow Viewer timeline'
    assert repeated==beginning, 'Returning to FBX frame 1 changed the preview (max byte difference: %s)'%max(abs(a-b) for a,b in zip(beginning,repeated))
    frame(13)
    original=[mesh.getParam('translate').getValueAtTime(13.,d) for d in range(3)]
    axis_drag(0,12);refresh()
    assert bytes(plot.property('capturedFrame').constBits())!=middle
    action('Undo').trigger();refresh()
    assert [mesh.getParam('translate').getValueAtTime(13.,d) for d in range(3)]==original
    assert bytes(plot.property('capturedFrame').constBits())==middle, 'Undo changed imported animation time'
    capture('fbx-animation-halfway')
    key(Qt.Key_F);refresh();assert not plot.property('lookThroughCamera')
    capture('fbx-animation-framed');camera_button.setChecked(True)
    mesh.getParam('animationSpeed').setValue(0.)
    mesh.getParam('animationOffset').setValue(.5)
    assert frame(13)==frame(25)
    mesh.getParam('animationSpeed').setValue(1.);mesh.getParam('animationOffset').setValue(0.)
    mesh.getParam('animationTake').setValue('Scale')
    assert frame(1)!=frame(25)
    mesh.getParam('animationTake').setValue('Hide');frame(25)
    hidden=plot.property('capturedFrame')
    # Viewer display composites transparency over its neutral backdrop.
    backdrop=hidden.pixelColor(0,0).getRgb()
    assert all(hidden.pixelColor(x,y).getRgb()==backdrop for y in range(hidden.height()) for x in range(hidden.width()))
    mesh.getParam('animationTake').setValue('MissingTake')
    assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
    assert 'take not found' in plot.property('renderError') and not plot.property('gizmoHandles')
    mesh.getParam('readAnimation').setValue(False);refresh()
    mesh.getParam('animationTake').setValue('Move');mesh.getParam('readAnimation').setValue(True)
    assert frame(13)==middle
    report['stages']['fbx_animation_timeline_retime_take_visibility_edit_undo_frame_error_recovery']=True
    later(gizmo_projection)

def gizmo_projection():
    global card, camera
    properties={'CreateNodeArgsPropSettingsOpened':NatronEngine.BoolNodeCreationProperty(True)}
    card=app.createNode('fr.inria.built-in.Card',-1,app,properties)
    card.setScriptName('GizmoProjectionCard');card.getParam('size').set(.5,.5)
    source=app.createNode('net.sf.openfx.ConstantPlugin');source.getParam('color').set(.3,.1,.05,1.)
    parent=app.createNode('fr.inria.built-in.Axis')
    assert card.connectInput(0,source) and card.connectInput(1,parent)
    camera=app.createNode('fr.inria.built-in.Camera')
    renderer.disconnectInput(0);renderer.disconnectInput(1)
    assert renderer.connectInput(0,card) and renderer.connectInput(1,camera)
    camera_button.setChecked(True);app.setSelection([card]);refresh();key(Qt.Key_E)
    def rings():
        refresh();center=plot.property('gizmoCenter');polygons=plot.property('gizmoRings')
        assert len(polygons)==3 and all(len(ring)==97 for ring in polygons)
        for ring in polygons:
            assert all(math.hypot((p-center).x(),(p-center).y())<=55.000001 for p in ring)
            for i in range(48):
                midpoint=(ring[i]+ring[i+48])*.5
                assert math.hypot((midpoint-center).x(),(midpoint-center).y())<1e-7, 'Rotation ring shifted away from its pivot'
        return [[(p-center).x() for p in ring]+[(p-center).y() for p in ring] for ring in polygons]
    def close_shape(first,last):
        assert all(abs(a-b)<1e-7 for before,after in zip(first,last) for a,b in zip(before,after)), 'Camera position, lens or parent scale distorted the Gizmo'
    initial=rings()
    # A front-facing ring is circular even below a nonuniformly scaled parent.
    parent.getParam('scale').set(4.,.25,1.);close_shape(initial,rings())
    center=plot.property('gizmoCenter')
    assert all(abs(math.hypot((p-center).x(),(p-center).y())-55)<1e-7 for p in plot.property('gizmoRings')[2])
    camera.getParam('translate').set(.7,.35,5.);close_shape(initial,rings())
    camera.getParam('focal').setValue(70.);close_shape(initial,rings())
    camera.getParam('translate').set(.07,.035,.5);close_shape(initial,rings())
    capture('gizmo-close-camera')
    # Picking the displayed arc must still edit the requested Euler axis.
    rotate_drag(15);assert abs(card.getParam('rotate').getValue(2)-15)<.01
    action('Undo').trigger();refresh();assert abs(card.getParam('rotate').getValue(2))<1e-8
    camera.getParam('translate').set(0.,0.,5.)
    camera.getParam('orthographic').setValue(True);close_shape(initial,rings())
    camera.getParam('orthoWidth').setValue(8.);close_shape(initial,rings())
    camera.getParam('orthographic').setValue(False)
    camera.getParam('focal').setValue(50.)
    camera.getParam('rotate').set(8.,18.,12.)
    rx,ry,rz=map(math.radians,(8.,18.,12.))
    camera.getParam('translate').set(5*(math.cos(rz)*math.sin(ry)*math.cos(rx)+math.sin(rz)*math.sin(rx)),5*(math.sin(rz)*math.sin(ry)*math.cos(rx)-math.cos(rz)*math.sin(rx)),5*math.cos(ry)*math.cos(rx))
    rings()
    arc=plot.property('gizmoRings')[2]
    drag(arc[12],arc[20]);refresh()
    assert abs(card.getParam('rotate').getValue(2)-30)<.01, 'Oblique ring picking disagreed with the displayed arc'
    action('Undo').trigger();refresh();assert abs(card.getParam('rotate').getValue(2))<1e-8
    capture('gizmo-rotated-camera')
    # Test real inspection navigation, retaining a constant overlay shape when
    # panning/dollying and a centered, bounded shape while orbiting.
    camera_button.setChecked(False)
    assert QMetaObject.invokeMethod(plot,'resetView',Qt.DirectConnection)
    inspection=rings();center=QPointF(plot.width()/2,plot.height()/2)
    drag(center,center+QPointF(40,18),button=Qt.MiddleButton);close_shape(inspection,rings())
    QApplication.sendEvent(plot,QWheelEvent(center,QPointF(plot.mapToGlobal(center.toPoint())),QPoint(),QPoint(0,480),Qt.NoButton,Qt.NoModifier,Qt.NoScrollPhase,False))
    close_shape(inspection,rings())
    drag(center,center+QPointF(70,-25),modifiers=Qt.AltModifier);rings()
    capture('gizmo-orbit-camera')
    key(Qt.Key_W);refresh()
    move_center=plot.property('gizmoCenter')
    move_vectors=[p-move_center for p in plot.property('gizmoHandles')]
    assert all(math.hypot(v.x(),v.y())<=65.000001 for v in move_vectors)
    drag(center,center+QPointF(25,-10),button=Qt.MiddleButton);refresh()
    last_center=plot.property('gizmoCenter')
    assert all(math.hypot((p-last_center-v).x(),(p-last_center-v).y())<1e-7 for p,v in zip(plot.property('gizmoHandles'),move_vectors))
    before=card.getParam('translate').getValue(0);axis_drag(0,12)
    assert abs(card.getParam('translate').getValue(0)-before)>.001
    action('Undo').trigger();refresh();assert abs(card.getParam('translate').getValue(0)-before)<1e-8
    report['stages']['gizmo_projection_pan_dolly_orbit_parent_scale_lens_rotation_pick_undo']=True
    later(framing_setup)

def framed_pixels():
    image=plot.property('capturedFrame')
    width,height=image.width(),image.height()
    pixels=bytes(image.constBits());background=pixels[:4]
    occupied=[(x,y) for y in range(height) for x in range(width)
              if pixels[(y*width+x)*4:(y*width+x+1)*4]!=background]
    assert occupied, 'Framing left the geometry outside the preview'
    left=min(p[0] for p in occupied);right=max(p[0] for p in occupied)
    top=min(p[1] for p in occupied);bottom=max(p[1] for p in occupied)
    assert left>width*.01 and right<width*.99 and top>height*.01 and bottom<height*.99, 'Framed geometry was clipped'
    assert max((right-left)/width,(bottom-top)/height)>.45, 'Framing left excessive empty space'

def check_pivot(expected):
    actual=plot.property('inspectionPivot')
    assert len(actual)==3 and all(abs(a-b)<1e-8 for a,b in zip(actual,expected)), (actual,expected)

def framing_setup():
    global framing_mesh,framing_card,framing_camera_values
    framing_mesh=app.createNode('fr.inria.built-in.ReadGeo')
    framing_mesh.setScriptName('FramingAnimatedMesh')
    framing_mesh.getParam('file').setValue(Path('Tests/fixtures/scene3d/animated-cube.fbx').resolve().as_posix())
    framing_mesh.getParam('translate').set(17.,-8.,4.)
    framing_mesh.getParam('scale').set(.4,.4,.4)
    framing_mesh.getParam('readAnimation').setValue(True)
    framing_card=app.createNode('fr.inria.built-in.Card')
    framing_card.getParam('size').set(1.,1.)
    framing_card.getParam('translate').set(-40.,20.,-15.)
    assert framing_card.connectInput(0,card.getInput(0))
    scene=app.createNode('fr.inria.built-in.Scene')
    assert scene.connectInput(0,framing_mesh) and scene.connectInput(1,framing_card)
    renderer.disconnectInput(0);assert renderer.connectInput(0,scene)
    assert QMetaObject.invokeMethod(plot,'resetView',Qt.DirectConnection)
    camera_button.setChecked(True);app.setSelection([framing_mesh])
    framing_camera_values={name:[camera.getParam(name).getValue(d) for d in range(3)] for name in ('translate','rotate','scale')}
    # Press F immediately after seeking: framing must refresh the evaluated FBX,
    # even before the periodic Viewer refresh has run.
    app.getViewer(viewer.getScriptName()).seek(25);key(Qt.Key_F)
    assert not plot.property('lookThroughCamera')
    check_pivot([17.4,-8.,4.4]);framed_pixels();capture('framed-animated-mesh')
    center=QPointF(plot.width()/2,plot.height()/2)
    drag(center,center+QPointF(35,-15),modifiers=Qt.AltModifier)
    check_pivot([17.4,-8.,4.4])
    app.getViewer(viewer.getScriptName()).seek(1);key(Qt.Key_F)
    check_pivot([17.,-8.,4.]);framed_pixels()
    for name,values in framing_camera_values.items():
        assert [camera.getParam(name).getValue(d) for d in range(3)]==values, 'Framing modified the render camera'
    # Empty selection frames geometry rather than a fixed world origin. A
    # selected mesh continues to frame only that node, excluding the outlier.
    app.setSelection([]);key(Qt.Key_F)
    check_pivot([-11.55,6.05,-5.3])
    for node in (framing_mesh,framing_card):
        info=objects(node)[0]
        assert info['visible'] and 0<info['icon'].x()<plot.width() and 0<info['icon'].y()<plot.height()
    capture('framed-entire-scene')
    report['stages']['frame_current_fbx_bounds_orbit_selection_all_scene_camera_independence']=True
    app.setSelection([framing_mesh]);plot.setFixedSize(1000,240)
    later(framing_wide)

def framing_wide():
    key(Qt.Key_F);check_pivot([17.,-8.,4.]);framed_pixels()
    capture('framed-wide-view')
    plot.setFixedSize(320,660)
    later(framing_tall)

def framing_tall():
    key(Qt.Key_F);check_pivot([17.,-8.,4.]);framed_pixels()
    capture('framed-tall-view')
    report['stages']['frame_wide_tall_viewport_fit']=True
    framing_mesh.getParam('translate').set(0.,0.,0.)
    framing_mesh.getParam('scale').set(.0005,.0005,.0005)
    framing_mesh.getParam('readAnimation').setValue(False)
    plot.setFixedSize(1000,300)
    later(framing_tiny)

def framing_tiny():
    key(Qt.Key_F);check_pivot([0.,0.,0.]);framed_pixels()
    before=plot.property('inspectionDistance')
    assert .002<=before<.01, before
    capture('framed-millimeter-mesh')
    center=QPointF(plot.width()/2,plot.height()/2)
    QApplication.sendEvent(plot,QWheelEvent(center,QPointF(plot.mapToGlobal(center.toPoint())),QPoint(),QPoint(0,120),Qt.NoButton,Qt.NoModifier,Qt.NoScrollPhase,False))
    assert plot.property('inspectionDistance')<=before, 'Dollying a tiny framed mesh moved the camera backward'
    framing_mesh.getParam('readAnimation').setValue(True)
    framing_mesh.getParam('animationTake').setValue('Hide')
    app.getViewer(viewer.getScriptName()).seek(25);key(Qt.Key_F)
    check_pivot([0.,0.,0.]);assert plot.property('inspectionDistance')==7.
    app.setSelection([]);key(Qt.Key_F)
    check_pivot([-40.,20.,-15.]);framed_pixels()
    report['stages']['frame_millimeter_mesh_dolly_empty_pose_bounds']=True
    later(read_camera)

def read_camera():
    global imported_camera
    plot.setFixedSize(1000,440)
    imported_camera=app.createNode('fr.inria.built-in.ReadCamera')
    imported_camera.setScriptName('ImportedViewerCamera');imported_camera.setLabel('Imported FBX Camera')
    camera_file=Path('Tests/fixtures/scene3d/animated-camera.fbx').resolve().as_posix()
    imported_camera.getParam('file').setValue(camera_file)
    imported_camera.getParam('animationTake').setValue('CameraMove')
    surface=app.createNode('fr.inria.built-in.Card');surface.getParam('size').set(4.,4.)
    assert surface.connectInput(0,card.getInput(0))
    renderer.disconnectInput(0);renderer.disconnectInput(1)
    assert renderer.connectInput(0,surface) and renderer.connectInput(1,imported_camera)
    camera_button.setChecked(True);app.setSelection([surface])
    frames=[]
    for number,x in ((1,0.),(13,.3),(25,.6)):
        app.getViewer(viewer.getScriptName()).seek(number);refresh()
        info=objects(imported_camera)[0]
        assert info['kind']=='ReadCamera' and info['renderCamera'] and all(abs(a-b)<1e-8 for a,b in zip(info['worldOrigin'],[x,0.,6.]))
        frames.append(bytes(plot.property('capturedFrame').constBits()))
    assert frames[0]!=frames[1] and frames[1]!=frames[2]
    app.getViewer(viewer.getScriptName()).seek(13);app.setSelection([imported_camera]);key(Qt.Key_F)
    info=objects(imported_camera)[0];assert len(info['frustum'])==4
    pick_object(info);key(Qt.Key_W)
    axis_drag(0,12);assert imported_camera.getParam('translate').getValue(0)>.001
    action('Undo').trigger();refresh();assert imported_camera.getParam('translate').getValue(0)==0.
    origin=objects(imported_camera)[0]['worldOrigin'];key(Qt.Key_E);rotate_drag(12)
    assert abs(imported_camera.getParam('rotate').getValue(2))>1
    assert all(abs(a-b)<1e-8 for a,b in zip(objects(imported_camera)[0]['worldOrigin'],origin)), 'ReadCamera offset rotation moved its position'
    action('Undo').trigger();refresh();key(Qt.Key_R);axis_drag(0,12)
    assert abs(imported_camera.getParam('scale').getValue(0)-math.exp(12/65))<1e-8
    action('Undo').trigger();refresh();capture('imported-camera-frustum')
    for name,value,message in (('cameraName','MissingCamera','camera not found'),('animationTake','MissingTake','take not found'),('file',Path('Tests/fixtures/scene3d/colored-cube.fbx').resolve().as_posix(),'no cameras')):
        knob=imported_camera.getParam(name);original=knob.getValue();knob.setValue(value)
        assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
        assert message in plot.property('renderError') and not plot.property('gizmoHandles'),plot.property('renderError')
        knob.setValue(original);refresh()
    # Use the imported camera as a fixed projector, with a separate native
    # render camera so the image must stay unchanged when the timeline moves.
    projected=app.createNode('fr.inria.built-in.Project3D')
    assert projected.connectInput(0,card.getInput(0)) and projected.connectInput(1,imported_camera)
    projected.getParam('referenceFrame').setValue(13)
    surface.disconnectInput(0);assert surface.connectInput(0,projected)
    native_camera=app.createNode('fr.inria.built-in.Camera');native_camera.getParam('translate').set(0.,0.,6.)
    renderer.disconnectInput(1);assert renderer.connectInput(1,native_camera)
    camera_button.setChecked(True);app.getViewer(viewer.getScriptName()).seek(13);refresh()
    frozen=bytes(plot.property('capturedFrame').constBits())
    app.getViewer(viewer.getScriptName()).seek(25);refresh()
    assert bytes(plot.property('capturedFrame').constBits())==frozen
    info=objects(imported_camera)[0]
    assert info['projectionCamera'] and not info['editable'] and info['time']==13 and not plot.property('gizmoHandles')
    projected.getParam('freeze').setValue(False);refresh()
    assert objects(imported_camera)[0]['editable'] and objects(imported_camera)[0]['time']==25
    assert bytes(plot.property('capturedFrame').constBits())!=frozen
    key(Qt.Key_F);capture('imported-camera-projection')
    assert app.saveProjectAs(str((output/'imported-camera-scene.ntp').resolve()))
    report['stages']['readcamera_timeline_frustum_pick_move_rotate_scale_undo_errors_fixed_live_projection_save']=True
    later(lighting_controls)

def lighting_controls():
    material=app.createNode('fr.inria.built-in.PBRMaterial')
    material.getParam('baseColor').set(.5,.3,.1,1.)
    material.getParam('roughness').setValue(.6)
    surface=app.createNode('fr.inria.built-in.Card');surface.getParam('size').set(4.,4.)
    assert surface.connectInput(0,material)
    scene=app.createNode('fr.inria.built-in.Scene');light=app.createNode('fr.inria.built-in.Light')
    light.getParam('translate').set(.3,.2,2.)
    assert scene.connectInput(0,surface) and scene.connectInput(1,light)
    renderer.disconnectInput(0);assert renderer.connectInput(0,scene)
    light_camera=app.createNode('fr.inria.built-in.Camera')
    renderer.disconnectInput(1);assert renderer.connectInput(1,light_camera)
    camera_button.setChecked(True);app.setSelection([light]);refresh()
    illuminated=bytes(plot.property('capturedFrame').constBits())
    assert objects(light)[0]['kind']=='Light' and len(plot.property('gizmoHandles'))==3
    info=objects(light)[0];pick_object(info);key(Qt.Key_W)
    axis_drag(0,12);assert light.getParam('translate').getValue(0)>.3
    action('Undo').trigger();refresh();assert light.getParam('translate').getValue(0)==.3
    key(Qt.Key_E);rotate_drag(12)
    assert abs(light.getParam('rotate').getValue(2))>1
    action('Undo').trigger();refresh()
    light.getParam('intensity').setValue(0.);refresh()
    assert bytes(plot.property('capturedFrame').constBits())!=illuminated
    light.getParam('intensity').setValue(1.);refresh();key(Qt.Key_F);capture('lighting-material-light-gizmo')
    assert app.saveProjectAs(str((output/'lighting-scene.ntp').resolve()))
    report['stages']['pbr_light_viewer_pick_move_rotate_undo_save']=True
    finish()

QTimer.singleShot(0, guarded(setup))
