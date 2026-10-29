"""Run with NatronRenderer through run-windows-qt6.ps1, not standalone Python."""
import json
import sys
from pathlib import Path

import NatronEngine
import PySide6
from PySide6.QtGui import QImage
from PySide6.QtCore import QCoreApplication

output_dir = Path.cwd() / 'build' / 'qt6-smoke'
assert QtCore.__name__ == 'PySide6.QtCore', 'Natron did not initialize native Qt 6 scripting'
output_dir.mkdir(parents=True, exist_ok=True)
source = output_dir / '入力.ppm'
destination = output_dir / '出力.png'
project = output_dir / '確認.ntp'
source.write_bytes(b'P6\n64 48\n255\n' + bytes((255, 0, 0)) * (64 * 48))
destination.unlink(missing_ok=True)

reader = app.createReader(source.as_posix())
writer = app.createWriter(destination.as_posix())
assert reader is not None and writer is not None, 'Could not create IO nodes'
assert writer.connectInput(0, reader), 'Could not connect IO nodes'
app.addFormat('Qt6Smoke 64x48 1')
app.getProjectParam('outputFormat').setValue('Qt6Smoke')
app.getProjectParam('frameRange').set(1, 1)
QCoreApplication.processEvents()
app.render(writer, 1, 1)

image = QImage(str(destination))
assert not image.isNull(), 'Rendered PNG is missing or unreadable'
assert (image.width(), image.height()) == (64, 48), f'Unexpected image dimensions: {image.width()}x{image.height()}'
pixel = image.pixelColor(32, 24)
assert pixel.red() > 240 and pixel.green() < 15 and pixel.blue() < 15, pixel.name()
assert app.saveProjectAs(project.as_posix()), 'Project save failed'
assert project.is_file() and project.stat().st_size > 0

frame_range = app.getProjectParam('frameRange').get()
assert (frame_range.x, frame_range.y) == (1, 1), 'Python parameter conversion failed'
loaded = app.loadProject(project.as_posix())
assert loaded is not None, 'Project reload failed'
assert loaded.getNode('Read1') is not None and loaded.getNode('Write1') is not None
report = {
    'python': sys.version.split()[0],
    'pyside': PySide6.__version__,
    'scripting_core_module': QtCore.__name__,
    'prefix': sys.prefix,
    'unicode_io': True,
    'project_roundtrip': True,
    'render_size': [image.width(), image.height()],
    'center_pixel': pixel.name(),
    'project': str(project),
}
(output_dir / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
print('QT6_SMOKE_OK ' + json.dumps(report, ensure_ascii=False))
