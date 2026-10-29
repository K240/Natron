"""Verify Dope Sheet raster rendering and editing without an OpenGL widget."""
import json
import os
from pathlib import Path
import traceback

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Dope Sheet smoke started', encoding='utf-8')

from PySide6.QtCore import QEvent, QMetaObject, QPointF, QRectF, QTimer, Qt
from PySide6.QtGui import QAction, QColor, QImage, QMouseEvent, QWheelEvent
from PySide6.QtWidgets import QApplication, QDialog, QLabel, QTreeWidget, QVBoxLayout
import NatronEngine

report = {'ok': False, 'stages': {}}
retained, retained_actions = {}, {}
plot = source = tree = pane = None
finished = False
allowed_dialog = None


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(w), w) for w in values)
    return values


def plain(value):
    if isinstance(value, QPointF):
        return [value.x(), value.y()]
    if isinstance(value, QRectF):
        return [value.x(), value.y(), value.width(), value.height()]
    if isinstance(value, QColor):
        return list(value.getRgb())
    if isinstance(value, dict):
        return {key: plain(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [plain(item) for item in value]
    return value


def guarded(function):
    def run():
        if finished:
            return
        report['active_stage'] = function.__name__
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def finish(error=None):
    global finished
    if finished:
        return
    finished = True
    if error:
        report['error'] = str(error)
        report['traceback'] = traceback.format_exc()
    viewers = [w for w in widgets() if w.property('graphicsBackend') == 'Vulkan']
    report['graphics_errors'] = [w.property('graphicsError') for w in viewers]
    report['validation_errors'] = [w.property('validationErrors') for w in viewers]
    report['devices'] = [w.property('gpuDeviceName') for w in viewers]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal()]
    report['legacy_gl_widgets'] = [w.objectName() for w in widgets() if w.inherits('QOpenGLWidget')]
    report['ok'] = not error and bool(viewers) and not any(report['graphics_errors']) and not any(report['validation_errors']) and not report['unexpected_dialogs'] and not report['legacy_gl_widgets']
    Path('build/dope-sheet-gui-result.json').write_text(json.dumps(plain(report), indent=2), encoding='utf-8')
    print('DOPE_SHEET_GUI_SMOKE', json.dumps(plain(report)))
    QApplication.exit(0)


def capture(name='frame'):
    frame = plot.grab().toImage()
    assert not frame.isNull() and frame.save(f'build/dope-sheet-gui-{name}.png')
    return frame


def pen(kind, position, button, buttons, modifiers=Qt.NoModifier):
    event = QMouseEvent(kind, position, QPointF(plot.mapToGlobal(position.toPoint())), button, buttons, modifiers)
    QApplication.sendEvent(plot, event)


def drag(start, end, button=Qt.LeftButton, screenshot=None, trace=None):
    plot.setFocus(Qt.OtherFocusReason)
    pen(QEvent.MouseButtonPress, start, button, button)
    if trace:
        plot.repaint()
        report.setdefault('clip_inputs', {})[trace] = {'start': start, 'end': end,
            'press_state': plot.property('dopeSheetEventState')}
    pen(QEvent.MouseMove, start+(end-start)*.5, Qt.NoButton, button)
    pen(QEvent.MouseMove, end, Qt.NoButton, button)
    if screenshot:
        plot.repaint()
        capture(screenshot)
    pen(QEvent.MouseButtonRelease, end, button, Qt.NoButton)
    if trace:
        report['clip_inputs'][trace].update({'reader': reader_values(),
            'offset': offset.getParam('timeOffset').get(), 'ranges': ranges()})


def key(time, selected=None):
    candidates = [item for item in plot.property('dopeSheetDrawnKeys')
                  if item['texture'] < 16 and abs(item['time']-time) < 1e-8 and (selected is None or item['texture'] % 2 == int(selected))]
    assert candidates, (time, selected, plot.property('dopeSheetDrawnKeys'))
    return candidates[0]


def time_x(time):
    viewport = plot.property('dopeSheetViewport')
    return (time-viewport.x())/viewport.width()*plot.width()


def key_times():
    color = source.getParam('color')
    values = [color.getKeyTime(i, 0) for i in range(color.getNumKeys(0))]
    assert all(value[0] for value in values)
    return [value[1] for value in values]


def invoke(slot):
    assert QMetaObject.invokeMethod(plot, slot, Qt.DirectConnection), slot


def action(prefix):
    for widget in widgets():
        for item in widget.actions()+widget.findChildren(QAction):
            retained_actions[id(item)] = item
    found = [item for item in retained_actions.values() if item.text().replace('&', '').startswith(prefix) and item.isEnabled()]
    assert found, f'Missing enabled {prefix} action'
    return found[0]


def inspect_icon(frame, item):
    names = ('interp_constant', 'interp_linear', 'interp_curve', 'interp_break', 'interp_curve_c',
             'interp_curve_h', 'interp_curve_r', 'interp_curve_z', 'keyframe_node_root')
    name = names[item['texture']//2]+('_selected' if item['texture'] % 2 else '')
    image = QImage(f'Gui/Resources/Images/{name}.png').scaled(14, 14, Qt.KeepAspectRatio, Qt.SmoothTransformation)
    assert not image.isNull()
    rectangle = item['rect']
    dpr = frame.width()/plot.width()
    samples = []
    for y in range(2, 12):
        for x in range(2, 12):
            expected = image.pixelColor(x, y)
            if expected.alpha() != 255:
                continue
            actual = frame.pixelColor(int((rectangle.x()+x+.5)*dpr), int((rectangle.y()+y+.5)*dpr))
            difference = max(abs(a-b) for a, b in zip(actual.getRgb()[:3], expected.getRgb()[:3]))
            samples.append(difference)
    assert len(samples) > 8 and max(samples) <= 3, (name, samples)
    return {'name': name, 'opaque_pixels': len(samples), 'maximum_error': max(samples)}


def inspect_selection():
    assert plot.property('dopeSheetSelectedKeyCount') == 1
    frame = capture('selected')
    selected = key(5, True)
    report['stages']['selection'] = inspect_icon(frame, selected)
    start = selected['rect'].center()
    drag(start, start+QPointF(time_x(7)-time_x(5), 0))
    QTimer.singleShot(500, guarded(inspect_move))


def inspect_move():
    assert key_times() == [1, 7, 10], key_times()
    report['stages']['move'] = {'times': key_times()}
    capture('moved')
    action('Undo').trigger()
    QTimer.singleShot(400, guarded(inspect_undo))


def inspect_undo():
    assert key_times() == [1, 5, 10], key_times()
    report['stages']['undo'] = {'times': key_times()}
    action('Redo').trigger()
    QTimer.singleShot(400, guarded(inspect_redo))


def inspect_redo():
    assert key_times() == [1, 7, 10], key_times()
    report['stages']['redo'] = {'times': key_times()}
    position = key(7)['rect'].center()
    drag(position, position)
    QTimer.singleShot(200, guarded(copy_and_scrub))


def copy_and_scrub():
    assert plot.property('dopeSheetSelectedKeyCount') == 1
    report['stages']['copy'] = {'selection': [item.text(0) for item in tree.selectedItems()]}
    invoke('copySelectedKeyframes')
    drag(QPointF(time_x(app.timelineGetTime()), plot.height()-4), QPointF(time_x(8), plot.height()-4))
    QTimer.singleShot(500, guarded(inspect_scrub))


def inspect_scrub():
    assert app.timelineGetTime() == plot.property('dopeSheetCurrentFrame') == 8, app.timelineGetTime()
    frame = capture('scrub')
    dpr = frame.width()/plot.width()
    colors = [frame.pixelColor(round(time_x(8)*dpr)+offset, round(93*dpr)) for offset in range(-2, 3)]
    assert any(c.red() > 150 and c.red() > c.green()*1.2 and c.blue() < 90 for c in colors), [c.getRgb() for c in colors]
    report['stages']['scrub'] = {'frame': app.timelineGetTime(), 'playhead_rgb': [c.getRgb() for c in colors]}
    item = tree.itemAt(5, round(key(7)['rect'].center().y()))
    assert item is not None and item.text(0) == 'r', item.text(0) if item else None
    tree.clearSelection()
    item.setSelected(True)
    report['stages']['paste_destination'] = [item.text(0) for item in tree.selectedItems()]
    invoke('pasteKeyframesRelative')
    QTimer.singleShot(500, guarded(inspect_paste))


def inspect_paste():
    assert key_times() == [1, 7, 8, 10], key_times()
    assert abs(source.getParam('color').getValueAtTime(8, 0)-.7) < 1e-9
    assert all(source.getParam('color').getNumKeys(dim) == 0 for dim in (1, 2, 3))
    report['stages']['paste'] = {'times': key_times()}
    empty = QPointF(plot.width()*.75, 93)
    drag(empty, empty)
    QTimer.singleShot(200, guarded(select_pasted_key))


def select_pasted_key():
    assert plot.property('dopeSheetSelectedKeyCount') == 0
    item = key(8)
    position = item['rect'].center()
    drag(position, position)
    QTimer.singleShot(200, guarded(delete_pasted_key))


def delete_pasted_key():
    assert plot.property('dopeSheetSelectedKeyCount') == 1, plot.property('dopeSheetSelectedKeyCount')
    invoke('deleteSelectedKeyframes')
    QTimer.singleShot(500, guarded(inspect_delete))


def inspect_delete():
    assert key_times() == [1, 7, 10], key_times()
    report['stages']['delete'] = {'times': key_times()}
    action('Undo').trigger()
    QTimer.singleShot(400, guarded(inspect_rectangle))


def inspect_rectangle():
    assert key_times() == [1, 7, 8, 10], key_times()
    first, last = key(1)['rect'].center(), key(10)['rect'].center()
    drag(first+QPointF(-16, -12), last+QPointF(16, 12), screenshot='rectangle')
    QTimer.singleShot(500, guarded(inspect_bbox))


def inspect_bbox():
    assert plot.property('dopeSheetSelectedKeyCount') == 4, plot.property('dopeSheetSelectedKeyCount')
    bbox = plot.property('dopeSheetSelectionBounds')
    assert bbox.width() > 9
    report['stages']['rectangle'] = {'selected_keys': 4, 'bounds': bbox}
    capture('bbox')
    viewport = plot.property('dopeSheetViewport')
    y = (viewport.bottom()-bbox.center().y())/viewport.height()*plot.height()
    start = QPointF(time_x(bbox.right())+4, y)
    drag(start, start+QPointF(time_x(1)-time_x(0), 0))
    QTimer.singleShot(500, guarded(inspect_transform))


def inspect_transform():
    times = key_times()
    assert len(times) == 4 and times[-1]-times[0] > 9.5 and abs(times[0]-1) < .03, times
    assert all(abs(source.getParam('color').getValueAtTime(time, 0)-value) < 1e-9
               for time, value in zip(times, (.2, .7, .7, .35)))
    report['stages']['transform'] = {'times': times}
    action('Undo').trigger()
    QTimer.singleShot(400, guarded(start_interpolations))


def start_interpolations():
    global interpolations
    assert key_times() == [1, 7, 8, 10]
    invoke('onSelectedAllTriggered')
    interpolations = iter((('constantInterpSelectedKeyframes', 1), ('linearInterpSelectedKeyframes', 3),
                           ('catmullRomInterpSelectedKeyframes', 13), ('cubicInterpSelectedKeyframes', 9),
                           ('horizontalInterpSelectedKeyframes', 11), ('breakInterpSelectedKeyframes', 7),
                           ('smoothInterpSelectedKeyframes', 15)))
    QTimer.singleShot(200, guarded(next_interpolation))


def next_interpolation():
    global expected_texture
    case = next(interpolations, None)
    if case is None:
        for time in key_times():
            assert source.getParam('color').setInterpolationAtTime(time, NatronEngine.Natron.KeyframeTypeEnum.eKeyframeTypeFree, 0)
        QTimer.singleShot(300, guarded(inspect_free_interpolation))
        return
    slot, expected_texture = case
    invoke(slot)
    QTimer.singleShot(300, guarded(inspect_interpolation))


def inspect_free_interpolation():
    item = key(7, True)
    assert item['texture'] == 5
    report['stages']['interpolations'].append(inspect_icon(capture('interpolation'), item))
    QTimer.singleShot(300, guarded(start_navigation))


def start_navigation():
    assert plot.isVisible() and plot.isEnabled()
    capture('navigation_before')
    report['viewport_before_zoom_out'] = plot.property('dopeSheetViewport')
    send_wheel(-240)
    QTimer.singleShot(400, guarded(inspect_zoom_out))


def send_wheel(delta):
    position = QPointF(plot.width()*.47, plot.height()*.53)
    wheel = QWheelEvent(position, QPointF(plot.mapToGlobal(position.toPoint())), QPointF().toPoint(),
                        QPointF(0, delta).toPoint(), Qt.NoButton, Qt.NoModifier, Qt.NoScrollPhase, False)
    QApplication.sendEvent(plot, wheel)


def inspect_zoom_out():
    before, after = report['viewport_before_zoom_out'], plot.property('dopeSheetViewport')
    assert after.width() > before.width() and after.height() == before.height(), (before, after)
    report['stages']['zoom_out'] = {'before': before, 'after': after}
    report['viewport_before_zoom'] = after
    send_wheel(120)
    QTimer.singleShot(400, guarded(inspect_zoom))


def inspect_interpolation():
    item = key(7, True)
    assert item['texture'] == expected_texture, item
    report['stages'].setdefault('interpolations', []).append(inspect_icon(capture('interpolation'), item))
    next_interpolation()


def inspect_zoom():
    before, after = report['viewport_before_zoom'], plot.property('dopeSheetViewport')
    assert after.width() < before.width() and after.height() == before.height(), (before, after)
    report['stages']['zoom'] = {'before': before, 'after': after}
    report['viewport_before_pan'] = after
    start = QPointF(plot.width()*.47, plot.height()*.53)
    drag(start, start+QPointF(27, 0), Qt.MiddleButton)
    QTimer.singleShot(400, guarded(inspect_pan))


def inspect_pan():
    before, after = report['viewport_before_pan'], plot.property('dopeSheetViewport')
    assert after.x() < before.x() and abs(after.width()-before.width()) < 1e-9
    report['stages']['pan'] = {'before': before, 'after': after}
    capture('navigation')
    start_clip_tests()


def start_clip_tests():
    global reader, offset, frame_range, retime, group, child
    empty = QPointF(plot.width()*.75, plot.height()*.5)
    drag(empty, empty)
    inputs = Path('build/dope-sheet-input')
    inputs.mkdir(exist_ok=True)
    image = QImage(64, 48, QImage.Format_RGBA8888)
    image.fill(QColor(80, 120, 160))
    for frame in range(1, 11):
        assert image.save(str(inputs/f'sequence.{frame:04d}.png'))
    props = {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)}
    reader = app.createReader((inputs/'sequence.####.png').resolve().as_posix(), app, props)
    assert reader is not None
    offset = app.createNode('net.sf.openfx.timeOffset', -1, app, props)
    frame_range = app.createNode('net.sf.openfx.FrameRange', -1, app, props)
    retime = app.createNode('net.sf.openfx.Retime', -1, app, props)
    assert offset.connectInput(0, reader) and frame_range.connectInput(0, reader) and retime.connectInput(0, reader)
    offset.getParam('timeOffset').setValue(3)
    frame_range.getParam('frameRange').set(3, 12)
    group = app.createNode('fr.inria.built-in.Group', -1, app, props)
    child = app.createNode('net.sf.openfx.ConstantPlugin', -1, group, props)
    child.setLabel('DopeGroupChild')
    report['reader_parameters'] = [parameter.getScriptName() for parameter in reader.getParams()]
    QTimer.singleShot(900, guarded(animate_child))


def animate_child():
    for time, value in ((2, .2), (16, .7)):
        child.getParam('color').setValueAtTime(value, time, 0)
    tree.expandAll()
    pane.setCurrentIndex([pane.getTabLabel(i) for i in range(pane.count())].index('Dope Sheet'))
    invoke('centerOnSelection')
    QTimer.singleShot(900, guarded(inspect_clips))


def inspect_clips():
    assert plot.isVisible()
    capture()
    report['clip_tree'] = [tree.topLevelItem(i).text(0) for i in range(tree.topLevelItemCount())]
    report['stages']['clips'] = {'ranges': plot.property('dopeSheetRanges'),
                                'reader_values': {name: reader.getParam(name).get() for name in ('firstFrame', 'lastFrame', 'startingTime')},
                                'node_labels': [node.getLabel() for node in (reader, offset, frame_range, retime, group)]}
    assert ranges() == {reader.getLabel(): [1, 11], offset.getLabel(): [4, 14],
                        frame_range.getLabel(): [3, 12], retime.getLabel(): [1, 10], group.getLabel(): [2, 16]}
    frame = capture('clips')
    dpr = frame.width()/plot.width()
    for node in (reader, offset, frame_range, retime, group):
        first, last = ranges()[node.getLabel()]
        center = QPointF(time_x((first+last)/2), row(node).center().y())
        color = frame.pixelColor(int(center.x()*dpr), int(center.y()*dpr))
        outside_x = 5 if time_x(first) > 10 else plot.width()-5
        assert time_x(first) > 10 or time_x(last) < plot.width()-10
        outside = frame.pixelColor(int(outside_x*dpr), int(center.y()*dpr))
        assert max(abs(a-b) for a, b in zip(color.getRgb()[:3], outside.getRgb()[:3])) > 12, (node.getLabel(), color, outside)
    global clip_operations
    clip_operations = iter(('reader_left', 'reader_right', 'reader_slip', 'reader_move', 'offset_move', 'range_move', 'group_move'))
    next_clip_operation()


def ranges():
    return {item['name']: [item['range'].x(), item['range'].y()] for item in plot.property('dopeSheetRanges')}


def row(node):
    matches = tree.findItems(node.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
    assert len(matches) == 1, (node.getLabel(), len(matches))
    item = matches[0]
    tree.scrollToItem(item)
    return QRectF(tree.visualItemRect(item))


def reader_values():
    return {name: reader.getParam(name).get() for name in ('firstFrame', 'lastFrame', 'startingTime', 'timeOffset')}


def child_times():
    color = child.getParam('color')
    return [color.getKeyTime(i, 0)[1] for i in range(color.getNumKeys(0))]


def range_values():
    value = frame_range.getParam('frameRange').get()
    return [value.x, value.y]


def next_clip_operation():
    global clip_operation
    clip_operation = next(clip_operations, None)
    if clip_operation is None:
        start_visibility_tests()
        return
    if clip_operation == 'reader_slip':
        reader.getParam('firstFrame').setValue(3)
        reader.getParam('lastFrame').setValue(8)
        reader.getParam('startingTime').setValue(3)
    drag(QPointF(plot.width()*.9, plot.height()*.7), QPointF(plot.width()*.9, plot.height()*.7))
    QTimer.singleShot(200, guarded(perform_clip_operation))


def perform_clip_operation():
    global clip_before
    capture('clip_before')
    clip_before = {'reader': reader_values(), 'offset': offset.getParam('timeOffset').get(),
                   'frame_range': range_values(), 'child_times': child_times(), 'ranges': ranges()}
    node = {'reader_left': reader, 'reader_right': reader, 'reader_slip': reader, 'reader_move': reader,
            'offset_move': offset, 'range_move': frame_range, 'group_move': group}[clip_operation]
    first, last = ranges()[node.getLabel()]
    rectangle = row(node)
    start = QPointF(time_x((first+last)/2), rectangle.center().y())
    delta = 2
    if clip_operation == 'reader_left':
        start.setX(time_x(first)-2)
    elif clip_operation == 'reader_right':
        # Reader ranges have an exclusive end; the trim value is inclusive.
        start.setX(time_x(last)+2)
        delta = -2
    elif clip_operation == 'reader_slip':
        # The handle overlaps the following TimeOffset row; hit it where both
        # clips contain the time so pointer ordering cannot hide a wrong target.
        start.setX(time_x((first+last)/2+.5))
        start.setY(rectangle.bottom()+2)
        delta = 1
    end = start+QPointF(time_x(delta)-time_x(0), 0)
    if clip_operation == 'reader_right':
        end.setX(time_x(last-1+delta))
    drag(start, end, trace=clip_operation)
    QTimer.singleShot(400, guarded(inspect_clip_operation))


def inspect_clip_operation():
    values = reader_values()
    if clip_operation == 'reader_left':
        assert values['firstFrame'] == clip_before['reader']['firstFrame']+2, values
        assert values['lastFrame'] == clip_before['reader']['lastFrame']
    elif clip_operation == 'reader_right':
        assert values['lastFrame'] == clip_before['reader']['lastFrame']-2, values
        assert values['firstFrame'] == clip_before['reader']['firstFrame']
    elif clip_operation == 'reader_slip':
        assert values == {**clip_before['reader'], 'firstFrame': clip_before['reader']['firstFrame']-1,
                          'lastFrame': clip_before['reader']['lastFrame']-1, 'timeOffset': clip_before['reader']['timeOffset']+1}, values
        assert ranges()[reader.getLabel()] == clip_before['ranges'][reader.getLabel()]
        assert offset.getParam('timeOffset').get() == clip_before['offset'], report['clip_inputs'][clip_operation]
    elif clip_operation == 'reader_move':
        assert values['startingTime'] == clip_before['reader']['startingTime']+2, values
        assert ranges()[reader.getLabel()] == [value+2 for value in clip_before['ranges'][reader.getLabel()]]
    elif clip_operation == 'offset_move':
        assert offset.getParam('timeOffset').get() == clip_before['offset']+2
        assert ranges()[offset.getLabel()] == [value+2 for value in clip_before['ranges'][offset.getLabel()]]
    elif clip_operation == 'range_move':
        assert range_values() == [value+2 for value in clip_before['frame_range']]
    elif clip_operation == 'group_move':
        assert child_times() == [value+2 for value in clip_before['child_times']], child_times()
        assert ranges()[group.getLabel()] == [value+2 for value in clip_before['ranges'][group.getLabel()]]
    start = values['startingTime']
    assert ranges()[reader.getLabel()] == [start, start+values['lastFrame']-values['firstFrame']+1]
    capture(clip_operation)
    report['stages'][clip_operation] = {'reader': values, 'offset': offset.getParam('timeOffset').get(),
                                      'frame_range': range_values(), 'child_times': child_times(), 'ranges': ranges()}
    action('Undo').trigger()
    QTimer.singleShot(300, guarded(inspect_clip_undo))


def inspect_clip_undo():
    assert reader_values() == clip_before['reader'], (reader_values(), clip_before)
    assert offset.getParam('timeOffset').get() == clip_before['offset']
    assert range_values() == clip_before['frame_range']
    assert child_times() == clip_before['child_times']
    assert ranges() == clip_before['ranges'], (ranges(), clip_before)
    report['stages'][clip_operation]['undo_restored'] = True
    if clip_operation == 'reader_slip':
        reader.getParam('firstFrame').setValue(1)
        reader.getParam('lastFrame').setValue(10)
        reader.getParam('startingTime').setValue(1)
        reader.getParam('timeOffset').setValue(0)
    next_clip_operation()


def start_visibility_tests():
    global visibility_count, source_item, color_item
    visibility_count = len(plot.property('dopeSheetDrawnKeys'))
    matches = tree.findItems(source.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
    assert len(matches) == 1, (source.getLabel(), len(matches))
    source_item = matches[0]
    color_item = next(source_item.child(i) for i in range(source_item.childCount()) if source_item.child(i).text(0) == 'Color')
    color_item.setExpanded(False)
    QTimer.singleShot(200, guarded(inspect_collapsed_color))


def inspect_collapsed_color():
    capture('collapsed_color')
    count = len(plot.property('dopeSheetDrawnKeys'))
    assert count == visibility_count-4, (count, visibility_count)
    report['stages']['collapsed_color'] = {'glyphs': count}
    source_item.setExpanded(False)
    QTimer.singleShot(200, guarded(inspect_collapsed_node))


def inspect_collapsed_node():
    frame = capture('collapsed_node')
    count = len(plot.property('dopeSheetDrawnKeys'))
    assert count == visibility_count-8, (count, visibility_count)
    master = next(item for item in plot.property('dopeSheetDrawnKeys')
                  if item['texture'] == 16 and abs(item['time']-7) < 1e-8
                  and abs(item['rect'].center().y()-tree.visualItemRect(source_item).center().y()) < 1)
    report['stages']['collapsed_node'] = {'glyphs': count, 'icon': inspect_icon(frame, master)}
    drag(master['rect'].center(), master['rect'].center())
    QTimer.singleShot(200, guarded(inspect_selected_master))


def inspect_selected_master():
    frame = capture('selected_master')
    master = next(item for item in plot.property('dopeSheetDrawnKeys')
                  if item['texture'] == 17 and abs(item['time']-7) < 1e-8
                  and abs(item['rect'].center().y()-tree.visualItemRect(source_item).center().y()) < 1)
    report['stages']['selected_master'] = inspect_icon(frame, master)
    drag(QPointF(plot.width()*.9, plot.height()*.7), QPointF(plot.width()*.9, plot.height()*.7))
    QTimer.singleShot(200, guarded(hide_source))


def hide_source():
    source_item.setHidden(True)
    QTimer.singleShot(200, guarded(inspect_hidden_node))


def inspect_hidden_node():
    capture('hidden_node')
    count = len(plot.property('dopeSheetDrawnKeys'))
    assert count == visibility_count-12, (count, visibility_count)
    report['stages']['hidden_node'] = {'glyphs': count}
    source_item.setHidden(False)
    source_item.setExpanded(True)
    color_item.setExpanded(True)
    QTimer.singleShot(200, guarded(start_resize))


def start_resize():
    global editor, editor_heights, height_before_resize
    capture('visibility_restored')
    assert len(plot.property('dopeSheetDrawnKeys')) == visibility_count
    editor = plot.parentWidget().parentWidget()
    editor_heights = (editor.minimumHeight(), editor.maximumHeight())
    height_before_resize = plot.height()
    editor.setFixedHeight(152)
    QTimer.singleShot(300, guarded(inspect_resize))


def inspect_resize():
    frame = capture('resize')
    assert plot.height() < height_before_resize and plot.height() == tree.height()
    assert tree.verticalScrollBar().maximum() > 0
    report['stages']['resize'] = {'logical_size': [plot.width(), plot.height()], 'physical_size': [frame.width(), frame.height()],
                                 'viewport': plot.property('dopeSheetViewport'), 'font_point_size': plot.property('dopeSheetFontPointSize')}
    tree.verticalScrollBar().setValue(tree.verticalScrollBar().maximum())
    QTimer.singleShot(300, guarded(inspect_scroll))


def inspect_scroll():
    frame = capture('scroll')
    drawn = plot.property('dopeSheetDrawnKeys')
    assert any(item['rect'].top() < 0 for item in drawn)
    child_item = tree.findItems(child.getLabel(), Qt.MatchExactly | Qt.MatchRecursive, 0)
    assert len(child_item) == 1
    child_color = next(child_item[0].child(i) for i in range(child_item[0].childCount()) if child_item[0].child(i).text(0) == 'Color')
    red_item = next(child_color.child(i) for i in range(child_color.childCount()) if child_color.child(i).text(0) == 'r')
    rectangle = tree.visualItemRect(red_item)
    assert rectangle.top() >= 0 and rectangle.bottom() < plot.height()
    keys = [item for item in drawn if item['texture'] < 16 and abs(item['rect'].center().y()-rectangle.center().y()) < 1]
    assert sorted(round(item['time']) for item in keys) == [2, 16], keys
    report['stages']['scroll'] = {'scroll_value': tree.verticalScrollBar().value(), 'row': QRectF(rectangle),
                                 'keys': keys, 'icon': inspect_icon(frame, keys[0])}
    editor.setMinimumHeight(editor_heights[0])
    editor.setMaximumHeight(editor_heights[1])
    tree.verticalScrollBar().setValue(0)
    QTimer.singleShot(300, guarded(start_modal_test))


def start_modal_test():
    global allowed_dialog
    capture('modal_before')
    assert key_times() == [1, 7, 8, 10]
    plot.setFocus(Qt.OtherFocusReason)
    start = QPointF(plot.width()*.85, plot.height()*.6)
    pen(QEvent.MouseButtonPress, start, Qt.LeftButton, Qt.LeftButton)
    pen(QEvent.MouseMove, start+QPointF(10, 10), Qt.NoButton, Qt.LeftButton)
    capture('interrupted_drag')
    assert plot.property('dopeSheetEventState') != 0
    allowed_dialog = QDialog(plot.window())
    allowed_dialog.setWindowTitle('Dope Sheet smoke: interrupt drag')
    layout = QVBoxLayout(allowed_dialog)
    layout.addWidget(QLabel('The test closes this confirmation automatically.', allowed_dialog))
    allowed_dialog.setModal(True)
    allowed_dialog.show()
    allowed_dialog.activateWindow()
    QTimer.singleShot(600, guarded(inspect_modal_test))


def inspect_modal_test():
    assert allowed_dialog.isVisible() and allowed_dialog.isModal()
    capture('modal_interruption')
    assert plot.property('dopeSheetEventState') == 0
    assert key_times() == [1, 7, 8, 10] and child_times() == [2, 16]
    report['stages']['modal_interruption'] = {'event_state': plot.property('dopeSheetEventState'), 'keys_unchanged': True}
    report['expected_dialogs'] = [allowed_dialog.windowTitle()]
    allowed_dialog.close()
    QTimer.singleShot(300, guarded(finish_visibility_tests))


def finish_visibility_tests():
    assert not allowed_dialog.isVisible()
    assert plot.isVisible()
    capture()
    report['stages']['final'] = {'logical_size': [plot.width(), plot.height()], 'font_point_size': plot.property('dopeSheetFontPointSize'),
                               'ranges': ranges(), 'key_times': key_times(), 'child_times': child_times(),
                               'event_state': plot.property('dopeSheetEventState')}
    finish()


def activate():
    global plot, tree, pane
    candidates = [w for w in widgets() if w.property('graphicsRole') == 'DopeSheet']
    assert len(candidates) == 1
    plot = candidates[0]
    container, pane = plot.parentWidget(), None
    while container and pane is None:
        pane = app.getTabWidget(container.objectName())
        container = container.parentWidget()
    assert pane is not None
    labels = [pane.getTabLabel(i) for i in range(pane.count())]
    pane.setCurrentIndex(labels.index('Dope Sheet'))
    container = plot.parentWidget()
    while container and not container.findChildren(QTreeWidget):
        container = container.parentWidget()
    assert container
    trees = container.findChildren(QTreeWidget)
    assert len(trees) == 1
    tree = trees[0]
    tree.expandAll()
    assert QMetaObject.invokeMethod(plot, 'centerOnSelection', Qt.DirectConnection)
    QTimer.singleShot(1200, guarded(inspect))


def inspect():
    assert plot.isVisible() and plot.property('graphicsBackend') == 'Raster'
    assert not plot.inherits('QOpenGLWidget')
    frame = capture('before')
    def describe(item):
        return {'text': item.text(0), 'hidden': item.isHidden(), 'expanded': item.isExpanded(),
                'rect': QRectF(tree.visualItemRect(item)), 'children': [describe(item.child(i)) for i in range(item.childCount())]}
    report['hierarchy'] = [describe(tree.topLevelItem(i)) for i in range(tree.topLevelItemCount())]
    report['stages']['initial'] = {'viewport': plot.property('dopeSheetViewport'),
                                  'font_point_size': plot.property('dopeSheetFontPointSize'),
                                  'device_pixel_ratio': plot.devicePixelRatioF(),
                                  'logical_size': [plot.width(), plot.height()],
                                  'physical_size': [frame.width(), frame.height()],
                                  'command_count': plot.property('dopeSheetCommandCount'),
                                  'keys': plot.property('dopeSheetDrawnKeys'),
                                  'ranges': plot.property('dopeSheetRanges')}
    assert len(plot.property('dopeSheetDrawnKeys')) >= 6, report['stages']['initial']
    report['stages']['initial']['icon'] = inspect_icon(frame, key(5))
    position = key(5)['rect'].center()
    drag(position, position)
    QTimer.singleShot(500, guarded(inspect_selection))


def setup():
    global source
    app.addFormat('VulkanDopeSheetSmoke 64x48 1')
    app.getProjectParam('outputFormat').setValue('VulkanDopeSheetSmoke')
    app.getProjectParam('frameRange').set(1, 20)
    source = app.createNode('net.sf.openfx.ConstantPlugin', -1, app,
                            {'CreateNodeArgsPropSettingsOpened': NatronEngine.BoolNodeCreationProperty(True)})
    source.setLabel('DopeSource')
    viewer = app.createNode('fr.inria.built-in.Viewer')
    assert viewer.connectInput(0, source)
    app.selectNode(source, True)
    QTimer.singleShot(5000, guarded(animate))


def animate():
    # Initialize GUI curve signals before setting scripted animation.
    for time, value in ((1, .2), (5, .7), (10, .35)):
        source.getParam('color').setValueAtTime(value, time, 0)
    QTimer.singleShot(600, guarded(activate))


def detect_dialog():
    dialogs = [w.windowTitle() for w in QApplication.topLevelWidgets() if w.isVisible() and w.isModal() and w != allowed_dialog]
    if dialogs and not finished:
        finish(RuntimeError(f'Unexpected modal dialogs: {dialogs}'))


dialog_timer = QTimer()
dialog_timer.timeout.connect(detect_dialog)
dialog_timer.start(250)
QTimer.singleShot(0, guarded(setup))
