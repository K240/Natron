"""Independently check relocated Windows package evidence; run with uv."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import zipfile
from PySide6.QtGui import QImage

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--archive', type=Path, required=True)
parser.add_argument('--repeat-archive', type=Path, required=True)
parser.add_argument('--hardware', type=Path, required=True)
parser.add_argument('--software', type=Path, required=True)
parser.add_argument('--software-core', type=Path, required=True)
parser.add_argument('--source-audit', type=Path, required=True)
parser.add_argument('--runtime-audit', type=Path, required=True)
parser.add_argument('--optimization-proof', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
inputs = {}


def digest(path):
    return hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()


def remember(path):
    inputs[str(path.resolve())] = digest(path)


def read(path):
    remember(path)
    return json.loads(path.read_text(encoding='utf-8-sig'))


def hashes(values):
    for name, expected in values.items():
        path = Path(name)
        if not path.is_absolute():
            path = ROOT/path
        assert digest(path) == expected, name
        remember(path)


remember(Path(__file__).resolve())
optimization = read(args.optimization_proof) if args.optimization_proof else None
if optimization:
    assert optimization['passed'] and optimization['gpuMedianImprovedOnBothDevicesAndResolutions']
    hashes(optimization['inputs'])
    current = read(Path(next(name for name in optimization['inputs'] if name.endswith('-after.json'))))
    for name, expected in current['artifactSha256'].items():
        assert digest(Path(name)) == expected, name
    for name, expected in current['inputs'].items():
        assert digest(Path(name)) == expected, name
    matrix_path = Path(next(name for name in current['inputs'] if name.endswith('report.json')))
    matrix = read(matrix_path)
    hashes(matrix['inputSha256'])

archive_hash = digest(args.archive)
assert digest(args.repeat_archive) == archive_hash, 'Same-input package is not deterministic'
remember(args.archive)
remember(args.repeat_archive)
with zipfile.ZipFile(args.archive) as archive:
    names = archive.namelist()
    assert len(names) == len(set(names))
    assert all(not Path(n).is_absolute() and '..' not in Path(n).parts for n in names)
    manifest = json.loads(archive.read('manifest.json'))
    provenance = json.loads(archive.read('provenance.json'))
    assert manifest['passed'] and set(names) == set(manifest['files']) | {'manifest.json'}
    for name, expected in manifest['files'].items():
        assert hashlib.sha256(archive.read(name)).hexdigest() == expected, name
    forbidden = {'opengl32.dll', 'opengl32sw.dll', 'libegl.dll', 'libglesv2.dll',
                 'qt6opengl.dll', 'qt6openglwidgets.dll'}
    assert provenance['buildType'] == 'Release' and provenance['qtVersion'] == '6.11.2'
    assert manifest['peFilesChecked'] == len(provenance['peImports'])
    for record in provenance['peImports']:
        assert not forbidden.intersection(n.casefold() for n in record['imports']), record
    # Each copied input still matches the exact binary/data placed in the ZIP.
    for name, source in provenance['inputSourceFiles'].items():
        assert digest(Path(source)) == manifest['files'][name], source

source = read(args.source_audit)
runtime = read(args.runtime_audit)
assert source['passed'] and runtime['passed']
hashes(source['fileSha256'])
for record in runtime['files']:
    hashes({record['file']: record['sha256']})
core = read(args.software_core)
assert core['passed'] and len(core['cases']) == 9
hashes(core['inputSha256'])
driver_root = Path(core['icd']).parent
driver = read(driver_root/'manifest.json')
assert driver['passed'] and digest(ROOT/'build-configs/vulkan/software-driver.json') == driver['lockSha256']
for name, expected in driver['files'].items():
    hashes({str(driver_root/'bin'/name): expected})

summaries = []
for name, path in (('hardware', args.hardware), ('software', args.software)):
    test = read(path)
    assert test['passed'] and test['archiveSha256'] == archive_hash
    assert test['settingsBefore'] == test['settingsAfter']
    assert 'parsec' in test['excludedTests']
    assert {c['name'] for c in test['cases']} == {'python', 'renderer', 'gui'}
    hashes({str(ROOT/'tools'/n): value for n, value in test['testDefinitions'].items()})
    for case in test['cases']:
        assert case['exitCode'] == 0
        for stream in ('stdout', 'stderr'):
            log = path.parent/f"{case['name']}.{stream}.log"
            assert digest(log) == case[f'{stream}Sha256']
            remember(log)
            text = log.read_text(encoding='utf-8-sig')
            assert not any(marker in text for marker in ('Traceback', 'VUID-', 'Vulkan runtime error:'))
    renderer = read(path.parent/'renderer/result.json')
    gui = read(path.parent/'build/gui-qt6-result.json')
    assert renderer == test['renderer'] and gui == test['gui']
    assert renderer['ok'] and renderer['nativeRenderCount'] > 0 and renderer['nativePixelsVerified'] == 3072
    assert renderer['cpuOfx']['project_roundtrip'] and len(renderer['cpuOfx']['renders']) == 7
    assert gui['ok'] and gui['platform'] == 'windows'
    assert not gui['unexpected_dialogs'] and not gui['qt_gl_modules'] and gui['legacy_gl_widgets'] == 0
    assert not any(gui['graphics_errors']) and not any(gui['validation_errors'])
    assert gui['presented_frames'] and all(gui['presented_frames'])
    assert len(gui['shared_device_ids']) >= 2 and len(set(gui['shared_device_ids'])) == 1
    assert gui['engine_device_ids'] == gui['shared_device_ids']
    assert gui['native_input']['pan'] and gui['native_input']['wheel_zoom']
    permitted = [Path(test['package']).resolve(), Path(os.environ['WINDIR']).resolve()]
    if name == 'software':
        permitted.append(driver_root.resolve())
        assert all('SwiftShader' in device for device in gui['devices'])
    for loaded, unexpected in ((renderer['loadedModules'], renderer['unexpectedModules']),
                               (gui['loaded_modules'], gui['unexpected_modules'])):
        assert not unexpected
        assert all(any(Path(p).resolve().is_relative_to(base) for base in permitted) for p in loaded)
        assert not forbidden.intersection(Path(p).name.casefold() for p in loaded if Path(p).name.casefold() != 'opengl32.dll')
    native = path.parent/'renderer/native.png'
    image = QImage(str(native))
    assert (image.width(), image.height()) == (64, 48)
    assert all(image.pixelColor(x, y).getRgb() == (255, 0, 0, 255) for y in range(48) for x in range(64))
    remember(native)
    frame = path.parent/'build/gui-qt6-frame.png'
    image = QImage(str(frame))
    assert not image.isNull() and [image.width(), image.height()] == gui['framebuffer_size']
    # Verify the actual saved presentation, including orientation, independently
    # of the GUI's serialized sample colors.
    red = [(x, y) for y in range(image.height()) for x in range(image.width())
           if image.pixelColor(x, y).getRgb() == (255, 0, 0, 255)]
    assert len(red) > 100
    x = (min(p[0] for p in red)+max(p[0] for p in red))//2
    y = (min(p[1] for p in red)+max(p[1] for p in red))//2
    dx = max(p[0] for p in red)-min(p[0] for p in red)+1
    dy = max(p[1] for p in red)-min(p[1] for p in red)+1
    for px, py, rgb in ((x, y, (255,0,0,255)), (x+dx, y, (0,255,0,255)),
                        (x, y+dy, (0,0,255,255)), (x+dx, y+dy, (255,255,255,255))):
        assert image.pixelColor(px, py).getRgb() == rgb, (name, px, py)
    remember(frame)
    summaries.append({'driver': name, 'devices': gui['devices'], 'nativePixelsVerified': 3072,
                      'cpuOfxRenders': 7, 'guiQuadrantsVerified': 4, 'unexpectedModules': 0,
                      'userSettingsUnchanged': True, 'systemOpenGLLoadedByWsi': gui['system_gl_loaded']})

proof = {'passed': True, 'completionPlatform': 'windows', 'fullVulkanMigrationComplete': False,
         'scope': 'Same-input portable packaging and local relocated hardware/software Vulkan acceptance',
         'archiveSha256': archive_hash, 'filesVerified': len(names), 'peFilesChecked': manifest['peFilesChecked'],
         'sameInputZipReproducible': True, 'softwareCoreCases': len(core['cases']), 'cases': summaries,
         'excludedTests': ['parsec'], 'remoteCiExecuted': False, 'cleanMachineTested': False,
         'performanceOptimizationVerified': bool(optimization),
         'remaining': (['Legacy 4K performance gap'] if optimization else ['Performance assessment']) +
                      ['Physical driver recovery', 'Remote CI and clean-machine validation',
                       'Complete functional equivalence of every shipped OFX algorithm'],
         'inputSha256': inputs}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(proof, indent=2), encoding='utf-8')
print(json.dumps({k:v for k,v in proof.items() if k != 'inputSha256'}))
