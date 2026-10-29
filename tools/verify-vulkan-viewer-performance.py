"""Verify actual Viewer seek/presentation performance and independent pixels."""
import argparse
import csv
import hashlib
import io
import itertools
import json
import math
from pathlib import Path
import re
import runpy
from PySide6.QtCore import QSettings

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--reports', nargs=2, type=Path, required=True, metavar=('PRODUCTION', 'VALIDATION'))
parser.add_argument('--source-audit', type=Path, required=True)
parser.add_argument('--runtime-audit', type=Path, required=True)
parser.add_argument('--fixture-imports', type=Path, required=True)
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-viewer-performance-proof.json')
args = parser.parse_args()
check_pixels = runpy.run_path(str(root / 'tools/vulkan-viewer-reference.py'))['check_pixels']


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes(values):
    for name, digest in values.items():
        assert sha(root / name) == digest, f'Changed input: {name}'


def key(case):
    return tuple(case[name] for name in ('gpu', 'resolution', 'depth', 'viewerCount', 'validation', 'mode'))


def percentile(values, fraction):
    ordered = sorted(values)
    at = (len(ordered) - 1) * fraction
    low = math.floor(at)
    return ordered[low] + (ordered[math.ceil(at)] - ordered[low]) * (at - low)


reports = [read(path) for path in args.reports]
assert reports[0]['proofHashes'] == reports[1]['proofHashes']
hashes(reports[0]['proofHashes'])
settings = QSettings(QSettings.NativeFormat, QSettings.UserScope, 'INRIA', 'Natron')
values = {name: settings.value(name) for name in settings.allKeys()}
settings_sha = hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest()
expected = [set(itertools.product((0, 1), ('4K', '8K'), ('8u', '32f'), (1, 2), (0,), ('native', 'cpu', 'raster'))),
            set(itertools.product((0, 1), ('4K', '8K'), ('8u', '32f'), (2,), (1,), ('native', 'cpu')))]
rows, comparisons, samples, checked_pixels = [], [], 0, 0
for report, required in zip(reports, expected):
    assert report['passed'] and report['environmentRestored']
    assert report['settingsBefore'] == report['settingsAfter'] == settings_sha
    assert report['viewerWsiTested'] and not report['continuousPlaybackTested'] and not report['physicalScanoutMeasured']
    assert not report['parsecTested'] and not report['glBaselineTested'] and not report['fullVulkanMigrationComplete']
    assert {key(case) for case in report['cases']} == required and len(report['cases']) == len(required)
    captured = {}
    for case in report['cases']:
        result, directory = case['result'], Path(case['artifacts'])
        assert read(directory / 'result.json') == result and not case['runFailure']
        assert result['ok'] and result['warmup'] == 3 and result['samples'] == 20
        assert result['mode'] == case['mode'] and result['viewer_count'] == case['viewerCount']
        assert result['validation_enabled'] == bool(case['validation']) and result['validation_errors'] == 0
        assert not result['unexpected_dialogs'] and not result['full_vulkan_migration_complete']
        assert not result['continuous_playback_tested'] and not result['physical_scanout_measured'] and not result['parsec_tested']
        width, height = (3840, 2160) if case['resolution'] == '4K' else (7680, 4320)
        assert result['source_size'] == [width, height] and result['surface_size'] == [960, 540]
        profile = Path(result['profile'])
        assert (profile / 'started.txt').exists() and not (profile / 'timeout.json').exists()
        assert 'NATRON_VIEWER_PERFORMANCE_EXIT_REQUESTED' in (profile / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
        assert f'GUI smoke profile and logs: {profile}' in (directory / 'run.log').read_text(encoding='utf-8-sig', errors='replace')
        stderr = (profile / 'stderr.log').read_text(encoding='utf-8-sig', errors='replace')
        assert not re.search(r'VUID-|Vulkan validation error:|Traceback|Vulkan runtime error:|Vulkan Viewer:', stderr)
        measurements = result['measurements']
        assert len(measurements) == 20 and [m['frame'] for m in measurements] == list(range(5, 25))
        durations = []
        for measurement in measurements:
            before, after = measurement['before'], measurement['after']
            assert before['generation'] == after['generation'] and before['validation_errors'] == after['validation_errors'] == 0
            assert after['managed_local_allocation_bytes'] <= after['managed_local_block_bytes'] <= after['managed_local_peak_block_bytes']
            assert measurement['counter_after'][6:8] == [width, height]
            index = 0 if case['mode'] == 'native' else 1
            assert measurement['counter_after'][index] > measurement['counter_before'][index]
            assert measurement['counter_after'][1-index] == measurement['counter_before'][1-index] == 0
            assert len(measurement['viewers']) == case['viewerCount']
            factor = 4 if width == 3840 else 8
            for viewer in measurement['viewers']:
                meta = viewer['presentation']
                assert meta['time0'] == measurement['frame'] and meta['surfaceSize'] == [960, 540]
                assert meta['format0'] == meta['rod0'] == [0, 0, width, height]
                assert meta['storage0'] == case['depth'] and meta['mipmap0'] == (2 if width == 3840 else 3)
                x, y, w, h = meta['bounds0']
                assert x <= 0 and y <= 0 and x+w >= width // factor and y+h >= height // factor
                assert not meta['capture'] and not meta['overlays']
                assert meta['backend'] == ('Raster' if case['mode'] == 'raster' else 'Vulkan')
                assert meta['generation'] == (0 if case['mode'] == 'raster' else after['generation'])
                assert math.isfinite(viewer['duration_ms']) and viewer['duration_ms'] > 0
            duration = max(viewer['duration_ms'] for viewer in measurement['viewers'])
            assert duration == measurement['all_viewers_duration_ms']
            durations.append(duration)
            delta = {name: after[name]-before[name] for name in ('image_uploads', 'image_downloads', 'image_upload_bytes', 'image_download_bytes')}
            uploads = case['viewerCount'] if case['mode'] == 'cpu' else 0
            assert delta == measurement['transfers'] == {'image_uploads': uploads, 'image_downloads': 0,
                'image_upload_bytes': uploads * (width // factor) * (height // factor) * 16, 'image_download_bytes': 0}
        assert abs(result['p50_ms'] - percentile(durations, .5)) < 1e-9
        assert abs(result['p95_ms'] - percentile(durations, .95)) < 1e-9
        assert len(result['captures']) == len(result['artifact_sha256']) == case['viewerCount'] * 2
        assert {(c['frame'], c['file'].rsplit('viewer', 1)[1]) for c in result['captures']} == {
            (frame, f'{index}.png') for frame in (2, 24) for index in range(case['viewerCount'])}
        for capture in result['captures']:
            name = capture['file']
            assert sha(directory / name) == result['artifact_sha256'][name]
            assert capture['presentation']['time0'] == capture['frame'] and not capture['presentation']['overlays']
            pixels, reference = check_pixels(directory / name, result['source_size'], capture['frame'], capture['presentation'])
            checked_pixels += reference['pixels']
            index = int(name.rsplit('viewer', 1)[1].split('.')[0])
            captured[key(case), capture['frame'], index] = pixels
            comparisons.append({'case': key(case), 'file': name, **reference})
        if case['viewerCount'] == 2:
            for frame in (2, 24):
                assert max(abs(a-b) for a, b in zip(captured[key(case), frame, 0], captured[key(case), frame, 1])) <= 1
        samples += len(measurements)
        rows.append({'gpu': case['gpu'], 'device': result['device'], 'resolution': case['resolution'], 'depth': case['depth'],
            'viewers': case['viewerCount'], 'validation': case['validation'], 'mode': case['mode'], 'samples': len(measurements),
            'p50_ms': result['p50_ms'], 'p95_ms': result['p95_ms'], 'upload_bytes_per_seek': measurements[0]['transfers']['image_upload_bytes'],
            'download_bytes_per_seek': 0, 'managed_local_peak_block_bytes_lifetime': result['final_statistics']['managed_local_peak_block_bytes'],
            'driver_local_usage_bytes_sampled_max': max(m['after']['driver_local_usage_bytes'] for m in measurements)})
    for gpu, resolution, depth, count, validation in {(k[0], k[1], k[2], k[3], k[4]) for k in required}:
        for frame in (2, 24):
            native = captured[(gpu, resolution, depth, count, validation, 'native'), frame, 0]
            for mode in ('cpu', 'raster'):
                if (gpu, resolution, depth, count, validation, mode) not in required:
                    continue
                other = captured[(gpu, resolution, depth, count, validation, mode), frame, 0]
                maximum = max(abs(a-b) for a, b in zip(native, other))
                assert maximum <= (2 if mode == 'raster' and depth == '8u' else 1), (gpu, resolution, depth, count, mode, maximum)
source, runtime, fixture = [read(path) for path in (args.source_audit, args.runtime_audit, args.fixture_imports)]
assert source['passed'] and source['ofxOpenGLSuiteDisabled']
hashes(source['fileSha256'])
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
assert runtime['passed']
hashes({record['file']: record['sha256'] for record in runtime['files']})
assert fixture['passed'] and sha(root / fixture['file']) == fixture['sha256']
assert not any(name.lower() in ('opengl32.dll', 'qt6opengl.dll', 'qt6openglwidgets.dll', 'libegl.dll', 'libglesv2.dll') for name in fixture['imports'])
table = io.StringIO(newline='')
writer = csv.DictWriter(table, fieldnames=list(rows[0]))
writer.writeheader()
writer.writerows(rows)
table_path = args.output.with_suffix('.csv')
table_path.write_text(table.getvalue(), encoding='utf-8-sig')
proof = {'passed': True, 'applicationCases': len(rows), 'timedSamples': samples, 'pixelsCompared': checked_pixels,
    'rows': rows, 'pixelReferences': comparisons, 'table': str(table_path), 'userSettingsSha256': settings_sha,
    'sourceFilesChecked': source['sourceFilesChecked'], 'peFilesChecked': runtime['filesChecked'],
    'performanceScope': 'full-source cold-cache timeline seeks, all independent Viewer present/queue wait or raster paint; cache purge and PNG readback excluded',
    'memoryPeakScope': 'lifetime VMA device-local blocks; driver usage sampled, swapchain and driver-private peaks excluded',
    'viewerWsiTested': True, 'continuousPlaybackTested': False, 'physicalScanoutMeasured': False,
    'parsecTested': False, 'glBaselineTested': False, 'fullVulkanMigrationComplete': False,
    'remainingGates': ['continuous playback, physical scanout, driver VRAM peak and legacy GL baseline',
        'all shipped plugin functionality and external OFX compatibility UI', 'reproducible software-Vulkan CI/package/Linux/macOS validation'],
    'inputs': {str(path): sha(path) for path in (*args.reports, args.source_audit, args.runtime_audit, args.fixture_imports)}}
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({name: value for name, value in proof.items() if name not in ('rows', 'pixelReferences', 'inputs')}))
