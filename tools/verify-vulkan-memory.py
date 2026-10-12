"""Independently verify the current Windows allocation-failure matrix and pixels."""
import argparse
import hashlib
import json
from pathlib import Path
from PySide6.QtGui import QImage

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--matrix', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
read = lambda path: json.loads(path.read_text(encoding='utf-8-sig'))
sha = lambda path: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
matrix = read(args.matrix)
assert matrix['passed'] and matrix['settingsBefore'] == matrix['settingsAfter']
assert len(matrix['core']) == 2 and len(matrix['cases']) == 12
inputs = {str(args.matrix.resolve()): sha(args.matrix), str(Path(__file__).resolve()): sha(Path(__file__))}
for name, expected in matrix['proofHashes'].items():
    path = ROOT/name
    assert sha(path) == expected, name
    inputs[str(path.resolve())] = expected
for core in matrix['core']:
    result = core['result']
    assert result['passed'] and result['validation_errors'] == 0
    assert result['allocation_failures'] == result['post_oom_retries'] == 3
    assert result['limit_bytes'] == 32*1024*1024 and result['peak_block_bytes'] <= result['limit_bytes']
    assert result['concurrent_transfers'] == 48

seen, pixels = set(), 0
for case in matrix['cases']:
    directory = args.matrix.parent/case['name']
    result = case['result']
    assert result == read(directory/'result.json') and result['ok']
    profile = Path(result['profile'])
    assert profile.is_relative_to(ROOT/'build/test-profiles')
    assert (profile/'started.txt').exists() and not (profile/'timeout.json').exists()
    stderr = (profile/'stderr.log').read_text(encoding='utf-8-sig')
    assert not any(s in stderr for s in ('VUID-', 'Vulkan validation error:', 'Traceback'))
    if case['kind'] == 'engine':
        oversized = result['host_allocation_retry']
        seen.add((case['gpu'], 'engine', oversized))
        assert result['cpu_retry'] and result['gpu_only_failed'] and result['limit_mb'] == '32'
        assert result['counters'][0] == (0 if oversized else 1) and result['counters'][1] >= 1
        assert result['counters'][2:4] == [0 if oversized else 1, 0]
        actual = QImage(str(directory/'fallback.png')).convertToFormat(QImage.Format_RGBA8888)
        reference = QImage(str(directory/'reference.png')).convertToFormat(QImage.Format_RGBA8888)
        assert not actual.isNull() and not reference.isNull()
        assert (actual.width(), actual.height()) == (reference.width(), reference.height()) == (result['width'], result['height'])
        assert bytes(actual.constBits()) == bytes(reference.constBits())
        assert result['pixels_compared'] == actual.width()*actual.height()
        pixels += result['pixels_compared']
    else:
        seen.add((case['gpu'], 'viewer', result['mode'], case['name'].rsplit('-', 1)[-1]))
        assert result['initial_frames'] > 1 and result['last_backend'] == 'Raster' and result['raster_frames'] > 1
        assert result['validation_errors'] == 0 and not result['unexpected_dialogs']
        assert 'VkResult -2' in result['fallback_reason']
        if result['mode'] == 'native-transfer':
            assert result['initial_native_transfers'] > 0
            assert result['fallback_reason'].startswith('Native Viewer image transfer:')
        path = directory/'recovered.png'
        assert sha(path) == result['screenshot_sha256']
        image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
        assert (image.width(), image.height()) == (640, 480)
        data = bytes(image.constBits())
        row = bytes((0, 255, 0, 255))*384
        assert all(data[y*image.bytesPerLine()+128*4:y*image.bytesPerLine()+512*4] == row for y in range(96, 384))
        pixels += 384*288
    for path in directory.glob('*.png'):
        inputs[str(path.resolve())] = sha(path)
    inputs[str((directory/'result.json').resolve())] = sha(directory/'result.json')

expected = {(g, 'engine', oversized) for g in (0, 1) for oversized in (False, True)}
expected |= {(g, 'viewer', mode, depth) for g in (0, 1) for mode, depth in
             (('transfer', '32f'), ('native-transfer', '32f'), ('canvas', '8u'), ('canvas', '32f'))}
assert seen == expected
proof = {'passed': True, 'completionPlatform': 'windows', 'fullVulkanMigrationComplete': False,
         'scope': 'Bounded allocation, CPU retry, GPU-only rejection and actual Viewer raster recovery',
         'coreCases': 2, 'engineCases': 4, 'viewerCases': 8, 'pixelsCompared': pixels,
         'userSettingsUnchanged': True, 'inputSha256': inputs}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in proof.items() if k != 'inputSha256'}))
