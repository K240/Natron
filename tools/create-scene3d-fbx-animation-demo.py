"""Create an editable FBX hierarchy/skinning/blend-shape animation example."""
from pathlib import Path
import shutil
from PySide6.QtGui import QImage, QColor, QPainter

directory=Path.cwd()/'build'/'scene3d-fbx-animation-demo'
directory.mkdir(parents=True,exist_ok=True)
for name in ('animated-cube.fbx','animated-skin.fbx','animated-blend-shape.fbx'):
    shutil.copyfile(Path('Tests/fixtures/scene3d')/name,directory/name)
# The source skin fixture animates its mesh node. Retarget its translation
# track to the root joint for a separate example of weighted bone animation.
skin=(directory/'animated-skin.fbx').read_text()
connection='C: "OP",2415043572384,2414390291472, "Lcl Translation"'
assert connection in skin
(directory/'animated-bones.fbx').write_text(skin.replace(connection,'C: "OP",2415043572384,2414390319312, "Lcl Translation"'))
app.addFormat('FbxAnimationDemo 640x360 1')
app.getProjectParam('outputFormat').setValue('FbxAnimationDemo')
app.getProjectParam('frameRate').setValue(24.)
app.getProjectParam('frameRange').set(1,49)

def native(kind,name):
    result=app.createNode('fr.inria.built-in.'+kind)
    assert result,kind
    result.setScriptName(name);result.setLabel(name)
    return result

image=QImage(640,360,QImage.Format_RGBA8888)
image.fill(QColor(25,55,95));painter=QPainter(image)
for y in range(0,360,30):
    for x in range(0,640,30):
        painter.fillRect(x,y,30,30,QColor(238,158,67) if (x//30+y//30)%2 else QColor(43,160,182))
painter.end();assert image.save(str(directory/'projection.png'))
reader=app.createReader((directory/'projection.png').as_posix());reader.setScriptName('ProjectionImage')
camera=native('Camera','RenderCamera');camera.getParam('translate').set(0.,0.,7.)
camera.getParam('focal').setValue(36.)
projector=native('Camera','ReferenceCamera');projector.getParam('translate').set(0.,0.,7.)
projector.getParam('focal').setValue(36.)
projection=native('Project3D','FrozenProjection')
assert projection.connectInput(0,reader) and projection.connectInput(1,projector)

moving=native('ReadGeo','HierarchyAnimation')
moving.getParam('file').setValue((directory/'animated-cube.fbx').as_posix())
moving.getParam('readAnimation').setValue(True);moving.getParam('animationTake').setValue('Move')
moving.getParam('animationSpeed').setValue(.5)
moving.getParam('scale').set(.55,.55,.55);moving.getParam('translate').set(-2.05,0.,0.)
moving.getParam('rotate').set(12.,-24.,0.)
skin=native('ReadGeo','BoneAnimation')
skin.getParam('file').setValue((directory/'animated-bones.fbx').as_posix())
skin.getParam('readAnimation').setValue(True);skin.getParam('animationSpeed').setValue(1./6.)
skin.getParam('scale').set(40.,40.,40.)
skin.getParam('translate').set(-1.3595,-.7544,-1.6)
skin_image=app.createNode('net.sf.openfx.ConstantPlugin');skin_image.setScriptName('SkinColor')
skin_image.getParam('color').set(.14,.48,.85,1.);assert skin.connectInput(0,skin_image)
blend=native('ReadGeo','BlendShapeAnimation')
blend.getParam('file').setValue((directory/'animated-blend-shape.fbx').as_posix())
blend.getParam('readAnimation').setValue(True);blend.getParam('animationSpeed').setValue(2.)
blend.getParam('scale').set(140.,140.,140.);blend.getParam('translate').set(1.9,0.,0.)
blend.getParam('rotate').set(15.,-25.,0.);assert blend.connectInput(0,projection)
background=native('Card','BackgroundCard');background.getParam('size').set(14.,8.)
background.getParam('translate').set(0.,0.,-3.)
back=app.createNode('net.sf.openfx.ConstantPlugin');back.setScriptName('BackgroundColor')
back.getParam('color').set(.025,.04,.075,1.);assert background.connectInput(0,back)
scene=native('Scene','FbxScene')
for index,effect in enumerate((moving,skin,blend,background)):assert scene.connectInput(index,effect)
renderer=native('Render3D','FbxRender3D')
assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
renderer.getParam('renderer').setValue('Auto');renderer.getParam('antialiasing').setValue('2x2')
renderer.getParam('depthRange').set(5.,10.)
color=app.createWriter((directory/'color-####.png').as_posix());color.setScriptName('ColorWriter')
assert color.connectInput(0,renderer)
for frame in (1,25,49):app.render(color,frame,frame)
renderer.getParam('output').setValue('DepthPreview')
preview=app.createWriter((directory/'depth-preview.png').as_posix());preview.setScriptName('DepthPreviewWriter')
assert preview.connectInput(0,renderer);app.render(preview,25,25)
renderer.getParam('output').setValue('Color')
exr=app.createNode('fr.inria.openfx.WriteOIIO');exr.setScriptName('ColorDepthWriter')
assert exr.connectInput(0,renderer)
exr.getParam('filename').setValue((directory/'color-and-depth.exr').as_posix())
exr.getParam('bitDepth').setValue('32f');exr.getParam('processAllPlanes').setValue(True)
exr.getParam('partSplitting').setValue('single');exr.getParam('ocioOutputSpace').setValue('linear')
app.render(exr,25,25)
assert app.saveProjectAs((directory/'fbx-animation.ntp').as_posix())
print('SCENE3D_FBX_ANIMATION_DEMO',directory/'fbx-animation.ntp')
