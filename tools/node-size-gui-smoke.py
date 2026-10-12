"""Check that Reader frame/label refreshes preserve the node's logical size."""
import json
import os
from pathlib import Path
import shutil
import traceback

from PySide6.QtCore import QMetaObject, QTimer, Qt
from PySide6.QtWidgets import QApplication

root = Path.cwd()
result = Path(os.environ['NATRON_GUI_TEST_RESULT'])
result.parent.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Reader node size regression started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'cases': []}
fixture = result.parent / 'fixtures'
fixture.mkdir(parents=True, exist_ok=True)
prefix = 'HdrOutdoorAlanyaResidentialNightClear001_HDR_1K'
for frame in range(1, 5):
    shutil.copyfile(root / 'build/reader-vulkan-fixtures/zero.exr', fixture / f'{prefix}.{frame:04d}.exr')
reader = app.createReader((fixture / f'{prefix}.####.exr').as_posix())
assert reader
reader.setPosition(0, 0)
app.getProjectParam('frameRange').set(1, 4)
viewer_node = app.createNode('fr.inria.built-in.Viewer')
assert viewer_node.connectInput(0, reader)
viewer_node.setPosition(0, 160)
viewer = app.getViewer(viewer_node.getScriptName())
index = 0
sizes = []
labels = []
phase = 0
phases = [('reader-sequence-time-changes', 128), ('preview-disabled-time-changes', 64),
          ('preview-reenabled-time-changes', 64), ('reader-label-changes', 64)]


def guarded(function):
    def run():
        try:
            function()
        except Exception as error:
            finish(error)
    return run


def finish(error=None):
    if error:
        report.update(error=str(error), traceback=traceback.format_exc())
    widgets = QApplication.allWidgets()
    displays = [w for w in widgets if w.property('graphicsBackend') == 'Vulkan']
    report['graphics_errors'] = [w.property('graphicsError') for w in displays]
    report['validation_errors'] = [w.property('validationErrors') for w in displays]
    report['unexpected_dialogs'] = [w.windowTitle() for w in QApplication.topLevelWidgets()
                                    if w.isVisible() and w.isModal()]
    graphs = [w for w in widgets if w.isVisible() and w.metaObject().className().endswith('NodeGraph')]
    if graphs:
        QMetaObject.invokeMethod(graphs[0], 'centerOnAllNodes', Qt.DirectConnection)
        graphs[0].grab().save(str(result.parent / 'node-graph.png'))
    report['ok'] = (not error and len(report['cases']) == len(phases) + 1 and not report['unexpected_dialogs']
                    and not any(report['graphics_errors']) and not any(report['validation_errors']))
    result.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('NODE_SIZE_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def sample():
    global index, phase, sizes, labels
    sizes.append(list(reader.getSize()))
    labels.append(reader.getParam('NatronOfxParamStringSublabelName').getValue())
    name, changes = phases[phase]
    if index == changes:
        report['cases'].append({'name': name, 'changes': index,
                                'initial_size': sizes[0], 'final_size': sizes[-1],
                                'width_range': [min(s[0] for s in sizes), max(s[0] for s in sizes)],
                                'height_range': [min(s[1] for s in sizes), max(s[1] for s in sizes)],
                                'distinct_filenames': sorted(set(labels))})
        assert all(s == sizes[0] for s in sizes), report['cases'][-1]
        if phase < 3:
            assert len(set(labels)) == 4, 'Reader filenames did not follow the timeline'
        phase += 1
        if phase == len(phases):
            save_roundtrip()
            return
        index, sizes, labels = 0, [], []
        if phase == 1:
            reader.setSize(960, reader.getSize()[1])
            report['legacy_size_repair'] = {'inflated_size': list(reader.getSize())}
            reader.getParam('enablePreview').setValue(False)
            report['legacy_size_repair']['preview_off_size'] = list(reader.getSize())
            assert reader.getSize()[0] < 960
        elif phase == 2:
            reader.getParam('enablePreview').setValue(True)
        if phase < 3:
            # Let the first filename refresh lay out the new preview state
            # before measuring repeated refreshes in that state.
            viewer.seek(2)
        QTimer.singleShot(500, guarded(sample))
        return
    index += 1
    if phase == 3:
        reader.setLabel(f'Read{index % 4 + 1}')
    else:
        viewer.seek(index % 4 + 1)
    QTimer.singleShot(100, guarded(sample))


def open_reader_panel():
    app.selectNode(reader, True)
    graphs = [w for w in QApplication.allWidgets() if w.isVisible()
              and w.metaObject().className().endswith('NodeGraph')]
    assert len(graphs) == 1
    assert QMetaObject.invokeMethod(graphs[0], 'showSelectedNodeSettingsPanel', Qt.DirectConnection)
    QTimer.singleShot(1500, guarded(sample))


def save_roundtrip():
    expected = list(reader.getSize())
    name = reader.getScriptName()
    project = result.parent / 'NodeSize.ntp'
    assert app.saveProjectAs(project.as_posix())
    loaded = app.loadProject(project.as_posix())
    assert loaded is not None
    restored = next(n for n in loaded.getChildren() if n.getScriptName() == name)
    actual = list(restored.getSize())
    report['cases'].append({'name': 'project-size-roundtrip', 'initial_size': expected, 'final_size': actual})
    assert actual == expected, report['cases'][-1]
    finish()


QTimer.singleShot(6000, guarded(open_reader_panel))
