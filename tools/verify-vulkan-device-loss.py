"""Recheck immutable core/host loss evidence against current sources and pixels."""
import argparse
import hashlib
import json
from pathlib import Path
from PySide6.QtCore import QSettings
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--hardware', type=Path, required=True)
parser.add_argument('--software', type=Path, required=True)
parser.add_argument('--core-hardware', type=Path, required=True)
parser.add_argument('--core-software', type=Path, required=True)
parser.add_argument('--source-audit', type=Path, required=True)
parser.add_argument('--runtime-audit', type=Path, required=True)
parser.add_argument('--fixture-imports', type=Path, required=True)
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-device-loss-proof.json')
args = parser.parse_args()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes(values, directory=root):
    for name, expected in values.items():
        assert sha(directory / name) == expected, f'Input changed: {name}'


settings = QSettings(QSettings.NativeFormat, QSettings.UserScope, 'INRIA', 'Natron')
values = {key: settings.value(key) for key in settings.allKeys()}
settings_sha = hashlib.sha256(json.dumps(values, sort_keys=True, default=str).encode()).hexdigest()
cases, pixels = [], 0
for path, software in ((args.hardware, False), (args.software, True)):
    report = read(path)
    assert report['passed'] and report['environmentRestored'] and not report['physicalDriverReset']
    assert report['settingsBefore'] == report['settingsAfter'] == settings_sha
    hashes(report['proofHashes'])
    expected = {(g, 'render', '8u', 1) for g in ((0,) if software else (0, 1))}
    expected |= {(g, 'viewer', d, s) for g in ((0,) if software else (0, 1))
                 for d in ('8u', '32f') for s in (1, 2)}
    actual = {(c['gpu'], c['mode'], c['depth'], c['scale']) for c in report['cases']}
    assert expected == actual and len(expected) == len(report['cases'])
    for case in report['cases']:
        result, directory = case['result'], Path(case['artifacts'])
        assert directory.is_relative_to(root / 'build') and read(directory / 'result.json') == result
        assert result['ok'] and result['validation_errors'] == 0 and not result['physical_driver_reset']
        if software:
            assert report['icd'] and 'SwiftShader' in result['device']
        profile = Path(result['profile'])
        assert profile.is_relative_to(root / 'build/test-profiles')
        assert (profile / 'started.txt').exists() and not (profile / 'timeout.json').exists()
        stderr = (profile / 'stderr.log').read_text(encoding='utf-8')
        assert not any(e in stderr for e in ('VUID-', 'Vulkan validation error:', 'Vulkan runtime error:', 'Vulkan Viewer:'))
        assert all(c['new_generation'] > c['old_generation'] and c['retired_statuses'] == [1001] * 5
                   for c in result['cases'])
        if case['mode'] == 'render':
            assert len(result['cases']) == 5 and result['action_loss_count'] == 1
            assert [c['point'] for c in result['cases'] if 'point' in c and not c.get('gpu_required_shader')] == [1, 2]
            assert sum(bool(c.get('ofx_action_loss')) for c in result['cases']) == 1
            assert sum(bool(c.get('ocio_processor_recreated')) for c in result['cases']) == 1
            assert sum(bool(c.get('gpu_required_shader')) for c in result['cases']) == 1
            assert result['final_counters'][1] == result['final_counters'][3] == 0
            assert 'NATRON_INTERPRETER_EOF_OK' in (profile / 'stdout.log').read_text(encoding='utf-8')
            assert len(result['artifact_sha256']) == 8
        else:
            assert [c['point'] for c in result['cases']] == [1, 2, 3]
            assert not result['unexpected_dialogs'] and result['native_recovered']
            assert len(result['artifact_sha256']) == 8
            for lost in result['cases']:
                assert lost['after'][0] > lost['before'][0] and lost['after'][1] == lost['before'][1]
                viewers = lost['viewers']
                assert len(viewers) == 2 and viewers[0]['device_id'] == viewers[1]['device_id']
                assert all(w['backend'] == 'Vulkan' and w['device_id'] == w['engine_device_id'] and
                           w['frames'] > 1 and w['native_transfers'] > 0 and not w['error'] and not w['validation_errors']
                           for w in viewers)
        hashes(result['artifact_sha256'], directory)
        compared = 0
        for name in result['artifact_sha256']:
            image = QImage(str(directory / name)).convertToFormat(QImage.Format_RGBA8888)
            assert not image.isNull()
            if case['mode'] == 'render':
                assert (image.width(), image.height()) == (64, 48)
                x1, x2, y1, y2 = 0, image.width(), 0, image.height()
            else:
                assert (image.width(), image.height()) == (480, 360)
                x1, x2, y1, y2 = image.width()//5, 4*image.width()//5, image.height()//5, 4*image.height()//5
            data, row = bytes(image.constBits()), bytes((0, 255, 0, 255)) * (x2-x1)
            assert all(data[y*image.bytesPerLine()+4*x1:y*image.bytesPerLine()+4*x2] == row for y in range(y1, y2)), name
            compared += (x2-x1)*(y2-y1)
        assert compared == result['pixels_compared']
        pixels += compared
        cases.append({key: case[key] for key in ('gpu', 'mode', 'depth', 'scale')})

software_core = read(args.core_software)
assert software_core['passed'] and software_core['environmentRestored']
hashes(software_core['inputSha256'])
loss_core = [c for c in software_core['cases'] if c['test'] == 'DeviceLoss']
assert len(loss_core) == 1
for result in [loss_core[0]['result']] + [read(args.core_hardware / f'gpu{g}/DeviceLoss.stdout.log') for g in (0, 1)]:
    assert result['passed'] and result['injected_loss_points'] == result['replacements'] == 3
    assert result['retired_operation_rejections'] == 126 and result['compared_pixels'] == 1152
    assert not result['physical_driver_reset'] and result['validation_errors'] == 0
    assert result['recording_loss_rejected']

source = read(args.source_audit)
assert source['passed'] and source['ofxOpenGLSuiteDisabled']
hashes(source['fileSha256'])
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
runtime = read(args.runtime_audit)
assert runtime['passed'] and runtime['filesChecked'] == 332 and runtime['runtimeBinDlls'] == 184
hashes({f['file']: f['sha256'] for f in runtime['files']})
fixture = read(args.fixture_imports)
assert fixture['passed'] and sha(root / fixture['file']) == fixture['sha256']
assert not any(d.lower() in ('opengl32.dll', 'qt6opengl.dll', 'qt6openglwidgets.dll', 'libegl.dll', 'libglesv2.dll')
               for d in fixture['imports'])
proof = {'passed': True, 'hostCases': len(cases), 'coreLossCases': 3, 'pixelsCompared': pixels,
         'completionPlatform': 'windows', 'excludedTests': ['parsec'],
         'sourceFilesChecked': source['sourceFilesChecked'], 'peFilesChecked': runtime['filesChecked'],
         'userSettingsSha256': settings_sha, 'cases': cases, 'physicalDriverReset': False,
         'fullVulkanMigrationComplete': False,
         'remainingGates': ['Physical Windows driver recovery',
                            'Separate Windows performance/package acceptance',
                            'Complete functional equivalence of all shipped OFX algorithms',
                            'Remote Windows CI and clean-machine validation'],
         'inputs': {str(p): sha(p) for p in (args.hardware, args.software, args.core_software,
                                            args.source_audit, args.runtime_audit, args.fixture_imports, Path(__file__))}}
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in proof.items() if k not in ('cases', 'inputs')}))
