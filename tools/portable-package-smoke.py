"""Exercise installed CPU OFX, native-only rendering and dependency isolation."""
import ctypes
import json
import os
from pathlib import Path
import runpy
import sys
import traceback
import NatronEngine
import qtpy
from PySide6.QtGui import QImage

output = Path(os.environ['NATRON_GUI_TEST_RESULT'])
output.parent.mkdir(parents=True,exist_ok=True)
package = Path(os.environ['NATRON_PORTABLE_ROOT']).resolve()
report = {'ok':False,'profile':os.environ['NATRON_TEST_PROFILE'],'package':str(package)}


def modules():
    kernel = ctypes.WinDLL('kernel32',use_last_error=True)
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    kernel.K32EnumProcessModules.argtypes = [ctypes.c_void_p,ctypes.POINTER(ctypes.c_void_p),ctypes.c_uint32,ctypes.POINTER(ctypes.c_uint32)]
    kernel.GetModuleFileNameW.argtypes = [ctypes.c_void_p,ctypes.c_wchar_p,ctypes.c_uint32]
    handles, size = (ctypes.c_void_p*4096)(),ctypes.c_uint32()
    assert kernel.K32EnumProcessModules(kernel.GetCurrentProcess(),handles,ctypes.sizeof(handles),ctypes.byref(size))
    assert size.value <= ctypes.sizeof(handles)
    result=[]
    for handle in handles[:size.value//ctypes.sizeof(ctypes.c_void_p)]:
        buffer=ctypes.create_unicode_buffer(32768)
        assert kernel.GetModuleFileNameW(handle,buffer,len(buffer))
        result.append(buffer.value)
    return result


try:
    assert Path(sys.prefix).resolve() == package,sys.prefix
    assert ctypes.WinDLL('kernel32').GetACP() == 65001
    assert qtpy.API_NAME == 'PySide6'
    os.environ['NATRON_OFX_SMOKE_OUTPUT'] = str(output.parent/'cpu-ofx')
    cpu = runpy.run_path(os.environ['NATRON_PORTABLE_OFX_TEST'],init_globals={'app':app})['report']
    assert cpu['ok'] and cpu['project_roundtrip'] and len(cpu['renders']) == 7
    report['cpuOfx'] = cpu
    # The required GPU plugin must execute through the shipped opaque suites.
    shader = app.createNode('net.sf.openfx.Shadertoy')
    assert shader
    shader.getParam('imageShaderSource').setValue('void mainImage(out vec4 c,in vec2 p){c=vec4(1.0,0.0,0.0,1.0);}')
    shader.getParam('imageShaderCompile').trigger()
    path=output.parent/'native.png'
    writer=app.createWriter(path.as_posix())
    assert writer and writer.connectInput(0,shader)
    for name in ('ocioInputSpace','ocioOutputSpace'):
        if param := writer.getParam(name):
            param.setValue('Linear')
    app.render(writer,1,1)
    image=QImage(str(path))
    assert not image.isNull() and (image.width(),image.height()) == (64,48)
    assert all(image.pixelColor(x,y).getRgb() == (255,0,0,255) for y in range(48) for x in range(64))
    misc=ctypes.CDLL(str(package/'Plugins/OFX/Natron/Misc.ofx.bundle/Contents/Win64/Misc.ofx'))
    misc.OfxShadertoyVulkanClipRenderCount.restype=ctypes.c_uint64
    assert misc.OfxShadertoyVulkanClipRenderCount() > 0
    report['nativePixelsVerified']=3072
    loaded=modules()
    # Windows system files include the hardware driver store and WSI internals.
    permitted=[package,Path(os.environ['WINDIR']).resolve()]
    if icd := os.environ.get('NATRON_PORTABLE_SOFTWARE_DRIVER'):
        permitted.append(Path(icd).resolve())
    unexpected=[p for p in loaded if not any(Path(p).resolve().is_relative_to(base) for base in permitted)]
    assert not unexpected,unexpected
    assert not any(Path(p).name.casefold() in ('qt6opengl.dll','qt6openglwidgets.dll','libegl.dll','libglesv2.dll','opengl32sw.dll') for p in loaded)
    report.update(ok=True,pythonPrefix=sys.prefix,loadedModules=loaded,unexpectedModules=unexpected,
                  nativeRenderCount=misc.OfxShadertoyVulkanClipRenderCount(),qtBinding=qtpy.API_NAME)
except Exception:
    report['error']=traceback.format_exc()
finally:
    output.write_text(json.dumps(report,indent=2),encoding='utf-8')
    os.write(1,b'NATRON_PORTABLE_SMOKE_EXIT_REQUESTED\n')
assert report['ok'],report.get('error')
