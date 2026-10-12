"""Measure full-size CPU/native OCIO graphs through NatronRenderer.

The 64x48 PNG sink samples the whole image. Timed calls include the application
scheduler and PNG boundary, with quiescent cache purges outside each call. A
separate, untimed native readback checks every full-size pixel against a CPU
formula. This is not a Viewer, WSI, playback, GL baseline or Parsec benchmark.
"""
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import runpy
import time
import traceback
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

root = Path.cwd()
output = root / 'build/vulkan-performance-render'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Application performance render started', encoding='utf-8')
width = int(os.environ.get('NATRON_BENCHMARK_WIDTH', '3840'))
height = int(os.environ.get('NATRON_BENCHMARK_HEIGHT', '2160'))
samples = int(os.environ.get('NATRON_BENCHMARK_SAMPLES', '20'))
warmup = 3
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'source_size': [width, height],
          'output_size': [64, 48], 'warmup': warmup, 'samples': samples,
          'scope': 'NatronRenderer full-size procedural source, shipped OCIO CDL, dispersed PNG probe',
          'cache_policy': 'quiescent Engine node cache purge before each render, outside timed interval',
          'validation_enabled': os.environ.get('NATRON_VULKAN_VALIDATION') == '1', 'viewer_wsi_tested': False, 'parsec_tested': False,
          'gl_baseline_tested': False, 'full_vulkan_migration_complete': False, 'cases': []}
artifacts, abi, cpu_reference = [], None, []


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lo = math.floor(index)
    return ordered[lo] + (ordered[math.ceil(index)] - ordered[lo]) * (index - lo)


def png_pixels(path, frame):
    image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and (image.width(), image.height()) == (64, 48), str(path)
    actual = bytes(image.constBits())
    # Writer's PNG rows run top to bottom, while native OFX rows start at y=0.
    maximum = 0
    for row in range(48):
        y = (47 - row) * (height - 1) // 47
        for column in range(64):
            x = column * (width - 1) // 63
            values = (((x + frame) & 63) / 128 * 1.2,
                      ((y + 3 * frame) & 63) / 128 * .8,
                      ((x + y + 7 * frame) & 63) / 128 * 1.1, 1)
            for channel, value in enumerate(values):
                expected = round(value * 255)
                maximum = max(maximum, abs(actual[(row * 64 + column) * 4 + channel] - expected))
    assert maximum <= 1, (path.name, maximum)
    return actual, maximum


try:
    assert (width, height) in ((3840, 2160), (7680, 4320)) and 5 <= samples <= 40
    abi = runpy.run_path(str(root / 'tools/vulkan-performance-abi.py'))['BenchmarkAbi'](root)
    report['accounting_check'] = abi.verify_accounting()
    app.addFormat('PerformanceProbe 64x48 1')
    app.getProjectParam('outputFormat').setValue('PerformanceProbe')
    app.getProjectParam('frameRange').set(1, warmup + samples + 1)
    io = ctypes.CDLL(str(root / 'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx'))
    io.OfxOCIOVulkanClipRenderCount.restype = ctypes.c_uint64
    for mode in ('cpu', 'native'):
        prefix = 'Native' if mode == 'native' else 'Cpu'
        source = app.createNode('org.natron.test.VulkanBenchmark' + prefix + 'Source')
        probe = app.createNode('org.natron.test.VulkanBenchmark' + prefix + 'Probe')
        grade = app.createNode('fr.inria.openfx.OCIOCDLTransform')
        assert source and probe and grade
        for node in (source, probe):
            node.getParam('width').setValue(width)
            node.getParam('height').setValue(height)
        assert grade.connectInput(0, source) and probe.connectInput(0, grade)
        grade.getParam('enableGPU').setValue(mode == 'native')
        for channel, gain in enumerate((1.2, .8, 1.1)):
            grade.getParam('slope').setValue(gain, channel)
        for knob in ('ocioInputSpace', 'ocioOutputSpace'):
            if param := grade.getParam(knob):
                param.setValue('Linear')
        writer = app.createWriter((output / f'{mode}-####.png').as_posix())
        assert writer and writer.connectInput(0, probe)
        # Keep the identical small PNG boundary on CPU in both graphs. The
        # native graph downloads only the 64x48 probe, outside the full-size
        # native source/CDL chain; the CPU graph needs no pixel transfer.
        writer_gpu = writer.getParam('enableGPU')
        assert writer_gpu, 'Writer GPU setting is required to control the boundary'
        writer_gpu.setValue(False)
        for knob in ('ocioInputSpace', 'ocioOutputSpace'):
            if param := writer.getParam(knob):
                param.setValue('Linear')
        QCoreApplication.processEvents()
        measurements = []
        for index in range(warmup + samples):
            frame = index + 1
            path = output / f'{mode}-{frame:04d}.png'
            path.unlink(missing_ok=True)
            abi.clear()
            before, counter_before = abi.snapshot(), abi.counters()
            ocio_before = io.OfxOCIOVulkanClipRenderCount()
            start = time.perf_counter_ns()
            app.render(writer, frame, frame)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            after, counter_after = abi.snapshot(), abi.counters()
            assert path.exists(), f'Missing output: {path}'
            pixels, byte_error = png_pixels(path, frame)
            artifacts.append(path)
            assert counter_after[6:10] == [width, height, width, height], counter_after
            expected_counter = (0, 2) if mode == 'native' else (1, 3)
            assert all(counter_after[i] == counter_before[i] + 1 for i in expected_counter), (counter_before, counter_after)
            assert all(counter_after[i] == counter_before[i] for i in ((1, 3) if mode == 'native' else (0, 2)))
            ocio_delta = io.OfxOCIOVulkanClipRenderCount() - ocio_before
            assert (ocio_delta > 0) == (mode == 'native'), ocio_delta
            assert after['generation'] == before['generation'] and after['validation_errors'] == 0
            transfers = {key: after[key] - before[key] for key in (
                'image_uploads', 'image_downloads', 'image_upload_bytes', 'image_download_bytes')}
            if mode == 'cpu':
                assert all(value == 0 for value in transfers.values()), transfers
                cpu_reference.append(pixels)
            else:
                assert transfers == {'image_uploads': 0, 'image_downloads': 1,
                                     'image_upload_bytes': 0, 'image_download_bytes': 64 * 48 * 16}, transfers
                assert max(abs(a - b) for a, b in zip(cpu_reference[index], pixels)) <= 1
            if index >= warmup:
                measurements.append({'frame': frame, 'duration_ms': elapsed, 'before': before, 'after': after,
                                     'transfers': transfers, 'native_ocio_actions': ocio_delta,
                                     'counter_before': counter_before, 'counter_after': counter_after,
                                     'png': path.name, 'reference_byte_error': byte_error})
        case = {'mode': mode, 'measurements': measurements,
                'p50_ms': percentile([m['duration_ms'] for m in measurements], .5),
                'p95_ms': percentile([m['duration_ms'] for m in measurements], .95),
                'managed_local_peak_block_bytes_lifetime': max(m['after']['managed_local_peak_block_bytes'] for m in measurements),
                'driver_local_usage_bytes_sampled_max': max(m['after']['driver_local_usage_bytes'] for m in measurements)}
        report['cases'].append(case)
        if mode == 'native':
            # Full-image readback and CPU formula comparison are deliberately
            # outside the timed render and transfer totals.
            probe.getParam('verifyFull').setValue(1)
            abi.clear()
            before = abi.counters()
            frame = warmup + samples + 1
            path = output / f'native-{frame:04d}.png'
            path.unlink(missing_ok=True)
            app.render(writer, frame, frame)
            assert path.exists()
            png_pixels(path, frame)
            artifacts.append(path)
            after = abi.counters()
            assert after[4] - before[4] == width * height
            report['full_reference_pixels'] = width * height
            report['full_reference_max_error'] = after[5] / 1e9
    info = abi.loss.info(abi.context)
    assert bool(info.validation_enabled) == report['validation_enabled']
    report.update(ok=True, device=info.name.decode('utf-8'), validation_errors=info.validation_errors,
                  pixels_compared=len(artifacts) * 64 * 48 + width * height + 17 * 13,
                  unexpected_dialogs=[])
except Exception:
    report['exception'] = traceback.format_exc()
finally:
    if abi:
        report['final_statistics'] = abi.snapshot()
        abi.close()
    report['artifact_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
print(json.dumps({key: value for key, value in report.items() if key != 'cases'}))
