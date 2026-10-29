"""Render the rebuilt CPU OFX sets without initializing a graphics driver."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import runpy
import traceback

from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

root = Path.cwd()
output = root / 'build/headless-cpu-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Headless CPU smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE')}

try:
    os.environ['NATRON_OFX_SMOKE_OUTPUT'] = str(output / 'ofx')
    fixture = runpy.run_path(str(root / 'tools/ofx-smoke.py'), init_globals={'app': app})
    result = fixture['report']
    assert result['ok'] and result['project_roundtrip'] and len(result['renders']) == 7
    report['ofx'] = result
    artifacts = {}
    pixels = 0
    comparisons = []
    for name, item in result['renders'].items():
        path = Path(item['path'])
        actual = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
        reference_path = root / 'build/ofx-smoke' / (name + '.png')
        reference_snapshot = output / 'native-reference' / reference_path.name
        reference_snapshot.parent.mkdir(exist_ok=True)
        reference_snapshot.write_bytes(reference_path.read_bytes())
        reference = QImage(str(reference_path)).convertToFormat(QImage.Format_RGBA8888)
        assert not actual.isNull() and not reference.isNull() and actual.size() == reference.size(), name
        actual_bytes, reference_bytes = bytes(actual.constBits()), bytes(reference.constBits())
        maximum = max(abs(a-b) for a, b in zip(actual_bytes, reference_bytes))
        # CPU/native color conversion can straddle an 8-bit quantization step.
        # Record every channel and keep alpha exact rather than comparing only
        # the fixture's two sample colors.
        assert maximum <= 1, f'{name}: CPU/native difference exceeds one 8-bit code value: {maximum}'
        assert actual_bytes[3::4] == reference_bytes[3::4], f'{name}: alpha differs'
        comparisons.append({'name': name, 'maximum_code_difference': maximum,
                            'changed_channels': sum(a != b for a, b in zip(actual_bytes, reference_bytes)),
                            'reference_file': str(reference_snapshot.relative_to(output)),
                            'reference_sha256': hashlib.sha256(reference_snapshot.read_bytes()).hexdigest()})
        pixels += actual.width() * actual.height()
        artifacts[str(path.relative_to(output))] = hashlib.sha256(path.read_bytes()).hexdigest()
        artifacts[str(reference_snapshot.relative_to(output))] = hashlib.sha256(reference_snapshot.read_bytes()).hexdigest()
    assert 'net.sf.openfx.Shadertoy' in fixture['ids'], 'GPU-only shader disappeared from the node inventory'
    shader = None
    shader_error = None
    try:
        shader = app.createNode('net.sf.openfx.Shadertoy')
    except Exception as error:
        shader_error = str(error)
    shader_output = output / 'gpu-only-must-not-succeed.png'
    shader_output.unlink(missing_ok=True)
    if shader is not None:
        shader.getParam('imageShaderSource').setValue('void mainImage(out vec4 c,in vec2 p){c=vec4(0.,0.,1.,1.);}')
        writer = app.createWriter(shader_output.as_posix())
        assert writer and writer.connectInput(0, shader)
        QCoreApplication.processEvents()
        try:
            app.render(writer, 1, 1)
        except Exception as error:
            shader_error = str(error)
    assert not shader_output.exists(), 'GPU-only processing reported success without a Vulkan driver'
    report['gpu_only_failure'] = {'registered': True, 'instance_created': shader is not None,
                                  'render_output_created': False, 'exception': shader_error}
    get_module = ctypes.WinDLL('kernel32').GetModuleHandleW
    get_module.argtypes = [ctypes.c_wchar_p]
    get_module.restype = ctypes.c_void_p
    modules = [name for name in ('vulkan-1.dll', 'opengl32.dll', 'Qt6OpenGL.dll',
                                'Qt6OpenGLWidgets.dll', 'libEGL.dll', 'libGLESv2.dll') if get_module(name)]
    allowed = [] if os.environ.get('NATRON_VULKAN_DISABLED') == '1' else ['vulkan-1.dll']
    assert not set(modules) - set(allowed), modules
    report.update(ok=True, graphics_modules=modules, pixels_compared=pixels, artifacts=artifacts,
                  native_comparisons=comparisons,
                  gpu_only_shader_registered='net.sf.openfx.Shadertoy' in fixture['ids'])
except Exception:
    report['exception'] = traceback.format_exc()
finally:
    (output / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
