"""Measure controlled Natron frame seeks through readback-free presentation signals."""
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import time
import traceback
from PySide6.QtCore import QMetaObject, QTimer, Qt, Q_ARG, Q_RETURN_ARG
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / 'build/vulkan-viewer-performance'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Viewer performance started', encoding='utf-8')
width = int(os.environ.get('NATRON_BENCHMARK_WIDTH', '3840'))
height = int(os.environ.get('NATRON_BENCHMARK_HEIGHT', '2160'))
samples = int(os.environ.get('NATRON_BENCHMARK_SAMPLES', '5'))
mode = os.environ.get('NATRON_BENCHMARK_GUI_MODE', 'native')
count = int(os.environ.get('NATRON_BENCHMARK_VIEWERS', '2'))
warmup = 3
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'mode': mode,
          'source_size': [width, height], 'surface_size': [960, 540], 'viewer_count': count,
          'warmup': warmup, 'samples': samples, 'measurements': [], 'unexpected_dialogs': [],
          'captures': [],
          'scope': 'controlled full-source frame seeks, Engine cache purge outside timing, native present/queue wait or raster paint',
          'physical_scanout_measured': False, 'parsec_tested': False, 'continuous_playback_tested': False,
          'full_vulkan_migration_complete': False}
abi, source, grade = None, None, None
viewers, displays, retained, artifacts = [], [], {}, []
expected, armed, seen = 1, False, {}
requested_at, before, counter_before, stage_started = 0, None, None, time.monotonic()
finished = False


def phase(value):
    report['last_phase'] = value
    # Natron redirects Python print() to its Script Editor in GUI mode.
    Path(report['profile'], 'progress.json').write_text(json.dumps({'phase': value}), encoding='utf-8')


def serializable(value):
    if hasattr(value, 'x') and hasattr(value, 'width'):
        return [value.x(), value.y(), value.width(), value.height()]
    if hasattr(value, 'width') and hasattr(value, 'height'):
        return [value.width(), value.height()]
    if isinstance(value, dict):
        return {key: serializable(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [serializable(item) for item in value]
    return value


def widgets():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    return values


def finish():
    global finished, armed
    if finished:
        return
    finished, armed = True, False
    for viewer in viewers:
        app.getViewer(viewer.getScriptName()).pause()
    if abi:
        report['final_statistics'] = abi.snapshot()
        report['final_counters'] = abi.counters()
        abi.close()
    report['artifact_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts}
    os.write(1, b'NATRON_VIEWER_PERFORMANCE_EXIT_REQUESTED\n')
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    QApplication.exit(0)


def guarded(function):
    def invoke(*args):
        if finished or report.get('exception'):
            return
        try:
            function(*args)
        except Exception:
            report['exception'] = traceback.format_exc()
            QTimer.singleShot(1000, finish)
    return invoke


def capture(frame):
    # These explicit readbacks are outside the armed/timed interval.
    for index, display in enumerate(displays):
        image = QMetaObject.invokeMethod(display, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
        assert not image.isNull() and (image.width(), image.height()) == (960, 540)
        path = output / f'{mode}-frame{frame:02d}-viewer{index}.png'
        assert image.save(str(path))
        artifacts.append(path)
        report['captures'].append({'file': path.name, 'frame': frame,
                                   'presentation': serializable(display.property('lastPresentation'))})
    if count == 2:
        first, second = [QImage(str(path)).convertToFormat(QImage.Format_RGBA8888) for path in artifacts[-2:]]
        a, b = bytes(first.constBits()), bytes(second.constBits())
        assert max(abs(x-y) for x, y in zip(a, b)) <= 1, 'Sharing Viewers differ'


def presented(index, metadata):
    global armed
    metadata = serializable(metadata)
    if not armed or metadata.get('time0') != expected or index in seen:
        return
    assert not metadata['capture'] and not metadata['overlays'], metadata
    assert metadata['surfaceSize'] == [960, 540]
    assert metadata['format0'] == [0, 0, width, height], metadata
    assert metadata['rod0'] == [0, 0, width, height], metadata
    factor = 1 << metadata['mipmap0']
    x, y, w, h = metadata['bounds0']
    assert x <= 0 and y <= 0 and x + w >= width // factor and y + h >= height // factor, metadata
    assert metadata['backend'] == ('Raster' if mode == 'raster' else 'Vulkan')
    assert not displays[index].property('graphicsError') and not displays[index].property('validationErrors')
    seen[index] = {'duration_ms': (time.perf_counter_ns() - requested_at) / 1e6, 'presentation': metadata}
    if len(seen) != count:
        return
    armed = False
    phase(f'frame {expected} all Viewers presented')
    after, counters_after = abi.snapshot(), abi.counters()
    assert after['validation_errors'] == 0 and after['generation'] == before['generation']
    assert counters_after[6:8] == [width, height], counters_after
    assert counters_after[0 if mode == 'native' else 1] > counter_before[0 if mode == 'native' else 1]
    transfers = {key: after[key] - before[key] for key in ('image_uploads', 'image_downloads', 'image_upload_bytes', 'image_download_bytes')}
    if mode in ('native', 'raster'):
        assert all(value == 0 for value in transfers.values()), transfers
    else:
        assert transfers == {'image_uploads': count, 'image_downloads': 0,
                             'image_upload_bytes': count * (width // factor) * (height // factor) * 16,
                             'image_download_bytes': 0}, transfers
    if expected > warmup + 1:
        report['measurements'].append({'frame': expected, 'all_viewers_duration_ms': max(item['duration_ms'] for item in seen.values()),
                                       'viewers': [seen[index] for index in range(count)], 'before': before,
                                       'after': after, 'transfers': transfers, 'counter_before': counter_before, 'counter_after': counters_after})
    # Return to Qt before capture, renderer cancellation or shutdown. This
    # notification can run inside a worker's synchronous GUI presentation.
    QTimer.singleShot(0, guarded(complete_request))


def complete_request():
    phase(f'frame {expected} completion')
    if expected in (2, warmup + samples + 1):
        capture(expected)
    if expected == warmup + samples + 1:
        durations = sorted(item['all_viewers_duration_ms'] for item in report['measurements'])
        def percentile(fraction):
            index = (len(durations)-1) * fraction
            low = math.floor(index)
            return durations[low] + (durations[math.ceil(index)]-durations[low]) * (index-low)
        info = abi.loss.info(abi.context)
        report.update(ok=True, p50_ms=percentile(.5), p95_ms=percentile(.95), device=info.name.decode(),
                      validation_enabled=bool(info.validation_enabled), validation_errors=info.validation_errors)
        QTimer.singleShot(300, finish)
    else:
        QTimer.singleShot(0, guarded(request_next))


def request_next():
    global expected, armed, seen, before, counter_before, requested_at, stage_started
    phase(f'frame {expected + 1} clear cache')
    abi.clear()
    expected += 1
    seen = {}
    before, counter_before = abi.snapshot(), abi.counters()
    phase(f'frame {expected} seek')
    requested_at, stage_started = time.perf_counter_ns(), time.monotonic()
    armed = True
    app.getViewer(viewers[0].getScriptName()).seek(expected)


def ready():
    report['last_presentations'] = [serializable(display.property('lastPresentation')) for display in displays]
    if all(value and value.get('time0') == 1 for value in report['last_presentations']):
        phase('initial frame ready')
        for display, metadata in zip(displays, report['last_presentations']):
            if metadata['overlays']:
                assert QMetaObject.invokeMethod(display, 'toggleOverlays', Qt.DirectConnection)
            assert QMetaObject.invokeMethod(display, 'zoomSlot', Qt.DirectConnection, Q_ARG(int, 25 if width == 3840 else 12))
        app.clearSelection()
        QTimer.singleShot(750, guarded(request_next))
    elif time.monotonic() - stage_started > 45:
        raise RuntimeError('Viewer initialization timed out: ' + json.dumps(report['last_presentations']))
    else:
        QTimer.singleShot(100, guarded(ready))


def watchdog():
    dialogs = [widget.windowTitle() for widget in QApplication.topLevelWidgets() if widget.isVisible() and widget.isModal()]
    report['unexpected_dialogs'] = dialogs
    assert not dialogs, dialogs
    if armed and time.monotonic() - stage_started > 45:
        raise RuntimeError(f'Frame {expected} timed out: {serializable(seen)}')
    QTimer.singleShot(500, guarded(watchdog))


def setup():
    global abi, source, grade, stage_started
    phase('setup started')
    assert mode in ('native', 'cpu', 'raster') and count in (1, 2)
    assert (width, height) in ((3840, 2160), (7680, 4320)) and 5 <= samples <= 40
    abi = runpy.run_path(str(root / 'tools/vulkan-performance-abi.py'))['BenchmarkAbi'](root)
    phase('telemetry ready')
    app.addFormat(f'ViewerPerformance {width}x{height} 1')
    app.getProjectParam('outputFormat').setValue('ViewerPerformance')
    app.getProjectParam('frameRange').set(1, warmup + samples + 1)
    app.getProjectParam('autoPreviews').setValue(False)
    source = app.createNode('org.natron.test.VulkanBenchmark' + ('Native' if mode == 'native' else 'Cpu') + 'Source')
    phase('source created')
    assert source
    source.getParam('width').setValue(width)
    source.getParam('height').setValue(height)
    source.getParam('forceCaching').setValue(True)
    grade = app.createNode('fr.inria.openfx.OCIOCDLTransform')
    phase('grade created')
    assert grade and grade.connectInput(0, source)
    grade.getParam('enableGPU').setValue(mode == 'native')
    for channel, gain in enumerate((1.2, .8, 1.1)):
        grade.getParam('slope').setValue(gain, channel)
    for knob in ('ocioInputSpace', 'ocioOutputSpace'):
        if param := grade.getParam(knob):
            param.setValue('Linear')
    pane = None
    for index in range(count):
        viewer = app.createNode('fr.inria.built-in.Viewer')
        phase(f'Viewer {index} created')
        assert viewer
        viewers.append(viewer)
        if pane:
            assert app.moveTab(viewer.getScriptName(), pane)
        candidates = [w for w in widgets() if w.objectName().startswith('NatronVulkanViewer')
                      and w.property('graphicsBackend') is not None and w not in displays]
        assert len(candidates) == 1
        display = candidates[0]
        displays.append(display)
        scale = display.devicePixelRatioF()
        display.setFixedSize(round(960/scale), round(540/scale))
        geometry = QApplication.primaryScreen().availableGeometry()
        display.window().setGeometry(geometry.x()+10+index*1100, geometry.y()+20, 1050, 820)
        display.window().showNormal()
        display.framePresented.connect(guarded(lambda metadata, index=index: presented(index, metadata)))
        assert viewer.connectInput(0, grade)
        if count == 2 and index == 0:
            parent = display.parentWidget()
            while parent and not (owner := app.getTabWidget(parent.objectName())):
                parent = parent.parentWidget()
            assert parent and owner
            pane = owner.splitHorizontally()
            pane.floatPane()
    app.clearSelection()
    phase('initial render requests')
    for viewer in viewers:
        app.getViewer(viewer.getScriptName()).renderCurrentFrame()
    stage_started = time.monotonic()
    phase('graph ready')
    QTimer.singleShot(750, guarded(ready))


phase('script loaded')
QTimer.singleShot(500, guarded(watchdog))
QTimer.singleShot(0, guarded(setup))
