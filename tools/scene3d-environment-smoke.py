"""Run in NatronRenderer: native image-based lighting, USD domes and project IO."""
import json
import os
import re
import subprocess
import traceback
from pathlib import Path
from PySide6.QtGui import QImage,QColor

cpu_only=os.environ.get('NATRON_VULKAN_DISABLED')=='1'
directory=Path.cwd()/'build'/('scene3d-environment-smoke-cpu' if cpu_only else 'scene3d-environment-smoke-gpu')
directory.mkdir(parents=True,exist_ok=True)
report={'ok':False}
oiio=os.environ.get('OIIO_TOOL') or str(Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve())

def node(kind,name):
    effect=app.createNode('fr.inria.built-in.'+kind);assert effect
    effect.setScriptName(name);return effect

def replace(effect,index,source):
    effect.disconnectInput(index);assert effect.connectInput(index,source)

def render(name,frame=1,application=None):
    application=application or app
    path=directory/(name+'.png');path.unlink(missing_ok=True)
    writer=application.createWriter(path.as_posix());assert writer.connectInput(0,application.getNode('EnvironmentRender3D'))
    application.render(writer,frame,frame)
    image=QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and image.width()==96 and image.height()==64,name
    return bytes(image.constBits())

def exr(name):
    path=directory/(name+'.exr');path.unlink(missing_ok=True)
    writer=app.createNode('fr.inria.openfx.WriteOIIO');assert writer.connectInput(0,renderer)
    writer.getParam('filename').setValue(path.as_posix());writer.getParam('bitDepth').setValue('32f')
    writer.getParam('processAllPlanes').setValue(True);writer.getParam('partSplitting').setValue('single')
    writer.getParam('ocioOutputSpace').setValue('linear');app.render(writer,1,1);assert path.exists();return path

try:
    app.addFormat('EnvironmentSmoke 96x64 1');app.getProjectParam('outputFormat').setValue('EnvironmentSmoke')
    app.getProjectParam('frameRange').set(1,25)
    material=node('PBRMaterial','EnvironmentMaterial');material.getParam('baseColor').set(.6,.3,.15,1.)
    material.getParam('metallic').setValue(1.);material.getParam('roughness').setValue(.1)
    card=node('Card','EnvironmentCard');card.getParam('size').set(6.,6.);assert card.connectInput(0,material)
    light=node('Light','EnvironmentLight');light.getParam('lightType').setValue('Environment');light.getParam('shadows').setValue(False)
    scene=node('Scene','EnvironmentScene');assert scene.connectInput(0,card) and scene.connectInput(1,light)
    camera=node('Camera','EnvironmentCamera');camera.getParam('orthographic').setValue(True)
    renderer=node('Render3D','EnvironmentRender3D');assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
    renderer.getParam('renderer').setValue('CPU' if cpu_only else 'Vulkan')
    assert not light.connectInput(1,material) and not light.connectInput(0,camera)
    uniform=render('uniform');assert max(uniform[0::4])>100 and set(uniform[3::4])=={255}
    source=app.createNode('net.sf.openfx.ConstantPlugin');source.setScriptName('EnvironmentImage')
    source.getParam('color').set(1.,1.,1.,0.);assert light.connectInput(1,source)
    assert render('alpha-zero')==uniform,'Environment image alpha changed illumination'
    source.getParam('color').set(1.,1.,1.,1.);assert render('alpha-one')==uniform
    source.getParam('color').set(4.,.5,.25,0.)
    constant=exr('hdr-constant')
    hdr=directory/'radiance.exr'
    subprocess.run([oiio,'--pattern','constant:color=4,0.5,0.25,0','16x8','4','-d','float','-o',str(hdr)],check=True,capture_output=True)
    reader=app.createReader(hdr.as_posix());reader.setScriptName('EnvironmentHDR')
    reader.getParam('ocioInputSpace').setValue('linear');reader.getParam('ocioOutputSpace').setValue('linear')
    reader.getParam('filePremult').setValue(2);reader.getParam('outputPremult').setValue(2)
    replace(light,1,reader)
    # Warm up the Reader before restoring the project format it first adopts.
    warmup=app.createWriter((directory/'warmup.png').as_posix());assert warmup.connectInput(0,renderer);app.render(warmup,1,1)
    app.getProjectParam('outputFormat').setValue('EnvironmentSmoke');from_file=exr('hdr-reader')
    result=subprocess.run([oiio,'-v',str(constant),str(from_file),'--diff'],capture_output=True,text=True)
    match=re.search(r'Max error\s*=\s*([-+0-9.eE]+)',result.stdout+result.stderr);assert match,result.stdout+result.stderr
    assert float(match.group(1))<.0001,'HDR Reader radiance differed from the image graph'
    report['hdr_reader']=True
    image=QImage(16,8,QImage.Format_RGBA8888)
    for y in range(8):
        for x in range(16):image.setPixelColor(x,y,QColor(16*x,255-16*x,32*y,0))
    image_path=directory/'latlong-source.png';assert image.save(str(image_path))
    png=app.createReader(image_path.as_posix());png.setScriptName('EnvironmentLatLong')
    for name in ('ocioInputSpace','ocioOutputSpace'):png.getParam(name).setValue('linear')
    png.getParam('filePremult').setValue(2);png.getParam('outputPremult').setValue(2);replace(light,1,png)
    mapped=render('latlong');light.getParam('rotate').set(0.,90.,0.);rotated=render('rotated');assert rotated!=mapped
    light.getParam('translate').set(30.,-10.,50.);light.getParam('scale').set(.01,8.,2.)
    assert render('position-scale')==rotated
    axis=node('Axis','EnvironmentAxis');assert light.connectInput(0,axis);axis.getParam('rotate').set(0.,90.,0.)
    assert render('axis')!=rotated;axis.getParam('rotate').set(0.,0.,0.)
    material.getParam('roughness').setValue(.8);rough=render('roughness');assert rough!=rotated
    material.getParam('metallic').setValue(0.);assert render('diffuse')!=rough
    material.getParam('metallic').setValue(1.)
    for count in ('16','32','64'):light.getParam('environmentSamples').setValue(count);render('samples-'+count)
    # Nested scenes must carry image bindings with the correct light index.
    extra=node('Light','DisabledLight');extra.getParam('intensity').setValue(0.)
    nested=node('Scene','NestedEnvironment');assert nested.connectInput(0,extra) and nested.connectInput(1,scene)
    transformed=node('TransformGeo','EnvironmentTransform');assert transformed.connectInput(0,nested)
    reference=render('direct');replace(renderer,0,transformed);assert render('nested')==reference
    # Import an animated dome into the same native geometry scene.
    usd_text='''#usda 1.0
( metersPerUnit = 1
  startTimeCode = 1
  endTimeCode = 25
  timeCodesPerSecond = 24
)
def DomeLight "Dome" {
 asset inputs:texture:file = @latlong-source.png@ ( colorSpace = "raw" )
 token inputs:texture:format = "latlong"
 float xformOp:rotateY.timeSamples = { 1: 0, 25: 90 }
 uniform token[] xformOpOrder = ["xformOp:rotateY"]
}
'''
    usd_path=directory/'dome.usda';usd_path.write_text(usd_text,encoding='utf-8')
    usd=node('ReadUSD','ImportedDome');usd.getParam('file').setValue(usd_path.as_posix())
    scene.disconnectInput(1);assert scene.connectInput(1,usd)
    assert render('usd-dome-1',1)!=render('usd-dome-25',25)
    scene.disconnectInput(1);assert scene.connectInput(1,light)
    # Image nodes keep following output time, independently of light transforms.
    replace(light,1,source);source.getParam('color').set(1.,.5,.2,1.)
    source.getParam('color').setValueAtTime(.5,1,0);source.getParam('color').setValueAtTime(2.,25,0)
    assert render('image-time-1',1)!=render('image-time-25',25);replace(light,1,png)
    if not cpu_only:
        gpu=exr('gpu');renderer.getParam('renderer').setValue('CPU');cpu=exr('cpu')
        result=subprocess.run([oiio,'-v',str(cpu),str(gpu),'--diff'],capture_output=True,text=True)
        match=re.search(r'Max error\s*=\s*([-+0-9.eE]+)',result.stdout+result.stderr);assert match,result.stdout+result.stderr
        error=float(match.group(1));assert error<.001,error;report['cpu_vulkan_max_error']=error
        renderer.getParam('renderer').setValue('Vulkan')
    before=render('before-save');saved=directory/'environment.ntp';assert app.saveProjectAs(saved.as_posix())
    application=app.loadProject(saved.as_posix());assert application
    restored=application.getNode('EnvironmentLight');assert restored.getParam('lightType').getValue()==3
    assert restored.getInput(0).getScriptName()=='EnvironmentAxis' and restored.getInput(1).getScriptName()=='EnvironmentLatLong'
    assert restored.getParam('environmentSamples').getValue()==2 and render('after-load',application=application)==before
    report.update(ok=True,latlong=True,alpha=True,transforms=True,nested_scene=True,roughness_metallic=True,usd_dome=True,image_animation=True,project_roundtrip=True)
except Exception:
    report['exception']=traceback.format_exc()
finally:
    (directory/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
assert report['ok'],report.get('exception')
