"""Recover a real Natron render/cache and a persistent GPU-required shader."""
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
output = root / 'build/vulkan-device-loss-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Device loss render smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'cases': [],
          'physical_driver_reset': False}
abi = None
artifacts = []
try:
    abi = runpy.run_path(str(root / 'tools/vulkan-device-loss-abi.py'))['LossAbi'](root)

    def node(kind, source=None):
        effect = app.createNode('org.natron.test.VulkanPressure' + kind)
        assert effect, kind
        if source:
            assert effect.connectInput(0, source)
        return effect

    def render(effect, name, frame=1):
        path = output / (name + '.png')
        path.unlink(missing_ok=True)
        writer = app.createWriter(path.as_posix())
        assert writer and writer.connectInput(0, effect)
        for knob in ('ocioInputSpace', 'ocioOutputSpace'):
            if param := writer.getParam(knob):
                param.setValue('Linear')
        QCoreApplication.processEvents()
        app.render(writer, frame, frame)
        assert path.exists(), f'{name}: missing rendered output'
        image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
        assert (image.width(), image.height()) == (64, 48) and not image.isNull()
        assert bytes(image.constBits()) == bytes((0, 255, 0, 255)) * (64 * 48), name
        artifacts.append(path)

    app.addFormat('DeviceLoss 64x48 1')
    app.getProjectParam('outputFormat').setValue('DeviceLoss')
    app.getProjectParam('frameRange').set(1, 2)
    source = node('Source')
    source.getParam('forceCaching').setValue(True)
    render(node('Inspect', source), 'prewarm')
    for point in (1, 2):
        held = abi.retain()
        before = abi.counters()
        assert abi.test.armDeviceLoss(held[0], point) == 0
        render(node('Inspect', source), f'recovered-cache-point-{point}')
        statuses = abi.retired(held)
        fresh = abi.context()
        after = abi.counters()
        assert abi.generation(fresh) > held[3]
        assert after[0] > before[0] and after[1] == before[1] and after[3] == before[3], (before, after)
        report['cases'].append({'point': point, 'old_generation': held[3],
                                'new_generation': abi.generation(fresh), 'retired_statuses': statuses,
                                'before': before, 'after': after, 'native_source_rerendered': True})
    held = abi.retain()
    before = abi.counters()
    abi.module.OfxPressureActionLossCount.restype = ctypes.c_uint64
    abi.module.OfxPressureArmActionLoss.argtypes = [ctypes.c_uint32]
    abi.module.OfxPressureArmActionLoss.restype = ctypes.c_int
    action_before = abi.module.OfxPressureActionLossCount()
    assert abi.module.OfxPressureArmActionLoss(1) == 0
    render(node('LossFilter', source), 'ofx-action-recovered')
    statuses = abi.retired(held)
    fresh = abi.context()
    after = abi.counters()
    assert abi.module.OfxPressureActionLossCount() == action_before + 1
    assert after[2] >= before[2] + 2 and after[3] == before[3] and after[0] > before[0], (before, after)
    assert abi.generation(fresh) > held[3]
    report['cases'].append({'ofx_action_loss': True, 'old_generation': held[3],
                            'new_generation': abi.generation(fresh), 'retired_statuses': statuses,
                            'before': before, 'after': after})
    ocio = app.createNode('fr.inria.openfx.OCIOCDLTransform')
    assert ocio and ocio.connectInput(0, source)
    assert ocio.getParam('enableGPU').getValue()
    for dim, value in enumerate((1.2, 1.0, 1.1)):
        ocio.getParam('slope').setValue(value, dim)
    for knob in ('ocioInputSpace', 'ocioOutputSpace'):
        if param := ocio.getParam(knob):
            param.setValue('Linear')
    io = ctypes.CDLL(str(root / 'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx'))
    io.OfxOCIOVulkanClipRenderCount.restype = ctypes.c_uint64
    ocio_before = io.OfxOCIOVulkanClipRenderCount()
    render(ocio, 'ocio-prewarm', 1)
    assert io.OfxOCIOVulkanClipRenderCount() > ocio_before
    held = abi.retain()
    ocio_before = io.OfxOCIOVulkanClipRenderCount()
    assert abi.test.armDeviceLoss(held[0], 2) == 0
    render(ocio, 'ocio-recovered', 2)
    statuses = abi.retired(held)
    fresh = abi.context()
    assert abi.generation(fresh) > held[3] and io.OfxOCIOVulkanClipRenderCount() > ocio_before
    report['cases'].append({'ocio_processor_recreated': True, 'old_generation': held[3],
                            'new_generation': abi.generation(fresh), 'retired_statuses': statuses,
                            'native_clip_renders': io.OfxOCIOVulkanClipRenderCount() - ocio_before})
    shader = app.createNode('net.sf.openfx.Shadertoy')
    assert shader
    shader.getParam('imageShaderSource').setValue('void mainImage(out vec4 c,in vec2 p){c=vec4(0,1,0,1);}')
    shader.getParam('imageShaderCompile').trigger()
    render(shader, 'gpu-required-prewarm', 1)
    held = abi.retain()
    assert abi.test.armDeviceLoss(held[0], 1) == 0
    render(shader, 'gpu-required-recovered', 2)
    statuses = abi.retired(held)
    fresh = abi.context()
    assert abi.generation(fresh) > held[3]
    info = abi.info(fresh)
    assert info.validation_enabled == 1 and info.validation_errors == 0
    report['cases'].append({'point': 1, 'gpu_required_shader': True, 'old_generation': held[3],
                            'new_generation': abi.generation(fresh), 'retired_statuses': statuses})
    report.update(ok=True, pixels_compared=len(artifacts) * 64 * 48,
                  device=info.name.decode('utf-8'), validation_errors=info.validation_errors)
except Exception:
    report['exception'] = traceback.format_exc()
finally:
    if abi:
        report['final_counters'] = abi.counters()
        if hasattr(abi.module, 'OfxPressureActionLossCount'):
            abi.module.OfxPressureActionLossCount.restype = ctypes.c_uint64
            report['action_loss_count'] = abi.module.OfxPressureActionLossCount()
        abi.close()
    report['artifact_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
print(json.dumps(report))
