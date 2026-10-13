"""Run in NatronRenderer: shared native/FBX PBR and lighting example."""
import shutil
from pathlib import Path

directory=Path.cwd()/'build/scene3d-lighting-demo'
directory.mkdir(parents=True,exist_ok=True)
shutil.copyfile('Tests/fixtures/scene3d/colored-cube.fbx',directory/'colored-cube.fbx')
for name in ('normal.png','orm.png'):
    shutil.copyfile(Path('Tests/fixtures/scene3d/usd/textures')/name,directory/name)
app.addFormat('LightingDemo 640x400 1')
app.getProjectParam('outputFormat').setValue('LightingDemo')
app.getProjectParam('frameRange').set(1,25)
app.getProjectParam('frameRate').setValue(24.)

def node(kind,name):
    effect=app.createNode('fr.inria.built-in.'+kind);assert effect
    effect.setScriptName(name);effect.setLabel(name);return effect

scene=node('Scene','LightingScene')
white=app.createNode('net.sf.openfx.ConstantPlugin');assert white
white.setScriptName('SurfaceWhite');white.setLabel('SurfaceWhite');white.getParam('color').set(1.,1.,1.,1.)
for index,(name,x,color,metallic,roughness) in enumerate((
    ('BluePlastic',-1.25,(.2,.5,1.),0.,.65),
    ('GoldMetal',1.25,(1.,.75,.3),1.,.5))):
    material=node('PBRMaterial',name+'Material')
    material.getParam('baseColor').set(*color,1.)
    material.getParam('metallic').setValue(metallic)
    material.getParam('roughness').setValue(roughness)
    assert material.connectInput(0,white)
    mesh=node('ReadGeo',name)
    mesh.getParam('file').setValue((directory/'colored-cube.fbx').as_posix())
    mesh.getParam('translate').set(x,-.1,0.)
    mesh.getParam('rotate').set(0.,-15. if index==0 else 20.,0.)
    mesh.getParam('scale').set(.8,.8,.8)
    assert mesh.connectInput(0,material) and scene.connectInput(index,mesh)
# Data readers keep packed channels linear and independent of alpha.
normal=app.createReader((directory/'normal.png').as_posix());normal.setScriptName('NormalMap');normal.setLabel('NormalMap')
orm=app.createReader((directory/'orm.png').as_posix());orm.setScriptName('PackedORM');orm.setLabel('PackedORM')
for reader in (normal,orm):
    reader.getParam('ocioInputSpace').setValue('linear');reader.getParam('ocioOutputSpace').setValue('linear')
    reader.getParam('filePremult').setValue(2);reader.getParam('outputPremult').setValue(2)
# Natron adopts the first Reader's format; retain the intended render format.
app.getProjectParam('outputFormat').setValue('LightingDemo')
material=app.getNode('BluePlasticMaterial')
assert material.connectInput(1,normal) and material.connectInput(2,orm) and material.connectInput(3,orm)
material.getParam('roughnessChannel').setValue('G');material.getParam('metallicChannel').setValue('B')

ground_material=node('PBRMaterial','GroundMaterial')
ground_material.getParam('baseColor').set(.18,.2,.24,1.)
ground_material.getParam('roughness').setValue(.85)
ground=node('Card','Ground')
ground.getParam('size').set(10.,10.)
ground.getParam('translate').set(0.,-.9,0.)
ground.getParam('rotate').set(-90.,0.,0.)
assert ground.connectInput(0,ground_material) and scene.connectInput(2,ground)
key=node('Light','KeyLight')
key.getParam('intensity').setValue(3.)
key.getParam('color').set(1.,.9,.75,1.)
key.getParam('rotate').set(-35.,-30.,0.)
key.getParam('rotate').setValueAtTime(-45.,1,1)
key.getParam('rotate').setValueAtTime(45.,25,1)
assert scene.connectInput(3,key)
fill=node('Light','FillLight')
fill.getParam('lightType').setValue('Point')
fill.getParam('translate').set(1.5,1.,6.)
fill.getParam('color').set(.45,.65,1.,1.)
fill.getParam('intensity').setValue(100.)
assert scene.connectInput(4,fill)
camera=node('Camera','RenderCamera')
camera.getParam('translate').set(0.,2.8,7.5)
camera.getParam('rotate').set(-22.,0.,0.)
camera.getParam('focal').setValue(34.)
renderer=node('Render3D','LightingRender3D')
renderer.getParam('antialiasing').setValue('2x2')
renderer.getParam('depthRange').set(4.,15.)
assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
writer=app.createWriter((directory/'color-####.png').as_posix())
writer.setScriptName('ColorWriter');assert writer.connectInput(0,renderer)
# The first Reader publishes its format when pixels are fetched. Warm up once
# before applying the final project format and generating the delivery frames.
app.render(writer,1,1)
app.getProjectParam('outputFormat').setValue('LightingDemo')
for frame in (1,13,25):app.render(writer,frame,frame)
renderer.getParam('output').setValue('DepthPreview')
depth=app.createWriter((directory/'depth-preview.png').as_posix());depth.setScriptName('DepthWriter')
assert depth.connectInput(0,renderer);app.render(depth,13,13)
renderer.getParam('output').setValue('Color')
exr=app.createNode('fr.inria.openfx.WriteOIIO');exr.setScriptName('ColorDepthWriter')
exr.getParam('filename').setValue((directory/'color-and-depth.exr').as_posix())
exr.getParam('bitDepth').setValue('32f');exr.getParam('processAllPlanes').setValue(True)
exr.getParam('partSplitting').setValue('single');exr.getParam('ocioOutputSpace').setValue('linear')
assert exr.connectInput(0,renderer);app.render(exr,13,13)
assert app.saveProjectAs((directory/'lighting.ntp').as_posix())
print('SCENE3D_LIGHTING_DEMO',directory/'lighting.ntp')
