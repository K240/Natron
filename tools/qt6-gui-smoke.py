"""Inspect a launched Natron GUI, then quit the test instance."""
import json
from pathlib import Path

from PySide6.QtCore import QTimer, qVersion
from PySide6.QtWidgets import QApplication, QMainWindow
from PySide6.QtOpenGLWidgets import QOpenGLWidget

_smoke_connected = False


def inspect_gui():
    widgets = QApplication.allWidgets()
    windows = [w for w in widgets if isinstance(w, QMainWindow) and w.isVisible()]
    viewers = [w for w in widgets if isinstance(w, QOpenGLWidget) and w.isVisible()]
    report = {
        'qt': qVersion(),
        'windows': [w.windowTitle() for w in windows],
        'opengl_viewers': len(viewers),
        'valid_contexts': sum(w.isValid() for w in viewers),
    }
    report['ok'] = bool(windows) and bool(viewers) and report['valid_contexts'] == len(viewers)
    image_viewers = [w for w in viewers if 'ViewerGL' in w.metaObject().className()]
    if image_viewers:
        frame = image_viewers[0].grabFramebuffer()
        report['framebuffer_size'] = [frame.width(), frame.height()]
        report['ok'] = report['ok'] and not frame.isNull()
        if _smoke_connected and not frame.isNull():
            pixel = frame.pixelColor(frame.width() // 2, frame.height() // 2)
            report['center_pixel'] = pixel.name()
            report['ok'] = report['ok'] and pixel.red() > 240 and pixel.green() < 15 and pixel.blue() < 15
        frame.save('build/gui-qt6-frame.png')
    Path('build/gui-qt6-result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print('GUI_QT6_SMOKE', json.dumps(report))
    QApplication.exit(0)


QTimer.singleShot(8000, inspect_gui)

viewer = app.createNode('fr.inria.built-in.Viewer')
assert viewer is not None, 'Could not create a viewer'
source = Path.cwd() / 'build' / 'qt6-smoke' / '出力.png'
if source.is_file():
    reader = app.createReader(source.as_posix())
    assert reader is not None and viewer.connectInput(0, reader)
    _smoke_connected = True
