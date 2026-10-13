"""Run in NatronRenderer: USD composition in the shared native 3D graph."""
import json
import os
import re
import subprocess
import traceback
from pathlib import Path
from PySide6.QtGui import QImage

cpu_only=os.environ.get('NATRON_VULKAN_DISABLED')=='1'
directory=Path.cwd()/'build'/('scene3d-usd-smoke-cpu' if cpu_only else 'scene3d-usd-smoke-gpu')
directory.mkdir(parents=True,exist_ok=True)
report={'ok':False}

def node(kind,name):
    effect=app.createNode('fr.inria.built-in.'+kind);assert effect
    effect.setScriptName(name);return effect

def render(name,frame=1,application=None):
    application=application or app
    path=directory/(name+'.png');path.unlink(missing_ok=True)
    writer=application.createWriter(path.as_posix());assert writer.connectInput(0,application.getNode('UsdRender3D'))
    application.render(writer,frame,frame)
    image=QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and image.width()==96 and image.height()==60,name
    return bytes(image.constBits())

def same(a,b):
    return len(a)==len(b) and max(abs(x-y) for x,y in zip(a,b))<=1

def exr(name):
    path=directory/(name+'.exr');path.unlink(missing_ok=True)
    writer=app.createNode('fr.inria.openfx.WriteOIIO');assert writer.connectInput(0,renderer)
    writer.getParam('filename').setValue(path.as_posix());writer.getParam('bitDepth').setValue('32f')
    writer.getParam('processAllPlanes').setValue(True);writer.getParam('partSplitting').setValue('single')
    writer.getParam('ocioOutputSpace').setValue('linear');app.render(writer,13,13);assert path.exists();return path

try:
    app.addFormat('UsdSmoke 96x60 1');app.getProjectParam('outputFormat').setValue('UsdSmoke')
    app.getProjectParam('frameRange').set(1,25);app.getProjectParam('frameRate').setValue(24.)
    source=node('ReadUSD','UsdStage');camera=node('ReadUSDCamera','UsdCamera')
    for effect in (source,camera):effect.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/usd/scene.usda').as_posix())
    assert 'Meshes: 2  Lights: 2' in source.getParam('sourceInfo').getValue()
    assert '/World/Camera' in camera.getParam('sourceInfo').getValue()
    camera.getParam('cameraName').setValue('/World/Camera')
    scene=node('Scene','UsdScene');renderer=node('Render3D','UsdRender3D')
    assert scene.connectInput(0,source) and renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
    assert not camera.connectInput(0,source) and not source.connectInput(0,camera)
    assert not renderer.connectInput(1,source)
    renderer.getParam('renderer').setValue('CPU' if cpu_only else 'Vulkan')
    first=render('first');last=render('last',25);assert first!=last
    assert sum(first[i]>0 for i in range(3,len(first),4))>500
    source.getParam('readAnimation').setValue(False);camera.getParam('readAnimation').setValue(False)
    assert same(render('frozen',25),first)
    source.getParam('readAnimation').setValue(True);camera.getParam('readAnimation').setValue(True)
    source.getParam('animationSpeed').setValue(0.);source.getParam('animationOffset').setValue(1.)
    camera.getParam('animationSpeed').setValue(0.);camera.getParam('animationOffset').setValue(1.)
    assert same(render('retimed',1),last)
    for effect in (source,camera):effect.getParam('animationSpeed').setValue(1.);effect.getParam('animationOffset').setValue(0.)
    source.getParam('variants').setValue('/World/Animated look=gold')
    gold=render('gold');assert gold!=first
    source.getParam('variants').setValue('')
    assert same(render('blue'),first)
    axis=node('Axis','UsdAxis');assert source.connectInput(1,axis)
    axis.getParam('translate').set(.4,.2,0.);assert render('axis')!=first
    axis.getParam('translate').set(0.,0.,0.)
    camera.getParam('translate').set(.3,0.,0.);assert render('camera-offset')!=first
    camera.getParam('translate').set(0.,0.,0.)
    camera.getParam('focalMultiplier').setValue(1.2);assert render('camera-focal')!=first
    camera.getParam('focalMultiplier').setValue(1.)
    source.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/usd/textured.usda').as_posix())
    assert 'cannot decode texture' not in source.getParam('sourceInfo').getValue(),source.getParam('sourceInfo').getValue()
    textured=render('textured');assert textured!=first
    source.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/usd/pbr-maps.usda').as_posix())
    assert 'unsupported surface input normal' not in source.getParam('sourceInfo').getValue()
    assert 'cannot decode texture' not in source.getParam('sourceInfo').getValue()
    mapped=render('pbr-maps');assert mapped!=textured
    # USD geometry participates in the existing material and frozen projector
    # pipeline without creating a separate renderer or image data type.
    image=QImage(96,60,QImage.Format_RGBA8888);image.fill(0xffff8040)
    assert image.save(str(directory/'projection.png'))
    reader=app.createReader((directory/'projection.png').as_posix())
    projector=node('Project3D','UsdProjector');assert projector.connectInput(0,reader) and projector.connectInput(1,camera)
    material=node('PBRMaterial','UsdOverride')
    material.getParam('emission').set(.1,.05,0.,1.);assert source.connectInput(0,material)
    flat_override=render('flat-material-override')
    source.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/usd/scene.usda').as_posix())
    assert same(render('flat-material-original'),flat_override)
    source.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/usd/pbr-maps.usda').as_posix())
    assert material.connectInput(0,projector)
    projected=render('projected');assert projected!=first
    source.getParam('primPath').setValue('/World/Animated')
    subset=render('subset');assert subset!=projected
    assert 'Meshes: 1  Lights: 0' in source.getParam('sourceInfo').getValue()
    source.getParam('primPath').setValue('')
    transform=node('TransformGeo','UsdTransform');assert transform.connectInput(0,source)
    scene.disconnectInput(0);assert scene.connectInput(0,transform)
    transform.getParam('translate').set(-.4,0.,0.);assert render('transform')!=projected
    transform.getParam('translate').set(0.,0.,0.)
    source.disconnectInput(0)
    assert render('imported-texture',13)!=projected
    if not cpu_only:
        gpu=exr('gpu');renderer.getParam('renderer').setValue('CPU');cpu=exr('cpu')
        tool=Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve()
        result=subprocess.run([str(tool),str(cpu),str(gpu),'--diff'],capture_output=True,text=True)
        match=re.search(r'Max error\s*=\s*([-+0-9.eE]+)',result.stdout+result.stderr);assert match,result.stdout+result.stderr
        error=float(match.group(1));assert error<.002,error;report['cpu_vulkan_max_error']=error
        renderer.getParam('renderer').setValue('Vulkan')
    reference=render('before-save',13)
    saved=directory/'usd.ntp';assert app.saveProjectAs(saved.as_posix())
    restored=app.loadProject(saved.as_posix());assert restored
    assert restored.getNode('UsdStage').getParam('loadPayloads').getValue()
    assert restored.getNode('UsdCamera').getParam('cameraName').getValue()=='/World/Camera'
    assert same(render('after-load',13,restored),reference)
    report.update(ok=True,typed_sockets=True,animation=True,retiming=True,variants=True,axis=True,
                  camera_offsets=True,pbr_projection=True,subtree=True,transform_scene=True,usd_texture=True,pbr_maps=True,save_reload=True)
except Exception:
    report['exception']=traceback.format_exc()
finally:
    (directory/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
assert report['ok'],report.get('exception')
