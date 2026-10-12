"""Verify current Windows OFX declarations and independent GUI fixtures."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
from PySide6.QtGui import QImage

parser = argparse.ArgumentParser()
parser.add_argument('--before', type=Path, default=Path('build/drawsuite-inventory/native.json'))
parser.add_argument('--after', type=Path, default=Path('build/drawsuite-inventory/after-native.json'))
parser.add_argument('--matrix', type=Path, default=Path('build/windows-vulkan-ofx-interact-tests/report.json'))
parser.add_argument('--output', type=Path, default=Path('build/windows-vulkan-ofx-interact-proof.json'))
parser.add_argument('--recorded-verifier', type=Path,
                    help='Preserved verification script whose hash was recorded during an earlier GUI run; only this verification-only input may differ.')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
read = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
expected = {'net.sf.openfx.CheckerBoardPlugin', 'net.sf.openfx.ColorBars', 'net.sf.openfx.ColorWheel',
            'net.sf.openfx.ConstantPlugin', 'net.sf.openfx.Noise', 'net.sf.openfx.Radial',
            'net.sf.openfx.Rectangle', 'net.sf.openfx.Solid'}
key = lambda p: (Path(p['bundle']).name, p['id'], p['major'], p['minor'])


def issues(p):
    found = []
    for c in p['contexts']:
        if c['legacyOverlay'] and not c['drawSuiteOverlay']:
            found.append((c['context'], 'overlay', ''))
        for param in c['params']:
            if param['legacyCustom'] and not param['drawSuiteCustom']:
                found.append((c['context'], 'custom-ui', param['name']))
            if param['legacyBackground'] and not param['drawSuiteBackground']:
                found.append((c['context'], 'curve-background', param['name']))
    return found


before, after = read(args.before), read(args.after)
assert not before['failures'] and not after['failures']
b = {key(p): p for p in before['pluginVersions']}
a = {key(p): p for p in after['pluginVersions']}
assert b.keys() == a.keys() and len(a) == 579
assert {p['id'] for p in b.values() if issues(p)} == expected
assert not any(issues(p) for p in a.values())
for k, p in a.items():
    if p['id'] in expected:
        assert all(c['drawSuiteOverlay'] and not c['legacyOverlay'] for c in p['contexts'])
    else:
        assert p['contexts'] == b[k]['contexts'], f'Unrelated interact declaration changed: {k}'

matrix = read(args.matrix)
assert matrix['passed'] and matrix['completionPlatform'] == 'windows'
assert matrix['settingsBefore'] == matrix['settingsAfter']
cells = {(r['gpu'], r['scale'], r['depth'], r['backend'], r['test']) for r in matrix['cases']}
expected_cells = set(itertools.product((0, 1), (1, 2), ('8u', '32f'), ('Vulkan', 'Raster'), ('generator', 'legacy')))
assert len(matrix['cases']) == len(cells) == 32 and cells == expected_cells
for name, digest in matrix['inputSha256'].items():
    path = Path(name)
    if not path.is_absolute():
        path = root / path
    if path.resolve() == Path(__file__).resolve() and args.recorded_verifier:
        path = args.recorded_verifier
    assert sha(path) == digest, f'Current test input changed: {name}'
source = read(root / 'build/generator-drawsuite-source-audit.json')
runtime = read(root / 'build/generator-drawsuite-runtime-audit.json')
assert source['passed'] and runtime['passed'] and source['sourceFilesChecked'] == 738
for name, digest in source['fileSha256'].items():
    assert sha(root / name) == digest, name
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
for record in runtime['files']:
    assert sha(Path(record['file'])) == record['sha256'], record['file']
for p in a.values():
    assert str(Path(p['binary']).resolve()).lower() in {str(Path(r['file']).resolve()).lower() for r in runtime['files']}

artifacts, generator_cases, legacy_cases, cpu_pixels = {}, 0, 0, 0


def retain(path):
    artifacts[str(path.resolve())] = sha(path)


def image(path, size=None):
    result = QImage(str(path))
    assert not result.isNull(), path
    if size:
        assert [result.width(), result.height()] == size, path
    retain(path)
    return result


for row in matrix['cases']:
    assert row['passed'] and not row['failure'], row
    result_path = Path(row['result'])
    report = read(result_path)
    profile = Path(row['profile'])
    assert report['ok'] and report['profile'] == str(profile) and report['backend'] == row['backend']
    assert not report['unexpected_dialogs'] and not any(report['graphics_errors']) and not any(report['validation_errors'])
    assert not (profile / 'timeout.json').exists() and (profile / 'started.txt').is_file()
    stderr = (profile / 'stderr.log').read_text(encoding='utf-8-sig')
    assert not any(pattern in stderr for pattern in ('VUID-', 'Vulkan validation error:', 'Vulkan runtime error:', 'Vulkan Viewer:'))
    stdout = (profile / 'stdout.log').read_text(encoding='utf-8-sig')
    marker = 'NATRON_GENERATOR_OVERLAY_EXIT_REQUESTED' if row['test'] == 'generator' else 'NATRON_LEGACY_INTERACT_EXIT_REQUESTED'
    assert marker in stdout
    for path in (result_path, profile / 'stdout.log', profile / 'stderr.log', profile / 'started.txt'):
        retain(path)
    if row['test'] == 'generator':
        if row['backend'] == 'Raster':
            assert report['raster_explicitly_requested']
        assert len(report['cases']) == 8 and {c['plugin'] for c in report['cases']} == expected
        for case in report['cases']:
            assert case['undo_redo'] and case['focus_modifier_reset'] and case['kind'] == 'rectangle'
            before_value, after_value, final_value = case['before_value'], case['after_value'], case['final_value']
            assert abs(after_value[0] - before_value[0] - 12) <= case['tolerance']
            assert abs(after_value[1] - before_value[1] - 9) <= case['tolerance']
            assert abs(final_value[0] - after_value[0] - 5) <= case['tolerance']
            assert abs(final_value[1] - after_value[1] - 4) <= case['tolerance']
            for label in ('before', 'outline', 'after', 'focus'):
                capture = case[label]
                assert capture['commands'] >= 8 and abs(capture['device_pixel_ratio'] - row['scale']) < .01
                frame = image(result_path.parent / capture['artifact'], capture['frame_size'])
                vx, vy, vw, vh = capture['canonical_viewport']
                sx, sy = capture['sample']
                px, py = round((sx - vx) / vw * frame.width()), round((vy + vh - sy) / vh * frame.height())
                radius = 4 * row['scale']
                colours = [frame.pixelColor(x, y) for y in range(max(0, py-radius), min(frame.height(), py+radius+1))
                           for x in range(max(0, px-radius), min(frame.width(), px+radius+1))]
                assert any(min(c.red(), c.green(), c.blue()) > 150 or (c.green() > 150 and c.red() < 60) for c in colours)
            # Consecutive edits of the same Generator knobs merge into one undo
            # command. Undo restores the value preceding both drags.
            assert max(abs(x-y) for x, y in zip(case['undone_value'], case['before_value'])) < .001
            assert max(abs(x-y) for x, y in zip(case['redone_value'], case['final_value'])) < .001
            generator_cases += 1
    else:
        assert [c['id'] for c in report['cases']] == ['org.natron.test.LegacyOverlay', 'org.natron.test.LegacyCustomUI', 'org.natron.test.NoOverlay']
        assert all(not any(c[:6]) and c[6] > 0 and c[7] > 0 for c in report['counters'])
        for index, case in enumerate(report['cases']):
            assert case['warning_present'] == (index < 2)
            if index < 2:
                assert 'DrawSuite' in case['compatibility_detail']
                image(result_path.parent / f'{index}-warning.png')
            assert case['cpu_pixels_verified'] == 64 * 48
            frame = image(result_path.parent / f'{index}-identity.png', [64, 48])
            for y in range(48):
                for x in range(64):
                    expected_pixel = (255, 0, 0, 255) if x < 32 else (0, 255, 0, 255)
                    assert frame.pixelColor(x, y).getRgb() == expected_pixel
            image(result_path.parent / f'{index}-viewer.png')
            cpu_pixels += 64 * 48
            legacy_cases += 1
    retain(Path(row['result']).parent.parent / (Path(row['result']).parent.name + '.log'))

if args.recorded_verifier:
    retain(args.recorded_verifier)

proof = {'passed': True, 'completionPlatform': 'windows', 'fullVulkanMigrationComplete': False,
         'scope': 'All shipped interact declarations; all eight previously legacy Generator UI edits; independent legacy-only and no-UI CPU fixtures.',
         'shippedPluginVersions': len(a), 'shippedPluginIds': len({p['id'] for p in a.values()}),
         'shippedUnsupportedInteractsBefore': 8, 'shippedUnsupportedInteractsAfter': 0,
         'guiProcesses': 32, 'generatorCases': generator_cases, 'legacyCases': legacy_cases,
         'cpuPixelsCompared': cpu_pixels, 'legacyCallbacksExecuted': 0, 'externalPluginsExcluded': False,
         'allShippedRenderFunctionsVerified': False, 'userPreferencesUnchanged': True,
         'verificationScriptSha256': sha(Path(__file__)),
         'recordedVerificationScript': str(args.recorded_verifier.resolve()) if args.recorded_verifier else None,
         'recordedVerificationScriptSha256': sha(args.recorded_verifier) if args.recorded_verifier else sha(Path(__file__)),
         'inputSha256': matrix['inputSha256'], 'artifactSha256': artifacts,
         'beforeSha256': sha(args.before), 'afterSha256': sha(args.after), 'matrixSha256': sha(args.matrix)}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({k: v for k, v in proof.items() if k not in ('inputSha256', 'artifactSha256')}))
