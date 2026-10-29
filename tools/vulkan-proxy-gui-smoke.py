"""Compare native proxy renders with CPU clips in the real Vulkan Viewer."""
import ctypes
import hashlib
import json
import math
import os
import time
import traceback
from pathlib import Path

import NatronEngine
from PySide6.QtCore import QMetaObject, Q_RETURN_ARG, QTimer, Qt
from PySide6.QtGui import QColor, QImage
from PySide6.QtWidgets import QApplication

root = Path.cwd()
output = root / "build/vulkan-proxy-gui-smoke"
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get("NATRON_TEST_START_MARKER"):
    Path(marker).write_text("Vulkan proxy test started", encoding="utf-8")
report = {"ok": False, "cases": [], "profile": os.environ.get("NATRON_TEST_PROFILE")}
report["texture_depth_choice"] = NatronEngine.natron.getSettings().getParam("texturesBitDepth").get()
retained = {}
artifacts = []


def module(path, names):
    value = ctypes.CDLL(str(path))
    for name in names:
        getattr(value, name).restype = ctypes.c_uint64
    return value


test = module(root / "build/ofx-tests/VulkanImageSuiteSmoke.ofx.bundle/Contents/Win64/VulkanImageSuiteSmoke.ofx",
              ("OfxVulkanSmokeRenderCount", "OfxVulkanSmokeNativeRenderCount", "OfxVulkanSmokeNativePixelTransfers",
               "OfxVulkanSmokeNativeScaleRenderCount"))
test.OfxVulkanSmokeNativeScaleRenderCount.argtypes = [ctypes.c_uint]
test.OfxVulkanSmokeTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
test.OfxVulkanSmokeTransferCounts.restype = ctypes.c_int


def transfers():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert test.OfxVulkanSmokeTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 1
    return uploads.value, downloads.value


io = module(root / "build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx",
            ("OfxOCIOVulkanRenderCount", "OfxOCIOVulkanClipRenderCount"))
misc = module(root / "build/Plugins/Misc.ofx.bundle/Contents/Win64/Misc.ofx",
              ("OfxShadertoyVulkanRenderCount", "OfxShadertoyVulkanClipRenderCount"))
width, height = 137, 91
app.addFormat(f"VulkanProxySmoke {width}x{height} 1")
app.getProjectParam("outputFormat").setValue("VulkanProxySmoke")
app.getProjectParam("frameRange").set(1, 1)
app.getProjectParam("autoPreviews").setValue(False)
source = output / "入力.png"
image = QImage(width, height, QImage.Format_RGBA8888)
for y in range(height):
    for x in range(width):
        image.setPixelColor(x, y, QColor(40 + x % 180, 80 + y % 150, 160 - x % 120, 64 + (x + y) % 160))
assert image.save(str(source))
artifacts.append(source)
reader = app.createReader(source.as_posix())
reader.getParam("enableGPU").setValue(False)
assert reader
reader.getParam("outputComponents").setValue("RGBA")
for name in ("ocioInputSpace", "ocioOutputSpace"):
    reader.getParam(name).setValue("Linear")


def chain(identifier, native, control, *, high_def=False, masked=False):
    previous = reader
    for index in range(2):
        node = app.createNode(identifier)
        assert node and node.connectInput(0, previous)
        node.getParam(control).setValue(native)
        node.getParam("forceCaching").setValue(True)
        if "VulkanFullScale" in identifier:
            node.getParam("highDefUpstream").setValue(high_def)
        if masked:
            assert node.connectInput(1, reader)
            node.getParam("hostMix").setValue(.55)
            node.getParam("NatronOfxParamProcessG").setValue(False)
        if identifier.endswith("OCIOCDLTransform"):
            for name, values in (("slope", (1.1, .9, 1.2)), ("offset", (.02, .01, .03)), ("power", (1.05, .9, 1.1))):
                for channel, value in enumerate(values):
                    node.getParam(name).setValue(value + index * .005, channel)
            node.getParam("saturation").setValue(.85)
        previous = node
    return previous


graphs = [
    {"name": "native-chain", "native": chain("org.natron.test.VulkanImageSuite", True, "nativeEnabled"),
     "reference": chain("org.natron.test.VulkanImageSuite", False, "nativeEnabled"),
     "count": test.OfxVulkanSmokeNativeRenderCount, "total": test.OfxVulkanSmokeRenderCount},
    {"name": "ocio-chain", "native": chain("fr.inria.openfx.OCIOCDLTransform", True, "enableGPU"),
     "reference": chain("fr.inria.openfx.OCIOCDLTransform", False, "enableGPU"),
     "count": io.OfxOCIOVulkanClipRenderCount, "total": io.OfxOCIOVulkanRenderCount},
]
full_scale_graphs = []
for name, high_def, masked in (("fullscale-proxy-inputs", False, False),
                               ("fullscale-highdef-inputs", True, False),
                               ("fullscale-mask-mix", False, True)):
    identifier = "org.natron.test.VulkanFullScaleMaskSuite" if masked else "org.natron.test.VulkanFullScaleSuite"
    full_scale_graphs.append({"name": name, "native": chain(identifier, True, "nativeEnabled", high_def=high_def, masked=masked),
                             "reference": chain(identifier, False, "nativeEnabled", high_def=high_def, masked=masked),
                             "count": test.OfxVulkanSmokeNativeRenderCount, "total": test.OfxVulkanSmokeRenderCount,
                             "full_scale": True, "masked": masked})
shader = app.createNode("net.sf.openfx.Shadertoy")
assert shader
for index in range(4):
    shader.getParam(f"inputEnable{index}").setValue(False)
viewer_node = app.createNode("fr.inria.built-in.Viewer")
assert viewer_node
viewer = app.getViewer(viewer_node.getScriptName())
assert viewer
app.clearSelection()
steps = [(graph, level) for graph in graphs for level in range(6)]
steps += [({"name": "shader-scale", "native": shader, "count": misc.OfxShadertoyVulkanClipRenderCount,
            "total": misc.OfxShadertoyVulkanRenderCount}, level) for level in range(6)]
steps += [(dict(graph, name="cached-" + graph["name"], cached_mipmap=True), level)
          for graph in graphs for level in range(6)]
steps += [(graph, level) for graph in full_scale_graphs for level in range(6)]
steps += [(dict(full_scale_graphs[0], name="cached-fullscale-proxy-inputs", cached_quality=True), level) for level in range(6)]
steps += [(dict(full_scale_graphs[1], name="cached-fullscale-highdef-inputs", cached_mipmap=True), level) for level in range(6)]
current = {}
position = 0


def widget():
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    viewers = [value for value in values if value.property("graphicsBackend") == "Vulkan" and value.isVisible()]
    assert len(viewers) == 1, f"Expected one Viewer: {len(viewers)}"
    return viewers[0]


def capture():
    frame = QMetaObject.invokeMethod(widget(), "grabFrame", Qt.DirectConnection, Q_RETURN_ARG(QImage))
    assert not frame.isNull()
    return frame


def samples(frame):
    viewport = widget().property("canonicalViewport")
    result = []
    # Stay away from format outlines and proxy padding at the image edges.
    for row in range(17):
        for column in range(25):
            x = width * (.1 + .8 * column / 24)
            y = height * (.1 + .8 * row / 16)
            px = round((x - viewport.x()) / viewport.width() * frame.width())
            py = round((viewport.y() + viewport.height() - y) / viewport.height() * frame.height())
            assert 0 <= px < frame.width() and 0 <= py < frame.height()
            result.append(frame.pixelColor(px, py).getRgb())
    return result


def finish(error=None):
    if error:
        report["error"] = str(error)
        report["traceback"] = traceback.format_exc()
        report["failed_stage"] = {key: current.get(key) for key in
                                  ("level", "phase", "native_before", "native_finished", "native_calls")}
        if current.get("graph"):
            report["failed_stage"]["graph"] = current["graph"]["name"]
            report["failed_stage"]["native_now"] = current["graph"]["count"]()
    values = QApplication.allWidgets()
    retained.update((id(value), value) for value in values)
    viewers = [value for value in values if value.property("graphicsBackend") == "Vulkan"]
    report["unexpected_dialogs"] = [value.windowTitle() for value in QApplication.topLevelWidgets()
                                    if value.isVisible() and value.isModal()]
    report["graphics_errors"] = [value.property("graphicsError") for value in viewers]
    report["validation_errors"] = [value.property("validationErrors") for value in viewers]
    report["devices"] = [value.property("gpuDeviceName") for value in viewers]
    report["native_scale_renders"] = [test.OfxVulkanSmokeNativeScaleRenderCount(level) for level in range(6)]
    report["native_pixel_transfers"] = test.OfxVulkanSmokeNativePixelTransfers()
    report["ok"] = (not error and len(report["cases"]) == 60 and all(report["native_scale_renders"])
                    and report["native_pixel_transfers"] == 0 and not report["unexpected_dialogs"]
                    and not any(report["graphics_errors"]) and not any(report["validation_errors"]))
    report["artifact_sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts}
    (output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("VULKAN_PROXY_SMOKE", json.dumps(report))
    QApplication.exit(0)


def guarded(function):
    def call():
        try:
            function()
        except Exception as error:
            finish(error)
    return call


def activity():
    return (*transfers(), test.OfxVulkanSmokeRenderCount(), io.OfxOCIOVulkanRenderCount(),
            misc.OfxShadertoyVulkanRenderCount())


def transition(callback):
    # A completed presentation can precede a queued, abortable render. Drain
    # that previous graph while disconnected before measuring the next graph.
    viewer.pause()
    viewer_node.disconnectInput(0)
    current.update(transition_started=time.monotonic(), quiet_since=time.monotonic(), activity=activity())
    QTimer.singleShot(150, guarded(lambda: settle(callback)))


def settle(callback):
    assert not any(value.isVisible() and value.isModal() for value in QApplication.topLevelWidgets()), "Unexpected modal dialog"
    now, state = time.monotonic(), activity()
    if state != current["activity"]:
        current.update(activity=state, quiet_since=now)
    if now-current["quiet_since"] >= .6:
        report.setdefault("settled_transitions", 0)
        report["settled_transitions"] += 1
        callback()
        return
    assert now-current["transition_started"] < 12, "Previous graph did not become idle"
    QTimer.singleShot(150, guarded(lambda: settle(callback)))


def begin():
    global position
    if position == len(steps):
        finish()
        return
    graph, level = steps[position]
    if position == 0:
        # Keep frame readback and its canonical viewport at the same extent
        # while dock panels settle during this long scripted comparison.
        widget().setFixedSize(1400, 600)
    current.clear()
    current.update(graph=graph, level=level, phase="transition")
    transition(configure)


def configure():
    graph, level = current["graph"], current["level"]
    # Each scale gets a new Reader hash. CPU references must render the requested
    # scale instead of downsampling the output cached at a finer scale.
    if not (graph.get("cached_mipmap") or graph.get("cached_quality")) or level == 0:
        fixture = output / f"入力-{position}.png"
        fixture.write_bytes(source.read_bytes())
        artifacts.append(fixture)
        reader.getParam("filename").setValue(fixture.as_posix())
    viewer.setProxyModeEnabled(level > 0)
    if level:
        viewer.setProxyIndex(level - 1)
    if graph["name"] == "shader-scale":
        scale = 2. ** -level
        shader.getParam("imageShaderSource").setValue(f"""
void mainImage(out vec4 color,in vec2 p) {{
 bool valid=all(equal(iRenderScale,vec2({scale})))
   && all(equal(iResolution,vec3({math.ceil(width * scale)}.,{math.ceil(height * scale)}.,1.)));
 color=vec4(vec3(float(valid)),1.);
}}
""")
        shader.getParam("imageShaderCompile").trigger()
    # Reader metadata and proxy changes can queue additional GUI renders. Keep
    # the Viewer disconnected until those changes settle, then measure all
    # uploads/downloads for the new graph without weakening their assertions.
    QTimer.singleShot(600, guarded(start_native))


def start_native():
    graph = current["graph"]
    current.update(phase="native", native_before=graph["count"](), total_before=graph["total"](),
                   stable=0, transfers_before=transfers(),
                   native_copies_before=widget().property("nativeImageTransfers"),
                   fullscale_before=test.OfxVulkanSmokeNativeScaleRenderCount(0))
    assert viewer_node.connectInput(0, graph["native"])
    render()


def start_reference():
    assert viewer_node.connectInput(0, current["graph"]["reference"])
    current["phase"] = "reference"
    render()


def start_cache():
    current.update(phase="cache", cache_transfers_before=transfers(),
                   cache_copies_before=widget().property("nativeImageTransfers"))
    assert viewer_node.connectInput(0, current["graph"]["native"])
    render()


def render():
    current.update(deadline=time.monotonic() + 12, frames_before=widget().property("presentedFrames"),
                   stable=0, digest=None)
    # Input/proxy changes also schedule abortable GUI renders. Let those settle,
    # then request the current state from cache and wait for its presentation.
    # A frame emitted by the temporary disconnected Viewer is not test output.
    QTimer.singleShot(500, guarded(present_current))


def present_current():
    current.update(frames_before=widget().property("presentedFrames"), stable=0, digest=None)
    viewer.renderCurrentFrame(True)
    QTimer.singleShot(150, guarded(wait))


def wait():
    global position
    w = widget()
    assert not any(value.isVisible() and value.isModal() for value in QApplication.topLevelWidgets()), "Unexpected modal dialog"
    assert not w.property("graphicsError") and not w.property("validationErrors"), "Vulkan Viewer failure"
    graph, level, phase = current["graph"], current["level"], current["phase"]
    ready = w.property("presentedFrames") > current["frames_before"]
    if phase == "native":
        if not (graph.get("cached_mipmap") and level > 0):
            ready = ready and graph["count"]() > current["native_before"]
    if ready:
        frame = capture()
        values = samples(frame)
        digest = hashlib.sha256(bytes(channel for pixel in values for channel in pixel)).hexdigest()
        current["stable"] = current["stable"] + 1 if digest == current["digest"] else 0
        current["digest"] = digest
        if current["stable"] >= 2:
            path = output / f"{position}-{phase}.png"
            assert frame.save(str(path))
            artifacts.append(path)
            if phase == "native":
                calls = graph["count"]() - current["native_before"]
                assert calls == graph["total"]() - current["total_before"], "Native render staged through CPU clips"
                assert w.property("nativeImageTransfers") > current["native_copies_before"], "Native image did not reach the GPU Viewer"
                if graph.get("cached_mipmap") and level > 0:
                    assert calls == 0, "Cached higher-resolution image was rendered again"
                    before, after = current["transfers_before"], transfers()
                    assert after[0] == before[0], "Cached native mipmap uploaded CPU pixels"
                    assert after[1] == before[1], "Cached native mipmap downloaded CPU pixels"
                    current["mipmap_transfers"] = [after[index] - before[index] for index in range(2)]
                current.update(native_samples=values, native_calls=calls, native_finished=graph["count"](),
                               native_copies=w.property("nativeImageTransfers"))
                if graph.get("full_scale"):
                    assert test.OfxVulkanSmokeNativeScaleRenderCount(0) - current["fullscale_before"] == calls
                    before, after = current["transfers_before"], transfers()
                    current["fullscale_transfers"] = [after[index] - before[index] for index in range(2)]
                    expected_uploads = 0 if graph.get("cached_mipmap") and level else 3 if graph["masked"] else 1
                    assert current["fullscale_transfers"] == [expected_uploads, 0], current["fullscale_transfers"]
                if graph["name"] == "shader-scale":
                    assert all(min(pixel[:3]) >= 253 for pixel in values), "Scaled Shadertoy uniforms differ from the render grid"
                    report["cases"].append({"graph": graph["name"], "level": level, "native_renders": calls,
                                            "pixels_compared": len(values), "uniforms": True})
                    position += 1
                    QTimer.singleShot(0, guarded(begin))
                else:
                    transition(start_reference)
            elif phase == "reference":
                assert graph["count"]() == current["native_finished"], "CPU reference selected native rendering"
                error = max(abs(a-b) for actual, expected in zip(current["native_samples"], values)
                            for a, b in zip(actual, expected))
                assert error <= 2, f"{graph['name']} scale {level} CPU/Viewer difference: {error}"
                current["error"] = error
                transition(start_cache)
            else:
                assert graph["count"]() == current["native_finished"], "Proxy cache rendered a resident native image again"
                assert w.property("nativeImageTransfers") > current["cache_copies_before"], "Cached image did not use native Viewer conversion"
                assert transfers() == current["cache_transfers_before"], "Proxy cache redisplay transferred CPU pixels"
                cache_error = max(abs(a-b) for actual, expected in zip(values, current["native_samples"])
                                  for a, b in zip(actual, expected))
                assert cache_error == 0, "Proxy cache changed displayed pixels"
                report["cases"].append({"graph": graph["name"], "level": level, "native_renders": current["native_calls"],
                                        "cache_renders": 0, "cache_maximum_byte_error": cache_error,
                                        "pixels_compared": len(values), "maximum_byte_error": current["error"],
                                        "mipmap_transfers": current.get("mipmap_transfers"),
                                        "fullscale_transfers": current.get("fullscale_transfers"),
                                        "native_viewer_copy": True, "cache_pixel_transfers": [0, 0],
                                        "fullscale_action": bool(graph.get("full_scale"))})
                position += 1
                QTimer.singleShot(0, guarded(begin))
            return
    assert time.monotonic() < current["deadline"], f"Timed out: {graph['name']} level {level} {phase}"
    QTimer.singleShot(150, guarded(wait))


QTimer.singleShot(750, guarded(begin))
