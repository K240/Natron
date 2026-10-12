"""Revalidate cached-input and concurrent-Viewer pressure evidence on this build."""
import argparse
import hashlib
import json
from pathlib import Path

from PySide6.QtCore import QSettings
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cached-hardware', type=Path, required=True)
parser.add_argument('--cached-software', type=Path, required=True)
parser.add_argument('--viewer-hardware', type=Path, required=True)
parser.add_argument('--viewer-software', type=Path, required=True)
parser.add_argument('--source-audit', type=Path, required=True)
parser.add_argument('--runtime-audit', type=Path, required=True)
parser.add_argument('--fixture-imports', type=Path, required=True)
parser.add_argument('--device-loss-proof', type=Path, help='Optional separately validated recovery evidence for this same build')
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-memory-pressure-proof.json')
args = parser.parse_args()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_hashes(values, directory=root):
    for name, expected in values.items():
        assert sha(directory / name) == expected, f'Proof input changed: {name}'


settings = QSettings(QSettings.NativeFormat, QSettings.UserScope, 'INRIA', 'Natron')
values = {key: settings.value(key) for key in settings.allKeys()}
settings_sha = hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest()
cases, pixels = [], 0
for path, viewer, software in ((args.cached_hardware, False, False), (args.cached_software, False, True),
                               (args.viewer_hardware, True, False), (args.viewer_software, True, True)):
    report = read(path)
    assert report['passed'] and report['settingsBefore'] == report['settingsAfter'] == settings_sha, path
    check_hashes(report['proofHashes'])
    expected_gpus = (0,) if software else (0, 1)
    expected = {(g, s, d) for g in expected_gpus for s in (1, 2) for d in ('8u', '32f')} if viewer else set(expected_gpus)
    actual = {(c['gpu'], c['scale'], c['depth']) for c in report['cases']} if viewer else {c['gpu'] for c in report['cases']}
    assert actual == expected and len(report['cases']) == len(expected), path
    for case in report['cases']:
        result = case['result']
        assert result['ok'] and result['native_recovered'] and result['validation_errors'] == 0
        if software:
            assert report['icd'] and 'SwiftShader' in result['device'], result['device']
        directory = path.parent / (f"gpu{case['gpu']}-scale{case['scale']}-{case['depth']}" if viewer else f"gpu{case['gpu']}")
        assert read(directory / 'result.json') == result
        check_hashes(result['artifact_sha256'], directory)
        profile = Path(result['profile'])
        assert profile.is_relative_to(root / 'build/test-profiles')
        assert (profile / 'started.txt').exists() and not (profile / 'timeout.json').exists()
        stderr = (profile / 'stderr.log').read_text(encoding='utf-8')
        assert not any(error in stderr for error in ('VUID-', 'Vulkan validation error:', 'Vulkan runtime error:', 'Vulkan Viewer:'))
        if viewer:
            assert not result['qt_opengl_loaded'] and not result['unexpected_dialogs']
            assert result['display_scale'] == case['scale'] and result['overlapping_cpu_actions'] >= 2
            assert result['raster_count_under_pressure'] >= 1
            stages = {s['name']: s for s in result['stages']}
            initial = stages['initial']['viewers']
            assert len(initial) == 2 and all(w['backend'] == 'Vulkan' and w['frames'] > 1 and w['native_transfers'] for w in initial)
            assert all(w['device_id'] == w['engine_device_id'] == result['initial_device_id'] for w in initial)
            counts = stages['pressure']['counters']
            assert counts[3] >= 2 and counts[7] == 8 and counts[8] > 0 and counts[9] == 2
            assert counts[12] == 2 and counts[13] >= 2 and counts[14] >= 2 and counts[15] == 0
            assert counts[16] and counts[17]
            raster = [w for w in stages['pressure']['viewers'] if w['backend'] == 'Raster']
            assert len(raster) == result['raster_count_under_pressure']
            assert all('VkResult -2' in w['error'] and not w['device_id'] and not w['engine_device_id'] for w in raster)
            resumed = result['final_state'][-1]
            assert resumed['backend'] == 'Vulkan' and resumed['device_id'] == resumed['engine_device_id'] == result['initial_device_id']
            compared = 0
            for name in result['artifact_sha256']:
                image = QImage(str(directory / name)).convertToFormat(QImage.Format_RGBA8888)
                assert not image.isNull(), name
                rgb = (0, 255, 255) if name.startswith(('pressure-', 'recovered-')) else (0, 255, 0)
                x1, x2, y1, y2 = image.width()//5, 4*image.width()//5, image.height()//5, 4*image.height()//5
                data, row = bytes(image.constBits()), bytes((*rgb, 255)) * (x2-x1)
                assert all(data[y*image.bytesPerLine()+4*x1:y*image.bytesPerLine()+4*x2] == row for y in range(y1, y2)), name
                compared += (x2-x1)*(y2-y1)
            assert compared == result['pixels_compared'] and compared >= 1000000
        else:
            assert result['cached_native_input'] and result['cpu_retry'] and result['pressure_status'] == 8
            assert result['cached_probe_transfers'] == [0, 1] and not result['opengl_loaded']
            assert 'NATRON_INTERPRETER_EOF_OK' in (profile / 'stdout.log').read_text(encoding='utf-8')
            reference = QImage(str(directory / 'reference.png')).convertToFormat(QImage.Format_RGBA8888)
            assert (reference.width(), reference.height()) == (64, 48)
            assert bytes(reference.constBits()) == bytes((0, 255, 0, 255)) * (64*48)
            assert all(QImage(str(directory / name)) == QImage(str(directory / 'reference.png')) for name in result['artifact_sha256'])
            compared = 4*64*48
            assert compared == result['pixels_compared']
        pixels += compared
        cases.append({'report': str(path), 'gpu': case['gpu'], 'viewer': viewer, 'software': software,
                      'device': result['device'], 'pixels_compared': compared})

source, runtime, fixture = read(args.source_audit), read(args.runtime_audit), read(args.fixture_imports)
assert source['passed'] and not source['legacyApiViolations'] and not source['buildViolations'] and not source['retiredFilesStillPresent']
check_hashes(source['fileSha256'])
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
assert runtime['passed'] and runtime['filesChecked'] == len(runtime['files'])
for record in runtime['files']:
    assert sha(root / record['file']) == record['sha256'] and not record['legacyGraphicsImports']
assert fixture['passed'] and sha(root / fixture['file']) == fixture['sha256']
assert not set(fixture['imports']).intersection(('opengl32.dll', 'qt6opengl.dll', 'qt6openglwidgets.dll', 'libegl.dll', 'libglesv2.dll'))
loss = None
if args.device_loss_proof:
    loss = read(args.device_loss_proof)
    assert loss['passed'] and loss['hostCases'] == 15 and loss['coreLossCases'] == 3
    assert loss['sourceFilesChecked'] == source['sourceFilesChecked'] and loss['peFilesChecked'] == runtime['filesChecked']
    assert loss['userSettingsSha256'] == settings_sha and not loss['physicalDriverReset']
    check_hashes(loss['inputs'])
proof = {'passed': True, 'scope': 'Cached native input and concurrent Viewer recovery from real managed allocation refusal',
         'cases': cases, 'pixels_compared': pixels, 'settings_sha256': settings_sha,
         'sources_checked': source['sourceFilesChecked'], 'runtime_pe_files_checked': runtime['filesChecked'],
         'evidence_sha256': {str(p): sha(p) for p in (args.cached_hardware, args.cached_software, args.viewer_hardware,
                             args.viewer_software, args.source_audit, args.runtime_audit, args.fixture_imports)},
         'full_vulkan_migration_complete': False,
         'remaining_gates': ['Device lost reinitialization', 'Natron 4K/8K performance',
                             'All-node functional and external OFX UI acceptance', 'Software Vulkan CI, packaging and Linux/macOS']}
if loss:
    proof['device_loss_evidence'] = str(args.device_loss_proof)
    proof['evidence_sha256'][str(args.device_loss_proof)] = sha(args.device_loss_proof)
    proof['device_loss_pixels_compared'] = loss['pixelsCompared']
    proof['remaining_gates'].remove('Device lost reinitialization')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in proof.items() if k not in ('cases', 'evidence_sha256')}))
