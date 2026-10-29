"""Real RGB plate: USD asset shadows, proxy occlusion, EXR outputs and reload.

Requires reconstruction-layout-smoke.py's saved project. Set
NATRON_SHADOW_BACKEND=hdNVP to exercise the composed USD renderer.
"""
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import traceback
from pathlib import Path
from PySide6.QtGui import QImage

directory = Path.cwd() / 'build/camera-tracker-real'
backend = os.environ.get('NATRON_SHADOW_BACKEND', 'CPU')
suffix = backend.lower()
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE'], 'backend': backend}


def pixels(image):
    converted = image.convertToFormat(QImage.Format_RGBA8888)
    return bytes(converted.constBits())


def difference(a, b):
    a, b = pixels(a), pixels(b)
    return max(abs(x-y) for x, y in zip(a, b)), sum(x != y for x, y in zip(a, b))


def mean_difference(a, b):
    a, b = pixels(a), pixels(b)
    return sum(abs(x-y) for x, y in zip(a, b)) / len(a)


try:
    source_file = Path.cwd() / 'Tests/fixtures/scene3d/usd/layout-asset.usda'
    digest = hashlib.sha256(source_file.read_bytes()).hexdigest()
    application = app.loadProject((directory / 'reconstruction-layout.ntp').as_posix())
    assert application
    setup = application.getNode('SceneSetup1')
    render = application.getNode('Render3D1')
    asset = application.getNode('ReadUSD1')
    assert setup and render and asset
    render.getParam('renderer').setValue(backend)
    render.getParam('antialiasing').setValue('2x2')
    render.getParam('pathSamples').setValue(32)
    render.getParam('pathDenoiser').setValue('None')
    if backend == 'hdNVP':
        # Outline adds selective USD opinions downstream of SceneSetup. Its
        # placement actions must remain usable through that linear edit chain.
        edit = application.createNode('fr.inria.built-in.USDCompositing')
        edit.getParam('primPaths').setValue('/Natron/Assets/n_' + asset.getParam('usdIdentity').getValue() + '/Source/Box')
        assert edit.connectInput(0, setup)
        render.disconnectInput(0)
        assert render.connectInput(0, edit)
        report['downstream_usd_edit'] = edit.getScriptName()
    # A visible projected plate is replaced by the transparent catcher ground.
    for node in application.getChildren():
        if node.getPluginID() == 'fr.inria.built-in.Light' and node.getParam('lightType').getValue() == 3:
            node.getParam('shadows').setValue(False)
        if node.getPluginID() == 'fr.inria.built-in.Card' and node.getInput(0) and node.getInput(0).getPluginID() == 'fr.inria.built-in.Project3D':
            node.getParam('opacity').setValue(0.)
    setup.getParam('selectedLandmarks').setValue('49')
    setup.getParam('placementSize').setValue(1.65)
    setup.getParam('createShadowCatcher').trigger()
    catcher = application.getNode('ShadowCatcher1')
    assert catcher, setup.getParam('layoutStatus').getValue()
    setup.getParam('createKeyLight').trigger()
    light = setup.getInput(0).getInput(1)
    assert light.getPluginID() == 'fr.inria.built-in.Light', setup.getParam('layoutStatus').getValue()
    writer = application.getNode('LayoutOutput')
    assert writer

    def capture(name):
        (directory / ('catcher-progress-' + suffix + '.txt')).write_text(name, encoding='utf-8')
        target = directory / (name + '-' + suffix + '.png')
        writer.getParam('filename').setValue(target.as_posix())
        application.render(writer, 1, 1)
        result = QImage(str(target))
        assert result.width() == 640 and result.height() == 480, target
        return result

    catcher.getParam('shadowDensity').setValue(0.)
    no_shadow = capture('catcher-no-shadow')
    catcher.getParam('shadowDensity').setValue(1.)
    with_shadow = capture('catcher-shadow')
    report['shadow_image_difference'] = difference(no_shadow, with_shadow)
    assert report['shadow_image_difference'][1] > 200, report
    render.getParam('output').setValue('ShadowPreview')
    shadow_mask = capture('catcher-shadow-mask')
    assert max(pixels(shadow_mask)[0::4]) > 30, 'Shadow preview is empty'
    render.getParam('output').setValue('Color')

    setup.getParam('placementSize').setValue(.1)
    setup.getParam('createOccluder').trigger()
    proxy = application.getNode('Occluder1')
    assert proxy, setup.getParam('layoutStatus').getValue()
    proxy.getParam('shadowVisible').setValue(False)
    card = proxy.getInput(0)
    card.getParam('size').set(.09, .15)
    # Pixel (317,224), depth 35 in the accepted solve's initial camera frame.
    # The card faces the solved camera, and its scale already includes the
    # inverse SceneSetup frame, so these are the upstream raw coordinates.
    card.getParam('translate').set((317-320.6)/535.4*35, -(224-248.1)/539.2*35, -35.)
    front = capture('catcher-occluder-front')
    report['front_occluder_difference'] = difference(with_shadow, front)
    assert report['front_occluder_difference'][1] > 200, report
    render.getParam('output').setValue('OcclusionPreview')
    occlusion_mask = capture('catcher-occlusion-mask')
    assert max(pixels(occlusion_mask)[0::4]) > 200
    render.getParam('output').setValue('Color')
    card.getParam('translate').set((317-320.6)/535.4*35, -(224-248.1)/539.2*35, -50.)
    back = capture('catcher-occluder-back')
    report['back_occluder_difference'] = difference(with_shadow, back)
    report['back_occluder_mean_error'] = mean_difference(with_shadow, back)
    if backend == 'hdNVP':
        # Path-traced lighting can use different stochastic sequences after a
        # scene edit; depth/matte assertions independently verify ordering.
        assert report['back_occluder_mean_error'] < .5 and report['back_occluder_difference'][0] < 25, report
    else:
        assert report['back_occluder_difference'][1] < 100, report
    render.getParam('output').setValue('OcclusionPreview')
    back_mask = capture('catcher-occlusion-back-mask')
    assert max(pixels(back_mask)[0::4]) == 0, 'Back proxy still occludes visible geometry'
    render.getParam('output').setValue('Color')
    card.getParam('translate').set((317-320.6)/535.4*35, -(224-248.1)/539.2*35, -35.)

    layers = [p.getLayerName() for p in render.getAvailableLayers(-1)]
    assert 'Shadow' in layers and 'Occlusion' in layers, layers
    exr = directory / ('catcher-outputs-' + suffix + '.exr')
    data_writer = application.createNode('fr.inria.openfx.WriteOIIO')
    data_writer.setScriptName('CatcherDataOutput')
    assert data_writer.connectInput(0, render)
    for name, value in [('filename', exr.as_posix()), ('bitDepth', '32f'), ('processAllPlanes', True),
                        ('partSplitting', 'single'), ('ocioOutputSpace', 'linear')]:
        data_writer.getParam(name).setValue(value)
    application.render(data_writer, 1, 1)
    tool = shutil.which('oiiotool') or str(Path.cwd() / 'build/msys64/ucrt64/bin/oiiotool.exe')
    assert Path(tool).is_file(), tool
    stats = {}
    for channel in ['Shadow.A', 'Occlusion.R', 'Depth.Z']:
        output = subprocess.run([tool, str(exr), '--ch', channel, '--printstats'], capture_output=True, text=True, check=True).stdout
        maximum = re.search(r'Stats Max:\s+([-+0-9.eE]+)', output)
        assert maximum, output
        stats[channel] = float(maximum.group(1))
    assert stats['Shadow.A'] > .05 and stats['Occlusion.R'] > .9 and stats['Depth.Z'] > 0, stats
    report['exr_maxima'] = stats
    project = directory / ('shadow-occlusion-' + suffix + '.ntp')
    assert application.saveProjectAs(project.as_posix())
    application = application.loadProject(project.as_posix())
    writer = application.getNode('LayoutOutput')
    restored = capture('catcher-restored')
    report['reload_image_difference'] = difference(front, restored)
    report['reload_mean_error'] = mean_difference(front, restored)
    assert report['reload_mean_error'] < .5 if backend == 'hdNVP' else report['reload_image_difference'][1] < 100, report
    assert hashlib.sha256(source_file.read_bytes()).hexdigest() == digest
    report.update(ok=True, project=project.as_posix(), image=(directory / ('catcher-occluder-front-' + suffix + '.png')).as_posix(), source_unchanged=True)
except Exception as error:
    report.update(error=str(error), traceback=traceback.format_exc())
finally:
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
