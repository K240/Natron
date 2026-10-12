"""Independently verify bounded Natron application performance evidence."""
import argparse
import csv
import hashlib
import io
import json
import math
from pathlib import Path
import re
from PySide6.QtCore import QSettings
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--report', type=Path, required=True)
parser.add_argument('--source-audit', type=Path, required=True)
parser.add_argument('--runtime-audit', type=Path, required=True)
parser.add_argument('--fixture-imports', type=Path, required=True)
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-application-performance-proof.json')
args = parser.parse_args()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes(values):
    for name, expected in values.items():
        assert sha(root / name) == expected, f'Input changed: {name}'


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    low = math.floor(index)
    return ordered[low] + (ordered[math.ceil(index)] - ordered[low]) * (index - low)


def pixels(path, width, height, frame):
    image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and (image.width(), image.height()) == (64, 48), str(path)
    data = bytes(image.constBits())
    error = 0
    for row in range(48):
        y = (47 - row) * (height - 1) // 47
        for column in range(64):
            x = column * (width - 1) // 63
            expected = (round(((x + frame) & 63) / 128 * 1.2 * 255),
                        round(((y + frame * 3) & 63) / 128 * .8 * 255),
                        round(((x + y + frame * 7) & 63) / 128 * 1.1 * 255), 255)
            error = max(error, *(abs(data[(row * 64 + column) * 4 + c] - expected[c]) for c in range(4)))
    assert error <= 1, (path, error)
    return data, error


report = read(args.report)
assert report['passed'] and report['environmentRestored']
assert not report['viewerWsiTested'] and not report['parsecTested'] and not report['glBaselineTested']
assert not report['fullVulkanMigrationComplete']
hashes(report['proofHashes'])
settings = QSettings(QSettings.NativeFormat, QSettings.UserScope, 'INRIA', 'Natron')
values = {key: settings.value(key) for key in settings.allKeys()}
settings_sha = hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest()
assert report['settingsBefore'] == report['settingsAfter'] == settings_sha
assert len(report['cases']) == 8
assert {(c['gpu'], c['resolution'], c['validation']) for c in report['cases']} == {
    (gpu, resolution, validation) for gpu in (0, 1) for resolution in ('4K', '8K') for validation in (0, 1)}
rows, compared, timed_samples = [], 0, 0
for case in report['cases']:
    result, directory = case['result'], Path(case['artifacts'])
    assert read(directory / 'result.json') == result
    assert result['ok'] and result['warmup'] == 3 and result['samples'] == 20
    assert result['validation_enabled'] == bool(case['validation']) and result['validation_errors'] == 0
    assert not result['unexpected_dialogs'] and not result['viewer_wsi_tested'] and not result['parsec_tested']
    width, height = (3840, 2160) if case['resolution'] == '4K' else (7680, 4320)
    assert result['source_size'] == [width, height] and result['output_size'] == [64, 48]
    assert result['full_reference_pixels'] == width * height and result['full_reference_max_error'] <= 2e-6
    accounting = result['accounting_check']
    assert accounting['passed'] and accounting['compared_pixels'] == 221
    for field, delta in (('image_uploads', 1), ('image_downloads', 1), ('image_upload_bytes', 3536), ('image_download_bytes', 3536)):
        assert accounting['after'][field] - accounting['before'][field] == delta
    profile = Path(result['profile'])
    assert (profile / 'started.txt').exists()
    assert 'NATRON_INTERPRETER_EOF_OK' in (profile / 'stdout.log').read_text(encoding='utf-8-sig', errors='replace')
    stderr = (profile / 'stderr.log').read_text(encoding='utf-8-sig', errors='replace')
    assert not re.search(r'VUID-|Vulkan validation error:|Traceback|Vulkan runtime error:', stderr), profile
    actual_artifacts = {}
    assert len(result['artifact_sha256']) == 47
    for name, expected_sha in result['artifact_sha256'].items():
        assert sha(directory / name) == expected_sha, name
        frame = int(name.rsplit('-', 1)[1].split('.')[0])
        actual_artifacts[name], _ = pixels(directory / name, width, height, frame)
    for frame in range(1, 24):
        assert max(abs(a - b) for a, b in zip(actual_artifacts[f'cpu-{frame:04d}.png'], actual_artifacts[f'native-{frame:04d}.png'])) <= 1
    assert [c['mode'] for c in result['cases']] == ['cpu', 'native']
    for graph in result['cases']:
        measurements = graph['measurements']
        assert len(measurements) == 20 and [m['frame'] for m in measurements] == list(range(4, 24))
        durations = [m['duration_ms'] for m in measurements]
        assert all(math.isfinite(d) and d > 0 for d in durations)
        assert abs(graph['p50_ms'] - percentile(durations, .5)) < 1e-9
        assert abs(graph['p95_ms'] - percentile(durations, .95)) < 1e-9
        assert graph['managed_local_peak_block_bytes_lifetime'] == max(m['after']['managed_local_peak_block_bytes'] for m in measurements)
        assert graph['driver_local_usage_bytes_sampled_max'] == max(m['after']['driver_local_usage_bytes'] for m in measurements)
        expected_transfers = {'image_uploads': 0, 'image_downloads': int(graph['mode'] == 'native'),
                             'image_upload_bytes': 0, 'image_download_bytes': 49152 if graph['mode'] == 'native' else 0}
        for m in measurements:
            assert m['transfers'] == expected_transfers
            for field, delta in expected_transfers.items():
                assert m['after'][field] - m['before'][field] == delta
            assert m['before']['generation'] == m['after']['generation'] > 0
            for snapshot in (m['before'], m['after']):
                assert snapshot['validation_errors'] == 0
                assert snapshot['managed_local_peak_block_bytes'] >= snapshot['managed_local_block_bytes'] >= snapshot['managed_local_allocation_bytes']
                if snapshot['driver_budget_available']:
                    assert snapshot['driver_local_budget_bytes'] > 0
            before, after = m['counter_before'], m['counter_after']
            assert after[6:10] == [width, height, width, height]
            assert [after[i] - before[i] for i in range(4)] == ([1, 0, 1, 0] if graph['mode'] == 'native' else [0, 1, 0, 1])
            assert (m['native_ocio_actions'] > 0) == (graph['mode'] == 'native')
            _, error = pixels(directory / m['png'], width, height, m['frame'])
            assert error == m['reference_byte_error']
        timed_samples += len(measurements)
        rows.append({'gpu': case['gpu'], 'device': result['device'], 'resolution': case['resolution'],
                     'validation': case['validation'], 'mode': graph['mode'], 'samples': len(measurements),
                     'p50_ms': graph['p50_ms'], 'p95_ms': graph['p95_ms'],
                     'pixel_upload_bytes_per_frame': expected_transfers['image_upload_bytes'],
                     'pixel_download_bytes_per_frame': expected_transfers['image_download_bytes'],
                     'managed_local_peak_block_bytes_lifetime': graph['managed_local_peak_block_bytes_lifetime'],
                     'driver_local_usage_bytes_sampled_max': graph['driver_local_usage_bytes_sampled_max']})
    count = len(actual_artifacts) * 64 * 48 + width * height + 221
    assert result['pixels_compared'] == count
    compared += count
source, runtime, fixture = read(args.source_audit), read(args.runtime_audit), read(args.fixture_imports)
assert source['passed'] and source['ofxOpenGLSuiteDisabled']
hashes(source['fileSha256'])
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
assert runtime['passed']
hashes({f['file']: f['sha256'] for f in runtime['files']})
assert fixture['passed'] and sha(root / fixture['file']) == fixture['sha256']
assert not any(name.lower() in ('opengl32.dll', 'qt6opengl.dll', 'qt6openglwidgets.dll', 'libegl.dll', 'libglesv2.dll') for name in fixture['imports'])
csv_output = io.StringIO(newline='')
writer = csv.DictWriter(csv_output, fieldnames=list(rows[0]))
writer.writeheader()
writer.writerows(rows)
table_path = args.output.with_suffix('.csv')
table_path.write_text(csv_output.getvalue(), encoding='utf-8-sig')
proof = {'passed': True, 'applicationCases': 8, 'graphCases': len(rows), 'timedSamples': timed_samples,
         'pixelsCompared': compared, 'sourceFilesChecked': source['sourceFilesChecked'], 'peFilesChecked': runtime['filesChecked'],
         'userSettingsSha256': settings_sha, 'rows': rows, 'table': str(table_path),
         'memoryPeakScope': 'callback tracked lifetime VMA device-local blocks; driver usage is sampled, not a peak',
         'performanceScope': 'cold-cache headless application OCIO processing and small PNG boundary; cache purge excluded',
         'viewerWsiTested': False, 'parsecTested': False, 'glBaselineTested': False, 'fullVulkanMigrationComplete': False,
         'remainingGates': ['Viewer playback/scrubbing/multiple windows performance, driver VRAM peak and legacy GL baseline',
                            'all shipped plugin functionality and external OFX compatibility UI',
                            'reproducible software-Vulkan CI/package/Linux/macOS validation'],
         'inputs': {str(path): sha(path) for path in (args.report, args.source_audit, args.runtime_audit, args.fixture_imports)}}
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({key: value for key, value in proof.items() if key not in ('rows', 'inputs')}))
