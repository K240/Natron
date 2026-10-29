"""Create a graph using all rebuilt OFX sets in the Qt 6 GUI."""
import json
import os
from pathlib import Path

if marker := os.environ.get('NATRON_TEST_START_MARKER'):
    Path(marker).write_text('Python OFX GUI smoke started', encoding='utf-8')

import NatronEngine
from PySide6.QtCore import QTimer, qVersion, QMetaObject, Q_RETURN_ARG, Qt
from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication, QMainWindow

nodes = []


def inspect_gui():
    windows = [widget for widget in QApplication.allWidgets()
               if isinstance(widget, QMainWindow) and widget.isVisible()]
    viewers = [widget for widget in QApplication.allWidgets()
               if widget.property("graphicsBackend") == "Vulkan" and widget.isVisible()]
    report = {"qt": qVersion(), "nodes": [node.getPluginID() for node in nodes],
              "unexpected_dialogs": [w.windowTitle() for w in QApplication.topLevelWidgets()
                                     if w.isVisible() and w.isModal()],
              "registered_nodes": len(NatronEngine.natron.getPluginIDs()),
              "valid_viewers": sum(widget.property("presentedFrames") > 0 and not widget.property("graphicsError") for widget in viewers),
              "graphics_backend": [widget.property("graphicsBackend") for widget in viewers],
              "validation_errors": [widget.property("validationErrors") for widget in viewers]}
    report["ok"] = (bool(windows) and bool(viewers)
                     and report["valid_viewers"] == len(viewers)
                     and not report["unexpected_dialogs"] and not any(report["validation_errors"]))
    if viewers:
        frame = QMetaObject.invokeMethod(viewers[0], 'grabFrame', Qt.DirectConnection, Q_RETURN_ARG(QImage))
        assert not frame.isNull(), "Vulkan Viewer returned an empty presentation readback"
        pixel = frame.pixelColor(frame.width() // 2, frame.height() // 2)
        report["center_pixel"] = pixel.name()
        # Half-brightness cyan catches an accidental second display transform.
        report["ok"] = report["ok"] and pixel.red() <= 2 and abs(pixel.green() - 188) <= 2 and abs(pixel.blue() - 188) <= 2
        frame.save("build/ofx-gui-frame.png")
        report["validation_errors"] = [widget.property("validationErrors") for widget in viewers]
        report["ok"] = report["ok"] and not any(report["validation_errors"])
    if windows:
        windows[0].grab().save("build/ofx-gui-window.png")
    Path("build/ofx-gui-result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("OFX_GUI_SMOKE", json.dumps(report))
    QApplication.exit(0)


QTimer.singleShot(8000, inspect_gui)
source = Path.cwd() / "build" / "ofx-smoke" / "gui-input.ppm"
source.parent.mkdir(parents=True, exist_ok=True)
source.write_bytes(b"P6\n64 48\n255\n" + bytes((255, 0, 0)) * (64 * 48))
app.addFormat("OFXGuiSmoke 64x48 1")
app.getProjectParam("outputFormat").setValue("OFXGuiSmoke")
previous = app.createReader(source.as_posix())
assert previous is not None
previous.setPosition(0, 0)
for identifier in ("net.sf.openfx.Invert", "net.sf.cimg.CImgBlur",
                   "net.fxarena.openfx.Modulate", "eu.gmic.BlurGaussian"):
    node = app.createNode(identifier)
    assert node is not None and node.connectInput(0, previous), identifier
    nodes.append(node)
    node.setPosition(0, len(nodes) * 70)
    previous = node
nodes[1].getParam("size").setValue(8.0)
nodes[2].getParam("brightness").setValue(50.0)
viewer = app.createNode("fr.inria.built-in.Viewer")
assert viewer is not None and viewer.connectInput(0, previous)
viewer.setPosition(0, 350)
