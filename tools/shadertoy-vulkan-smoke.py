"""Render shipped Shadertoy through Vulkan in NatronRenderer's interpreter."""
import ctypes
import hashlib
import json
import os
from pathlib import Path

import NatronEngine
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QColor, QImage

root = Path.cwd()
output = root / "build/shadertoy-vulkan-smoke"
output.mkdir(parents=True, exist_ok=True)
if marker := os.environ.get("NATRON_TEST_START_MARKER"):
    Path(marker).write_text("Shadertoy Vulkan smoke started", encoding="utf-8")
assert "net.sf.openfx.Shadertoy" in NatronEngine.natron.getPluginIDs()
module_path = root / "build/Plugins/Misc.ofx.bundle/Contents/Win64/Misc.ofx"
module = ctypes.CDLL(str(module_path))
module.OfxShadertoyVulkanRenderCount.restype = ctypes.c_uint64
module.OfxShadertoyVulkanClipRenderCount.restype = ctypes.c_uint64
module.OfxShadertoyVulkanTransferCounts.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
module.OfxShadertoyVulkanTransferCounts.restype = ctypes.c_int


def transfer_counts():
    uploads, downloads = ctypes.c_uint64(), ctypes.c_uint64()
    assert module.OfxShadertoyVulkanTransferCounts(ctypes.byref(uploads), ctypes.byref(downloads)) == 0
    return uploads.value, downloads.value


def linear_boundary(effect):
    for name in ("ocioInputSpace", "ocioOutputSpace"):
        param = effect.getParam(name)
        if param is not None:
            param.setValue("Linear")


class DeviceInfo(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 256)] + [(name, ctypes.c_uint32) for name in (
        "vendor_id", "device_id", "api_version", "validation_enabled", "validation_errors")]


module.OfxShadertoyVulkanDeviceInfo.argtypes = [ctypes.POINTER(DeviceInfo)]
module.OfxShadertoyVulkanDeviceInfo.restype = ctypes.c_int
width, height = 137, 91
app.addFormat(f"ShadertoyVulkanSmoke {width}x{height} 1")
app.getProjectParam("outputFormat").setValue("ShadertoyVulkanSmoke")
app.getProjectParam("frameRange").set(1, 2)
node = app.createNode("net.sf.openfx.Shadertoy")
assert node is not None
cases, artifacts = [], []


def render(name, source, expected, frame=1, current=None, owner=None, auto_params=False, expected_uploads=0):
    current = current or node
    render_app = owner or app
    current.getParam("imageShaderSource").setValue(source)
    current.getParam("autoParams" if auto_params else "imageShaderCompile").trigger()
    destination = output / f"{name}.png"
    destination.unlink(missing_ok=True)
    writer = render_app.createWriter(destination.as_posix())
    assert writer and writer.connectInput(0, current)
    linear_boundary(writer)
    before = module.OfxShadertoyVulkanRenderCount()
    clips_before = module.OfxShadertoyVulkanClipRenderCount()
    transfers_before = transfer_counts()
    QCoreApplication.processEvents()
    render_app.render(writer, frame, frame)
    after = module.OfxShadertoyVulkanRenderCount()
    assert after > before, f"{name}: shipped Vulkan render path did not execute"
    clips = module.OfxShadertoyVulkanClipRenderCount() - clips_before
    transfers = [after - previous for after, previous in zip(transfer_counts(), transfers_before)]
    assert clips == after - before, (name, "output staged through CPU", clips, after - before)
    assert transfers == [expected_uploads, 1], (name, "Reader/Writer boundaries", transfers, expected_uploads)
    image = QImage(str(destination))
    assert not image.isNull() and (image.width(), image.height()) == (width, height), name
    for y in range(height):
        for x in range(width):
            rgba = image.pixelColor(x, y).getRgb()
            reference = expected(x, height - 1 - y)
            assert all(abs(actual - target) <= 2 for actual, target in zip(rgba, reference)), (name, x, y, rgba, reference)
    info = DeviceInfo()
    assert module.OfxShadertoyVulkanDeviceInfo(ctypes.byref(info)) == 1
    assert info.validation_enabled == 1 and info.validation_errors == 0, name
    cases.append({"name": name, "frame": frame, "pixels_compared": width * height,
                  "native_renders": after - before, "native_clip_renders": clips,
                  "boundary_transfers": transfers, "output": str(destination)})
    artifacts.append(destination)


builtins = """
void mainImage(out vec4 color,in vec2 p){
 bool derivatives=abs(dFdx(p.x)-1.)<.00001 && abs(dFdy(p.y)-1.)<.00001 && abs(fwidth(p.x*2.+p.y*3.)-5.)<.00001;
 bool values=all(equal(iResolution,vec3(137.,91.,1.))) && abs(iTime-float(iFrame)*iTimeDelta)<.00001
   && iTime==iGlobalTime && iSampleRate==44100. && all(equal(iRenderScale,vec2(1.))) && gl_FragCoord.z==.5 && gl_FragCoord.w==1.;
 color=vec4(float((int(p.x)+int(p.y))%2),float(derivatives),float(values),1.);
}
"""
for frame in (1, 2):
    # A source edit forces actual execution independently of frame cache reuse.
    render(f"builtin-frame-{frame}", builtins + f"\n// frame {frame}",
           lambda x, y: (255 * ((x + y) % 2), 255, 255, 255), frame)

types = ((1, "flag", "Bool", True), (2, "count", "Int", -7), (3, "amount", "Float", .25),
         (4, "point", "Vec2", (2., 4.)), (5, "rgb", "Vec3", (.25, .5, .75)),
         (6, "rgba", "Vec4", (.125, .25, .5, 1.)))
node.getParam("paramCount").setValue(len(types))
for i, (kind, name, suffix, value) in enumerate(types):
    node.getParam(f"paramType{i}").setValue(kind)
    node.getParam(f"paramName{i}").setValue(name)
    parameter = node.getParam(f"paramValue{suffix}{i}")
    if isinstance(value, tuple):
        for dimension, component in enumerate(value):
            parameter.setValue(component, dimension)
    else:
        parameter.setValue(value)
uniform_source = """
uniform bool flag; uniform int count; uniform float amount;
uniform vec2 point; uniform vec3 rgb; uniform vec4 rgba;
void mainImage(out vec4 color,in vec2 p){
 bool ok=flag && count==-7 && amount==.25 && all(equal(point,vec2(2.,4.)))
   && all(equal(rgb,vec3(.25,.5,.75))) && all(equal(rgba,vec4(.125,.25,.5,1.)));
 color=vec4(float(ok),float(ok),float(ok),1.);
}
"""
render("uniform-types", uniform_source, lambda x, y: (255, 255, 255, 255))
auto_source = """
uniform float exposure; // Exposure (default=.75, min=0, max=2)
uniform float unused;
uniform sampler2D iChannel3;
void mainImage(out vec4 color,in vec2 p){color=vec4(float(exposure>=0.),float(exposure>=0.),float(exposure>=0.),1.);}
"""
render("auto-params", auto_source, lambda x, y: (255, 255, 255, 255), auto_params=True)
assert node.getParam("paramCount").getValue() == 1
assert node.getParam("paramName0").getValue() == "exposure"
assert node.getParam("paramType0").getValue() == 3
assert not node.getParam("inputEnable3").getValue()
node.getParam("paramValueFloat0").setValue(.75)
render("auto-param-value", auto_source.replace("exposure>=0.", "exposure==.75"),
       lambda x, y: (255, 255, 255, 255))

for i, color in enumerate((QColor(255, 0, 0), QColor(0, 255, 0), QColor(0, 0, 255), QColor(255, 255, 255))):
    fixture = output / f"入力-{i}.png"
    image = QImage(width, height, QImage.Format_RGBA8888)
    image.fill(color)
    assert image.save(str(fixture))
    reader = app.createReader(fixture.as_posix())
    reader.getParam("enableGPU").setValue(False)
    assert reader and node.connectInput(i, reader)
    linear_boundary(reader)
    reader.getParam("outputComponents").setValue("RGBA")
    node.getParam(f"inputEnable{i}").setValue(True)
    artifacts.append(fixture)
texture_source = """
precision highp float;
const vec2 iRenderScale=vec2(1.);
const vec2 iChannelOffset[4]=vec2[4](vec2(0.),vec2(0.),vec2(0.),vec2(0.));
void mainImage(out vec4 color,in vec2 p){
 vec2 uv=p/iResolution.xy;
 vec4 a=texture2D(iChannel0,uv),b=texture(iChannel1,uv),c=texture2D(iChannel2,uv),d=texture(iChannel3,uv);
 bool ok=all(lessThan(abs(a-vec4(1.,0.,0.,1.)),vec4(.00001))) && all(lessThan(abs(b-vec4(0.,1.,0.,1.)),vec4(.00001)))
   && all(lessThan(abs(c-vec4(0.,0.,1.,1.)),vec4(.00001))) && all(lessThan(abs(d-vec4(1.)),vec4(.00001)))
   && all(equal(iChannelResolution[3],iResolution)) && iChannelTime[2]==iTime;
 color=vec4(float(ok),float(ok),float(ok),1.);
}
"""
for filtering in range(4):
    for wrapping in range(3):
        for i in range(4):
            node.getParam(f"mipmap{i}").setValue(filtering)
            node.getParam(f"wrap{i}").setValue(wrapping)
        render(f"textures-filter{filtering}-wrap{wrapping}", texture_source + f"\n// case {filtering}:{wrapping}",
               lambda x, y: (255, 255, 255, 255), expected_uploads=4)

preset_path = root / "build/Plugins/Misc.ofx.bundle/Contents/Resources/presets/default/gaussianblur.frag.glsl"
preset_source = preset_path.read_text(encoding="utf-8")
render("shipped-gaussian-blur", preset_source, lambda x, y: (255, 0, 0, 255), auto_params=True, expected_uploads=4)
project = output / "Vulkanシェーダー.ntp"
assert app.saveProjectAs(project.as_posix())
loaded = app.loadProject(project.as_posix())
assert loaded is not None
loaded_node = next(n for n in loaded.getChildren() if n.getPluginID() == "net.sf.openfx.Shadertoy")
render("loaded-project", preset_source + "\n// loaded project", lambda x, y: (255, 0, 0, 255), current=loaded_node, owner=loaded, expected_uploads=4)
artifacts.append(project)
info = DeviceInfo()
assert module.OfxShadertoyVulkanDeviceInfo(ctypes.byref(info)) == 1
report = {"ok": True, "device": info.name.decode("utf-8"), "gpu_index": os.environ.get("NATRON_VULKAN_DEVICE"),
          "validation_errors": info.validation_errors, "validation_enabled": bool(info.validation_enabled),
          "shipped_plugin": "net.sf.openfx.Shadertoy", "native_renders": module.OfxShadertoyVulkanRenderCount(),
          "native_clip_renders": module.OfxShadertoyVulkanClipRenderCount(),
          "cases": cases, "project_roundtrip": True, "profile": os.environ.get("NATRON_TEST_PROFILE"),
          "artifact_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}}
import runpy
modules = runpy.run_path('tools/windows-module-audit.py')['loaded_modules']()
report['runtime_modules'] = modules
report['opengl_loaded'] = any(Path(name).name.lower() == 'opengl32.dll' for name in modules)
report['ok'] = report['ok'] and not report['opengl_loaded']
(output / "result.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print("SHADERTOY_VULKAN_SMOKE_OK " + json.dumps(report, ensure_ascii=False))
