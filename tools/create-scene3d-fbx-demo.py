"""Run in NatronRenderer to create and render an editable ufbx/FBX test scene."""
from pathlib import Path
import shutil
from PySide6.QtGui import QImage, QColor, QPainter
from PySide6.QtCore import Qt

camera_demo=globals().get('camera_demo',False)
directory=Path.cwd()/'build'/('scene3d-fbx-camera-demo' if camera_demo else 'scene3d-fbx-demo')
directory.mkdir(parents=True,exist_ok=True)
for name in ('colored-cube.fbx','blender-cube-binary.fbx')+(('animated-camera.fbx',) if camera_demo else ()):
    shutil.copyfile(Path('Tests/fixtures/scene3d')/name,directory/name)
app.addFormat('FbxDemo 640x360 1')
app.getProjectParam('outputFormat').setValue('FbxDemo')
app.getProjectParam('frameRange').set(1,25 if camera_demo else 48)
if camera_demo:app.getProjectParam('frameRate').setValue(24.)

def native(kind,name):
    result=app.createNode('fr.inria.built-in.'+kind)
    assert result,kind
    result.setScriptName(name);result.setLabel(name)
    return result

image=QImage(640,360,QImage.Format_RGBA8888)
image.fill(QColor(22,36,62))
painter=QPainter(image)
for y in range(0,360,40):
    for x in range(0,640,40):
        painter.fillRect(x,y,40,40,QColor(237,157,65) if (x//40+y//40)%2 else QColor(40,146,173))
painter.end();assert image.save(str(directory/'projection.png'))
reader=app.createReader((directory/'projection.png').as_posix())
camera=native('ReadCamera' if camera_demo else 'Camera','AnimatedCamera')
projector=native('ReadCamera' if camera_demo else 'Camera','ReferenceCamera')
if camera_demo:
    for imported in (camera,projector):
        imported.getParam('file').setValue((directory/'animated-camera.fbx').as_posix())
        imported.getParam('cameraName').setValue('Rig/MainCamera')
        imported.getParam('animationTake').setValue('CameraMove')
        imported.getParam('translate').set(-.3,0.,1.)
        imported.getParam('rotate').set(0.,-6.,0.)
        imported.getParam('focalMultiplier').setValue(.8)
    projector.getParam('readAnimation').setValue(False)
else:
    camera.getParam('focal').setValue(40.);camera.getParam('translate').set(0.,0.,7.)
    camera.getParam('translate').setValueAtTime(-.5,1.,0)
    camera.getParam('translate').setValueAtTime(.5,48.,0)
    projector.getParam('focal').setValue(40.);projector.getParam('translate').set(0.,0.,7.)
projection=native('Project3D','FrozenProjection')
assert projection.connectInput(0,reader) and projection.connectInput(1,projector)
axis=native('Axis','MeshAxis');axis.getParam('translate').set(-.85,0.,0.)
warm=native('ReadGeo','FbxMaterialCube')
warm.getParam('file').setValue((directory/'colored-cube.fbx').as_posix())
warm.getParam('scale').set(.7,.7,.7)
warm.getParam('rotate').set(18.,-28.,-5.)
assert warm.connectInput(1,axis)
textured=native('ReadGeo','ProjectedFbxCube')
textured.getParam('file').setValue((directory/'blender-cube-binary.fbx').as_posix())
textured.getParam('scale').set(.6,.6,.6)
textured.getParam('translate').set(.95,.05,-.4)
textured.getParam('rotate').set(12.,32.,0.)
assert textured.connectInput(0,projection)
textured.getParam('rotate').setValueAtTime(12.,1.,0)
textured.getParam('rotate').setValueAtTime(28.,48.,0)
background=native('Card','BackgroundCard')
back=app.createNode('net.sf.openfx.ConstantPlugin');back.getParam('color').set(.035,.065,.13,1.)
background.getParam('size').set(10.,6.);background.getParam('translate').set(0.,0.,-2.)
assert background.connectInput(0,back)
scene=native('Scene','FbxScene')
for index,effect in enumerate((warm,textured,background)):assert scene.connectInput(index,effect)
renderer=native('Render3D','FbxRender3D')
assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
renderer.getParam('renderer').setValue('Auto')
renderer.getParam('antialiasing').setValue('2x2')
renderer.getParam('depthRange').set(5.,10.)
demo_frame=13 if camera_demo else 24
color=app.createWriter((directory/('color-####.png' if camera_demo else 'color.png')).as_posix());assert color.connectInput(0,renderer)
for frame in ((1,13,25) if camera_demo else (24,)):app.render(color,frame,frame)
renderer.getParam('output').setValue('DepthPreview')
preview=app.createWriter((directory/'depth-preview.png').as_posix());assert preview.connectInput(0,renderer)
app.render(preview,demo_frame,demo_frame)
renderer.getParam('output').setValue('Color')
exr=app.createNode('fr.inria.openfx.WriteOIIO');assert exr.connectInput(0,renderer)
exr.getParam('filename').setValue((directory/'color-and-depth.exr').as_posix())
exr.getParam('bitDepth').setValue('32f');exr.getParam('processAllPlanes').setValue(True)
exr.getParam('partSplitting').setValue('single');exr.getParam('ocioOutputSpace').setValue('linear')
app.render(exr,demo_frame,demo_frame)
project_file='fbx-camera.ntp' if camera_demo else 'fbx-meshes.ntp'
assert app.saveProjectAs((directory/project_file).as_posix())
print('SCENE3D_FBX_DEMO',directory/project_file)
