"""Verify shipped IO overlay geometry and pen editing in the Vulkan Viewer."""
import json
import os
from pathlib import Path
import time
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('IO DrawSuite smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QMetaObject, QPointF, Q_RETURN_ARG, QTimer, Qt
from PySide6.QtGui import QFocusEvent, QGuiApplication, QImage, QKeyEvent, QMouseEvent
from PySide6.QtWidgets import QApplication, QMenu
import NatronEngine

remaining_plugins = os.environ.get('NATRON_OFX_OVERLAY_SET') == 'remaining'
generator_plugins = os.environ.get('NATRON_OFX_OVERLAY_SET') == 'generator'
backend = os.environ.get('NATRON_TEST_VIEWER_BACKEND', 'Vulkan')
assert backend in ('Vulkan', 'Raster')
directory = Path(os.environ['NATRON_GUI_TEST_RESULT']).parent if generator_plugins else Path('build/raster-io-overlay-gui-smoke' if backend == 'Raster' else
                 'build/plugin-overlay-gui-smoke' if remaining_plugins else 'build/io-overlay-gui-smoke')
directory.mkdir(parents=True, exist_ok=True)
report = {'ok': False, 'cases': [], 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'backend': backend}
effect = reader = viewer_node = None
position = 0
cases = [('fr.inria.openfx.SeExpr', 'rectangle'),
         ('net.sf.openfx.SeNoise', 'transform'), ('net.sf.openfx.SeNoise', 'ramp'),
         ('fr.inria.openfx.OIIOText', 'position')]
if remaining_plugins:
    cases = [('net.sf.openfx.CopyRectanglePlugin', 'rectangle'),
             ('net.sf.openfx.TransformPlugin', 'transform'), ('net.sf.openfx.Ramp', 'ramp'),
             ('net.sf.openfx.Position', 'position'), ('net.sf.openfx.CornerPinPlugin', 'cornerpin'),
             ('net.sf.openfx.TrackerPM', 'tracking'), ('net.fxarena.openfx.Text', 'transform'),
             ('net.sf.openfx.ContactSheetOFX', 'contact')]
if generator_plugins:
    cases = [(identifier, 'rectangle') for identifier in (
        'net.sf.openfx.CheckerBoardPlugin', 'net.sf.openfx.ColorBars', 'net.sf.openfx.ColorWheel',
        'net.sf.openfx.ConstantPlugin', 'net.sf.openfx.Noise', 'net.sf.openfx.Radial',
        'net.sf.openfx.Rectangle', 'net.sf.openfx.Solid')]
current = None


def checkpoint(value):
    (directory / 'checkpoint.txt').write_text(value, encoding='utf-8')
    if remaining_plugins or generator_plugins:
        import runpy
        modules = runpy.run_path('tools/windows-module-audit.py')['loaded_modules']()
        selected = [name for name in modules if Path(name).name.lower() in ('opengl32.dll', 'libegl.dll')]
        report.setdefault('module_trace', []).append({'stage': value, 'modules': selected})
        (directory / 'module-trace.json').write_text(json.dumps(report['module_trace'], indent=2), encoding='utf-8')


def finish(error=None):
    if error:
        report.update(error=str(error), traceback=traceback.format_exc(), pending_case=current)
    widgets = [w for w in QApplication.allWidgets() if w.property('graphicsBackend') == backend
               and w.objectName().startswith('NatronVulkanViewer')]
    report['graphics_errors'] = [w.property('graphicsError') for w in widgets] if backend == 'Vulkan' else []
    if backend == 'Raster':
        report['fallback_reasons'] = [w.property('graphicsError') for w in widgets]
        report['raster_explicitly_requested'] = generator_plugins and os.environ.get('NATRON_TEST_VIEWER_BACKEND') == 'Raster'
        if not widgets or (not report['raster_explicitly_requested'] and not all(report['fallback_reasons'])) or any(w.property('sharedDeviceId') or w.property('nativeImageTransfers') for w in widgets):
            error = error or 'CPU overlay Viewer unexpectedly used a GPU or omitted its fallback reason'
    report['validation_errors'] = [w.property('validationErrors') for w in widgets]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['devices'] = [w.property('gpuDeviceName') for w in widgets]
    if remaining_plugins or generator_plugins or backend == 'Raster':
        import runpy
        modules = runpy.run_path('tools/windows-module-audit.py')['loaded_modules']()
        report['runtime_modules'] = modules
        report['opengl_loaded'] = any(Path(name).name.lower() == 'opengl32.dll' for name in modules)
        if backend == 'Raster':
            forbidden = {'opengl32.dll', 'qt6opengl.dll', 'qt6openglwidgets.dll', 'libegl.dll', 'libglesv2.dll'}
            if os.environ.get('NATRON_VULKAN_DISABLED') == '1':
                forbidden.add('vulkan-1.dll')
            if any(Path(name).name.lower() in forbidden for name in modules):
                error = error or 'CPU overlay process loaded a forbidden graphics module'
    if error and not report.get('error'):
        report['error'] = str(error)
    report['ok'] = not error and len(report['cases']) == len(cases) and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs']
    (directory / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('IO_OVERLAY_GUI_SMOKE', json.dumps(report))
    if generator_plugins:
        os.write(1, b'NATRON_GENERATOR_OVERLAY_EXIT_REQUESTED\n')
    # Avoid the application's close/save confirmation path in the test process.
    QApplication.exit(0)


def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def viewer():
    widgets = [w for w in QApplication.allWidgets() if w.property('graphicsBackend') == backend
               and w.objectName().startswith('NatronVulkanViewer') and w.isVisible()]
    assert len(widgets) == 1
    return widgets[0]


def point(widget, x, y):
    viewport = widget.property('canonicalViewport')
    return QPointF((x - viewport.x()) / viewport.width() * widget.width(),
                   (viewport.y() + viewport.height() - y) / viewport.height() * widget.height())


def surface(widget):
    if backend == 'Raster':
        return widget
    windows = [w for w in QGuiApplication.allWindows() if w.objectName() == widget.objectName() + '/surface']
    assert len(windows) == 1
    return windows[0]


def mouse(widget, kind, xy, button, buttons):
    local = point(widget, *xy)
    global_point = QPointF(widget.mapToGlobal(local.toPoint()))
    QGuiApplication.sendEvent(surface(widget), QMouseEvent(kind, local, local, global_point, button, buttons, Qt.NoModifier))


def drag(widget, start, end):
    mouse(widget, QEvent.MouseMove, start, Qt.NoButton, Qt.NoButton)
    mouse(widget, QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    mouse(widget, QEvent.MouseMove, end, Qt.NoButton, Qt.LeftButton)
    mouse(widget, QEvent.MouseButtonRelease, end, Qt.LeftButton, Qt.NoButton)


def capture(label, sample):
    checkpoint('capture ' + label + ' before grab')
    widget = viewer()
    frame = QMetaObject.invokeMethod(widget, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
    checkpoint('capture ' + label + ' after grab')
    if frame.isNull():
        report['surface_state'] = [{'name': w.objectName(), 'exposed': w.isExposed(), 'visible': w.isVisible(),
                                    'x': w.x(), 'y': w.y(), 'width': w.width(), 'height': w.height()}
                                   for w in QGuiApplication.allWindows()]
    assert not frame.isNull() and widget.property('nodeOverlaysAvailable'), f'Frame unavailable: widget={widget.width()}x{widget.height()}, null={frame.isNull()}, overlays={widget.property("nodeOverlaysAvailable")}'
    commands = widget.property('nodeOverlayCommandCount')
    # ContactSheet emits one complete line loop per foreground/shadow pass.
    minimum_commands = {'position': 1, 'contact': 2}.get(current['kind'], 8)
    assert commands >= minimum_commands, f'Missing {current["kind"]} drawing: {commands} commands'
    local = point(widget, *sample)
    x = round(local.x() * frame.width() / widget.width())
    y = round(local.y() * frame.height() / widget.height())
    radius = max(4, round(4 * widget.devicePixelRatioF()))
    pixels = [frame.pixelColor(px, py) for py in range(max(0, y-radius), min(frame.height(), y+radius+1))
              for px in range(max(0, x-radius), min(frame.width(), x+radius+1))]
    assert pixels and any(min(c.red(), c.green(), c.blue()) > 150 or (c.green() > 150 and c.red() < 60) for c in pixels), f'Handle absent at {sample}'
    filename = f'{position}-{current["kind"]}-{label}.png'
    assert frame.save(str(directory / filename))
    current[label] = {'commands': commands, 'sample': sample, 'artifact': filename,
                      'frame_size': [frame.width(), frame.height()], 'device_pixel_ratio': widget.devicePixelRatioF()}
    if generator_plugins:
        viewport = widget.property('canonicalViewport')
        current[label]['canonical_viewport'] = [viewport.x(), viewport.y(), viewport.width(), viewport.height()]


def xy(parameter):
    value = effect.getParam(parameter).get()
    return (value[0], value[1])


def start_case():
    global effect, current
    if position == len(cases):
        finish()
        return
    if effect is not None:
        viewer_node.disconnectInput(0)
        effect.destroy(False)
    identifier, kind = cases[position]
    current = {'plugin': identifier, 'kind': kind}
    checkpoint('create ' + identifier + ' ' + kind)
    effect = app.createNode(identifier, -1, app, {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    checkpoint('created')
    assert effect is not None, f'Plugin unavailable: {identifier}'
    if generator_plugins:
        effect.getParam('extent').set('size')
        effect.getParam('bottomLeft').set(-32, -24)
        effect.getParam('size').set(320, 240)
        assert effect.getParam('ofxDrawSuiteWarning') is None, 'Migrated generator was marked incompatible'
        current.update(parameter='bottomLeft', handle=(128, 96))
    elif remaining_plugins:
        configure_remaining(identifier, kind)
    elif kind == 'rectangle':
        effect.getParam('rod').set('size')
        effect.getParam('bottomLeft').set(-32, -24)
        effect.getParam('size').set(320, 240)
        effect.getParam('script').set('[0,0,0]')
        effect.getParam('alphaScript').set('1')
        effect.getParam('validate').set(True)
        current['parameter'] = 'bottomLeft'
        current['handle'] = (128, 96)
    elif kind == 'position':
        if effect.getMaxInputCount() > 0:
            assert effect.connectInput(0, reader)
        effect.getParam('position').set(120, 90)
        effect.getParam('text').set('')
        current['parameter'] = 'position'
        current['handle'] = (120, 90)
    else:
        assert effect.connectInput(0, reader)
        effect.getParam('mix').set(0)
        effect.getParam('transformInteractOpen').set(kind == 'transform')
        effect.getParam('rampInteractOpen').set(kind == 'ramp')
        effect.getParam('transformCenter').set(128, 96)
        effect.getParam('transformTranslate').set(14, -7)
        effect.getParam('transformRotate').set(27)
        effect.getParam('transformSkewX').set(.23)
        effect.getParam('transformSkewY').set(-.17)
        effect.getParam('transformScale').set(1.1, .8)
        if kind == 'transform':
            current['parameter'] = 'transformTranslate'
            current['handle'] = (142, 89)
        else:
            effect.getParam('rampType').set(0)
            effect.getParam('rampPoint0').set(72, 65)
            effect.getParam('rampPoint1').set(225, 180)
            current['parameter'] = 'rampPoint0'
            current['handle'] = (72, 65)
    assert viewer_node.connectInput(0, effect)
    checkpoint('connected ' + identifier + ' ' + kind)
    app.selectNode(effect, True)
    QTimer.singleShot(4000 if position == 0 else 1800, guarded(inspect))


def configure_remaining(identifier, kind):
    if effect.getMaxInputCount() > 0:
        assert effect.connectInput(0, reader)
    if kind == 'rectangle':
        effect.getParam('bottomLeft').set(-32, -24)
        effect.getParam('size').set(320, 240)
        current.update(parameter='bottomLeft', handle=(128, 96))
    elif kind == 'transform':
        if identifier == 'net.fxarena.openfx.Text':
            effect.getParam('text').set('')
        effect.getParam('center').set(128, 96)
        translation = effect.getParam('translate')
        if translation is not None:
            translation.set(14, -7)
        effect.getParam('rotate').set(27)
        effect.getParam('skewX').set(.23)
        effect.getParam('skewY').set(-.17)
        effect.getParam('scale').set(1.1, .8)
        current.update(parameter='translate' if translation else 'center', handle=(142, 89) if translation else (128, 96))
    elif kind == 'ramp':
        effect.getParam('type').set(0)
        effect.getParam('point0').set(72, 65)
        effect.getParam('point1').set(225, 180)
        effect.getParam('color0').set(0, 0, 0, 1)
        effect.getParam('color1').set(0, 0, 0, 1)
        current.update(parameter='point0', handle=(72, 65))
    elif kind == 'position':
        effect.getParam('translate').set(120, 90)
        current.update(parameter='translate', handle=(120, 90))
    elif kind == 'cornerpin':
        for i, xy in enumerate(((64, 48), (64, 192), (256, 192), (256, 48)), 1):
            effect.getParam('to' + str(i)).set(*xy)
        current.update(parameter='to1', handle=(64, 48))
    elif kind == 'tracking':
        effect.getParam('center').set(128, 96)
        current.update(parameter='center', handle=(128, 96))
    elif kind == 'contact':
        effect.getParam('resolution').set(320, 240)
        effect.getParam('rowsColumns').set(2, 2)
        effect.getParam('selection').set(True)
        effect.getParam('frameRange').set(1, 1)
        effect.getParam('selectionFrame').set(1)
        effect.getParam('rowOrder').set(0)
        current.update(handle=(160, 180))


def inspect():
    widget = viewer()
    capture('before', current['handle'])
    if current['kind'] == 'contact':
        global position
        current['draw_only'] = True
        report['cases'].append(current)
        position += 1
        QTimer.singleShot(800, guarded(start_case))
        return
    # The rectangle edge midpoint is outside its corner and centre handles.
    # Its pixels prove the plugin's outline was drawn, beyond host handles.
    if current['kind'] == 'rectangle':
        capture('outline', (288, 61))
    before = xy(current['parameter'])
    current['before_value'] = before
    start = current['handle']
    drag(widget, start, (start[0]+12, start[1]+9))
    QTimer.singleShot(800, guarded(inspect_edit))


def inspect_edit():
    widget = viewer()
    before = current['before_value']
    after = xy(current['parameter'])
    viewport = widget.property('canonicalViewport')
    tolerance = max(viewport.width()/widget.width(), viewport.height()/widget.height(), .1)
    assert abs(after[0]-before[0]-12) <= tolerance and abs(after[1]-before[1]-9) <= tolerance, f'Drag failed: {before} -> {after}'
    current.update(after_value=after, tolerance=tolerance)
    handle = current['handle']
    capture('after', (handle[0]+12, handle[1]+9))
    target = surface(widget)
    # Lose focus while Control is held, then confirm a fresh drag has no stale
    # centre-edit or aspect-ratio modifier. This exercises real plugin callbacks.
    QGuiApplication.sendEvent(target, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.ControlModifier))
    QGuiApplication.sendEvent(target, QFocusEvent(QEvent.FocusOut, Qt.OtherFocusReason))
    QGuiApplication.sendEvent(target, QFocusEvent(QEvent.FocusIn, Qt.OtherFocusReason))
    start = (handle[0]+12, handle[1]+9)
    drag(widget, start, (start[0]+5, start[1]+4))
    final = xy(current['parameter'])
    assert abs(final[0]-after[0]-5) <= tolerance and abs(final[1]-after[1]-4) <= tolerance, f'Focus left a modifier active: {after} -> {final}'
    current.update(final_value=final, focus_modifier_reset=True)
    capture('focus', (handle[0]+17, handle[1]+13))
    menus = [w for w in QApplication.allWidgets() if isinstance(w, QMenu) and w.title().replace('&', '') == 'Edit']
    assert len(menus) == 1, 'Edit menu is unavailable'
    undo = [a for a in menus[0].actions() if a.text().replace('&', '').startswith('Undo') and a.isEnabled()]
    assert len(undo) == 1, 'Active node undo action is unavailable'
    current.update(undo_action=undo[0].text(), undo_started=time.monotonic())
    undo[0].trigger()
    # An image render can defer value commits until its abort completes. Read
    # after returning to the event loop, as a real subsequent user action does.
    QTimer.singleShot(100, guarded(inspect_undo))


def inspect_undo():
    undone = xy(current['parameter'])
    current['undone_value'] = undone
    parameter = effect.getParam(current['parameter'])
    current['undo_key_counts'] = [parameter.getNumKeys(dimension) for dimension in range(2)]
    current['undo_time_values'] = [parameter.getValueAtTime(app.timelineGetTime(), dimension) for dimension in range(2)]
    current['undo_menu_state'] = [[action.text(), action.isEnabled()]
                                for menu in QApplication.allWidgets() if isinstance(menu, QMenu) and menu.title().replace('&', '') == 'Edit'
                                for action in menu.actions() if action.text().replace('&', '').startswith(('Undo', 'Redo'))]
    changed = max(abs(a-b) for a,b in zip(undone, current['final_value'])) > current['tolerance']
    if not changed and time.monotonic() - current['undo_started'] < 3:
        QTimer.singleShot(100, guarded(inspect_undo))
        return
    assert changed, 'Overlay edit was not undoable'
    menus = [w for w in QApplication.allWidgets() if isinstance(w, QMenu) and w.title().replace('&', '') == 'Edit']
    assert len(menus) == 1, 'Edit menu is unavailable'
    redo = [a for a in menus[0].actions() if a.text().replace('&', '').startswith('Redo') and a.isEnabled()]
    assert len(redo) == 1, 'Active node redo action is unavailable'
    current['redo_started'] = time.monotonic()
    redo[0].trigger()
    QTimer.singleShot(100, guarded(inspect_redo))


def inspect_redo():
    global position
    redone = xy(current['parameter'])
    final = current['final_value']
    restored = max(abs(a-b) for a,b in zip(redone, final)) < .001
    if not restored and time.monotonic() - current['redo_started'] < 3:
        QTimer.singleShot(100, guarded(inspect_redo))
        return
    assert restored, f'Overlay redo changed values: {final} -> {redone}'
    current.update(redone_value=redone, undo_redo=True)
    report['cases'].append(current)
    position += 1
    QTimer.singleShot(800, guarded(start_case))


def setup():
    global reader, viewer_node
    checkpoint('setup')
    app.addFormat('VulkanIOOverlaySmoke 320x240 1')
    app.getProjectParam('outputFormat').setValue('VulkanIOOverlaySmoke')
    app.getProjectParam('frameRange').set(1, 1)
    app.getProjectParam('autoPreviews').set(False)
    source = directory / 'source.ppm'
    source.write_bytes(b'P6\n320 240\n255\n' + bytes((0,0,0))*(320*240))
    reader = app.createReader(source.resolve().as_posix())
    checkpoint('reader')
    viewer_node = app.createNode('fr.inria.built-in.Viewer')
    window = viewer().window()
    window.hide()
    window.showNormal()
    window.move(0, 0)
    window.raise_()
    window.activateWindow()
    viewer().setFixedSize(640, 480)
    checkpoint('viewer')
    start_case()


QTimer.singleShot(0, guarded(setup))
