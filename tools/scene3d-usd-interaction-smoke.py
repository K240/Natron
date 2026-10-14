"""Render the USD editing/projection/utility workflow through the EXR Writer."""
import hashlib
import json
import re
import subprocess
import traceback
from pathlib import Path
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage, QColor

directory = Path.cwd() / 'build/scene3d-usd-interaction'
directory.mkdir(parents=True, exist_ok=True)
report = {'ok': False}
tool = Path('build/msys64/ucrt64/bin/oiiotool.exe').resolve()


def node(kind, name):
    effect = app.createNode('fr.inria.built-in.' + kind)
    assert effect
    effect.setScriptName(name)
    return effect


def render(name, frame=1, application=None):
    application = application or app
    application.getProjectParam('outputFormat').setValue('UsdInteraction')
    path = directory / (name + '.exr')
    path.unlink(missing_ok=True)
    writer = application.createNode('fr.inria.openfx.WriteOIIO')
    assert writer.connectInput(0, application.getNode('UsdInteractionRender'))
    for knob, value in [('filename', path.as_posix()), ('bitDepth', '32f'),
                        ('processAllPlanes', True), ('partSplitting', 'single'), ('ocioOutputSpace', 'linear')]:
        writer.getParam(knob).setValue(value)
    QCoreApplication.processEvents()
    application.getProjectParam('outputFormat').setValue('UsdInteraction')
    application.render(writer, frame, frame)
    assert path.exists(), name
    return path


def sample(path, channel, x=48, y=32):
    result = subprocess.run([str(tool), str(path), '--ch', channel, '--cut', f'1x1+{x}+{y}', '--printstats'],
                            check=True, capture_output=True, text=True)
    match = re.search(r'Stats Min:\s+([-+0-9.eE]+)', result.stdout)
    assert match, result.stdout + result.stderr
    return float(match.group(1))


try:
    app.addFormat('UsdInteraction 96x64 1')
    app.getProjectParam('outputFormat').setValue('UsdInteraction')
    app.getProjectParam('frameRange').set(1, 2)
    fixture = directory / 'projection.usda'
    fixture.write_text('''#usda 1.0
(metersPerUnit = 1; upAxis = "Y"; defaultPrim = "World")
def Xform "World" {
    def Mesh "Panel" {
        point3f[] points = [(-2,-1,0),(2,-1,0),(2,1,1),(-2,1,1)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        uniform token subdivisionScheme = "none"
        bool doubleSided = true
        texCoord2f[] primvars:st = [(0,0),(1,0),(1,1),(0,1)] (interpolation = "vertex")
    }
    def Camera "Camera" {
        double3 xformOp:translate = (0,0,5)
        uniform token[] xformOpOrder = ["xformOp:translate"]
        float focalLength = 0.5
        float horizontalAperture = 0.36
        float verticalAperture = 0.24
        float2 clippingRange = (0.01,1000)
    }
}
''', encoding='utf-8')
    original_hash = hashlib.sha256(fixture.read_bytes()).hexdigest()
    source = node('ReadUSD', 'UsdInteractionSource')
    source.getParam('file').setValue(fixture.as_posix())
    projector = node('Camera', 'ProjectionCamera')
    projector.getParam('translate').set(0., 0., 5.)
    projector.getParam('translate').setValueAtTime(0., 1., 0)
    projector.getParam('translate').setValueAtTime(.5, 2., 0)
    for frame in (1, 2):
        image = QImage(96, 64, QImage.Format_RGBA8888)
        for y in range(64):
            for x in range(96):
                image.setPixelColor(x, y, QColor(40+2*x if frame==1 else 0, 40+2*x if frame==2 else 0, 0, 255))
        assert image.save(str(directory / f'projection.{frame:04d}.png'))
    reader = app.createReader((directory / 'projection.####.png').as_posix())
    projection = node('Project3D', 'ProjectionMaterial')
    assert projection.connectInput(0, reader) and projection.connectInput(1, projector)
    projection.getParam('referenceFrame').setValue(1)
    binding = node('BindMaterial', 'UsdImageBinding')
    binding.getParam('primPaths').setValue('/World/Panel')
    assert binding.connectInput(0, source) and binding.connectInput(1, projection)
    transform = node('USDTransform', 'UsdDelta')
    transform.getParam('primPaths').setValue('/World/Panel')
    assert transform.connectInput(0, binding)
    visibility = node('USDVisibility', 'UsdVisibility')
    visibility.getParam('primPaths').setValue('/World/Panel')
    assert visibility.connectInput(0, transform)
    comp = node('USDCompositing', 'UsdComp')
    comp.getParam('primPaths').setValue('/World/Panel')
    assert comp.connectInput(0, visibility)
    renderer = node('Render3D', 'UsdInteractionRender')
    renderer.getParam('renderer').setValue('hdNVP')
    renderer.getParam('pathDenoiser').setValue('None')
    renderer.getParam('pathSamples').setValue(4)
    renderer.getParam('pathBounces').setValue(0)
    renderer.getParam('usdCamera').setValue('/World/Camera')
    assert renderer.connectInput(0, comp)  # Camera comes directly from the stage.
    layers = [(layer.getLayerName(), list(layer.getComponentsNames())) for layer in renderer.getAvailableLayers(-1)]
    assert all(name in [x[0] for x in layers] for name in ('Depth', 'Albedo', 'N', 'P')), layers
    first = render('projected-first')
    assert sample(first, 'R') > .1 and abs(sample(first, 'G')) < .001, (sample(first, 'R'), sample(first, 'G'))
    assert 4. < sample(first, 'Depth.Z') < 5.
    assert .4 < sample(first, 'P.Z') < .6
    assert sample(first, 'N.Z') > .8
    held = render('camera-image-held', 2)
    assert abs(sample(first, 'R')-sample(held, 'R')) < .001
    projection.getParam('imageTimeMode').setValue('Live')
    live_image = render('camera-held-image-live', 2)
    assert sample(live_image, 'G') > .1 and abs(sample(live_image, 'R')) < .001
    projection.getParam('freeze').setValue(False)
    projection.getParam('imageTimeMode').setValue('Frozen')
    live_camera = render('camera-live-image-held', 2)
    assert sample(live_camera, 'R') > .1 and abs(sample(live_camera, 'G')) < .001
    assert abs(sample(live_camera, 'R')-sample(first, 'R')) > .01
    transform.getParam('translate').set(0., 0., .5)
    shifted = render('usd-transform', 2)
    assert .9 < sample(shifted, 'P.Z') < 1.1
    comp.getParam('holdout').setValue(True)
    holdout = render('holdout', 2)
    assert sample(holdout, 'A') == 0. and sample(holdout, 'Depth.Z') > 0.
    comp.getParam('holdout').setValue(False)
    comp.getParam('cameraVisible').setValue(False)
    invisible = render('camera-invisible', 2)
    assert sample(invisible, 'A') == 0. and sample(invisible, 'Depth.Z') == 0.
    comp.getParam('cameraVisible').setValue(True)
    visibility.getParam('visible').setValue(False)
    hidden = render('usd-hidden', 2)
    assert sample(hidden, 'A') == 0.
    visibility.getParam('visible').setValue(True)
    saved = directory / 'interaction.ntp'
    assert app.saveProjectAs(saved.as_posix())
    restored = app.loadProject(saved.as_posix())
    assert restored and restored.getNode('UsdDelta').getParam('translate').getValue(2) == .5
    assert restored.getNode('ProjectionMaterial').getParam('imageTimeMode').getValue() == 1
    after = render('after-load', 2, restored)
    assert abs(sample(after, 'R')-sample(shifted, 'R')) < .001
    assert hashlib.sha256(fixture.read_bytes()).hexdigest() == original_hash
    report.update(ok=True, layers=layers, independent_projection_timing=True, hit_projection=True,
                  usd_edits=True, stage_camera=True, source_unchanged=True, utility_exr=True,
                  holdout=True, camera_visibility=True, project_roundtrip=True)
except Exception:
    report['exception'] = traceback.format_exc()
finally:
    (directory / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
