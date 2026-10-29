"""Tracked RGB plate with USD model reflections, roughness, combined shadows and EXR/reload."""
import hashlib
import json
import os
import re
import subprocess
import traceback
from pathlib import Path
from PySide6.QtGui import QImage

directory = Path.cwd() / 'build/camera-tracker-real'
backend = os.environ.get('NATRON_REFLECTION_BACKEND', 'hdNVP')
suffix = backend.lower()
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE'], 'backend': backend}


def pixels(image):
    converted = image.convertToFormat(QImage.Format_RGBA8888)
    return bytes(converted.constBits())


def difference(a, b):
    a, b = pixels(a), pixels(b)
    assert len(a) == len(b)
    return sum(abs(x-y) for x, y in zip(a, b)) / len(a)


try:
    source = Path.cwd() / 'Tests/fixtures/scene3d/usd/layout-asset.usda'
    checksum = hashlib.sha256(source.read_bytes()).digest()
    application = app.loadProject((directory / ('shadow-occlusion-hdnvp.ntp' if backend == 'hdNVP' else 'shadow-occlusion-cpu.ntp')).as_posix())
    assert application
    render = application.getNode('Render3D1')
    catcher = application.getNode('ShadowCatcher1')
    assert render and catcher
    render.getParam('renderer').setValue(backend)
    render.getParam('pathSamples').setValue(64)
    render.getParam('pathBounces').setValue(3)
    render.getParam('pathDenoiser').setValue('None')
    render.getParam('antialiasing').setValue('1x1')
    # The existing shadow proxy now receives both effects without coplanar cards.
    catcher.getParam('catchReflections').setValue(True)
    catcher.getParam('reflectionStrength').setValue(0.)
    catcher.getParam('reflectionRoughness').setValue(0.)
    # Move the old demonstration occluder behind CG so it does not hide the reflection.
    occluder = application.getNode('Occluder1')
    if occluder:
        occluder.getParam('cameraVisible').setValue(False)
    writer = application.getNode('LayoutOutput')

    def capture(name):
        target = directory / ('reflection-' + name + '-' + suffix + '.png')
        writer.getParam('filename').setValue(target.as_posix())
        application.render(writer, 1, 1)
        image = QImage(str(target))
        assert image.width() == 640 and image.height() == 480
        return image

    render.getParam('output').setValue('Color')
    original = capture('off')
    catcher.getParam('reflectionStrength').setValue(.7)
    sharp = capture('sharp')
    report['sharp_difference'] = difference(original, sharp)
    assert report['sharp_difference'] > .02, report
    catcher.getParam('reflectionRoughness').setValue(.4)
    rough = capture('rough')
    report['roughness_difference'] = difference(sharp, rough)
    assert report['roughness_difference'] > .01, report
    render.getParam('output').setValue('ReflectionPreview')
    capture('contribution')
    render.getParam('output').setValue('Color')
    catcher.getParam('reflectionRoughness').setValue(.15)
    # The photographed desk/floor is much darker than the white reference
    # reflector under this fixture's environment fill. Keep the saved look subtle.
    catcher.getParam('reflectionStrength').setValue(.02)
    final = capture('composite')
    assert 'Reflection' in [p.getLayerName() for p in render.getAvailableLayers(-1)]
    exr = directory / ('reflection-outputs-' + suffix + '.exr')
    data_writer = application.getNode('CatcherDataOutput')
    assert data_writer
    data_writer.getParam('filename').setValue(exr.as_posix())
    application.render(data_writer, 1, 1)
    tool = Path.cwd() / 'build/msys64/ucrt64/bin/oiiotool.exe'
    stats = subprocess.run([str(tool), str(exr), '--ch', 'Reflection.R,Reflection.G,Reflection.B', '--printstats'], capture_output=True, text=True, check=True).stdout
    maximum = re.search(r'Stats Max:\s+([-+0-9.eE]+)', stats)
    minimum = re.search(r'Stats Min:\s+([-+0-9.eE]+)', stats)
    assert maximum and minimum, stats
    report['reflection_red_range'] = [float(minimum.group(1)), float(maximum.group(1))]
    assert max(abs(v) for v in report['reflection_red_range']) > .005, report
    project = directory / ('reflection-catcher-' + suffix + '.ntp')
    assert application.saveProjectAs(project.as_posix())
    application = application.loadProject(project.as_posix())
    writer = application.getNode('LayoutOutput')
    restored = capture('restored')
    assert difference(final, restored) < .1, report
    restored_catcher = application.getNode('ShadowCatcher1')
    assert restored_catcher.getParam('catchReflections').getValue()
    assert abs(restored_catcher.getParam('reflectionRoughness').getValue()-.15) < 1e-8
    assert hashlib.sha256(source.read_bytes()).digest() == checksum
    report.update(ok=True, project=project.as_posix(), exr=exr.as_posix(), source_unchanged=True, reload_mean_error=difference(final, restored))
except Exception as error:
    report.update(error=str(error), traceback=traceback.format_exc())
finally:
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
