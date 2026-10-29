"""Keep a proven native cache input resident across real device heap exhaustion."""
import ctypes
import hashlib
import json
import os
import traceback
from pathlib import Path

from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

root = Path.cwd()
output = root / 'build/vulkan-memory-pressure-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Memory pressure smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'),
          'limit_mb': os.environ.get('NATRON_VULKAN_MEMORY_LIMIT_MB'),
          'gpu': os.environ.get('NATRON_VULKAN_DEVICE'), 'stages': []}
module = None
pressure = False
artifacts = []
try:
    assert report['limit_mb'] == '32', 'This fixture requires a 32 MiB allocator limit'
    module = ctypes.CDLL(str(root / 'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx'))
    module.OfxPressureCounter.argtypes = [ctypes.c_int]
    module.OfxPressureCounter.restype = ctypes.c_uint64
    module.OfxPressureGetTransfers.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
    module.OfxPressureGetTransfers.restype = ctypes.c_int
    module.OfxPressureFillHeap.restype = ctypes.c_int
    module.OfxPressureReleaseHeap.restype = ctypes.c_int

    class DeviceInfo(ctypes.Structure):
        _fields_ = [('name', ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in (
            'vendor_id', 'device_id', 'api_version', 'validation_enabled', 'validation_errors')]

    module.OfxPressureGetDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
    module.OfxPressureGetDeviceInfo.restype = ctypes.c_int

    def counters():
        return [module.OfxPressureCounter(i) for i in range(12)]

    def transfers():
        uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
        assert module.OfxPressureGetTransfers(ctypes.byref(uploads), ctypes.byref(downloads)) == 0
        return [uploads.value, downloads.value]

    def node(kind, source=None):
        effect = app.createNode('org.natron.test.VulkanPressure' + kind)
        assert effect, kind
        if source:
            assert effect.connectInput(0, source)
        return effect

    def render(effect, name):
        path = output / (name + '.png')
        path.unlink(missing_ok=True)
        writer = app.createWriter(path.as_posix())
        assert writer and writer.connectInput(0, effect)
        for knob in ('ocioInputSpace', 'ocioOutputSpace'):
            if param := writer.getParam(knob):
                param.setValue('Linear')
        QCoreApplication.processEvents()
        app.render(writer, 1, 1)
        assert path.exists(), f'{name}: render produced no output; counters={counters()}'
        image = QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
        assert not image.isNull() and (image.width(), image.height()) == (64, 48)
        artifacts.append(path)
        report['stages'].append({'name': name, 'counters': counters(), 'transfers': transfers()})
        return bytes(image.constBits())

    app.addFormat('MemoryPressure 64x48 1')
    app.getProjectParam('outputFormat').setValue('MemoryPressure')
    reference = render(node('CpuFilter', node('CpuSource')), 'reference')
    assert reference == bytes((0, 255, 0, 255)) * (64 * 48)
    source = node('Source')
    source.getParam('forceCaching').setValue(True)
    # Download only the small Inspect output. The full source must remain
    # exclusively native, without a CPU snapshot from an earlier Writer.
    assert render(node('Inspect', source), 'prewarm') == reference
    before = counters()
    assert before[0] >= 1 and before[1] == 0, before
    initial_transfers = transfers()
    assert render(node('Inspect', source), 'cached-native-input') == reference
    cached = counters()
    delta = [a-b for a, b in zip(transfers(), initial_transfers)]
    assert cached[0] == before[0] and cached[1] == 0 and cached[10] > before[10], cached
    assert delta[0] == 0, f'Cached input was uploaded from CPU: {delta}'
    report['cached_native_input'] = True
    report['cached_probe_transfers'] = delta

    status = module.OfxPressureFillHeap()
    pressure = True
    report['pressure_status'] = status
    report['pressure_counters'] = counters()
    assert status == 8 and counters()[8] > 0 and counters()[9] == 1, counters()
    report['stages'].append({'name': 'heap-exhausted', 'counters': counters(), 'transfers': transfers()})
    assert render(node('Filter', source), 'fallback') == reference
    fallback = counters()
    assert fallback[3] >= 1 and fallback[11] > cached[11], fallback
    report['cpu_retry'] = True
    report['cached_source_cpu_renders'] = fallback[1] - cached[1]
    assert module.OfxPressureReleaseHeap() == 0
    pressure = False
    resumed = counters()
    assert render(node('Inspect', node('Source')), 'recovered') == reference
    assert counters()[0] > resumed[0] and counters()[10] > resumed[10], counters()
    assert counters()[1] == resumed[1] and counters()[3] == resumed[3], counters()
    info = DeviceInfo()
    assert module.OfxPressureGetDeviceInfo(ctypes.byref(info)) == 0
    assert info.validation_enabled == 1 and info.validation_errors == 0
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    kernel.GetModuleHandleW.restype = ctypes.c_void_p
    loaded = [name for name in ('opengl32.dll', 'Qt6OpenGL.dll', 'libEGL.dll', 'libGLESv2.dll')
              if kernel.GetModuleHandleW(name)]
    assert not loaded, f'Unexpected graphics libraries: {loaded}'
    report.update(ok=True, native_recovered=True, counters=counters(), pixels_compared=64*48*4,
                  device=info.name.decode('utf-8'), validation_errors=info.validation_errors,
                  opengl_loaded=loaded)
except Exception:
    report['exception'] = traceback.format_exc()
finally:
    if module:
        if pressure:
            report['pressure_release_status'] = module.OfxPressureReleaseHeap()
        report['final_counters'] = counters()
    report['artifact_sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
print(json.dumps(report))
