"""NatronRenderer: compare shipped OCIO Vulkan nodes to their CPU paths."""
import ctypes
import hashlib
import json
import math
import os
import struct
from pathlib import Path

import NatronEngine
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QColor, QImage

root = Path.cwd()
output = root / "build/ocio-vulkan-smoke"
output.mkdir(parents=True, exist_ok=True)
bundle = Path(os.environ["OFX_PLUGIN_PATH"].split(";")[0]) / "IO.ofx.bundle/Contents/Win64/IO.ofx"
module = ctypes.CDLL(str(bundle))
module.OfxOCIOVulkanRenderCount.restype = ctypes.c_uint64
module.OfxOCIOVulkanClipRenderCount.restype = ctypes.c_uint64
module.OfxOCIOVulkanMaskUploadCount.restype = ctypes.c_uint64
module.OfxOCIOVulkanTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
module.OfxOCIOVulkanTransferCounts.restype = ctypes.c_int


def transfer_counts():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert module.OfxOCIOVulkanTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 0
    return uploads.value, downloads.value


class DeviceInfo(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in (
        "vendor_id", "device_id", "api_version", "validation_enabled", "validation_errors")]


module.OfxOCIOVulkanDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
module.OfxOCIOVulkanDeviceInfo.restype = ctypes.c_int
names = ("OCIOColorSpace", "OCIOCDLTransform", "OCIODisplay", "OCIOFileTransform", "OCIOLogConvert", "OCIOLookTransform")
ids = list(NatronEngine.natron.getPluginIDs())
assert all("fr.inria.openfx." + name in ids for name in names)
assert "org.natron.test.OcioVulkanSuite" in ids
abi = ctypes.CDLL(str(root / "build/ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64/OcioVulkanSuiteSmoke.ofx"))
abi.OfxOCIOVulkanSelfTest.restype = ctypes.c_char_p
raw_test = abi.OfxOCIOVulkanSelfTest().decode()
assert raw_test.startswith("{"), raw_test
reference = json.loads(raw_test)
assert reference["passed"] and reference["referenceCases"] == 64 and reference["validationErrors"] == 0
width, height = 43, 29
app.addFormat(f"OcioVulkanSmoke {width}x{height} 1")
app.getProjectParam("outputFormat").setValue("OcioVulkanSmoke")
app.getProjectParam("frameRange").set(1, 1)
artifacts = []
source = output / "入力-HDR.pfm"
values = [((x * 3 + y * 7 + c * 13) % 79) / 38.0 - .125
          for y in range(height) for x in range(width) for c in range(3)]
source.write_bytes(f"PF\n{width} {height}\n-1.0\n".encode() + struct.pack("<" + "f" * len(values), *values))
artifacts.append(source)
cube = output / "grade.cube"
edge = 17
cube.write_text("LUT_3D_SIZE 17\n" + "\n".join(
    f"{(r / (edge - 1)) ** 1.2:.9f} {(g / (edge - 1)) ** .8:.9f} {(b / (edge - 1)) ** 1.1:.9f}"
    for b in range(edge) for g in range(edge) for r in range(edge)), encoding="utf-8")
artifacts.append(cube)
config = output / "テスト.ocio"
config.write_text("""ocio_profile_version: 2
search_path: .
strictparsing: true
roles:
  default: linear
  scene_linear: linear
  compositing_log: encoded
  color_picking: linear
  color_timing: linear
  reference: linear
displays:
  Test:
    - !<View> {name: Encoded, colorspace: encoded}
active_displays: [Test]
active_views: [Encoded]
looks:
  - !<Look>
    name: Grade
    process_space: linear
    transform: !<CDLTransform> {slope: [1.1, 0.9, 1.2], offset: [0.02, 0.01, 0.03], power: [1.05, 0.9, 1.1], sat: 0.85}
colorspaces:
  - !<ColorSpace>
    name: linear
    family: Test
    bitdepth: 32f
    isdata: false
    allocation: uniform
  - !<ColorSpace>
    name: encoded
    family: Test
    bitdepth: 32f
    isdata: false
    allocation: uniform
    from_scene_reference: !<ExponentTransform> {value: [0.5, 0.6, 0.7, 1]}
  - !<ColorSpace>
    name: cube
    family: Test
    bitdepth: 32f
    isdata: false
    allocation: uniform
    from_scene_reference: !<FileTransform> {src: grade.cube, interpolation: tetrahedral}
""", encoding="utf-8")
artifacts.append(config)
reader = app.createReader(source.as_posix())
reader.getParam("enableGPU").setValue(False)
assert reader is not None


def set_spaces(node):
    for name, value in (("ocioConfigFile", config.as_posix()), ("ocioInputSpace", "linear"), ("ocioOutputSpace", "linear")):
        param = node.getParam(name)
        if param is not None:
            param.setValue(value)


set_spaces(reader)
reader.getParam("outputComponents").setValue("RGBA")
cases = []


def pfm(path):
    with path.open("rb") as stream:
        assert stream.readline().strip() == b"PF"
        assert list(map(int, stream.readline().split())) == [width, height]
        scale = float(stream.readline())
        data = stream.read()
    assert len(data) == width * height * 3 * 4
    return struct.unpack(("<" if scale < 0 else ">") + "f" * (width * height * 3), data)


def render(node, name, owner=app, expected_mask_uploads=0, expected_mask_downloads=0, native_nodes=None):
    native_nodes = native_nodes or [node]
    paths = []
    gpu_calls = 0
    native_clips = 0
    gpu_transfers = None
    for enabled, suffix in ((True, "gpu"), (False, "cpu")):
        for native_node in native_nodes:
            native_node.getParam("enableGPU").setValue(enabled)
        destination = output / f"{name}-{suffix}.pfm"
        destination.unlink(missing_ok=True)
        writer = owner.createNode("fr.inria.openfx.WritePFM")
        assert writer is not None and writer.connectInput(0, node)
        writer.getParam("filename").setValue(destination.as_posix())
        writer.getParam("outputComponents").setValue("RGB")
        set_spaces(writer)
        before = module.OfxOCIOVulkanRenderCount()
        clips_before = module.OfxOCIOVulkanClipRenderCount()
        masks_before = module.OfxOCIOVulkanMaskUploadCount()
        transfers_before = transfer_counts()
        QCoreApplication.processEvents()
        owner.render(writer, 1, 1)
        assert destination.exists(), name
        difference = module.OfxOCIOVulkanRenderCount() - before
        clips = module.OfxOCIOVulkanClipRenderCount() - clips_before
        masks = module.OfxOCIOVulkanMaskUploadCount() - masks_before
        transfers = [after - previous for after, previous in zip(transfer_counts(), transfers_before)]
        if enabled:
            assert difference > 0, f"{name} did not execute Vulkan"
            assert clips == difference, f"{name} staged color pixels through CPU: renders={difference}, clips={clips}"
            assert clips == len(native_nodes), (name, "unexpected duplicate render", clips, len(native_nodes))
            assert masks == expected_mask_uploads, (name, masks, expected_mask_uploads)
            assert transfers == [1 + masks, 1 + expected_mask_downloads], (name, "Reader/Writer and Alpha mask boundaries", transfers, "renders", difference, "clips", clips)
            gpu_calls = difference
            native_clips = clips
            gpu_transfers = transfers
        else:
            assert difference == 0, f"{name} CPU reference executed Vulkan"
            assert clips == 0 and masks == 0, (name, "CPU reference borrowed GPU clips")
        paths.append(destination)
        artifacts.append(destination)
    gpu, cpu = map(pfm, paths)
    maximum = 0.0
    for i, (actual, expected) in enumerate(zip(gpu, cpu)):
        assert math.isfinite(actual) and math.isfinite(expected), (name, i)
        error = abs(actual - expected)
        maximum = max(maximum, error)
        assert error <= .00003 + abs(expected) * .00003, (name, i, actual, expected, error)
    cases.append({"name": name, "pixels_compared": width * height, "native_renders": gpu_calls,
                  "native_clip_renders": native_clips, "boundary_transfers": gpu_transfers,
                  "mask_uploads": expected_mask_uploads, "mask_downloads": expected_mask_downloads, "max_absolute_error": maximum})
    for native_node in native_nodes:
        native_node.getParam("enableGPU").setValue(True)


nodes = {}
for name in names:
    node = app.createNode("fr.inria.openfx." + name)
    assert node is not None and node.connectInput(0, reader)
    node.getParam("forceCaching").setValue(True)
    set_spaces(node)
    assert node.getParam("enableGPU").getValue()
    if name == "OCIOColorSpace":
        node.getParam("ocioOutputSpace").setValue("encoded")
    elif name == "OCIOCDLTransform":
        for param, vals in (("slope", (1.1, .9, 1.2)), ("offset", (.02, .01, .03)), ("power", (1.05, .9, 1.1))):
            for dim, value in enumerate(vals):
                node.getParam(param).setValue(value, dim)
        node.getParam("saturation").setValue(.85)
    elif name == "OCIODisplay":
        node.getParam("display").setValue("Test")
        node.getParam("view").setValue("Encoded")
        node.getParam("gain").setValue(.4)
        node.getParam("gamma").setValue(1.1)
    elif name == "OCIOFileTransform":
        node.getParam("file").setValue(cube.as_posix())
        node.getParam("interpolation").setValue("tetrahedral")
    elif name == "OCIOLogConvert":
        node.getParam("operation").setValue("lin2log")
    elif name == "OCIOLookTransform":
        node.getParam("singleLook").setValue(False)
        node.getParam("lookCombination").setValue("Grade")
    render(node, name)
    nodes[name] = node

file_node = nodes["OCIOFileTransform"]
file_node.getParam("interpolation").setValue("linear")
render(file_node, "FileTransform-LUT3D-linear")
lut1d = output / "curve.cube"
lut1d.write_text("LUT_1D_SIZE 1024\n" + "\n".join(
    f"{(i / 1023) ** 1.2:.9f} {(i / 1023) ** .8:.9f} {(i / 1023) ** 1.1:.9f}" for i in range(1024)), encoding="utf-8")
artifacts.append(lut1d)
file_node.getParam("file").setValue(lut1d.as_posix())
for interpolation in ("linear", "nearest"):
    file_node.getParam("interpolation").setValue(interpolation)
    render(file_node, "FileTransform-LUT1D-" + interpolation)

alpha_source = output / "入力-alpha.png"
alpha_image = QImage(width, height, QImage.Format_RGBA8888)
for y in range(height):
    for x in range(width):
        alpha_image.setPixelColor(x, y, QColor(35 + x % 160, 50 + y % 160, 100 + (x + y) % 100, (x * 11 + y * 7) % 256))
assert alpha_image.save(str(alpha_source))
artifacts.append(alpha_source)
alpha_reader = app.createReader(alpha_source.as_posix())
alpha_reader.getParam("enableGPU").setValue(False)
assert alpha_reader is not None
set_spaces(alpha_reader)
alpha_reader.getParam("outputComponents").setValue("RGBA")
for name in names:
    if name == "OCIOCDLTransform":
        continue
    effect = nodes[name]
    effect.disconnectInput(0)
    assert effect.connectInput(0, alpha_reader)
    effect.getParam("premult").setValue(True)
    render(effect, name + "-premult")
cdl = nodes["OCIOCDLTransform"]
cdl.disconnectInput(0)
assert cdl.connectInput(0, alpha_reader)
cdl.getParam("premult").setValue(True)
render(cdl, "CDL-premult")
assert cdl.connectInput(1, alpha_reader)
cdl.getParam("mix").setValue(.625)
for inverted in (False, True):
    cdl.getParam("maskInvert").setValue(inverted)
    render(cdl, "CDL-mask-inverted" if inverted else "CDL-mask", expected_mask_uploads=1)

# A separate Alpha CPU source and a shared RGBA color/mask source both need
# just one explicit mask upload; prefetch keeps mask clips on the CPU.
mask_reader = app.createReader(alpha_source.as_posix())
mask_reader.getParam("enableGPU").setValue(False)
assert mask_reader
set_spaces(mask_reader)
mask_reader.getParam("outputComponents").setValue("Alpha")
cdl.disconnectInput(1)
assert cdl.connectInput(1, mask_reader)
render(cdl, "CDL-CPU-alpha-mask", expected_mask_uploads=1)

color = nodes["OCIOColorSpace"]
color.getParam("ocioOutputSpace").setValue("cube")
render(color, "ColorSpace-LUT3D")
color.getParam("mix").setValue(.375)
render(color, "ColorSpace-mix")
chain_first = app.createNode("fr.inria.openfx.OCIOColorSpace")
chain_second = app.createNode("fr.inria.openfx.OCIOCDLTransform")
assert chain_first and chain_second and chain_first.connectInput(0, alpha_reader) and chain_second.connectInput(0, chain_first)
for effect in (chain_first, chain_second):
    set_spaces(effect)
    effect.getParam("premult").setValue(True)
    effect.getParam("forceCaching").setValue(True)
chain_first.getParam("ocioOutputSpace").setValue("encoded")
chain_second.getParam("saturation").setValue(.65)
render(chain_second, "native-two-node-chain", native_nodes=[chain_first, chain_second])

# Invalidate the source, render both native effects once, then reuse their GPU cache.
cache_source = output / "入力-alpha-cache.png"
alpha_image.setPixelColor(width // 2, height // 2, QColor(180, 90, 45, 170))
assert alpha_image.save(str(cache_source))
artifacts.append(cache_source)
alpha_reader.getParam("filename").setValue(cache_source.as_posix())
cache_transfers, cache_renders = [], []
for suffix in ("seed", "reuse"):
    destination = output / f"native-cache-{suffix}.pfm"
    destination.unlink(missing_ok=True)
    writer = app.createNode("fr.inria.openfx.WritePFM")
    assert writer and writer.connectInput(0, chain_second)
    writer.getParam("filename").setValue(destination.as_posix())
    writer.getParam("outputComponents").setValue("RGB")
    set_spaces(writer)
    before = module.OfxOCIOVulkanClipRenderCount()
    transfers_before = transfer_counts()
    app.render(writer, 1, 1)
    assert destination.exists()
    cache_renders.append(module.OfxOCIOVulkanClipRenderCount() - before)
    cache_transfers.append([after - previous for after, previous in zip(transfer_counts(), transfers_before)])
    artifacts.append(destination)
assert cache_renders == [2, 0], cache_renders
assert cache_transfers == [[1, 1], [0, 1]], cache_transfers
assert (output / "native-cache-seed.pfm").read_bytes() == (output / "native-cache-reuse.pfm").read_bytes()
project = output / "Vulkanカラー処理.ntp"
assert app.saveProjectAs(project.as_posix())
artifacts.append(project)
loaded = app.loadProject(project.as_posix())
assert loaded is not None
loaded_color = next(n for n in loaded.getChildren() if n.getPluginID() == "fr.inria.openfx.OCIOColorSpace")
loaded_color.getParam("mix").setValue(.625)
render(loaded_color, "loaded-project", loaded)
info = DeviceInfo()
assert module.OfxOCIOVulkanDeviceInfo(ctypes.byref(info)) == 0
assert info.validation_enabled and info.validation_errors == 0
report = {"ok": True, "device": info.name.decode(), "gpu_index": os.environ.get("NATRON_VULKAN_DEVICE"),
          "validation_enabled": bool(info.validation_enabled), "validation_errors": info.validation_errors,
          "native_renders": module.OfxOCIOVulkanRenderCount(), "native_clip_renders": module.OfxOCIOVulkanClipRenderCount(),
          "cases": cases, "project_roundtrip": True,
          "native_chain_length": 2, "native_cache_reuse": True,
          "cache_native_renders": cache_renders, "cache_boundary_transfers": cache_transfers,
          "ocio_reference": reference,
          "profile": os.environ.get("NATRON_TEST_PROFILE"), "plugin_binary": str(bundle),
          "artifact_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}}
(output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("OCIO_VULKAN_SMOKE_OK " + json.dumps(report, ensure_ascii=False))
