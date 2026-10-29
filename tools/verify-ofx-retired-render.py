"""Verify descriptor policy, actual native/CPU pixels and owned cache migration."""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import xml.etree.ElementTree as ET
from PySide6.QtGui import QImage

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--matrix', type=Path, default=Path('build/windows-ofx-retired-render-tests/report.json'))
parser.add_argument('--output', type=Path, default=Path('build/windows-ofx-retired-render-proof.json'))
args = parser.parse_args()
read = lambda p: json.loads(p.read_text(encoding='utf-8-sig'))
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
matrix = read(args.matrix)
assert matrix['passed'] and matrix['completionPlatform'] == 'windows'
assert matrix['settingsBefore'] == matrix['settingsAfter']
expected = {f'gpu{g}-scale{s}-{d}-{b}' for g,s,d,b in itertools.product((0,1),(1,2),('8u','32f'),('Vulkan','Raster'))}
expected |= {f'disabled-scale{s}-{d}' for s,d in itertools.product((1,2),('8u','32f'))}
expected |= {'cache-v2', 'cache-v1-rebuild'}
assert len(matrix['cases']) == len(expected) == 22 and {r['name'] for r in matrix['cases']} == expected
for name, digest in matrix['inputSha256'].items():
    assert sha(root / name) == digest, name
source = read(root / 'build/retired-render-source-audit.json')
runtime = read(root / 'build/retired-render-runtime-audit.json')
assert source['passed'] and runtime['passed'] and source['sourceFilesChecked'] == 739
for name, digest in source['fileSha256'].items():
    assert sha(root / name) == digest, name
for record in runtime['files']:
    assert sha(root / record['file']) == record['sha256'], record['file']
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
inventory = root / 'build/retired-render-native-shipped-inventory'
declarations = read(inventory / 'native.json')
discovery = read(inventory / 'legacy-discovery.json')
assert not declarations['failures'] and not discovery['failures']
assert declarations['runtimeVulkanSuiteCalls'] == discovery['runtimeVulkanSuiteCalls'] == 0
assert declarations['pluginVersions'] == discovery['pluginVersions']
assert len(declarations['pluginVersions']) == 579
assert len({p['id'] for p in declarations['pluginVersions']}) == 556
for plugin in declarations['pluginVersions']:
    for context in plugin['contexts']:
        assert context['retiredOpenGLRendering'] in ('false','true','needed')
        assert context['retiredOpenGLRendering'] != 'needed' or context['vulkanRendering']
summary = read(inventory / 'report.json')
assert summary['passed'] and summary['unsupportedVersions'] == summary['unsupportedRenderVersions'] == 0
artifacts = {}
def retain(path):
    artifacts[str(path.resolve())] = sha(path)
def check_image(path, pixels=False):
    image = QImage(str(path))
    assert not image.isNull(), path
    if pixels:
        assert [image.width(),image.height()] == [64,48]
        for y in range(48):
            for x in range(64):
                assert image.pixelColor(x,y).getRgb() == ((255,0,0,255) if x < 32 else (0,255,0,255)), (path,x,y)
    retain(path)
ids = ['org.natron.test.RequiredOpenGL','org.natron.test.OptionalOpenGL','org.natron.test.ExplicitCPU',
       'org.natron.test.ContextOpenGL','org.natron.test.DualRenderAPI','org.natron.test.DefaultCPU']
pixels = native_pixels = rejected = 0
for row in matrix['cases']:
    assert row['passed'] and not row['failure']
    result = Path(row['result'])
    report = read(result)
    profile = Path(row['profile'])
    assert report['ok'] and report['profile'] == str(profile)
    assert report['backend'] == row['backend'] and report['vulkan_disabled'] == row['disabled']
    assert not report['unexpected_dialogs'] and not any(report['graphics_errors']) and not any(report['validation_errors'])
    assert [c['id'] for c in report['cases']] == ids
    assert not (profile/'timeout.json').exists()
    stdout, stderr = (profile/'stdout.log').read_text(encoding='utf-8-sig'), (profile/'stderr.log').read_text(encoding='utf-8-sig')
    assert 'NATRON_RETIRED_RENDER_EXIT_REQUESTED' in stdout
    assert not any(s in stderr for s in ('VUID-','Vulkan validation error:','Vulkan runtime error:','Vulkan Viewer:'))
    assert 'requires OpenGL rendering' in stderr and 'CPU-compatible or Vulkan-compatible' in stderr
    dialog = report['expected_error_dialog']
    assert dialog['title'] == 'Error while creating node' and ids[0] in dialog['text'] and 'requires OpenGL rendering' in dialog['text']
    check_image(result.parent/'required-render-error.png')
    for index, case in enumerate(report['cases']):
        values = report['counters'][index]
        assert values[0] > 0 and values[1] > 0 and not any(values[7:9]), values
        if index in (0,3):
            assert case['rejected_before_create'] and not any(values[2:10]), values
            rejected += 1
        else:
            assert values[2] > 0 and values[3] > 0, values
            if index == 4 and row['disabled']:
                assert case['native_unavailable_failure'] and not values[4]
                assert not (result.parent/f'{index}-output.png').exists()
            else:
                assert values[4] > 0 and values[5] > 0 and values[6] > 0
                assert values[9] == (values[4] if index == 4 else 0), values
                assert case['pixels_verified'] == 64*48 and case['native_render'] == (index == 4)
                check_image(result.parent/f'{index}-output.png', True)
                check_image(result.parent/f'{index}-viewer.png')
                pixels += 64*48
                if index == 4:
                    assert values[10] == (0x10de if row['gpu'] == 0 else 0x1002) and values[11] > 0
                    native_pixels += 64*48
    caches = list((profile/'cache/OFXLoadCache').glob('OFXCache_*.xml'))
    assert len(caches) == 1 and ET.parse(caches[0]).getroot().attrib['version'] == 'NatronOFXCachev2'
    if row['cacheSource']:
        seed = Path(row['cacheSource'])
        assert sha(seed) == row['cacheSourceSha256']
        version = ET.parse(seed).getroot().attrib['version']
        assert version == ('NatronOFXCachev1' if row['name'] == 'cache-v1-rebuild' else 'NatronOFXCachev2')
        if version.endswith('v1'):
            assert not ET.parse(seed).findall(".//property[@name='OfxImageEffectPropOpenGLRenderSupported']")
        retain(seed)
    for path in (result,profile/'stdout.log',profile/'stderr.log',profile/'started.txt',caches[0],result.parent.parent/(result.parent.name+'.log')):
        retain(path)
proof = {'passed':True,'completionPlatform':'windows','fullVulkanMigrationComplete':False,
         'scope':'All shipped render/interact declarations; six independent ABI variants and cache migration. Full shipped render functionality remains separately unverified.',
         'shippedVersions':579,'shippedIds':556,'unsupportedRenderDeclarations':0,'unsupportedInteractDeclarations':0,
         'renderModeCounts':summary['declaredRenderModeCounts'],'guiProcesses':22,'fixtureCases':132,
         'openGLRequiredRejectionsBeforeCreate':rejected,'pixelsCompared':pixels,'nativePixelsCompared':native_pixels,
         'cpuFallbackForRequiredNative':False,'userPreferencesUnchanged':True,'inputSha256':matrix['inputSha256'],
         'artifactSha256':artifacts,'matrixSha256':sha(args.matrix)}
args.output.parent.mkdir(parents=True,exist_ok=True)
args.output.write_text(json.dumps(proof,indent=2),encoding='utf-8')
print(json.dumps({k:v for k,v in proof.items() if k not in ('inputSha256','artifactSha256')}))
