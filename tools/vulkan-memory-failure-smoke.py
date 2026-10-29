"""Exercise real host allocation failures through independent OFX render actions."""
import ctypes
import json
import os
import traceback
from pathlib import Path
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage
import NatronEngine

root = Path.cwd()
output = root / 'build/vulkan-memory-failure-smoke'
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Memory failure smoke started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE'), 'limit_mb': os.environ.get('NATRON_VULKAN_MEMORY_LIMIT_MB')}
try:
    assert report['limit_mb'] == '32', 'This fixture requires a 32 MiB allocator limit'
    oversized = os.environ.get('NATRON_TEST_MEMORY_OVERSIZED') == '1'
    # A distinct size avoids addFormat deduplicating an existing built-in
    # format with a different choice ID.
    width, height = (2048, 2051) if oversized else (64, 48)
    app.addFormat(f'MemoryFailure {width}x{height} 1')
    app.getProjectParam('outputFormat').setValue('MemoryFailure')
    module = ctypes.CDLL(str(root / 'build/ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/Win64/VulkanMemoryFailureSmoke.ofx'))
    module.OfxMemoryCounter.argtypes = [ctypes.c_int]
    module.OfxMemoryCounter.restype = ctypes.c_ulonglong
    def counters():
        return [module.OfxMemoryCounter(i) for i in range(7)]
    def render(name, filename):
        node = app.createNode('org.natron.test.VulkanMemory' + name)
        assert node
        path = output / filename
        path.unlink(missing_ok=True)
        writer = app.createWriter(path.as_posix())
        assert writer and writer.connectInput(0, node)
        QCoreApplication.processEvents()
        app.render(writer, 1, 1)
        return path
    reference = render('Reference', 'reference.png')
    assert reference.exists() and counters()[4] == 0 and counters()[5] >= 1, counters()
    fallback = render('Fallback', 'fallback.png')
    assert fallback.exists(), 'GPU memory error was not retried with CPU storage'
    assert counters()[0] == (0 if oversized else 1) and counters()[1] >= 1, counters()
    assert counters()[6] == (0 if oversized else 8), counters()
    actual, expected = QImage(str(fallback)), QImage(str(reference))
    assert not actual.isNull() and not expected.isNull()
    assert actual.size() == expected.size() and actual.width() == width and actual.height() == height
    actual = actual.convertToFormat(QImage.Format_RGBA8888)
    expected = expected.convertToFormat(QImage.Format_RGBA8888)
    assert bytes(actual.constBits()) == bytes(expected.constBits()), 'CPU retry differs from reference'
    try:
        required = render('Required', 'required-must-fail.png')
    except Exception as error:
        report['required_exception'] = str(error)
        required = output / 'required-must-fail.png'
    assert not required.exists(), 'GPU-only effect reported successful output after allocation failure'
    assert counters()[2:4] == [0 if oversized else 1, 0], 'GPU-only effect attempted an unsupported render action: ' + str(counters())
    report.update(ok=True, counters=counters(), width=width, height=height,
                  pixels_compared=width*height, cpu_retry=True,
                  gpu_only_failed=True, host_allocation_retry=oversized)
except Exception:
    report['exception'] = traceback.format_exc()
finally:
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
print(json.dumps(report))
