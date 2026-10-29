"""Exercise stock Reader native color/premult/proxy paths in the real Viewer."""
import ctypes
import hashlib
import json
import os
import time
import traceback
from pathlib import Path

import NatronEngine
from PySide6.QtCore import QMetaObject, Q_ARG, Q_RETURN_ARG, QTimer, Qt
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / 'build/reader-vulkan-gui-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Reader Vulkan GUI test started', encoding='utf-8')
module = ctypes.CDLL(str(root / 'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx'))
module.OfxReaderVulkanRenderCount.restype = ctypes.c_uint64
module.OfxReaderVulkanRenderCount.argtypes = [ctypes.c_uint]
module.OfxOCIOVulkanTransferCounts.restype = ctypes.c_int
module.OfxOCIOVulkanTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64)] * 2
report = {'ok': False, 'cases': [], 'profile': os.environ.get('NATRON_TEST_PROFILE')}
report['cpu_display_reference'] = True
report['display_matrix'] = os.environ.get('NATRON_TEST_VIEWER_DISPLAY_MATRIX') == '1'
retained, current, artifacts = {}, {}, []
fixtures = root / 'build/reader-vulkan-fixtures'
config = fixtures / 'reader.ocio'
width, height = 43, 29
app.addFormat(f'ReaderVulkanSmoke {width}x{height} 1')
app.getProjectParam('outputFormat').setValue('ReaderVulkanSmoke')
app.getProjectParam('frameRange').set(1, 1)
app.getProjectParam('autoPreviews').setValue(False)
report['node_previews_enabled'] = app.getProjectParam('autoPreviews').getValue()
reader = app.createReader((fixtures / 'negative.exr').as_posix())
reader.getParam('outputComponents').setValue('RGBA')
reader.getParam('ocioConfigFile').setValue(config.as_posix())
reader.getParam('ocioInputSpace').setValue('linear')
reference = app.createReader((fixtures / 'negative.exr').as_posix())
reference.getParam('enableGPU').setValue(False)
reference.getParam('outputComponents').setValue('RGBA')
reference.getParam('ocioConfigFile').setValue(config.as_posix())
reference.getParam('ocioInputSpace').setValue('linear')
viewer_node = app.createNode('fr.inria.built-in.Viewer')
viewer = app.getViewer(viewer_node.getScriptName())
assert viewer_node.connectInput(0, reader)
variants = [('identity', 2, 2), ('identity-premult', 2, 1), ('identity-associated', 1, 1),
            ('color', 2, 2), ('color-premult', 2, 1), ('color-unpremult', 1, 2), ('color-associated', 1, 1)]
steps = [(variant, level) for variant in variants for level in range(6)]
display_settings = []
if report['display_matrix']:
    steps = []
    for lut in range(4):
        for exposure, gamma in ((0., 1.), (1.25, .45), (-2., 2.2), (0., 0.)):
            for variant in variants[:2]:
                steps.append((variant, (0, 2, 5)[len(steps) % 3]))
                display_settings.append({'lut': lut, 'exposure': exposure, 'gamma': gamma})
position = 0


def count():
    return sum(module.OfxReaderVulkanRenderCount(level) for level in range(31))


def transfers():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert module.OfxOCIOVulkanTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 0
    return [uploads.value, downloads.value]


def widget():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    viewers = [value for value in values if value.property('graphicsBackend') == 'Vulkan' and value.isVisible()]
    assert len(viewers) == 1
    return viewers[0]


def samples(frame):
    viewport = widget().property('canonicalViewport')
    rod = reader.getRegionOfDefinition(1, 0)
    current['sample_geometry'] = {'viewport': [viewport.x(), viewport.y(), viewport.width(), viewport.height()],
                                  'rod': [rod.x1, rod.y1, rod.x2, rod.y2],
                                  'frame_size': [frame.width(), frame.height()]}
    result = []
    for row in range(17):
        for column in range(25):
            x = rod.x1 + (rod.x2 - rod.x1) * (.08 + .84 * column / 24)
            y = rod.y1 + (rod.y2 - rod.y1) * (.08 + .84 * row / 16)
            px = round((x - viewport.x()) / viewport.width() * frame.width())
            py = round((viewport.y() + viewport.height() - y) / viewport.height() * frame.height())
            assert 0 <= px < frame.width() and 0 <= py < frame.height(), ('Viewer sample outside frame', px, py, current['sample_geometry'])
            result.append(frame.pixelColor(px, py).getRgb())
    return result


def finish(error=None):
    os.environ.pop('NATRON_TEST_VIEWER_CPU_REFERENCE', None)
    if error:
        report.update(error=str(error), traceback=traceback.format_exc(), failed_stage=dict(current))
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    viewers = [value for value in values if value.property('graphicsBackend') == 'Vulkan']
    report['unexpected_dialogs'] = [value.windowTitle() for value in QApplication.topLevelWidgets() if value.isVisible() and value.isModal()]
    report['graphics_errors'] = [value.property('graphicsError') for value in viewers]
    report['validation_errors'] = [value.property('validationErrors') for value in viewers]
    report['devices'] = [value.property('gpuDeviceName') for value in viewers]
    report['shared_device_ids'] = [value.property('sharedDeviceId') for value in viewers]
    report['engine_device_ids'] = [value.property('engineDeviceId') for value in viewers]
    report['native_viewer_copies'] = [value.property('nativeImageTransfers') for value in viewers]
    report['native_scale_renders'] = [module.OfxReaderVulkanRenderCount(level) for level in range(6)]
    # Natron decodes Readers at full resolution, then uses Engine native mipmaps.
    report['ok'] = (not error and len(report['cases']) == len(steps) and report['native_scale_renders'][0] > 0
                    and not report['unexpected_dialogs'] and not any(report['graphics_errors']) and not any(report['validation_errors'])
                    and report['shared_device_ids'] == report['engine_device_ids'] and not report['node_previews_enabled'])
    report['artifact_sha256'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts}
    (output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print('READER_VULKAN_GUI_SMOKE', json.dumps(report))
    QApplication.exit(0)


def guarded(function):
    def call():
        try:
            function()
        except Exception as error:
            finish(error)
    return call


def begin():
    if position == len(steps):
        finish()
        return
    variant, level = steps[position]
    os.environ.pop('NATRON_TEST_VIEWER_CPU_REFERENCE', None)
    if position == 0:
        # A dock layout can resize the canvas while a frame is being read back.
        # Pixel comparisons require the same viewport/extent on both paths.
        widget().setFixedSize(1400, 600)
        QMetaObject.invokeMethod(widget(), 'zoomSlot', Qt.DirectConnection, Q_ARG(int, 500))
        for value in QApplication.allWidgets():
            if value.metaObject().indexOfSlot('onClipToProjectButtonToggle(bool)') >= 0:
                QMetaObject.invokeMethod(value, 'onClipToProjectButtonToggle', Qt.DirectConnection, Q_ARG(bool, False))
    if display_settings:
        tabs = [value for value in QApplication.allWidgets() if value.metaObject().indexOfSlot('onColorSpaceComboBoxChanged(int)') >= 0]
        assert len(tabs) == 1
        correction = display_settings[position]
        assert QMetaObject.invokeMethod(tabs[0], 'onColorSpaceComboBoxChanged', Qt.DirectConnection, Q_ARG(int, correction['lut']))
        assert QMetaObject.invokeMethod(tabs[0], 'onGainSpinBoxValueChanged', Qt.DirectConnection, Q_ARG(float, correction['exposure']))
        assert QMetaObject.invokeMethod(tabs[0], 'onGammaSpinBoxValueChanged', Qt.DirectConnection, Q_ARG(float, correction['gamma']))
    name, file_premult, output_premult = variant
    current.clear()
    fixture = output / f'input-{position}.exr'
    fixture.write_bytes((fixtures / 'negative.exr').read_bytes())
    artifacts.append(fixture)
    for node in (reader, reference):
        node.getParam('filename').setValue(fixture.as_posix())
        node.getParam('ocioOutputSpace').setValue('linear' if name.startswith('identity') else 'graded')
        node.getParam('filePremult').setValue(file_premult)
        node.getParam('outputPremult').setValue(output_premult)
    viewer.setProxyModeEnabled(level > 0)
    if level:
        viewer.setProxyIndex(level - 1)
    current.update(name=name, level=level, phase='native', count_before=count(), transfer_before=transfers())
    # GUI edits may start abortable renders. Discard those transfers; the
    # settled render uses this fresh filename hash and exactly one upload.
    QTimer.singleShot(500, guarded(start_native))


def start_native():
    current.update(count_before=count(), transfer_before=transfers())
    fixture = output / f'settled-{position}.exr'
    fixture.write_bytes((fixtures / 'negative.exr').read_bytes())
    artifacts.append(fixture)
    reader.getParam('filename').setValue(fixture.as_posix())
    render()


def render():
    current.update(deadline=time.monotonic() + 15, frames_before=widget().property('presentedFrames'), stable=0, digest=None)
    QTimer.singleShot(500, guarded(present))


def present():
    # Replacing a Viewer input can automatically fit its format after the
    # connection settles. Keep the negative-origin test's whole RoD visible.
    QMetaObject.invokeMethod(widget(), 'zoomSlot', Qt.DirectConnection, Q_ARG(int, 500))
    current.update(frames_before=widget().property('presentedFrames'))
    viewer.renderCurrentFrame(True)
    QTimer.singleShot(150, guarded(wait))


def wait():
    global position
    w = widget()
    assert not any(value.isVisible() and value.isModal() for value in QApplication.topLevelWidgets()), 'Unexpected modal dialog'
    assert not w.property('graphicsError') and not w.property('validationErrors')
    ready = w.property('presentedFrames') > current['frames_before']
    if current['phase'] == 'native':
        ready = ready and count() > current['count_before']
    if ready:
        frame = QMetaObject.invokeMethod(w, 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
        assert not frame.isNull()
        values = samples(frame)
        digest = hashlib.sha256(bytes(channel for pixel in values for channel in pixel)).hexdigest()
        current['stable'] = current['stable'] + 1 if current['digest'] == digest else 0
        current['digest'] = digest
        if current['stable'] >= 2:
            path = output / f'{position}-{current["phase"]}.png'
            assert frame.save(str(path))
            artifacts.append(path)
            if current['phase'] == 'native':
                actual = [after - before for after, before in zip(transfers(), current['transfer_before'])]
                calls = count() - current['count_before']
                native_display = w.property('nativeImageTransfers') > 0
                assert actual[0] == calls and 1 <= calls <= 3, ('Reader decode/Viewer upload', actual, calls)
                if native_display:
                    assert actual[1] == 0, ('Native Reader/Viewer downloaded CPU pixels', actual)
                else:
                    assert 1 <= actual[1] <= calls + 1, ('Reader decode/Viewer boundary', actual, calls)
                current.update(native_samples=values, native_count=count(), native_transfers=actual, native_renders=calls,
                               native_copies=w.property('nativeImageTransfers'), phase='cpu')
                os.environ['NATRON_TEST_VIEWER_CPU_REFERENCE'] = '1'
                viewer_node.disconnectInput(0)
                assert viewer_node.connectInput(0, reference)
                render()
            elif current['phase'] == 'cpu':
                assert count() == current['native_count'], 'CPU Reader selected native processing'
                assert w.property('nativeImageTransfers') == current['native_copies'], 'CPU Viewer reference used native display conversion'
                error = max(abs(a-b) for actual, expected in zip(current['native_samples'], values) for a, b in zip(actual, expected))
                assert error <= 2, (current['name'], current['level'], error)
                current.update(maximum_byte_error=error, phase='cache')
                os.environ.pop('NATRON_TEST_VIEWER_CPU_REFERENCE', None)
                viewer_node.disconnectInput(0)
                assert viewer_node.connectInput(0, reader)
                current['cache_before'] = transfers()
                render()
            else:
                assert count() == current['native_count'], 'Resident Reader was decoded again'
                assert w.property('nativeImageTransfers') > current['native_copies'], 'Cached Viewer did not use GPU conversion'
                delta = [after - before for after, before in zip(transfers(), current['cache_before'])]
                assert delta[0] == 0, ('Cached Reader upload', delta)
                if w.property('nativeImageTransfers') > 0:
                    assert delta[1] == 0, ('Resident Reader/Viewer downloaded CPU pixels', delta)
                else:
                    assert 1 <= delta[1] <= 2, ('Cached Reader/Viewer boundary', delta)
                cache_error = max(abs(a-b) for actual, expected in zip(current['native_samples'], values) for a, b in zip(actual, expected))
                assert cache_error <= 1
                report['cases'].append({key: current[key] for key in ('name', 'level', 'maximum_byte_error', 'native_transfers', 'native_renders')}
                                       | {'cache_transfers': delta, 'cache_maximum_byte_error': cache_error, 'pixels_compared': len(values),
                                          'cpu_display_reference': True}
                                       | ({'display_correction': display_settings[position]} if display_settings else {}))
                position += 1
                QTimer.singleShot(0, guarded(begin))
            return
    assert time.monotonic() < current['deadline'], (current['name'], current['level'], current['phase'], 'timeout')
    QTimer.singleShot(150, guarded(wait))


QTimer.singleShot(750, guarded(begin))
