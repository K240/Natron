"""Verify the same retired-render contract through NatronRenderer."""
import ctypes
import json
import os
from pathlib import Path
import traceback
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True, exist_ok=True)
Path(os.environ['NATRON_TEST_START_MARKER']).write_text('Retired render headless test started', encoding='utf-8')
disabled = os.environ.get('NATRON_VULKAN_DISABLED') == '1'
report = {'ok':False,'profile':os.environ['NATRON_TEST_PROFILE'],'vulkan_disabled':disabled,'cases':[]}
ids = ['org.natron.test.RequiredOpenGL','org.natron.test.OptionalOpenGL','org.natron.test.ExplicitCPU',
       'org.natron.test.ContextOpenGL','org.natron.test.DualRenderAPI','org.natron.test.DefaultCPU']
library = ctypes.CDLL(str(Path('build/ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/Win64/RetiredRenderSmoke.ofx').resolve()))
library.OfxRetiredRenderCounter.argtypes = [ctypes.c_int,ctypes.c_int]
library.OfxRetiredRenderCounter.restype = ctypes.c_ulonglong
counts = lambda: [[library.OfxRetiredRenderCounter(k,i) for i in range(12)] for k in range(6)]
try:
    app.addFormat('RetiredHeadless 64x48 1')
    app.getProjectParam('outputFormat').setValue('RetiredHeadless')
    app.getProjectParam('frameRange').set(1,1)
    source_file = output.parent/'source.ppm'
    source_file.write_bytes(b'P6\n64 48\n255\n' + (bytes((255,0,0))*32+bytes((0,255,0))*32)*48)
    source = app.createReader(source_file.as_posix())
    assert source
    for index, identifier in enumerate(ids):
        effect = app.createNode(identifier)
        if index in (0,3):
            assert effect is None and not any(counts()[index][2:10])
            report['cases'].append({'id':identifier,'rejected_before_create':True})
            continue
        assert effect and effect.connectInput(0,source)
        assert effect.getParam('enableGPURendering') is None
        destination = output.parent/f'{index}-output.png'
        writer = app.createWriter(destination.as_posix())
        assert writer and writer.connectInput(0,effect)
        render_exception = None
        try:
            app.render(writer,1,1)
        except Exception as error:
            render_exception = str(error)
        image = QImage(str(destination))
        if disabled and index == 4:
            assert image.isNull() and not counts()[4][4], 'GPU-only effect attempted a CPU callback'
            report['cases'].append({'id':identifier,'native_unavailable_failure':True,'exception':render_exception})
        else:
            assert not image.isNull() and [image.width(),image.height()] == [64,48], render_exception
            for y in range(48):
                for x in range(64):
                    assert image.pixelColor(x,y).getRgb() == ((255,0,0,255) if x < 32 else (0,255,0,255))
            values = counts()[index]
            assert values[4] > 0 and values[5] > 0 and values[6] > 0 and not any(values[7:9]), values
            assert values[9] == (values[4] if index == 4 else 0), values
            report['cases'].append({'id':identifier,'pixels_verified':64*48,'native_render':index == 4})
        writer.destroy(False)
        effect.destroy(False)
        QCoreApplication.processEvents()
    source.destroy(False)
    report.update(ok=True,counters=counts())
except Exception:
    report['error'] = traceback.format_exc()
finally:
    output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    os.write(1,b'NATRON_RETIRED_RENDER_HEADLESS_EXIT_REQUESTED\n')
assert report['ok'], report.get('error')
