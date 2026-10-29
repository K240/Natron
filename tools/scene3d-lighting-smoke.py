"""Run in NatronRenderer: node sockets, lighting, projection, animation and IO."""
import json
import os
import re
import subprocess
import traceback
from pathlib import Path
from PySide6.QtGui import QImage,QColor

cpu_only=os.environ.get('NATRON_VULKAN_DISABLED')=='1'
directory=Path.cwd()/'build'/('scene3d-lighting-smoke-cpu' if cpu_only else 'scene3d-lighting-smoke-gpu')
directory.mkdir(parents=True,exist_ok=True)
report={'ok':False}

def node(kind,name):
    effect=app.createNode('fr.inria.built-in.'+kind);assert effect
    effect.setScriptName(name);return effect

def replace(effect,index,source):
    effect.disconnectInput(index);assert effect.connectInput(index,source)

def render(name,frame=1,application=None):
    application=application or app
    target=application.getNode('LightingRender3D')
    path=directory/(name+'.png');path.unlink(missing_ok=True)
    writer=application.createWriter(path.as_posix());assert writer.connectInput(0,target)
    application.render(writer,frame,frame)
    image=QImage(str(path)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and image.width()==96 and image.height()==64,name
    return image

def pixels(image):return bytes(image.constBits())

def exr(name):
    path=directory/(name+'.exr');path.unlink(missing_ok=True)
    writer=app.createNode('fr.inria.openfx.WriteOIIO');assert writer.connectInput(0,renderer)
    writer.getParam('filename').setValue(path.as_posix());writer.getParam('bitDepth').setValue('32f')
    writer.getParam('processAllPlanes').setValue(True);writer.getParam('partSplitting').setValue('single')
    writer.getParam('ocioOutputSpace').setValue('linear');app.render(writer,1,1);assert path.exists();return path

try:
    app.addFormat('LightingSmoke 96x64 1');app.getProjectParam('outputFormat').setValue('LightingSmoke')
    app.getProjectParam('frameRange').set(1,25)
    material=node('PBRMaterial','TestMaterial');material.getParam('baseColor').set(.7,.3,.15,1.)
    material.getParam('ior').setValue(1.)
    card=node('Card','TestCard');card.getParam('size').set(6.,6.)
    light=node('Light','TestLight');scene=node('Scene','TestScene');camera=node('Camera','TestCamera')
    camera.getParam('orthographic').setValue(True)
    renderer=node('Render3D','LightingRender3D')
    assert card.connectInput(0,material) and scene.connectInput(0,card) and scene.connectInput(1,light)
    assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
    assert not scene.connectInput(2,material) and not card.connectInput(0,light)
    assert not light.connectInput(0,camera) and not renderer.connectInput(1,light)
    assert not material.connectInput(0,scene)
    renderer.getParam('renderer').setValue('CPU' if cpu_only else 'Vulkan')
    lit=render('lit');assert lit.pixelColor(48,32).red()>20 and lit.pixelColor(48,32).alpha()==255
    light.getParam('intensity').setValue(0.)
    black=render('no-light');assert black.pixelColor(48,32).red()==0 and black.pixelColor(48,32).alpha()==255
    material.getParam('emission').set(1.,.1,0.,1.)
    assert render('emission').pixelColor(48,32).red()>200
    material.getParam('emission').set(0.,0.,0.,1.);light.getParam('intensity').setValue(1.)
    material.getParam('opacity').setValue(.5)
    assert abs(render('transparent').pixelColor(48,32).alpha()-128)<=1
    material.getParam('opacity').setValue(1.)
    axis=node('Axis','LightAxis');assert light.connectInput(0,axis)
    axis.getParam('rotate').set(0.,90.,0.)
    assert render('axis-rotation').pixelColor(48,32).red()<2
    axis.getParam('rotate').set(0.,0.,0.)
    light.getParam('lightType').setValue('Point')
    near=render('point');assert pixels(near)!=pixels(lit)
    light.getParam('lightType').setValue('Spot');light.getParam('rotate').set(0.,70.,0.)
    assert render('spot-outside').pixelColor(48,32).red()==0
    light.getParam('lightType').setValue('Distant');light.getParam('rotate').set(0.,0.,0.)
    light.getParam('exposure').setValueAtTime(0.,1);light.getParam('exposure').setValueAtTime(1.,25)
    assert pixels(render('animated-1',1))!=pixels(render('animated-25',25))
    # PBR can wrap a frozen Project3D; its image and camera retain their own
    # reference time while surface and lights follow the output timeline.
    checker=QImage(96,64,QImage.Format_RGBA8888)
    from PySide6.QtGui import QColor
    for y in range(64):
        for x in range(96):checker.setPixelColor(x,y,QColor(220 if (x//8+y//8)%2 else 40,100,180,255))
    assert checker.save(str(directory/'checker.png'))
    image=app.createReader((directory/'checker.png').as_posix())
    projection=node('Project3D','TestProjection');assert projection.connectInput(0,image) and projection.connectInput(1,camera)
    assert material.connectInput(0,projection)
    material.getParam('metallic').setValue(.65);material.getParam('roughness').setValue(.4)
    projected=render('projected');assert pixels(projected)!=pixels(lit)
    # Native material sockets accept images; channel selection, signed normal
    # encoding and map time remain independent of the base projection.
    for index in (1,2,3):
        for invalid in (camera,scene,material,projection):assert not material.connectInput(index,invalid)
    normal=app.createNode('net.sf.openfx.ConstantPlugin');normal.setScriptName('NormalMap')
    normal.getParam('color').set(.5,.8,.9,1.)
    signed=app.createNode('net.sf.openfx.ConstantPlugin');signed.setScriptName('SignedNormalMap')
    signed.getParam('color').set(0.,.6,.8,1.)
    orm=app.createNode('net.sf.openfx.ConstantPlugin');orm.setScriptName('PackedORM')
    orm.getParam('color').set(.2,.65,.9,.4)
    maplight=node('Light','MapLight');maplight.getParam('rotate').set(-25.,-30.,0.)
    maplight.getParam('shadows').setValue(False);replace(scene,1,maplight)
    assert material.connectInput(2,orm) and material.connectInput(3,orm)
    material.getParam('roughnessChannel').setValue('G');material.getParam('metallicChannel').setValue('B')
    scalar=pixels(render('scalar-maps'))
    material.disconnectInput(2);material.disconnectInput(3)
    material.getParam('roughness').setValue(.65);material.getParam('metallic').setValue(.9)
    scalar_numeric=pixels(render('scalar-numeric'))
    assert max(abs(a-b) for a,b in zip(scalar,scalar_numeric))<=1,'Map values did not replace numeric controls'
    assert material.connectInput(2,orm) and material.connectInput(3,orm)
    material.getParam('roughness').setValue(.1);material.getParam('metallic').setValue(0.)
    assert max(abs(a-b) for a,b in zip(scalar,pixels(render('map-overrides-knobs'))))<=1
    material.getParam('roughnessChannel').setValue('A')
    alpha_channel=render('scalar-alpha');assert pixels(alpha_channel)!=scalar and alpha_channel.pixelColor(48,32).alpha()==255
    material.getParam('roughnessChannel').setValue('G')
    assert material.connectInput(1,normal);mapped=pixels(render('all-maps'));assert mapped!=scalar
    material.getParam('normalFlipY').setValue(True);flipped=pixels(render('normal-y-flip'));assert flipped!=mapped
    material.getParam('normalFlipY').setValue(False)
    replace(material,1,signed);material.getParam('normalEncoding').setValue('Signed')
    signed_image=pixels(render('signed-normal'));assert max(abs(a-b) for a,b in zip(mapped,signed_image))<=1
    replace(material,1,normal);material.getParam('normalEncoding').setValue('Unsigned')
    # Reader data is passed through the graph as raw, straight channels.
    normal_file=QImage(2,2,QImage.Format_RGBA8888);normal_file.fill(QColor(128,204,230,0))
    assert normal_file.save(str(directory/'normal-data.png'))
    raw=app.createReader((directory/'normal-data.png').as_posix());raw.setScriptName('RawNormalReader')
    raw.getParam('ocioInputSpace').setValue('linear');raw.getParam('ocioOutputSpace').setValue('linear')
    raw.getParam('filePremult').setValue(2);raw.getParam('outputPremult').setValue(2)
    replace(material,1,raw);raw_pixels=pixels(render('reader-normal'))
    normal.getParam('color').set(128/255.,204/255.,230/255.,1.);replace(material,1,normal)
    constant_pixels=pixels(render('constant-normal'))
    assert max(abs(a-b) for a,b in zip(raw_pixels,constant_pixels))<=1,'Normal data was color converted or premultiplied'
    # EXR data/display windows must retain their position in the map's format.
    oiio=str(Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve())
    full_window=directory/'map-full.exr';cropped_window=directory/'map-cropped.exr'
    subprocess.run([oiio,'--create','4x4','4','--fill:color=0.2,0.65,0.9,0.4','2x4+2+0','-d','float','-o',str(full_window)],check=True,capture_output=True)
    subprocess.run([oiio,str(full_window),'--crop','2x4+2+0','-d','float','-o',str(cropped_window)],check=True,capture_output=True)
    windows=[]
    for path in (full_window,cropped_window):
        reader=app.createReader(path.as_posix());reader.getParam('ocioInputSpace').setValue('linear');reader.getParam('ocioOutputSpace').setValue('linear')
        reader.getParam('filePremult').setValue(2);reader.getParam('outputPremult').setValue(2)
        replace(material,2,reader);replace(material,3,reader);windows.append(pixels(render(path.stem)))
    assert max(abs(a-b) for a,b in zip(*windows))<=1,'Cropped data window stretched native maps'
    alpha_reader=app.createReader(full_window.as_posix())
    alpha_reader.getParam('ocioInputSpace').setValue('linear');alpha_reader.getParam('ocioOutputSpace').setValue('linear')
    alpha_reader.getParam('filePremult').setValue(2);alpha_reader.getParam('outputPremult').setValue(2)
    replace(material,2,alpha_reader);replace(material,3,orm);material.getParam('roughnessChannel').setValue('A')
    alpha_reference=pixels(render('rgba-scalar-alpha'))
    alpha_reader.getParam('outputComponents').setValue('Alpha');material.getParam('roughnessChannel').setValue('R')
    assert max(abs(a-b) for a,b in zip(alpha_reference,pixels(render('single-channel-scalar'))))<=1,'Single-channel scalar map was converted to black RGB'
    material.getParam('roughnessChannel').setValue('G')
    replace(material,2,orm);replace(material,3,orm)
    renderer.getParam('output').setValue('DepthPreview');depth_mapped=pixels(render('depth-mapped'))
    for index in (1,2,3):material.disconnectInput(index)
    assert pixels(render('depth-unmapped'))==depth_mapped
    renderer.getParam('output').setValue('Color')
    for index,source_map in ((1,normal),(2,orm),(3,orm)):assert material.connectInput(index,source_map)
    # A frozen projected image uses reference time, while map nodes use output time.
    projection.getParam('freeze').setValue(True);projection.getParam('referenceFrame').setValue(1)
    normal.getParam('color').setValueAtTime(.2,1,1);normal.getParam('color').setValueAtTime(.8,25,1)
    assert pixels(render('animated-map-1',1))!=pixels(render('animated-map-25',25))
    material.disconnectInput(0);assert pixels(render('maps-without-base'))!=mapped
    assert material.connectInput(0,projection)
    # Imported FBX UVs use the same binding; map-specific USD UV arrays stay untouched.
    fbx=node('ReadGeo','MappedFbx');fbx.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/colored-cube.fbx').as_posix())
    assert fbx.connectInput(0,material);replace(scene,0,fbx)
    fbx_mapped=pixels(render('fbx-mapped'));material.disconnectInput(1)
    assert pixels(render('fbx-unmapped'))!=fbx_mapped
    assert material.connectInput(1,normal);replace(scene,0,card)
    # The material closest to geometry owns its complete surface, including maps.
    outer=node('PBRMaterial','OuterMaterial');assert outer.connectInput(0,material);replace(card,0,outer)
    outer_before=pixels(render('outer-material'));material.getParam('normalFlipY').setValue(True)
    assert pixels(render('outer-map-isolation'))==outer_before
    material.getParam('normalFlipY').setValue(False);replace(card,0,material)
    # ReadUSD overrides use its ordinary mesh UVs and restore imported maps on disconnect.
    usd=node('ReadUSD','NativeMappedUsd');usd.getParam('file').setValue((Path.cwd()/'Tests/fixtures/scene3d/usd/pbr-maps.usda').as_posix())
    assert usd.connectInput(0,material);replace(scene,0,usd)
    usd_native=pixels(render('usd-native-maps'));material.disconnectInput(1)
    assert pixels(render('usd-native-unmapped'))!=usd_native
    assert material.connectInput(1,normal);usd.disconnectInput(0)
    assert pixels(render('usd-imported-maps'))!=usd_native;replace(scene,0,card)
    report['material_maps']=True
    cpu=exr('cpu-projected') if cpu_only else None
    if not cpu_only:
        gpu=exr('gpu-projected');renderer.getParam('renderer').setValue('CPU');cpu=exr('cpu-projected')
        tool=os.environ.get('OIIO_TOOL') or str(Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve())
        assert Path(tool).exists(),tool
        result=subprocess.run([tool,'-v',str(cpu),str(gpu),'--diff'],capture_output=True,text=True)
        match=re.search(r'Max error\s*=\s*([-+0-9.eE]+)',result.stdout+result.stderr)
        assert match,result.stdout+result.stderr
        error=float(match.group(1));assert error<.001,error;report['cpu_vulkan_max_error']=error
        renderer.getParam('renderer').setValue('Vulkan')
    reference=pixels(render('before-save',1))
    overflow=node('Scene','OverflowLights')
    for index in range(16):
        extra=node('Light','DisabledLight'+str(index));extra.getParam('intensity').setValue(0.)
        assert overflow.connectInput(index,extra)
    combined=node('Scene','CombinedScene');assert combined.connectInput(0,scene) and combined.connectInput(1,overflow)
    replace(renderer,0,combined);renderer.getParam('renderer').setValue('Auto')
    assert pixels(render('auto-17-lights',1))==reference
    report['auto_light_limit_fallback']=True
    replace(renderer,0,scene);renderer.getParam('renderer').setValue('CPU' if cpu_only else 'Vulkan')
    saved=directory/'lighting.ntp';assert app.saveProjectAs(saved.as_posix())
    application=app.loadProject(saved.as_posix());assert application
    restored=application.getNode('TestMaterial');assert restored.getParam('metallic').getValue()==0.
    assert [restored.getInput(i).getScriptName() for i in (1,2,3)]==['NormalMap','PackedORM','PackedORM']
    assert restored.getParam('roughnessChannel').getValue()==1 and restored.getParam('metallicChannel').getValue()==2
    assert restored.getParam('normalEncoding').getValue()==0 and restored.getParam('mapWrap').getValue()==2
    assert application.getNode('TestLight').getParam('exposure').getValueAtTime(25)==1.
    assert pixels(render('after-load',1,application))==reference
    report.update(ok=True,sockets=True,axis=True,light_types=True,animation=True,projection=True,project_roundtrip=True)
except Exception:
    report['exception']=traceback.format_exc()
finally:
    (directory/'result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
assert report['ok'],report.get('exception')
