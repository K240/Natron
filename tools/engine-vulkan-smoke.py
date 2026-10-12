"""Run inside NatronRenderer; native OFX clip chain, cache and CPU boundaries."""
import ctypes
import hashlib
import json
import os
from pathlib import Path

import NatronEngine
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QColor, QImage

root = Path.cwd()
output_dir = root / "build" / "engine-vulkan-smoke"
output_dir.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get("NATRON_TEST_START_MARKER"):
    Path(marker).write_text("Engine Vulkan smoke started", encoding="utf-8")
plugin_path = root / "build/ofx-tests/VulkanImageSuiteSmoke.ofx.bundle/Contents/Win64/VulkanImageSuiteSmoke.ofx"
assert "org.natron.test.VulkanImageSuite" in NatronEngine.natron.getPluginIDs()
plugin = ctypes.CDLL(str(plugin_path))
plugin.OfxVulkanSmokeSelfTest.restype = ctypes.c_int
plugin.OfxVulkanSmokeRenderCount.restype = ctypes.c_ulonglong
plugin.OfxVulkanSmokeNativeRenderCount.restype = ctypes.c_ulonglong
plugin.OfxVulkanSmokeNativePixelTransfers.restype = ctypes.c_ulonglong
plugin.OfxVulkanSmokeTransferCounts.restype = ctypes.c_int
plugin.OfxVulkanSmokeTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
plugin.OfxVulkanSmokeMaximumError.restype = ctypes.c_float


class DeviceInfo(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in (
        "vendor_id", "device_id", "api_version", "validation_enabled", "validation_errors")]


plugin.OfxVulkanSmokeDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
plugin.OfxVulkanSmokeDeviceInfo.restype = ctypes.c_int
assert plugin.OfxVulkanSmokeSelfTest() == 1, "Native suite handle/stride/concurrent-call test failed"

def transfer_counts():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert plugin.OfxVulkanSmokeTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 1
    return uploads.value, downloads.value

def linear_boundary(effect):
    # Isolate Engine clip transfers from the IO bundle's separate GPU OCIO
    # staging path, which is covered by the OCIO regression suite.
    for name in ("ocioInputSpace", "ocioOutputSpace"):
        param = effect.getParam(name)
        if param is not None:
            param.setValue("Linear")

width, height = 137, 91
image = QImage(width, height, QImage.Format_RGBA8888)
for y in range(height):
    for x in range(width):
        image.setPixelColor(x, y, QColor(40 + x % 180, 80 + y % 150, 160 - x % 120, 64 + (x + y) % 160))
source = output_dir / "入力-alpha.png"
assert image.save(str(source)), "Could not write the source fixture"
second_source = output_dir / "入力-alpha-2.png"
image.setPixelColor(width // 2, height // 2, QColor(220, 170, 45, 160))
assert image.save(str(second_source))
reader = app.createReader(source.as_posix())
reader.getParam("enableGPU").setValue(False)
linear_boundary(reader)
node = app.createNode("org.natron.test.VulkanImageSuite")
assert reader is not None and node is not None
assert node.connectInput(0, reader)
first_native = node
node = app.createNode("org.natron.test.VulkanImageSuite")
assert node and node.connectInput(0, first_native)
for native_node in (first_native, node):
    native_node.getParam("forceCaching").setValue(True)
cpu_first = app.createNode("org.natron.test.VulkanImageSuite")
cpu_second = app.createNode("org.natron.test.VulkanImageSuite")
assert cpu_first and cpu_second and cpu_first.connectInput(0, reader) and cpu_second.connectInput(0, cpu_first)
for cpu_node in (cpu_first, cpu_second):
    cpu_node.getParam("nativeEnabled").setValue(False)
app.addFormat(f"EngineVulkanSmoke {width}x{height} 1")
app.getProjectParam("outputFormat").setValue("EngineVulkanSmoke")
app.getProjectParam("frameRange").set(1, 2)
destinations = []
reference_destinations = []
cache_destinations = []
maximum_byte_error = 0
graph_transfers = []
for frame in (1, 2):
    if frame == 2:
        # A static clip is cached across frame numbers. Change the input fixture
        # to require a second actual GPU render rather than accepting cache reuse.
        reader.getParam("filename").setValue(second_source.as_posix())
    destination = output_dir / f"出力-{frame}.png"
    destination.unlink(missing_ok=True)
    writer = app.createWriter(destination.as_posix())
    assert writer is not None and writer.connectInput(0, node)
    linear_boundary(writer)
    QCoreApplication.processEvents()
    before_transfers = transfer_counts()
    app.render(writer, frame, frame)
    after_transfers = transfer_counts()
    delta = [a-b for a,b in zip(after_transfers, before_transfers)]
    assert delta == [1, 1], f"Reader/native chain/Writer must transfer only at its two CPU boundaries: {delta}; native renders={plugin.OfxVulkanSmokeNativeRenderCount()}"
    graph_transfers.append(delta)
    rendered = QImage(str(destination))
    assert not rendered.isNull() and (rendered.width(), rendered.height()) == (width, height)
    assert rendered.pixelColor(width // 2, height // 2).alpha() > 0
    destinations.append(str(destination))
    # A second Write node requests the same resident output. Neither native
    # plugin may render again while the cache hash and source are unchanged.
    native_before = plugin.OfxVulkanSmokeNativeRenderCount()
    cache_destination = output_dir / f"キャッシュ-{frame}.png"
    cache_destination.unlink(missing_ok=True)
    cache_writer = app.createWriter(cache_destination.as_posix())
    assert cache_writer and cache_writer.connectInput(0, node)
    linear_boundary(cache_writer)
    app.render(cache_writer, frame, frame)
    assert plugin.OfxVulkanSmokeNativeRenderCount() == native_before, "Native cache rerendered identical input"
    assert QImage(str(cache_destination)) == rendered, "Resident native cache changed pixels"
    cache_destinations.append(str(cache_destination))
    reference_destination = output_dir / f"CPU比較-{frame}.png"
    reference_destination.unlink(missing_ok=True)
    reference_writer = app.createWriter(reference_destination.as_posix())
    assert reference_writer and reference_writer.connectInput(0, cpu_second)
    linear_boundary(reference_writer)
    app.render(reference_writer, frame, frame)
    reference = QImage(str(reference_destination))
    assert not reference.isNull() and reference.size() == rendered.size()
    for y in range(height):
        for x in range(width):
            actual, expected = rendered.pixelColor(x, y), reference.pixelColor(x, y)
            maximum_byte_error = max(maximum_byte_error, *(abs(a-b) for a,b in zip(actual.getRgb(), expected.getRgb())))
    assert maximum_byte_error <= 1, f"Native clip chain differs from CPU clip reference: {maximum_byte_error}"
    reference_destinations.append(str(reference_destination))
project = output_dir / "Vulkan画像処理.ntp"
assert app.saveProjectAs(project.as_posix())
loaded = app.loadProject(project.as_posix())
assert loaded is not None
assert any(n.getPluginID() == "org.natron.test.VulkanImageSuite" for n in loaded.getChildren())
loaded_native = [n for n in loaded.getChildren() if n.getPluginID() == "org.natron.test.VulkanImageSuite" and n.getParam("nativeEnabled").getValue()]
assert len(loaded_native) == 2, "Native render selection did not survive project loading"
loaded_tail = next(n for n in loaded_native if n.getInput(0).getPluginID() == "org.natron.test.VulkanImageSuite")
reload_source = output_dir / "入力再読込.png"
reload_source.write_bytes(second_source.read_bytes())
loaded_reader = loaded_tail.getInput(0).getInput(0)
loaded_reader.getParam("filename").setValue(reload_source.as_posix())
linear_boundary(loaded_reader)
loaded_destination = output_dir / "再読込.png"
loaded_destination.unlink(missing_ok=True)
loaded_writer = loaded.createWriter(loaded_destination.as_posix())
assert loaded_writer and loaded_writer.connectInput(0, loaded_tail)
linear_boundary(loaded_writer)
loaded_before = plugin.OfxVulkanSmokeNativeRenderCount()
before_transfers = transfer_counts()
loaded.render(loaded_writer, 2, 2)
delta = [a-b for a,b in zip(transfer_counts(), before_transfers)]
assert delta == [1, 1] and plugin.OfxVulkanSmokeNativeRenderCount() >= loaded_before + 2, f"Reloaded graph boundary/dispatch failure: {delta}, native renders={plugin.OfxVulkanSmokeNativeRenderCount()-loaded_before}"
assert QImage(str(loaded_destination)) == QImage(destinations[-1]), "Reloaded native graph changed pixels"
graph_transfers.append(delta)
count = plugin.OfxVulkanSmokeRenderCount()
native_count = plugin.OfxVulkanSmokeNativeRenderCount()
native_transfers = plugin.OfxVulkanSmokeNativePixelTransfers()
assert native_count >= 6 and native_transfers == 0, f"Native clip chain did not run without CPU transfers: {native_count}, {native_transfers}"
assert count >= 2, f"Actual OFX Vulkan rendering did not run twice: {count}"
first_image, second_image = (QImage(path) for path in destinations)
assert first_image.pixelColor(width // 2, height // 2) != second_image.pixelColor(width // 2, height // 2), "Changed source fixture did not reach the output"
info = DeviceInfo()
assert plugin.OfxVulkanSmokeDeviceInfo(ctypes.byref(info)) == 1
assert info.validation_enabled == 1 and info.validation_errors == 0
error = plugin.OfxVulkanSmokeMaximumError()
assert error <= 0.00001
generator_module = ctypes.CDLL(str(root / 'build/ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/Win64/VulkanMemoryFailureSmoke.ofx'))
generator_module.OfxMemoryCounter.argtypes = [ctypes.c_int]
generator_module.OfxMemoryCounter.restype = ctypes.c_ulonglong
generator = app.createNode('org.natron.test.VulkanMemorySuccess')
assert generator
generator_output = output_dir / 'native-generator.png'
generator_output.unlink(missing_ok=True)
generator_writer = app.createWriter(generator_output.as_posix())
assert generator_writer and generator_writer.connectInput(0, generator)
linear_boundary(generator_writer)
app.render(generator_writer, 1, 1)
generated = QImage(str(generator_output)).convertToFormat(QImage.Format_RGBA8888)
assert not generated.isNull() and (generated.width(), generated.height()) == (width, height)
assert bytes(generated.constBits()) == bytes((0, 255, 0, 255)) * (width*height)
assert generator_module.OfxMemoryCounter(10) > 0 and generator_module.OfxMemoryCounter(9) == 0
report = {
    "ok": True,
    "device": info.name.decode("utf-8"),
    "gpu_index": os.environ.get("NATRON_VULKAN_DEVICE"),
    "vendor_id": info.vendor_id,
    "native_c_abi": True,
    "native_generator_pixels": width*height,
    "native_generator_renders": generator_module.OfxMemoryCounter(10),
    "native_clip_transport": True,
    "native_chain_length": 2,
    "native_renders": native_count,
    "native_pixel_transfers": native_transfers,
    "graph_boundary_transfers": graph_transfers,
    "boundary_color_management": "linear_no_op",
    "native_cache_reuse": True,
    "cpu_boundary_reference": True,
    "max_byte_error": maximum_byte_error,
    "validation_errors": info.validation_errors,
    "validation_enabled": bool(info.validation_enabled),
    "ofx_renders": count,
    "max_absolute_error": error,
    "self_test": {"context_isolation": True, "expired_handles": True, "signed_padded_rows": True, "host_workers": 4,
                  "shader_compile_diagnostics": True, "shader_alias_roi_pixels": True, "shader_expired_handles": True},
    "project_roundtrip": True,
    "render_size": [width, height],
    "outputs": destinations,
    "reference_outputs": reference_destinations,
    "cache_outputs": cache_destinations,
    "loaded_output": str(loaded_destination),
    "inputs": [str(source), str(second_source)],
    "artifact_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in [source, second_source, reload_source, *(Path(d) for d in destinations + reference_destinations + cache_destinations), loaded_destination, project, generator_output]},
    "profile": os.environ.get("NATRON_TEST_PROFILE"),
}
(output_dir / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("ENGINE_VULKAN_SMOKE_OK " + json.dumps(report, ensure_ascii=False))
