"""Insert a Dot through the real Ctrl-click bend point on a Reader/Viewer edge."""
import ctypes
import json
import os
from pathlib import Path
import traceback

from PySide6.QtCore import QEvent, QPointF, QTimer, Qt
from PySide6.QtGui import QImage, QKeyEvent, QMouseEvent, QUndoStack
from PySide6.QtWidgets import QApplication

ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
root = Path.cwd()
result = Path(os.environ['NATRON_GUI_TEST_RESULT'])
result.parent.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Dot insertion regression started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'stages': []}
retained = []
image = QImage(64, 48, QImage.Format_RGBA8888)
image.fill(Qt.gray)
fixture = result.parent / 'input.png'
assert image.save(str(fixture))
reader = app.createReader(fixture.as_posix())
assert reader
reader.setPosition(120, 30)
viewer = app.createNode('fr.inria.built-in.Viewer')
assert viewer.connectInput(0, reader)
viewer.setPosition(120, 340)


def stage(name, **values):
    report['stages'].append({'name': name, **values})
    result.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('DOT_INSERTION_STAGE', name)


def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def widgets():
    values = QApplication.allWidgets()
    retained.extend(values)
    return values


def finish(error=None):
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
    displays = [w for w in widgets() if w.property('graphicsBackend') == 'Vulkan']
    report['devices'] = [w.property('gpuDeviceName') for w in displays]
    report['graphics_errors'] = [w.property('graphicsError') for w in displays]
    report['validation_errors'] = [w.property('validationErrors') for w in displays]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets()
                                    if w.isVisible() and w.isModal()]
    report['ok'] = (not error and any(s['name'] == 'project-roundtrip' for s in report['stages'])
                    and not report['unexpected_dialogs'] and not any(report['graphics_errors'])
                    and not any(report['validation_errors']))
    result.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('DOT_INSERTION_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def yellow_pixels(frame):
    converted = frame.convertToFormat(QImage.Format_RGBA8888)
    data = converted.constBits()
    points = set()
    for y in range(converted.height()):
        row = y * converted.bytesPerLine()
        for x in range(converted.width()):
            offset = row + x * 4
            if data[offset] > 210 and data[offset + 1] > 210 and data[offset + 2] < 50:
                points.add((x, y))
    return points


def insert_dot():
    graphs = [w for w in widgets() if w.isVisible() and w.metaObject().className().endswith('NodeGraph')]
    assert len(graphs) == 1
    graph = graphs[0]
    retained.append(graph.viewport())
    graph.setFocus(Qt.OtherFocusReason)
    before = graph.viewport().grab().toImage()
    # QKeyEvent toggles the modifier corresponding to its own key.
    QApplication.sendEvent(graph, QKeyEvent(QEvent.KeyPress, Qt.Key_Control, Qt.NoModifier))
    after = graph.viewport().grab().toImage()
    assert after.save(str(result.parent / 'ctrl-bend-point.png'))
    new_points = yellow_pixels(after) - yellow_pixels(before)
    # The main bend point is larger than its miniature in the navigator.
    groups = []
    while new_points:
        seed = new_points.pop()
        group, pending = {seed}, [seed]
        while pending:
            x, y = pending.pop()
            for point in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if point in new_points:
                    new_points.remove(point)
                    group.add(point)
                    pending.append(point)
        groups.append(group)
    assert groups, 'Ctrl did not expose an edge bend point'
    circle = max(groups, key=len)
    assert len(circle) >= 8, ('Visible bend point is too small', len(circle))
    x = sum(p[0] for p in circle) / len(circle)
    y = sum(p[1] for p in circle) / len(circle)
    position = QPointF(x / after.devicePixelRatio(), y / after.devicePixelRatio())
    stage('before-ctrl-click', bendPoint=[position.x(), position.y()], yellowPixels=len(circle))
    for kind, buttons in ((QEvent.MouseButtonPress, Qt.LeftButton), (QEvent.MouseButtonRelease, Qt.NoButton)):
        event = QMouseEvent(kind, position, QPointF(graph.viewport().mapToGlobal(position.toPoint())),
                            Qt.LeftButton, buttons, Qt.ControlModifier)
        QApplication.sendEvent(graph.viewport(), event)
    QApplication.sendEvent(graph, QKeyEvent(QEvent.KeyRelease, Qt.Key_Control, Qt.ControlModifier))
    dots = [node for node in app.getChildren() if node.getPluginID() == 'fr.inria.built-in.Dot']
    assert len(dots) == 1, ('Ctrl-click did not create exactly one Dot', len(dots))
    dot = dots[0]
    assert dot.getInput(0).getScriptName() == reader.getScriptName()
    assert viewer.getInput(0).getScriptName() == dot.getScriptName()
    size = list(dot.getSize())
    assert size[0] > 0 and size[1] > 0
    stage('inserted-dot-connected', dot=dot.getScriptName(), size=size)
    graph.viewport().grab().save(str(result.parent / 'inserted-dot.png'))
    stacks = graph.findChildren(QUndoStack)
    retained.extend(stacks)
    assert len(stacks) == 1 and stacks[0].canUndo()
    stack = stacks[0]
    for _ in range(16):
        stack.undo()
        assert viewer.getInput(0).getScriptName() == reader.getScriptName()
        assert stack.canRedo()
        stack.redo()
        assert viewer.getInput(0).getScriptName() == dot.getScriptName()
        assert dot.getInput(0).getScriptName() == reader.getScriptName()
        assert list(dot.getSize()) == size
    stage('undo-redo', cycles=16, dotSize=size)
    # A separately created Dot also follows the inherited size API. This
    # guards creation paths other than the edge insertion gesture.
    second_dot = app.createNode('fr.inria.built-in.Dot')
    assert second_dot and second_dot.connectInput(0, dot)
    viewer.disconnectInput(0)
    assert viewer.connectInput(0, second_dot)
    second_dot.setPosition(180, 250)
    assert list(second_dot.getSize()) == size
    stage('direct-dot-creation', dot=second_dot.getScriptName(), size=list(second_dot.getSize()))
    project = result.parent / 'DotInsertion.ntp'
    assert app.saveProjectAs(project.as_posix())
    loaded = app.loadProject(project.as_posix())
    assert loaded is not None
    nodes = {n.getScriptName(): n for n in loaded.getChildren()}
    restored_viewer, restored_dot, restored_second = [nodes[n.getScriptName()] for n in (viewer, dot, second_dot)]
    assert restored_viewer.getInput(0).getScriptName() == restored_second.getScriptName()
    assert restored_second.getInput(0).getScriptName() == restored_dot.getScriptName()
    assert restored_dot.getInput(0).getScriptName() == reader.getScriptName()
    assert list(restored_dot.getSize()) == list(restored_second.getSize()) == size
    stage('project-roundtrip', dotSizes=[list(restored_dot.getSize()), list(restored_second.getSize())])
    QTimer.singleShot(1500, guarded(finish))


QTimer.singleShot(6000, guarded(insert_dot))
