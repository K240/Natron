"""Run in NatronRenderer: composed USD/PBR/lights with existing native 3D."""
import shutil
from pathlib import Path

directory=Path.cwd()/'build/scene3d-usd-demo'
directory.mkdir(parents=True,exist_ok=True)
for name in ('scene.usda','asset.usda','textured.usda','pbr-maps.usda'):
    shutil.copyfile(Path('Tests/fixtures/scene3d/usd')/name,directory/name)
shutil.copytree(Path('Tests/fixtures/scene3d/usd/textures'),directory/'textures',dirs_exist_ok=True)
# Tilt the metal panel and keep direct-light highlights inside the PNG range.
# The regression fixtures retain their analytic strengths and transforms.
stage=directory/'scene.usda'
text=stage.read_text().replace('float inputs:intensity = 3','float inputs:intensity = 1.5').replace('float inputs:intensity = 80','float inputs:intensity = 12')
text=text.replace('double3 xformOp:translate = (1,0,0)\n        uniform token[] xformOpOrder = ["xformOp:translate"]',
                  'double3 xformOp:translate = (1,0,0)\n        float3 xformOp:rotateXYZ = (0,15,0)\n        uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]')
stage.write_text(text,encoding='utf-8')
app.addFormat('UsdDemo 640x400 1');app.getProjectParam('outputFormat').setValue('UsdDemo')
app.getProjectParam('frameRange').set(1,25);app.getProjectParam('frameRate').setValue(24.)

def node(kind,name):
    effect=app.createNode('fr.inria.built-in.'+kind);assert effect
    effect.setScriptName(name);effect.setLabel(name);return effect

source=node('ReadUSD','UsdStage');camera=node('ReadUSDCamera','UsdCamera')
for effect in (source,camera):effect.getParam('file').setValue((directory/'pbr-maps.usda').as_posix())
camera.getParam('cameraName').setValue('/World/Camera')
axis=node('Axis','StageAxis');assert source.connectInput(1,axis)
scene=node('Scene','UsdScene');assert scene.connectInput(0,source)
renderer=node('Render3D','UsdRender3D');renderer.getParam('antialiasing').setValue('2x2')
renderer.getParam('depthRange').set(3.,6.)
assert renderer.connectInput(0,scene) and renderer.connectInput(1,camera)
writer=app.createWriter((directory/'color-####.png').as_posix());writer.setScriptName('ColorWriter');assert writer.connectInput(0,renderer)
for frame in (1,13,25):app.render(writer,frame,frame)
renderer.getParam('output').setValue('DepthPreview')
depth=app.createWriter((directory/'depth-preview.png').as_posix());depth.setScriptName('DepthWriter')
assert depth.connectInput(0,renderer);app.render(depth,13,13)
renderer.getParam('output').setValue('Color')
assert app.saveProjectAs((directory/'usd.ntp').as_posix())
print('SCENE3D_USD_DEMO',directory/'usd.ntp')
