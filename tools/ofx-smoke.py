"""Run in NatronRenderer: exercise every rebuilt OFX set and Arena readers."""
import json
import os
from pathlib import Path

import NatronEngine
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

output_dir = Path(os.environ.get("NATRON_OFX_SMOKE_OUTPUT", Path.cwd() / "build" / "ofx-smoke"))
output_dir.mkdir(parents=True, exist_ok=True)
ids = list(NatronEngine.natron.getPluginIDs())
required = ["net.sf.openfx.Invert", "net.sf.cimg.CImgBlur", "net.fxarena.openfx.Modulate",
            "net.fxarena.openfx.Text", "net.fxarena.openfx.ReadSVG", "fr.inria.openfx.ReadPDF",
            "net.fxarena.openfx.AudioCurve", "eu.gmic.BlurGaussian"]
assert all(identifier in ids for identifier in required), "An OFX set did not load"
app.addFormat("OFXSmoke 64x48 1")
app.getProjectParam("outputFormat").setValue("OFXSmoke")
app.getProjectParam("frameRange").set(1, 1)
source = output_dir / "入力.ppm"
source.write_bytes(b"P6\n64 48\n255\n" +
                   (bytes((255, 0, 0)) * 32 + bytes((0, 0, 255)) * 32) * 48)
reader = app.createReader(source.as_posix())
assert reader is not None
results = {}


def render(node, name):
    destination = output_dir / (name + ".png")
    destination.unlink(missing_ok=True)
    writer = app.createWriter(destination.as_posix())
    assert writer is not None and writer.connectInput(0, node), name
    QCoreApplication.processEvents()
    app.render(writer, 1, 1)
    image = QImage(str(destination))
    assert not image.isNull() and (image.width(), image.height()) == (64, 48), name
    results[name] = {"path": str(destination), "left": image.pixelColor(16, 24).name(),
                     "edge": image.pixelColor(32, 24).name()}
    return image


invert = app.createNode("net.sf.openfx.Invert")
assert invert is not None and invert.connectInput(0, reader)
pixel = render(invert, "misc-invert").pixelColor(16, 24)
assert pixel.red() < 10 and pixel.green() > 240 and pixel.blue() > 240, pixel.name()

blur = app.createNode("net.sf.cimg.CImgBlur")
assert blur is not None and blur.connectInput(0, reader)
blur.getParam("size").setValue(8.0)
pixel = render(blur, "cimg-blur").pixelColor(32, 24)
assert pixel.red() > 20 and pixel.blue() > 20, pixel.name()

modulate = app.createNode("net.fxarena.openfx.Modulate")
assert modulate is not None and modulate.connectInput(0, reader)
modulate.getParam("brightness").setValue(50.0)
pixel = render(modulate, "arena-modulate").pixelColor(16, 24)
# A linear 0.5 red channel is encoded as approximately 188 in the sRGB PNG.
assert 178 < pixel.red() < 198 and pixel.green() < 10 and pixel.blue() < 10, pixel.name()

gmic = app.createNode("eu.gmic.BlurGaussian")
assert gmic is not None and gmic.connectInput(0, reader)
pixel = render(gmic, "gmic-blur").pixelColor(32, 24)
assert pixel.red() > 10 and pixel.blue() > 10, pixel.name()

svg = output_dir / "入力.svg"
svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="48">'
               '<rect width="64" height="48" fill="#008000"/></svg>', encoding="utf-8")
svg_reader = app.createNode("net.fxarena.openfx.ReadSVG")
assert svg_reader is not None
svg_reader.getParam("filename").setValue(svg.as_posix())
assert svg_reader.getParam("ocioConfigFile") is not None, "Arena color management is missing"
pixel = render(svg_reader, "arena-svg").pixelColor(32, 24)
assert 120 < pixel.green() < 140 and pixel.red() < 10 and pixel.blue() < 10, pixel.name()

# A small PDF fixture avoids an extra Python package in the build environment.
pdf = output_dir / "入力.pdf"
stream = b"0 1 1 rg 0 0 64 48 re f\n"
objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
           b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
           b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 64 48] /Resources << >> /Contents 4 0 R >>",
           b"<< /Length " + str(len(stream)).encode() + b" >>\nstream\n" + stream + b"endstream"]
data = bytearray(b"%PDF-1.4\n")
offsets = [0]
for index, obj in enumerate(objects, 1):
    offsets.append(len(data))
    data.extend(str(index).encode() + b" 0 obj\n" + obj + b"\nendobj\n")
xref = len(data)
data.extend(b"xref\n0 5\n0000000000 65535 f \n")
for offset in offsets[1:]:
    data.extend(f"{offset:010d} 00000 n \n".encode())
data.extend(b"trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + str(xref).encode() + b"\n%%EOF\n")
pdf.write_bytes(data)
pdf_reader = app.createNode("fr.inria.openfx.ReadPDF")
assert pdf_reader is not None
pdf_reader.getParam("dpi").setValue(72.0)
pdf_reader.getParam("filename").setValue(pdf.as_posix())
pixel = render(pdf_reader, "arena-pdf").pixelColor(32, 24)
assert pixel.red() < 10 and pixel.green() > 240 and pixel.blue() > 240, pixel.name()

text = app.createNode("net.fxarena.openfx.Text")
assert text is not None
text.getParam("text").setValue("Qt 6")
text.getParam("size").setValue(16)
text_image = render(text, "arena-text")
assert any(text_image.pixelColor(x, y).alpha() > 0 and text_image.pixelColor(x, y).red() > 80
           for y in range(48) for x in range(64)), "Text did not render"

project = output_dir / "確認.ntp"
assert app.saveProjectAs(project.as_posix())
loaded = app.loadProject(project.as_posix())
assert loaded is not None
report = {"ok": True, "registered_nodes": len(ids),
          "gmic_nodes": sum(identifier.startswith("eu.gmic.") for identifier in ids),
          "renders": results, "project_roundtrip": True}
(output_dir / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("OFX_SMOKE_OK " + json.dumps(report, ensure_ascii=False))
