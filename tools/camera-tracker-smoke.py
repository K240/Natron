"""Real-plate CameraTracker analysis, persistence and USD graph integration."""
import base64
import json
import os
import time
import traceback
import zlib
import math
from pathlib import Path
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage

directory = Path.cwd() / 'build/camera-tracker-real'
directory.mkdir(parents=True, exist_ok=True)
report = {'ok': False, 'profile': os.environ.get('NATRON_TEST_PROFILE')}
auto_lens = os.environ.get('NATRON_CAMERA_AUTO') == '1'
refine_principal = os.environ.get('NATRON_CAMERA_PRINCIPAL') == '1'
last_frame = int(os.environ.get('NATRON_CAMERA_FRAMES', '45'))


def blob(node, name):
    value = node.getParam(name).getValue()
    return json.loads(zlib.decompress(base64.b64decode(value)[4:])) if value else None


def wait(node, timeout=180):
    started = time.monotonic()
    pulses = 0
    while node.getParam('analysisRunning').getValue():
        QCoreApplication.processEvents()
        pulses += 1
        if time.monotonic() - started > timeout:
            raise RuntimeError(node.getParam('analysisStatus').getValue())
        time.sleep(.01)
    QCoreApplication.processEvents()
    return {'seconds': time.monotonic() - started, 'event_pulses': pulses,
            'status': node.getParam('analysisStatus').getValue()}


try:
    app.addFormat('CameraTrackerPlate 640x480 1')
    app.getProjectParam('outputFormat').setValue('CameraTrackerPlate')
    app.getProjectParam('frameRange').set(1, last_frame)
    reader = app.createReader((directory / 'plate/plate.####.png').as_posix())
    assert reader
    reader.setScriptName('TrackingPlate')
    tracker = app.createNode('fr.inria.built-in.CameraTracker')
    assert tracker and tracker.connectInput(0, reader)
    tracker.setScriptName('PlateCameraTracker')
    for key, value in [('firstFrame', 1), ('lastFrame', last_frame), ('maximumFeatures', 180),
                       ('minimumTrackLength', 8)]:
        tracker.getParam(key).setValue(value)
    if auto_lens:
        tracker.getParam('calibrationMode').setValue('AutoLens')
        tracker.getParam('refinePrincipalPoint').setValue(refine_principal)
    else:
        for key, value in [('pixelIntrinsics', True), ('focalPixelsX', 535.4), ('focalPixelsY', 539.2),
                           ('principalPointX', 320.6), ('principalPointY', 248.1)]:
            tracker.getParam(key).setValue(value)
    tracker.getParam('track').trigger()
    report['track'] = wait(tracker)
    data = blob(tracker, 'trackingData')
    assert data and len(data['observations']) > 500, report
    tracker.getParam('solve').trigger()
    report['solve'] = wait(tracker)
    result = blob(tracker, 'reconstructionData')
    assert result and len(result['cameras']) == last_frame and len(result['points']) >= 30, report
    assert result['rms'] < 1.5, result['rms']
    report.update(camera_count=len(result['cameras']), point_count=len(result['points']), rms=result['rms'])
    report['calibration'] = result['calibration']
    report['estimated_principal_point'] = result.get('estimatedPrincipalPoint', False)
    report['principal_validation_gain'] = result.get('principalPointValidationGain', 0)
    if refine_principal:
        assert 'estimatedPrincipalPoint' in result and 'principalPointValidationGain' in result
        assert (result['principalPointValidationGain'] > .01) == result['estimatedPrincipalPoint']
    if auto_lens:
        assert result['estimatedIntrinsics'] and 160 < result['calibration'][0] < 2000
    passthrough = app.createNode('fr.inria.openfx.WriteOIIO')
    assert passthrough.connectInput(0, tracker)
    passthrough.getParam('filename').setValue((directory / 'tracked-identity.png').as_posix())
    app.render(passthrough, 12, 12)
    identity = QImage(str(directory / 'tracked-identity.png'))
    assert identity.width() == 640 and identity.height() == 480
    assert max(identity.pixelColor(100, 100).getRgb()[:3]) > 20, 'CameraTracker image output is black'
    report['identity_image'] = True
    (directory / 'native-reconstruction.usda').write_text(result['usd'], encoding='utf-8')
    tracker.getParam('createScene').trigger()
    scene = app.getNode('ReconstructionScene1')
    camera = app.getNode('SceneCamera1')
    setup = app.getNode('SceneSetup1')
    render = app.getNode('Render3D1')
    assert scene and camera and render, tracker.getParam('analysisStatus').getValue()
    assert scene.getInput(0).getScriptName() == tracker.getScriptName()
    assert setup and setup.getInput(0).getScriptName() == scene.getScriptName()
    assert camera.getInput(0).getScriptName() == setup.getScriptName()
    if auto_lens:
        correction = render.getInput(2)
        assert correction and correction.getPluginID() == 'fr.inria.built-in.CameraTrackerUndistort'
        corrected = app.createNode('fr.inria.openfx.WriteOIIO')
        assert corrected.connectInput(0, correction)
        corrected.getParam('filename').setValue((directory / 'undistorted-plate.png').as_posix())
        app.render(corrected, 12, 12)
        assert QImage(str(directory / 'undistorted-plate.png')).size() == identity.size()
        report['undistorted_plate'] = True
    project = directory / ('camera-tracker-auto.ntp' if auto_lens else 'camera-tracker.ntp')
    assert app.saveProjectAs(project.as_posix())
    report['project'] = project.as_posix()
    loaded = app.loadProject(project.as_posix())
    assert loaded
    restored = loaded.getNode('PlateCameraTracker')
    restored_result = blob(restored, 'reconstructionData')
    assert restored_result['usd'] == result['usd'], 'Accepted USD solve was not persisted'
    assert restored_result['calibration'] == result['calibration']
    assert restored_result.get('estimatedPrincipalPoint') == result.get('estimatedPrincipalPoint')
    assert not restored.getParam('analysisRunning').getValue()
    for _ in range(20):
        QCoreApplication.processEvents()
        time.sleep(.01)
    report['restored_status'] = restored.getParam('analysisStatus').getValue()
    assert 'stale' not in report['restored_status'], report['restored_status']
    # A card at a solved landmark exercises the scene camera through Render3D.
    point = result['points'][len(result['points']) // 2]
    image = loaded.createNode('net.sf.openfx.ConstantPlugin')
    assert image
    image.getParam('color').set(.1, .8, .2, 1.)
    card = loaded.createNode('fr.inria.built-in.Card')
    assert card.connectInput(0, image)
    card.getParam('translate').set(*point[1:4])
    card.getParam('size').set(.25, .25)
    combined = loaded.createNode('fr.inria.built-in.Scene')
    assert combined.connectInput(0, loaded.getNode('ReconstructionScene1'))
    assert combined.connectInput(1, card)
    render = loaded.getNode('Render3D1')
    setup = loaded.getNode('SceneSetup1')
    setup.disconnectInput(0)
    assert setup.connectInput(0, combined)
    render.getParam('renderer').setValue('CPU')
    writer = loaded.createNode('fr.inria.openfx.WriteOIIO')
    assert writer.connectInput(0, render)
    writer.getParam('filename').setValue((directory / 'tracked-card.exr').as_posix())
    writer.getParam('bitDepth').setValue('32f')
    loaded.render(writer, 1, 1)
    assert (directory / 'tracked-card.exr').exists()
    # A prescribed calibration isolates the image warp from solve quality.
    # Compare with an independent pixel-center/Brown formula on smooth samples.
    fixture = loaded.createNode('fr.inria.built-in.CameraTracker')
    assert fixture.connectInput(0, loaded.getNode('TrackingPlate'))
    lens_data = dict(restored_result)
    lens_data['calibration'] = [640., 680., 308., 252., -.12, .035]
    encoded = json.dumps(lens_data, separators=(',', ':')).encode()
    fixture.getParam('reconstructionData').setValue(base64.b64encode(len(encoded).to_bytes(4, 'big') + zlib.compress(encoded)).decode())
    warp = loaded.createNode('fr.inria.built-in.CameraTrackerUndistort')
    assert warp.connectInput(0, loaded.getNode('TrackingPlate')) and warp.connectInput(1, fixture)
    check = loaded.createNode('fr.inria.openfx.WriteOIIO')
    assert check.connectInput(0, warp)
    check.getParam('filename').setValue((directory / 'prescribed-lens.png').as_posix())
    loaded.render(check, 12, 12)
    corrected = QImage(str(directory / 'prescribed-lens.png'))
    errors = []
    for y in range(20, 460, 27):
        for x in range(20, 620, 29):
            u, v = (x + .5 - 308) / 640, (y + .5 - 252) / 680
            r2 = u * u + v * v
            radial = 1 - .12 * r2 + .035 * r2 * r2
            sx, sy = 308 + 640 * u * radial - .5, 252 + 680 * v * radial - .5
            ix, iy = math.floor(sx), math.floor(sy)
            samples = [identity.pixelColor(ix + dx, iy + dy).getRgb()[:3] for dy in range(2) for dx in range(2)]
            if any(max(s[c] for s in samples) - min(s[c] for s in samples) > 15 for c in range(3)):
                continue
            dx, dy = sx - ix, sy - iy
            expected = [sum(samples[row * 2 + col][c] * (dx if col else 1 - dx) * (dy if row else 1 - dy)
                            for row in range(2) for col in range(2)) for c in range(3)]
            actual = corrected.pixelColor(x, y).getRgb()[:3]
            errors.extend(abs(a - b) for a, b in zip(actual, expected))
    assert len(errors) > 100 and max(errors) < 4, ('Lens image warp mismatch', max(errors, default=-1))
    report['warp_pixel_error'] = max(errors)
    report['ok'] = True
except Exception as error:
    report.update(error=str(error), traceback=traceback.format_exc())
finally:
    result_path = Path(os.environ.get('NATRON_GUI_TEST_RESULT', directory / 'native-result.json'))
    result_path.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
