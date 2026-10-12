"""A GPU-only instance must remain GPU-required when capability changes."""
import ctypes
import json
import os
from pathlib import Path
import traceback
from PySide6.QtGui import QImage

output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True, exist_ok=True)
Path(os.environ['NATRON_TEST_START_MARKER']).write_text('Dynamic native requirement started', encoding='utf-8')
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE'], 'stages': []}
module = ctypes.CDLL(str(Path('build/ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/Win64/RetiredRenderSmoke.ofx').resolve()))
module.OfxRetiredRenderCounter.argtypes = [ctypes.c_int, ctypes.c_int]
module.OfxRetiredRenderCounter.restype = ctypes.c_ulonglong
counter = lambda index: module.OfxRetiredRenderCounter(4, index)
try:
    app.addFormat('DynamicRequired 64x48 1')
    app.getProjectParam('outputFormat').setValue('DynamicRequired')
    app.getProjectParam('frameRange').set(1, 3)
    ppm = output.parent/'source.ppm'
    ppm.write_bytes(b'P6\n64 48\n255\n' + (bytes((255,0,0))*32 + bytes((0,255,0))*32)*48)
    source = app.createReader(ppm.as_posix())
    effect = app.createNode('org.natron.test.DualRenderAPI')
    assert source and effect and effect.connectInput(0, source)
    toggle = effect.getParam('nativeAvailable')
    assert toggle
    for frame, enabled in enumerate((True, False, True), 1):
        toggle.setValue(enabled)
        destination = output.parent/f'frame-{frame}.png'
        writer = app.createWriter(destination.as_posix())
        assert writer and writer.connectInput(0, effect)
        before = counter(4)
        error = None
        try:
            app.render(writer, frame, frame)
        except Exception as exception:
            error = str(exception)
        image = QImage(str(destination))
        report['lastRender'] = {'enabled':enabled,'before':before,'after':counter(4),'nativeAfter':counter(9),'exists':destination.exists(),'imageNull':image.isNull(),'bytes':destination.stat().st_size if destination.exists() else 0}
        if enabled:
            assert not image.isNull() and counter(4) > before
            for y in range(48):
                for x in range(64):
                    assert image.pixelColor(x,y).getRgb() == ((255,0,0,255) if x < 32 else (0,255,0,255))
        else:
            assert not destination.exists() and image.isNull() and counter(4) == before, 'GPU-required instance attempted a CPU callback'
        assert counter(4) == counter(9) and not counter(7) and not counter(8)
        report['stages'].append({'frame': frame, 'nativeAvailable': enabled, 'renderCallbacks': counter(4)-before,
                                 'nativeCallbacks': counter(9), 'error': error, 'pixelsVerified': 3072 if enabled else 0})
        writer.destroy(False)
    report.update(ok=True, vendorId=counter(10), deviceId=counter(11), cpuCallbacks=counter(4)-counter(9))
except Exception:
    report['error'] = traceback.format_exc()
finally:
    output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    os.write(1,b'NATRON_DYNAMIC_REQUIRED_EXIT_REQUESTED\n')
assert report['ok'], report.get('error')
