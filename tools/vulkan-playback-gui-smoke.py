"""Exercise the real playback scheduler; run inside a fresh Natron GUI profile."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import time
import traceback
import NatronEngine
from PySide6.QtCore import QMetaObject, QTimer, Qt, Q_ARG, Q_RETURN_ARG
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True, exist_ok=True)
profile = Path(os.environ['NATRON_TEST_PROFILE'])
Path(os.environ['NATRON_TEST_START_MARKER']).write_text('Playback script started', encoding='utf-8')
width = int(os.environ.get('NATRON_BENCHMARK_WIDTH', '3840'))
height = width * 9 // 16
mode = os.environ.get('NATRON_BENCHMARK_GUI_MODE', 'native')
reference = runpy.run_path(str(root / 'tools/vulkan-viewer-reference.py'))['check_pixels']
report = {'ok': False, 'profile': str(profile), 'mode': mode, 'source_size': [width, height],
          'stages': [], 'captures': [], 'physical_scanout_measured': False, 'parsec_tested': False}
viewers, displays, retained = [], [], {}
abi, finished, active, stage_index = None, False, None, -1
started = time.monotonic()
specs = [('forward', 0, 1, 12, 'Once'), ('backward', 0, 12, 1, 'Once'),
         ('loop', 0, 1, 4, 'Loop'), ('bounce', 0, 1, 4, 'Bounce'),
         ('cancel', 0, 1, 24, 'Once'), ('restart', 1, 7, 12, 'Once'),
         ('second-backward', 1, 12, 1, 'Once')]


def phase(value):
    report['last_phase'] = value
    (profile / 'progress.json').write_text(json.dumps(report), encoding='utf-8')


def serializable(value):
    if hasattr(value, 'x') and hasattr(value, 'width'):
        return [value.x(), value.y(), value.width(), value.height()]
    if hasattr(value, 'width') and hasattr(value, 'height'):
        return [value.width(), value.height()]
    if isinstance(value, dict):
        return {key: serializable(item) for key, item in value.items()}
    return value


def guarded(function):
    def invoke(*args):
        if finished or report.get('exception'):
            return
        try:
            function(*args)
        except Exception:
            report['exception'] = traceback.format_exc()
            phase('failed')
            QTimer.singleShot(1000, finish)
    return invoke


def finish():
    global finished
    if finished:
        return
    finished = True
    for viewer in viewers:
        app.getViewer(viewer.getScriptName()).pause()
    if abi:
        report['final_statistics'] = abi.snapshot()
        report['device'] = abi.loss.info(abi.context).name.decode()
        abi.close()
    output.write_text(json.dumps(report, indent=2), encoding='utf-8')
    os.write(1, b'NATRON_PLAYBACK_EXIT_REQUESTED\n')
    QApplication.exit(0)


def capture(index, label):
    display = displays[index]
    metadata = serializable(display.property('lastPresentation'))
    image = QMetaObject.invokeMethod(display, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
    assert not image.isNull() and (image.width(), image.height()) == (960, 540)
    path = output.parent / f'{label}-viewer{index}.png'
    assert image.save(str(path))
    _, checked = reference(path, [width, height], int(metadata['time0']), metadata)
    report['captures'].append({'file': path.name, 'frame': metadata['time0'], 'presentation': metadata,
                               'pixels': checked['pixels'], 'max_error': checked['maximum_byte_error'],
                               'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})


def present(index, value):
    if not active or active['state'] != 'playing':
        return
    metadata = serializable(value)
    frame = int(metadata['time0'])
    assert not metadata['capture'] and not metadata['overlays']
    assert metadata['backend'] == ('Raster' if mode == 'raster' else 'Vulkan')
    assert metadata['format0'] == metadata['rod0'] == [0, 0, width, height]
    assert not displays[index].property('validationErrors') and not displays[index].property('graphicsError')
    other = active['viewer_frames'][index]
    if not other or other[-1] != frame:
        other.append(frame)
    if index != active['viewer']:
        return
    if active['frames'] and active['frames'][-1]['frame'] == frame:
        return
    active['frames'].append({'frame': frame, 'elapsed_ms': (time.monotonic() - active['start']) * 1000,
                             'generation': metadata['generation']})
    name = active['name']
    frames = [row['frame'] for row in active['frames']]
    complete = (name in ('forward', 'restart') and frame == active['end'] or
                name in ('backward', 'second-backward') and frame == 1 or
                name == 'cancel' and frame >= 4 or
                name == 'loop' and sum(a == 4 and b == 1 for a, b in zip(frames, frames[1:])) >= 2 or
                name == 'bounce' and len(frames) >= 13 and 4 in frames and frames[-1] == 1)
    if complete:
        active['state'] = 'stopping'
        QTimer.singleShot(0, guarded(stop))


def stop():
    app.getViewer(viewers[active['viewer']].getScriptName()).pause()
    phase(active['name'] + ' stopped')
    QTimer.singleShot(750, guarded(stopped))


def stopped():
    index = active['viewer']
    active['stop_frame'] = serializable(displays[index].property('lastPresentation'))['time0']
    active['stop_timeline'] = app.getViewer(viewers[index].getScriptName()).getCurrentFrame()
    active['stop_frames'] = [serializable(display.property('lastPresentation'))['time0'] for display in displays]
    QTimer.singleShot(500, guarded(verify_stopped))


def verify_stopped():
    index = active['viewer']
    assert serializable(displays[index].property('lastPresentation'))['time0'] == active['stop_frame']
    assert app.getViewer(viewers[index].getScriptName()).getCurrentFrame() == active['stop_timeline']
    assert [serializable(display.property('lastPresentation'))['time0'] for display in displays] == active['stop_frames']
    active['pause_stable'] = True
    frames = [row['frame'] for row in active['frames']]
    assert len(frames) >= 3, frames
    low, high = sorted((active['begin'], active['end']))
    assert all(low <= frame <= high for frame in frames), frames
    if active['name'] in ('forward', 'restart', 'cancel'):
        assert all(a < b for a, b in zip(frames, frames[1:])), frames
    if active['name'] in ('backward', 'second-backward'):
        assert all(a > b for a, b in zip(frames, frames[1:])), frames
    for viewer_index in range(2):
        minimum = 1 if active['name'] == 'cancel' and viewer_index != index else 3
        assert len(active['viewer_frames'][viewer_index]) >= minimum, active['viewer_frames']
        capture(viewer_index, active['name'])
    if active['name'] in ('forward', 'backward', 'restart', 'second-backward'):
        assert active['stop_frame'] == active['end']
    report['stages'].append({key: value for key, value in active.items() if key != 'start'})
    QTimer.singleShot(0, guarded(next_stage))


def next_stage():
    global stage_index, active, started
    stage_index += 1
    if stage_index == len(specs):
        assert abi.snapshot()['validation_errors'] == 0
        report['ok'] = True
        phase('all playback stages passed')
        QTimer.singleShot(300, finish)
        return
    name, index, begin, end, playback_mode = specs[stage_index]
    active = {'name': name, 'viewer': index, 'begin': begin, 'end': end, 'playback_mode': playback_mode,
              'frames': [], 'viewer_frames': [[], []], 'state': 'preparing'}
    for viewer in viewers:
        api = app.getViewer(viewer.getScriptName())
        api.setFrameRange(min(begin, end), max(begin, end))
        api.setPlaybackMode(getattr(NatronEngine.Natron, 'ePlaybackMode' + playback_mode))
    app.getViewer(viewers[index].getScriptName()).seek(begin)
    started = time.monotonic()
    phase(name + ' preparing')
    QTimer.singleShot(250, guarded(prepared))


def prepared():
    global started
    index = active['viewer']
    metadata = serializable(displays[index].property('lastPresentation'))
    if not metadata or metadata.get('time0') != active['begin']:
        assert time.monotonic() - started < 30, metadata
        QTimer.singleShot(100, guarded(prepared))
        return
    active['state'], active['start'] = 'playing', time.monotonic()
    started = time.monotonic()
    phase(active['name'] + ' playing')
    api = app.getViewer(viewers[index].getScriptName())
    (api.startBackward if active['end'] < active['begin'] else api.startForward)()


def ready():
    report['last_presentations'] = [serializable(display.property('lastPresentation')) for display in displays]
    if not all(value and value.get('time0') == 1 for value in report['last_presentations']):
        report['display_state'] = [{'name': display.objectName(), 'visible': display.isVisible(),
                                    'backend': display.property('graphicsBackend')} for display in displays]
        assert time.monotonic() - started < 30, (report['last_presentations'], report['display_state'])
        QTimer.singleShot(100, guarded(ready))
        return
    for display in displays:
        if display.property('lastPresentation')['overlays']:
            assert QMetaObject.invokeMethod(display, 'toggleOverlays', Qt.DirectConnection)
        assert QMetaObject.invokeMethod(display, 'zoomSlot', Qt.DirectConnection, Q_ARG(int, 25 if width == 3840 else 12))
    app.clearSelection()
    QTimer.singleShot(750, guarded(next_stage))


def watchdog():
    assert not [widget.windowTitle() for widget in QApplication.topLevelWidgets() if widget.isVisible() and widget.isModal()]
    assert time.monotonic() - started < 45, report['last_phase']
    QTimer.singleShot(500, guarded(watchdog))


def setup():
    global abi, started
    assert width in (3840, 7680) and mode in ('native', 'raster')
    abi = runpy.run_path(str(root / 'tools/vulkan-performance-abi.py'))['BenchmarkAbi'](root)
    app.addFormat(f'PlaybackSmoke {width}x{height} 1')
    app.getProjectParam('outputFormat').setValue('PlaybackSmoke')
    app.getProjectParam('frameRange').set(1, 24)
    app.getProjectParam('frameRate').setValue(8)
    app.getProjectParam('autoPreviews').setValue(False)
    source = app.createNode('org.natron.test.VulkanBenchmark' + ('Native' if mode == 'native' else 'Cpu') + 'Source')
    assert source
    source.getParam('width').setValue(width)
    source.getParam('height').setValue(height)
    source.getParam('forceCaching').setValue(True)
    grade = app.createNode('fr.inria.openfx.OCIOCDLTransform')
    assert grade and grade.connectInput(0, source)
    grade.getParam('enableGPU').setValue(mode == 'native')
    for channel, gain in enumerate((1.2, .8, 1.1)):
        grade.getParam('slope').setValue(gain, channel)
    for knob in ('ocioInputSpace', 'ocioOutputSpace'):
        if param := grade.getParam(knob):
            param.setValue('Linear')
    pane = None
    for index in range(2):
        viewer = app.createNode('fr.inria.built-in.Viewer')
        assert viewer
        viewers.append(viewer)
        if pane:
            assert app.moveTab(viewer.getScriptName(), pane)
        values = QApplication.allWidgets()
        retained.update((id(value), value) for value in values)
        candidates = [value for value in values if value.objectName().startswith('NatronVulkanViewer')
                      and value.property('graphicsBackend') is not None and value not in displays]
        assert len(candidates) == 1
        display = candidates[0]
        displays.append(display)
        scale = display.devicePixelRatioF()
        display.setFixedSize(round(960 / scale), round(540 / scale))
        geometry = QApplication.primaryScreen().availableGeometry()
        display.window().setGeometry(geometry.x() + 10 + index * 1100, geometry.y() + 20, 1050, 820)
        display.window().showNormal()
        display.framePresented.connect(guarded(lambda value, index=index: present(index, value)))
        assert viewer.connectInput(0, grade)
        if index == 0:
            parent = display.parentWidget()
            while parent and not (owner := app.getTabWidget(parent.objectName())):
                parent = parent.parentWidget()
            assert parent and owner
            pane = owner.splitHorizontally()
            pane.floatPane()
    started = time.monotonic()
    app.clearSelection()
    phase('graph ready')
    def initial_requests():
        for display in displays:
            display.window().hide()
            display.window().showNormal()
        for viewer in viewers:
            app.getViewer(viewer.getScriptName()).renderCurrentFrame()
    QTimer.singleShot(250, guarded(initial_requests))
    QTimer.singleShot(750, guarded(ready))


phase('script loaded')
QTimer.singleShot(0, guarded(setup))
QTimer.singleShot(500, guarded(watchdog))
