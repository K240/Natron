"""Real-plate SceneSetup, shared frame, projection and USD placement regression.

Run camera-tracker-smoke.py first to produce the accepted RGB-only solve.
All test outputs remain under build/camera-tracker-real.
"""
import base64
import hashlib
import json
import math
import os
import traceback
import zlib
from pathlib import Path

from PySide6.QtGui import QImage

directory = Path.cwd() / 'build/camera-tracker-real'
report = {'ok': False, 'profile': os.environ['NATRON_TEST_PROFILE']}


def unpack(value):
    return json.loads(zlib.decompress(base64.b64decode(value)[4:]))


def pack(value):
    data = json.dumps(value, separators=(',', ':')).encode()
    return base64.b64encode(len(data).to_bytes(4, 'big') + zlib.compress(data)).decode()


def point(matrix, p):
    return [sum(matrix[c * 4 + r] * p[c] for c in range(3)) + matrix[12 + r] for r in range(3)]


def project(solution, p):
    m = solution['cameras'][0]['matrix']
    v = [sum(m[r * 4 + c] * (p[c] - m[12 + c]) for c in range(3)) for r in range(3)]
    fx, fy, cx, cy = solution['calibration'][:4]
    return [cx + fx * v[0] / -v[2], cy - fy * v[1] / -v[2]]


def difference(a, b):
    assert a.size() == b.size() and not a.isNull()
    converted_a = a.convertToFormat(QImage.Format_RGBA8888)
    converted_b = b.convertToFormat(QImage.Format_RGBA8888)
    a = bytes(converted_a.constBits())
    b = bytes(converted_b.constBits())
    return max(abs(x - y) for x, y in zip(a, b)), sum(x != y for x, y in zip(a, b))


try:
    application = app.loadProject((directory / 'camera-tracker.ntp').as_posix())
    assert application, 'Run tools/camera-tracker-smoke.py first'
    tracker = application.getNode('PlateCameraTracker')
    setup = application.getNode('SceneSetup1')
    render = application.getNode('Render3D1')
    source = application.getNode('ReconstructionScene1')
    assert setup and render and source
    solution = unpack(tracker.getParam('reconstructionData').getValue())
    (directory / 'layout-solution.json').write_text(json.dumps(solution), encoding='utf-8')
    locations = [(p, project(solution, p[1:4])) for p in solution['points'] if p[5] < .4]
    # The three targets are on the office tabletop, away from the bottles.
    # This distance is an example scale constraint, not dataset ground truth.
    chosen = [min(locations, key=lambda p: math.dist(p[1], target))
              for target in [(56, 240), (317, 248), (590, 238)]]
    selected_ids = [str(p[0][0]) for p in chosen]
    assert len(set(selected_ids)) == 3, chosen
    report['ground_landmarks'] = [{'id': p[0][0], 'pixel': p[1], 'raw': p[0][1:4]} for p in chosen]

    def node(kind, name):
        n = application.createNode('fr.inria.built-in.' + kind)
        assert n
        n.setScriptName(name)
        return n

    def action(name):
        (directory / 'layout-progress.txt').write_text(name, encoding='utf-8')
        setup.getParam(name).trigger()
        status = setup.getParam('layoutStatus').getValue()
        assert status.startswith(('Ground aligned.', 'Scale ', 'Geometry placed', 'Scene frame reset')), (name, status)
        return status

    def select(values):
        setup.getParam('selectedLandmarks').setValue(' '.join(map(str, values)))

    environment = node('Light', 'LayoutEnvironment')
    environment.getParam('lightType').setValue('Environment')
    environment.getParam('intensity').setValue(.65)
    image = application.createNode('net.sf.openfx.ConstantPlugin')
    image.getParam('color').set(.1, .8, .2, 1.)
    marker = node('Card', 'ReprojectionMarker')
    marker.getParam('translate').set(*solution['points'][len(solution['points']) // 2][1:4])
    marker.getParam('size').set(.3, .3)
    assert marker.connectInput(0, image)
    merge = node('Scene', 'LayoutInput')
    assert merge.connectInput(0, source) and merge.connectInput(1, marker) and merge.connectInput(2, environment)
    setup.disconnectInput(0)
    assert setup.connectInput(0, merge)
    render.getParam('renderer').setValue('CPU')
    render.getParam('antialiasing').setValue('1x')
    writer = application.createNode('fr.inria.openfx.WriteOIIO')
    writer.setScriptName('LayoutOutput')
    assert writer.connectInput(0, render)

    def capture(name, frame=1, owner=None):
        (directory / 'layout-progress.txt').write_text('capture ' + name, encoding='utf-8')
        owner = owner or application
        target = directory / ('layout-' + name + '.png')
        owner.getNode('LayoutOutput').getParam('filename').setValue(target.as_posix())
        owner.render(owner.getNode('LayoutOutput'), frame, frame)
        result = QImage(str(target))
        assert result.size().width() == 640 and result.size().height() == 480, str(target)
        return result

    before = capture('before')
    select(selected_ids)
    action('setGround')
    select([selected_ids[1]])
    action('setOrigin')
    select(selected_ids[:2])
    setup.getParam('knownDistance').setValue(.8)
    action('setScale')
    saved_frame = setup.getParam('sceneFrame').getValue()
    frame = json.loads(saved_frame)
    aligned = [point(frame['matrix'], p[0][1:4]) for p in chosen]
    assert max(abs(p[1]) for p in aligned) < 1e-7, aligned
    assert math.hypot(aligned[1][0], aligned[1][2]) < 1e-7, aligned
    assert abs(math.dist(aligned[0], aligned[1]) - .8) < 1e-7
    report['scene_frame'] = frame
    after = capture('aligned')
    report['shared_frame_image_difference'] = difference(before, after)
    assert report['shared_frame_image_difference'][0] <= 2, report
    merge.disconnectInput(1)

    select([selected_ids[1]])
    action('createAxis')
    assert application.getNode('Axis1')
    setup.getParam('placementSize').setValue(1.65)
    setup.getParam('projectionFrame').setValue(1)
    background = render.getInput(2)
    before_input = setup.getInput(0).getScriptName()
    render.disconnectInput(2)
    setup.getParam('createProjectedGround').trigger()
    assert 'background' in setup.getParam('layoutStatus').getValue()
    assert setup.getInput(0).getScriptName() == before_input and not application.getNode('SceneCamera2'), 'Failed projection left partial nodes'
    assert render.connectInput(2, background)
    report['failed_placement_rolled_back'] = True
    action('createProjectedGround')
    projection = application.getNode('Project3D1')
    assert projection and projection.getInput(1).getInput(0).getScriptName() != setup.getScriptName(), 'Projection camera formed a feedback loop'
    projected = capture('projected')
    fixture = Path.cwd() / 'Tests/fixtures/scene3d/usd/layout-asset.usda'
    checksum = hashlib.sha256(fixture.read_bytes()).hexdigest()
    setup.getParam('assetFile').setValue(fixture.as_posix())
    setup.getParam('assetScale').setValue(.16)
    action('placeAsset')
    asset = application.getNode('ReadUSD1')
    assert asset and asset.getParam('usdAssembly').getValue() == 1
    final = capture('composite')
    report['placed_asset_difference'] = difference(projected, final)
    assert report['placed_asset_difference'][1] > 500, report
    capture('composite-12', 12)

    # Changing the shared frame after placement must keep the plate projection.
    select(selected_ids[:2])
    setup.getParam('knownDistance').setValue(1.1)
    action('setScale')
    changed_scale = capture('rescaled')
    report['placed_rescale_difference'] = difference(final, changed_scale)
    # Independent renderer ray/UV calculations can move an edge sample after
    # uniform rescaling. Bound the affected channels rather than mask geometry.
    assert report['placed_rescale_difference'][0] <= 16 and report['placed_rescale_difference'][1] < 100, report
    setup.getParam('sceneFrame').setValue(saved_frame)
    select([selected_ids[1]])

    # Refuse an old layout after a changed solve; preserve the accepted frame.
    original = tracker.getParam('reconstructionData').getValue()
    replacement = dict(solution)
    replacement['usd'] += '\n# changed accepted solve\n'
    tracker.getParam('reconstructionData').setValue(pack(replacement))
    setup.getParam('setOrigin').trigger()
    assert 'camera solve changed' in setup.getParam('layoutStatus').getValue().lower()
    assert setup.getParam('sceneFrame').getValue() == saved_frame
    tracker.getParam('reconstructionData').setValue(original)
    assert hashlib.sha256(fixture.read_bytes()).hexdigest() == checksum, 'Source asset was modified'
    report['stale_layout_rejected'] = True
    report['source_asset_unchanged'] = True

    project = directory / 'reconstruction-layout.ntp'
    assert application.saveProjectAs(project.as_posix())
    loaded = application.loadProject(project.as_posix())
    assert loaded and loaded.getNode('SceneSetup1').getParam('sceneFrame').getValue() == saved_frame
    restored = capture('restored', owner=loaded)
    report['reload_image_difference'] = difference(final, restored)
    assert report['reload_image_difference'] == (0, 0), report
    report.update(ok=True, project=project.as_posix(), composite=(directory / 'layout-composite.png').as_posix())
except Exception as error:
    report.update(error=str(error), traceback=traceback.format_exc())
finally:
    Path(os.environ['NATRON_GUI_TEST_RESULT']).write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report, indent=2))
