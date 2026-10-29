"""Verify integrated CPU fallback evidence against the current Windows build."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--fallback-report', type=Path, default=root / 'build/raster-fallback-tests/report.json')
parser.add_argument('--output', type=Path, default=root / 'build/vulkan-raster-fallback-proof.json')
parser.add_argument('--native-prefix', type=Path, default=root / 'build/raster-native')
parser.add_argument('--native-engine-report', type=Path)
parser.add_argument('--native-gui-report', type=Path)
parser.add_argument('--native-proxy-report', type=Path)
parser.add_argument('--source-audit', type=Path, default=root / 'build/native-raster-source-audit.json')
parser.add_argument('--runtime-audit', type=Path, default=root / 'build/native-raster-runtime-audit.json')
parser.add_argument('--ctest-log', type=Path, default=root / 'build/native-raster-ctest-final.log')
parser.add_argument('--gpu-comparison-prefix', type=Path, default=root / 'build/native-raster-final')
parser.add_argument('--memory-report', type=Path)
parser.add_argument('--software-core-report', type=Path)
parser.add_argument('--software-engine-report', type=Path)
parser.add_argument('--software-gui-report', type=Path)
args = parser.parse_args()
args.fallback_report = args.fallback_report.resolve()


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_inputs(values):
    for name, expected in values.items():
        assert sha(root / name) == expected, f'Input changed: {name}'


def check_files(directory, values):
    for name, expected in values.items():
        path = Path(name)
        assert sha(path if path.is_absolute() else directory / name) == expected, name


def project_path(name):
    path = Path(name)
    return path if path.is_absolute() else root / path


def check_profile(name, require_marker=True):
    profile = Path(name)
    assert not (profile / 'timeout.json').exists(), profile
    if require_marker:
        assert (profile / 'started.txt').exists(), profile
    else:
        # The existing OCIO interpreter fixture predates startup markers.
        # Its completed script plus acknowledged stdin/EOF prove execution.
        assert 'NATRON_INTERPRETER_EOF_OK' in (profile / 'stdout.log').read_text(encoding='utf-8', errors='replace')
    assert not list((profile / 'cache/ViewerCache').glob('CachePart*')), profile
    errors = (profile / 'stderr.log').read_text(encoding='utf-8', errors='replace')
    assert not re.search(r'VUID-|Vulkan validation error:|Traceback', errors, re.I), profile


def png(path, size=None):
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    dimensions = struct.unpack('>II', data[16:24])
    if size:
        assert dimensions == tuple(size), (path, dimensions, size)


fallback = read(args.fallback_report)
assert fallback['passed'] and not fallback['fullVulkanMigrationComplete']
assert fallback['environmentRestored']
check_inputs(fallback['fileSha256'])
assert len(fallback['cases']) == 14
seen = set()
captures = overlay_handles = cpu_pixels = 0
for case in fallback['cases']:
    directory, result = Path(case['directory']), case['result']
    assert result == read(directory / 'result.json') and result['ok']
    check_files(directory, case['fileSha256'])
    check_profile(result['profile'])
    key = (case['mode'], case['program'], case['depth'], case['scale'])
    assert key not in seen
    seen.add(key)
    if case['program'] == 'HeadlessCpuSmoke':
        assert result['ofx']['project_roundtrip'] and len(result['ofx']['renders']) == 7
        assert result['ofx']['gmic_nodes'] == 360 and result['pixels_compared'] == 21504
        assert result['gpu_only_failure']['registered'] and not result['gpu_only_failure']['render_output_created']
        assert not (directory / 'gpu-only-must-not-succeed.png').exists()
        allowed = [] if case['mode'] == 'disabled' else ['vulkan-1.dll']
        assert not set(result['graphics_modules']) - set(allowed)
        for comparison in result['native_comparisons']:
            assert comparison['maximum_code_difference'] <= 1
            assert sha(directory / comparison['reference_file']) == comparison['reference_sha256']
            png(directory / 'ofx' / (comparison['name'] + '.png'), (64, 48))
        check_files(directory, result['artifacts'])
        cpu_pixels += result['pixels_compared']
    elif case['program'] == 'RasterGuiSmoke':
        assert result['pan'] and result['wheel_zoom'] and result['fallback_reason'] and not result['unexpected_dialogs']
        allowed = [] if case['mode'] == 'disabled' else ['vulkan-1.dll']
        assert not set(result['loaded_graphics_modules']) - set(allowed)
        assert len(result['captures']) == 2
        for capture in result['captures']:
            assert sha(directory / capture['file']) == capture['sha256']
            png(directory / capture['file'], (640 * case['scale'], 480 * case['scale']))
            expected = [[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]]
            assert len(capture['colors']) == 4 and all(abs(a-b) <= 2 for color, reference in zip(capture['colors'], expected)
                                                      for a, b in zip(color, reference))
            captures += 1
    else:
        assert case['program'] == 'RasterIOOverlaySmoke' and result['backend'] == 'Raster'
        assert len(result['cases']) == 4 and all(result['fallback_reasons']) and not result['graphics_errors']
        assert not any(result['validation_errors']) and not result['unexpected_dialogs'] and not result['opengl_loaded']
        for handle in result['cases']:
            assert handle['undo_redo'] and handle['focus_modifier_reset']
        overlay_handles += len(result['cases'])
        frames = list(directory.glob('*.png'))
        assert len(frames) == 13
        for path in frames:
            png(path, (640 * case['scale'], 480 * case['scale']))
        captures += len(frames)
for mode in ('disabled', 'missing-icd'):
    assert (mode, 'HeadlessCpuSmoke', '8u', 1) in seen
    for scale in (1, 2):
        assert (mode, 'RasterIOOverlaySmoke', '32f', scale) in seen
        for depth in ('8u', '32f'):
            assert (mode, 'RasterGuiSmoke', depth, scale) in seen

source_path = args.source_audit.resolve()
source = read(source_path)
assert source['passed'] and not source['legacyApiViolations'] and not source['buildViolations']
check_inputs(source['fileSha256'])
assert sha(root / 'build/windows-qt6/build.ninja') == source['buildNinjaSha256']
runtime_path = args.runtime_audit.resolve()
runtime = read(runtime_path)
assert runtime['passed'] and runtime['runtimeBinDlls'] == 184
for entry in runtime['files']:
    assert not entry['legacyGraphicsImports'] and sha(root / entry['file']) == entry['sha256']
ctest_path = args.ctest_log.resolve()
assert '100% tests passed, 0 tests failed out of 2' in ctest_path.read_text(encoding='utf-8-sig')
gpu_results = []
for index in (0, 1):
    gpu = read(Path(f'{args.gpu_comparison_prefix}-gpu{index}.json').resolve())
    assert gpu['passed'] and gpu['cases'] == 5184 and gpu['channels'] == 1886976
    assert gpu['maximum_error'] < 1e-6 and gpu['validation_errors'] == 0 and not gpu['opengl_loaded']
    gpu_results.append(gpu)

native_paths = [Path(f'{args.native_prefix}-{name}-tests/report.json').resolve()
                for name in ('gui', 'engine', 'proxy', 'io-overlay', 'ocio')]
if args.native_engine_report:
    native_paths[1] = args.native_engine_report.resolve()
if args.native_gui_report:
    native_paths[0] = args.native_gui_report.resolve()
if args.native_proxy_report:
    native_paths[2] = args.native_proxy_report.resolve()
definition_files = [root / 'tools/run-windows-qt6.ps1', root / 'tools/test-windows-vulkan-natron.ps1',
                    *sorted((root / 'tools').glob('*smoke.py'), key=lambda path: path.name.casefold())]
gui_definition_sha = hashlib.sha256(':'.join(sha(path).upper() for path in definition_files).encode()).hexdigest()
native_counts = {}
callback_failures = None
for path in native_paths:
    report = read(path)
    assert report['passed'] and report['preferencesSha256'] == fallback['preferencesSha256']
    if 'fileSha256' in report:
        check_inputs(report['fileSha256'])
    if 'binarySha256' in report:
        assert report['binarySha256'] == sha(root / 'build/windows-qt6/App/Natron.exe')
    if path == native_paths[0]:
        assert report['testDefinitionSha256'] == gui_definition_sha
    if path == native_paths[-1] and 'callbackFailureTransactions' in report:
        callback_failures = report['callbackFailureTransactions']
        assert callback_failures['passed'] and callback_failures['cases'] == 88
        assert callback_failures['transient_retries'] == 84 and not callback_failures['gpu_required']
        assert read(path.parent / 'callback-failures.json') == callback_failures
    for case in report['cases']:
        assert case.get('passed', True)
        directory = project_path(case['directory']) if 'directory' in case else None
        if 'fileSha256' in case:
            check_files(directory, case['fileSha256'])
        if directory and (directory / 'case.json').exists():
            detail = read(directory / 'case.json')
            assert detail['passed'] and detail['binarySha256'] == report['binarySha256']
            assert detail['testDefinitionSha256'] == report['testDefinitionSha256']
            check_files(directory, detail['fileSha256'])
        if directory and (directory / 'result.json').exists():
            data = read(directory / 'result.json')
            assert data['ok']
            assert not data.get('unexpected_dialogs') and not any(data.get('graphics_errors', []))
            errors = data.get('validation_errors', 0)
            assert not any(errors) if isinstance(errors, list) else errors == 0
            if data.get('profile'):
                check_profile(data['profile'], require_marker=path != native_paths[-1])
                if path == native_paths[-1]:
                    assert case['interpreterInputAndEOF'] and case['shippedOcioCases'] == 22 and case['ocioReferenceCases'] == 64
            if 'legacy_gl_widgets' in data:
                assert not data['legacy_gl_widgets'] and not data['qt_gl_modules'] and data['vulkan_viewers'] >= 1
                assert len(data['shared_device_ids']) >= 2 and len(set(data['shared_device_ids'])) == 1
                assert data['shared_device_ids'] == data['engine_device_ids']
            if path == native_paths[1]:
                assert data['native_generator_pixels'] == 12467 and data['native_generator_renders'] >= 1
                check_files(directory, data['artifact_sha256'])
                png(directory / 'native-generator.png', (137, 91))
        if 'result' in case:
            assert case['result']['ok'] and not case['result']['unexpected_dialogs']
            assert not any(case['result']['graphics_errors']) and not any(case['result']['validation_errors'])
            check_profile(case['profile'])
            directory = path.parent / f'gpu{case["gpu"]}-scale{case["scale"]}'
            for capture in case['captures']:
                assert sha(directory / capture['file']) == capture['sha256']
                png(directory / capture['file'], (640 * case['scale'], 480 * case['scale']))
    native_counts[path.parent.name] = len(report['cases'])
assert list(native_counts.values()) == [4, 2, 4, 4, 2]

software_evidence = None
software_paths = []
if any((args.software_core_report, args.software_engine_report, args.software_gui_report)):
    assert all((args.software_core_report, args.software_engine_report, args.software_gui_report)), 'Supply all three software Vulkan reports'
    software_paths = [path.resolve() for path in (args.software_core_report, args.software_engine_report, args.software_gui_report)]
    core, engine, gui = [read(path) for path in software_paths]
    assert core['passed'] and core['environmentRestored'] and not core['fullVulkanMigrationComplete']
    assert core['device']['type'] == 'VK_PHYSICAL_DEVICE_TYPE_CPU'
    assert tuple(map(int, core['device']['apiVersion'].split('.'))) >= (1, 3, 0)
    check_inputs(core['inputSha256'])
    assert {case['test'] for case in core['cases']} == {'Compute', 'MemoryBudget', 'Renderer', 'ViewerDisplay',
                                                     'ViewerGrid', 'ImageOperations', 'FragmentProgram', 'FragmentLut'}
    for case in core['cases']:
        data = case['result']
        assert data['passed'] and data['device'] == core['device']['name']
        assert not data.get('validationErrors', 0) and not data.get('validation_errors', 0) and not data.get('openGLLoaded', False)
        directory = software_paths[0].parent / 'core/gpu0'
        for stream in ('stdout', 'stderr'):
            assert sha(directory / f'{case["test"]}.{stream}.log') == case[f'{stream}Sha256']
        if case['test'] == 'MemoryBudget':
            assert data['limit_bytes'] == 32*1024*1024 and data['peak_block_bytes'] <= data['limit_bytes']
            assert data['allocation_failures'] == data['post_oom_retries'] == 3 and data['concurrent_transfers'] == 48
    assert engine['passed'] and len(engine['cases']) == 1 and engine['preferencesSha256'] == fallback['preferencesSha256']
    check_inputs(engine['fileSha256'])
    case = engine['cases'][0]
    assert case['gpu'] == 0 and case['device'] == core['device']['name']
    directory = project_path(case['directory'])
    check_files(directory, case['fileSha256'])
    data = read(directory / 'result.json')
    assert data['ok'] and data['device'] == core['device']['name'] and data['validation_errors'] == 0
    assert data['native_clip_transport'] and data['native_pixel_transfers'] == 0 and data['native_cache_reuse']
    assert data['native_generator_pixels'] == 12467 and data['native_generator_renders'] >= 1
    check_files(directory, data['artifact_sha256'])
    png(directory / 'native-generator.png', (137, 91))
    check_profile(data['profile'])
    assert gui['passed'] and gui['userPreferencesUnchanged'] and gui['preferencesSha256'] == fallback['preferencesSha256']
    assert gui['binarySha256'] == sha(root / 'build/windows-qt6/App/Natron.exe')
    assert gui['testDefinitionSha256'] == gui_definition_sha
    assert len(gui['cases']) == 2 and {case['depth'] for case in gui['cases']} == {'8u', '32f'}
    for case in gui['cases']:
        assert case['gpu'] == 0 and case['passed'] and case['test'] == 'GuiSmoke'
        directory = project_path(case['directory'])
        detail = read(directory / 'case.json')
        assert detail['passed'] and detail['binarySha256'] == gui['binarySha256']
        assert detail['testDefinitionSha256'] == gui['testDefinitionSha256']
        check_files(directory, detail['fileSha256'])
        data = read(directory / 'result.json')
        assert data['ok'] and not data['unexpected_dialogs'] and not any(data['graphics_errors']) and not any(data['validation_errors'])
        assert not data['legacy_gl_widgets'] and not data['qt_gl_modules'] and data['vulkan_viewers'] >= 1
        assert len(data['shared_device_ids']) >= 2 and len(set(data['shared_device_ids'])) == 1
        assert data['shared_device_ids'] == data['engine_device_ids']
        assert data['devices'] and all(name == core['device']['name'] for name in data['devices'])
        assert not data['system_gl_loaded']
        profiles = re.findall(r'^GUI smoke profile and logs:\s*(.+)$', (directory / 'run.log').read_text(encoding='utf-8-sig'), re.M)
        assert len(profiles) == 1
        check_profile(profiles[0].strip())
    software_evidence = {'passed': True, 'device': core['device']['name'], 'core_tests': 8,
                         'engine_cells': 1, 'gui_cells': 2, 'ci_execution_verified': False}

memory_evidence = None
if args.memory_report:
    memory_path = args.memory_report.resolve()
    memory = read(memory_path)
    assert memory['passed'] and memory['settingsBefore'] == memory['settingsAfter'] == fallback['preferencesSha256']
    check_inputs(memory['proofHashes'])
    assert len(memory['core']) == 2 and len(memory['cases']) == 12
    for item in memory['core']:
        core = item['result']
        assert core['passed'] and core['allocation_failures'] == 3 and core['post_oom_retries'] == 3
        assert core['limit_bytes'] == 32*1024*1024 and core['peak_block_bytes'] <= core['limit_bytes']
        assert core['concurrent_transfers'] == 48 and core['validation_errors'] == 0
    seen_memory = set()
    capture_hashes = {}
    for case in memory['cases']:
        data = case['result']
        assert data['ok']
        check_profile(data['profile'])
        directory = memory_path.parent / case['name']
        if case['kind'] == 'engine':
            oversized = data['host_allocation_retry']
            seen_memory.add((case['gpu'], 'engine', oversized))
            assert data['cpu_retry'] and data['gpu_only_failed'] and data['limit_mb'] == '32'
            assert data['pixels_compared'] == data['width']*data['height']
            assert data['counters'][0] == (0 if oversized else 1) and data['counters'][1] >= 1
            assert data['counters'][2:4] == [0 if oversized else 1, 0]
            for name in ('reference.png', 'fallback.png'):
                png(directory/name, (data['width'], data['height']))
                capture_hashes[str((directory/name).relative_to(root))] = sha(directory/name)
        else:
            seen_memory.add((case['gpu'], 'viewer', data['mode'], case['name'].rsplit('-', 1)[-1]))
            assert data['initial_frames'] > 1 and data['last_backend'] == 'Raster' and data['raster_frames'] > 1
            assert 'VkResult -2' in data['fallback_reason'] and data['validation_errors'] == 0
            assert not data['unexpected_dialogs']
            if data['mode'] == 'native-transfer':
                assert data['initial_native_transfers'] > 0
                assert data['fallback_reason'].startswith('Native Viewer image transfer:')
            png(directory/'recovered.png', (640, 480))
            assert sha(directory/'recovered.png') == data['screenshot_sha256']
            capture_hashes[str((directory/'recovered.png').relative_to(root))] = data['screenshot_sha256']
    expected_memory = {(gpu, 'engine', oversized) for gpu in (0, 1) for oversized in (False, True)}
    expected_memory |= {(gpu, 'viewer', mode, depth) for gpu in (0, 1)
                        for mode, depth in [('transfer', '32f'), ('native-transfer', '32f'), ('canvas', '8u'), ('canvas', '32f')]}
    assert seen_memory == expected_memory
    memory_evidence = {'passed': True, 'core_cases': 2, 'engine_cases': 4, 'viewer_cases': 8,
                       'managed_heap_limit_bytes': 32*1024*1024, 'report_sha256': sha(memory_path),
                       'capture_sha256': capture_hashes}

remaining_gates = ['allocation retry with cached GPU inputs and concurrent Viewers',
                   'device loss recovery', '4K/8K performance',
                   'all-node functional compatibility and external OFX explanation',
                   'software Vulkan CI, packaging, Linux and macOS validation']
if not memory_evidence:
    remaining_gates.insert(0, 'VRAM budget and allocation failure CPU retry')
result = {'passed': True, 'scope': 'Windows CPU startup, raster Viewer, OFX fallback and current native GPU regressions',
          'cpu_mode_gpu_loader_loaded': False, 'cpu_configuration_cases': len(fallback['cases']),
          'cpu_ofx_pixels_compared': cpu_pixels, 'cpu_overlay_handles': overlay_handles,
          'cpu_captures_verified': captures, 'gpu_display_comparisons': gpu_results,
          'native_regression_cells': native_counts, 'ctest_cases': 2,
          'preferences_sha256': fallback['preferencesSha256'], 'source_files_checked': source['sourceFilesChecked'],
          'runtime_files_checked': runtime['filesChecked'], 'runtime_dlls': runtime['runtimeBinDlls'],
          'ocio_callback_failure_transactions': callback_failures,
          'memory_allocation_failure_recovery': memory_evidence,
          'software_vulkan_validation': software_evidence,
          'full_vulkan_migration_complete': False,
          'remaining_acceptance_gates': remaining_gates,
          'reports_sha256': {str(path.relative_to(root)): sha(path) for path in
                             [args.fallback_report, source_path, runtime_path, ctest_path, *native_paths, *software_paths]},
          'binary_sha256': {name: sha(root / name) for name in
                            ('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
                             'build/windows-qt6/Tests/NatronRasterViewer.exe', 'build/windows-qt6/Tests/NatronRasterGpuCompare.exe')}}
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
print(f'CPU fallback proof passed: {len(fallback["cases"])} configurations, {captures} captures, {overlay_handles} handles; full migration remains in progress.')
