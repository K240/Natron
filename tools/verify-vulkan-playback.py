"""Check the Windows playback matrix, current inputs and captured pixels."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import runpy

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--report', type=Path, required=True)
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-windows-playback-proof.json')
args = parser.parse_args()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hashes(values):
    for file, expected in values.items():
        assert sha(root / file) == expected, file


report = read(args.report)
assert report['passed'] and report['environmentRestored'] and report['continuousPlaybackTested']
assert report['completionPlatform'] == 'windows' and not report['parsecTested'] and not report['physicalScanoutMeasured']
assert report['settingsBefore'] == report['settingsAfter']
hashes(report['inputSha256'])
source_path = root / 'build/viewer-presentation-source-audit.json'
runtime_path = root / 'build/viewer-presentation-runtime-audit.json'
source, runtime = read(source_path), read(runtime_path)
assert source['passed'] and source['ofxOpenGLSuiteDisabled'] and runtime['passed']
hashes(source['fileSha256'])
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
hashes({record['file']: record['sha256'] for record in runtime['files']})
expected = set(itertools.product((0, 1), ('4K', '8K'), ('8u', '32f'), ('native', 'raster')))
actual = {(case['gpu'], case['resolution'], case['depth'], case['mode']) for case in report['cases']}
assert actual == expected and len(report['cases']) == len(expected)
check_pixels = runpy.run_path(str(root / 'tools/vulkan-viewer-reference.py'))['check_pixels']
names = ('forward', 'backward', 'loop', 'bounce', 'cancel', 'restart', 'second-backward')
pixels, transitions, stages = 0, 0, 0
records = {}
for case in report['cases']:
    assert not case['failure']
    path = Path(case['result'])
    result = read(path)
    assert result['ok'] and len(result['stages']) == 7 and len(result['captures']) == 14
    assert result['mode'] == case['mode'] and result['source_size'] == ([3840, 2160] if case['resolution'] == '4K' else [7680, 4320])
    assert result['final_statistics']['validation_errors'] == 0
    profile = root / case['profile']
    assert Path(result['profile']) == profile and (profile / 'started.txt').is_file()
    assert not (profile / 'timeout.json').exists()
    assert 'NATRON_PLAYBACK_EXIT_REQUESTED' in (profile / 'stdout.log').read_text(encoding='utf-8-sig')
    errors = (profile / 'stderr.log').read_text(encoding='utf-8-sig')
    assert not any(marker in errors for marker in ('VUID-', 'Vulkan validation error:', 'Vulkan runtime error:', 'Vulkan Viewer:'))
    for file in (path, profile / 'started.txt', profile / 'stdout.log', profile / 'stderr.log', profile / 'progress.json'):
        records[str(file.relative_to(root))] = sha(file)
    assert [stage['name'] for stage in result['stages']] == list(names)
    for stage in result['stages']:
        assert stage['pause_stable'] and stage['state'] == 'stopping'
        frames = [row['frame'] for row in stage['frames']]
        assert len(frames) >= 3 and len(stage['viewer_frames']) == 2
        for index, stream in enumerate(stage['viewer_frames']):
            assert len(stream) >= (1 if stage['name'] == 'cancel' and index != stage['viewer'] else 3)
        generations = {row['generation'] for row in stage['frames']}
        assert len(generations) == 1
        assert (next(iter(generations)) > 0) == (case['mode'] == 'native')
        times = [row['elapsed_ms'] for row in stage['frames']]
        assert all(a < b for a, b in zip(times, times[1:]))
        low, high = sorted((stage['begin'], stage['end']))
        assert all(low <= frame <= high for frame in frames)
        if stage['name'] in ('forward', 'restart', 'cancel'):
            assert all(a < b for a, b in zip(frames, frames[1:]))
        if stage['name'] in ('backward', 'second-backward'):
            assert all(a > b for a, b in zip(frames, frames[1:]))
        if stage['name'] == 'loop':
            assert sum(a == 4 and b == 1 for a, b in zip(frames, frames[1:])) >= 2
        if stage['name'] == 'bounce':
            assert len(frames) >= 13 and 4 in frames and frames[-1] == 1
            assert any(a > b for a, b in zip(frames, frames[1:])) and any(a < b for a, b in zip(frames, frames[1:]))
        if stage['name'] in ('forward', 'backward', 'restart', 'second-backward'):
            assert stage['stop_frame'] == stage['end']
        transitions += sum(len(stream) - 1 for stream in stage['viewer_frames'])
        stages += 1
    for capture in result['captures']:
        file = path.parent / capture['file']
        assert sha(file) == capture['sha256']
        metadata = capture['presentation']
        assert metadata['surfaceSize'] == [960, 540] and metadata['storage0'] == case['depth']
        assert metadata['backend'] == ('Raster' if case['mode'] == 'raster' else 'Vulkan')
        _, checked = check_pixels(file, result['source_size'], int(capture['frame']), metadata)
        assert capture['pixels'] == checked['pixels'] and capture['max_error'] == checked['maximum_byte_error']
        records[str(file.relative_to(root))] = sha(file)
        pixels += checked['pixels']
proof = {'passed': True, 'completionPlatform': 'windows', 'applicationCases': 16,
         'playbackStages': stages, 'presentedFrameTransitions': transitions, 'pixelsCompared': pixels,
         'sourceFilesChecked': source['sourceFilesChecked'], 'peFilesChecked': runtime['filesChecked'],
         'userSettingsSha256': report['settingsBefore'], 'continuousPlaybackTested': True,
         'parsecTested': False, 'physicalScanoutMeasured': False, 'fullVulkanMigrationComplete': False,
         'scope': 'real scheduler at requested 8 fps, two visible Viewers, forward/backward/loop/bounce/pause/restart; functional evidence, not playback performance',
         'inputs': {str(path.relative_to(root)): sha(path) for path in (args.report.resolve(), source_path, runtime_path)},
         'artifactSha256': records}
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({key: value for key, value in proof.items() if key not in ('artifactSha256', 'inputs')}))
