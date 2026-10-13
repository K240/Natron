"""Run inside NatronRenderer to prepare the user's composed USD test scene."""
import json
import os
from pathlib import Path
from PySide6.QtGui import QImage

directory = Path.cwd() / 'build/scene3d-hdnvp-demo'
directory.mkdir(parents=True, exist_ok=True)
source_file = Path(os.environ.get('NATRON_HDNVP_TEST_SCENE', 'D:/test_0001.usd'))
assert source_file.is_file(), source_file
app.addFormat('HdNvpTest 640x360 1')
app.getProjectParam('outputFormat').setValue('HdNvpTest')
app.getProjectParam('frameRange').set(1, 1)
app.getProjectParam('frameRate').setValue(24.)

def node(kind, name):
    effect = app.createNode('fr.inria.built-in.' + kind)
    assert effect
    effect.setScriptName(name)
    effect.setLabel(name)
    return effect

source = node('ReadUSD', 'UsdStage')
camera = node('ReadUSDCamera', 'UsdCamera')
for effect in (source, camera):
    effect.getParam('file').setValue(source_file.as_posix())
camera.getParam('cameraName').setValue('/cameras/camera1')
renderer = node('Render3D', 'HdNvpRender3D')
renderer.getParam('renderer').setValue('hdNVP')
renderer.getParam('pathSamples').setValue(8)
renderer.getParam('pathBounces').setValue(4)
renderer.getParam('pathDenoiser').setValue('Auto')
assert renderer.connectInput(0, source) and renderer.connectInput(1, camera)
color = directory / 'color.png'
writer = app.createWriter(color.as_posix())
writer.setScriptName('ColorWriter')
assert writer.connectInput(0, renderer)
app.render(writer, 1, 1)
image = QImage(str(color))
assert not image.isNull() and image.size().width() == 640
assert app.saveProjectAs((directory / 'hdnvp.ntp').as_posix())
(directory / 'result.json').write_text(json.dumps({'ok': True, 'scene': source_file.as_posix(), 'project': str(directory / 'hdnvp.ntp')}, indent=2), encoding='utf-8')
print('SCENE3D_HDNVP_DEMO', directory / 'hdnvp.ntp')
