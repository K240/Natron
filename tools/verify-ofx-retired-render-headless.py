"""Check current NatronRenderer ABI results, pixels and orderly stdin EOF."""
import argparse
import hashlib
import json
from pathlib import Path
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--matrix',type=Path,default=Path('build/windows-ofx-retired-render-headless-tests/report.json'))
parser.add_argument('--output',type=Path,default=Path('build/windows-ofx-retired-render-headless-proof.json'))
args = parser.parse_args()
read = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
matrix = read(args.matrix)
assert matrix['passed'] and matrix['completionPlatform'] == 'windows'
assert matrix['settingsBefore'] == matrix['settingsAfter']
assert len(matrix['cases']) == 3 and {c['name'] for c in matrix['cases']} == {'gpu0','gpu1','disabled'}
for name,digest in matrix['inputSha256'].items():
    assert sha(root/name) == digest, name
source = read(root/matrix.get('sourceAudit','build/retired-render-source-audit.json'))
runtime = read(root/matrix.get('runtimeAudit','build/retired-render-runtime-audit.json'))
assert source['passed'] and runtime['passed']
assert sha(root/'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
for name,digest in source['fileSha256'].items():
    assert sha(root/name) == digest, name
for record in runtime['files']:
    assert sha(root/record['file']) == record['sha256'], record['file']
artifacts = {}
pixels = native_pixels = 0
for row in matrix['cases']:
    assert row['passed'] and not row['failure']
    result = Path(row['result'])
    profile = Path(row['profile'])
    report = read(result)
    assert report['ok'] and report['profile'] == str(profile) and report['vulkan_disabled'] == row['disabled']
    assert len(report['cases']) == 6
    assert not (profile/'timeout.json').exists()
    stdout = (profile/'stdout.log').read_text(encoding='utf-8-sig')
    stderr = (profile/'stderr.log').read_text(encoding='utf-8-sig')
    assert 'NATRON_RETIRED_RENDER_HEADLESS_EXIT_REQUESTED' in stdout and 'NATRON_OWNED_INTERPRETER_EOF_OK' in stdout
    assert 'requires OpenGL rendering' in stderr and not any(s in stderr for s in ('VUID-','Vulkan validation error:','Vulkan runtime error:'))
    for index,case in enumerate(report['cases']):
        values = report['counters'][index]
        assert values[0] > 0 and values[1] > 0 and not any(values[7:9]), values
        if index in (0,3):
            assert case['rejected_before_create'] and not any(values[2:10]), values
        elif index == 4 and row['disabled']:
            assert case['native_unavailable_failure'] and values[2] > 0 and not values[4]
            assert not (result.parent/f'{index}-output.png').exists()
        else:
            assert values[2] > 0 and values[4] > 0 and values[5] > 0 and values[6] > 0
            assert values[9] == (values[4] if index == 4 else 0), values
            assert case['pixels_verified'] == 64*48 and case['native_render'] == (index == 4)
            path = result.parent/f'{index}-output.png'
            image = QImage(str(path))
            assert not image.isNull() and [image.width(),image.height()] == [64,48]
            for y in range(48):
                for x in range(64):
                    assert image.pixelColor(x,y).getRgb() == ((255,0,0,255) if x < 32 else (0,255,0,255))
            pixels += 64*48
            if index == 4:
                assert values[10] == (0x10de if row['gpu'] == '0' else 0x1002) and values[11] > 0
                native_pixels += 64*48
            artifacts[str(path.resolve())] = sha(path)
    for path in (result,profile/'stdout.log',profile/'stderr.log',profile/'started.txt',result.parent.parent/(result.parent.name+'.log')):
        artifacts[str(path.resolve())] = sha(path)
proof = {'passed':True,'completionPlatform':'windows','fullVulkanMigrationComplete':False,
         'scope':'Three owned NatronRenderer processes, six ABI variants each, actual CPU/native pixels and stdin EOF.',
         'processes':3,'fixtureCases':18,'pixelsCompared':pixels,'nativePixelsCompared':native_pixels,
         'openGLRequiredRejectionsBeforeCreate':6,'userPreferencesUnchanged':True,'inputSha256':matrix['inputSha256'],
         'artifactSha256':artifacts,'matrixSha256':sha(args.matrix)}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(proof,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in proof.items() if k not in ('inputSha256','artifactSha256')}))
