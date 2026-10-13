"""Run in NatronRenderer: validate native 3D nodes, projection, animation and project IO."""
import json
import os
import re
import shutil
import subprocess
import traceback
from pathlib import Path
import NatronEngine
from PySide6.QtCore import QCoreApplication
from PySide6.QtGui import QImage, QColor

cpu_only = os.environ.get('NATRON_VULKAN_DISABLED') == '1'
suffix = os.environ.get('NATRON_VULKAN_DEVICE', 'auto') + ('-cpu-only' if cpu_only else '')
output = Path.cwd() / 'build' / ('scene3d-smoke-' + suffix)
output.mkdir(parents=True, exist_ok=True)
report = {'ok': False, 'renders': {}, 'device': os.environ.get('NATRON_VULKAN_DEVICE', 'auto')}

def node(name):
    result = app.createNode('fr.inria.built-in.' + name)
    assert result is not None, name
    return result

def replace(effect, index, source):
    effect.disconnectInput(index)
    assert effect.connectInput(index, source)

def fixture(name, color, gradient=False):
    image = QImage(96, 64, QImage.Format_RGBA8888)
    image.fill(QColor(*color))
    if gradient:
        for y in range(64):
            for x in range(96):
                image.setPixelColor(x, y, QColor(int((x+.5)*255/96), int((63-y+.5)*255/64), 32, 255))
    path = output / (name + '.png')
    assert image.save(str(path))
    return app.createReader(path.as_posix())

def render(effect, name, frame=1, application=None):
    application = application or app
    destination = output / (name + '.png')
    destination.unlink(missing_ok=True)
    writer = application.createWriter(destination.as_posix())
    assert writer and writer.connectInput(0, effect), name
    QCoreApplication.processEvents()
    application.render(writer, frame, frame)
    image = QImage(str(destination)).convertToFormat(QImage.Format_RGBA8888)
    assert not image.isNull() and image.size().width() == 96 and image.size().height() == 64, name
    report['renders'][name] = {'center': image.pixelColor(48, 32).getRgb(), 'frame': frame}
    return image

def depth_layers(application, effect, name, frame=1):
    path = output/(name+'.exr')
    path.unlink(missing_ok=True)
    writer = application.createNode('fr.inria.openfx.WriteOIIO')
    assert writer and writer.connectInput(0, effect)
    writer.getParam('filename').setValue(path.as_posix())
    writer.getParam('bitDepth').setValue('32f')
    writer.getParam('processAllPlanes').setValue(True)
    writer.getParam('partSplitting').setValue('single')
    writer.getParam('ocioOutputSpace').setValue('linear')
    QCoreApplication.processEvents()
    application.render(writer, frame, frame)
    assert path.exists()
    return path

def oiio_tool():
    executable = os.environ.get('OIIO_TOOL') or shutil.which('oiiotool')
    if not executable:
        candidate = Path('build/msys64/ucrt64/bin/oiiotool.exe')
        executable = str(candidate.resolve()) if candidate.exists() else None
    assert executable, 'Set OIIO_TOOL to validate float EXR depth values'
    return executable

def depth_sample(path, x=48, y=32):
    result = subprocess.run([oiio_tool(), str(path), '--ch', 'Depth.Z', '--cut', f'1x1+{x}+{y}', '--printstats'], capture_output=True, text=True, check=True)
    match = re.search(r'Stats Min:\s+([-+0-9.eE]+)', result.stdout)
    assert match, result.stdout+result.stderr
    return float(match.group(1))

try:
    assert all('fr.inria.built-in.' + name in NatronEngine.natron.getPluginIDs()
               for name in ('Card', 'ReadGeo', 'Camera', 'ReadCamera', 'Axis', 'TransformGeo', 'Scene', 'Project3D', 'Render3D'))
    app.addFormat('Scene3DSmoke 96x64 1')
    app.getProjectParam('outputFormat').setValue('Scene3DSmoke')
    app.getProjectParam('frameRange').set(1, 3)
    red = fixture('red', (255, 0, 0, 128))
    blue = fixture('blue', (0, 0, 255, 255))
    green = fixture('green', (0, 255, 0, 128))
    gradient = fixture('gradient', (0, 0, 0, 255), True)
    card, rear, middle = node('Card'), node('Card'), node('Card')
    card.setScriptName('ForegroundCard')
    for effect, source in ((card, red), (rear, blue), (middle, green)):
        assert effect.connectInput(0, source)
        effect.getParam('size').set(4., 4.)
    rear.getParam('translate').set(0., 0., -2.)
    middle.getParam('translate').set(0., 0., -1.)
    scene, camera, renderer = node('Scene'), node('Camera'), node('Render3D')
    renderer.setScriptName('MainRender3D')
    assert scene.connectInput(0, rear) and scene.connectInput(1, middle) and scene.connectInput(2, card)
    assert renderer.connectInput(0, scene) and renderer.connectInput(1, camera)
    assert not scene.connectInput(3, red), 'Image accepted as geometry'
    assert not renderer.connectInput(2, camera), 'Camera accepted as background'
    assert not card.connectInput(1, camera), 'Camera accepted as Axis'
    invert = app.createNode('net.sf.openfx.Invert')
    assert not invert.connectInput(0, card), '3D geometry accepted by a 2D image node'
    pending_dot = node('Dot')
    assert invert.connectInput(0, pending_dot)
    assert not pending_dot.connectInput(0, camera), 'Retyping a Dot invalidated a downstream image input'
    assert pending_dot.connectInput(0, red)
    renderer.getParam('renderer').setValue('CPU')
    cpu = render(renderer, 'three-cards-cpu')
    center = cpu.pixelColor(48, 32)
    assert 180 < center.red() < 195 and 130 < center.green() < 145 and 130 < center.blue() < 145 and center.alpha() == 255, center.getRgb()
    renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    gpu = render(renderer, 'three-cards-vulkan')
    error = max(abs(a-b) for a, b in zip(bytes(cpu.constBits()), bytes(gpu.constBits())))
    assert error <= 1, error
    report['cpu_gpu_max_code_difference'] = error
    many, extended = node('Scene'), node('Scene')
    for index in range(16):
        assert many.connectInput(index, card)
    assert extended.connectInput(0, many) and extended.connectInput(1, rear)
    replace(renderer, 0, extended)
    renderer.getParam('renderer').setValue('Auto')
    larger_auto = render(renderer, 'seventeen-cards-auto')
    renderer.getParam('renderer').setValue('CPU')
    larger_cpu = render(renderer, 'seventeen-cards-cpu')
    assert bytes(larger_auto.constBits()) == bytes(larger_cpu.constBits())
    assert larger_auto.pixelColor(48,32).red() > 250
    renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    axis, transformed = node('Axis'), node('TransformGeo')
    assert transformed.connectInput(0, scene) and transformed.connectInput(1, axis)
    replace(renderer, 0, transformed)
    axis.getParam('translate').set(10., 0., 0.)
    assert render(renderer, 'axis-offscreen').pixelColor(48, 32).alpha() == 0
    axis.getParam('translate').set(0., 0., 0.)
    # Animated render camera; geometry stays in world space.
    translate = camera.getParam('translate')
    translate.setValueAtTime(0., 1., 0)
    translate.setValueAtTime(.6, 2., 0)
    one = render(renderer, 'camera-frame1', 1)
    two = render(renderer, 'camera-frame2', 2)
    assert bytes(one.constBits()) != bytes(two.constBits()), 'Camera keys did not change the rendered image'
    projector, projection, surface = node('Camera'), node('Project3D'), node('Card')
    projection.setScriptName('FixedProjection')
    assert projection.connectInput(0, gradient) and projection.connectInput(1, projector)
    assert surface.connectInput(0, projection)
    surface.getParam('size').set(4., 4.)
    replace(renderer, 0, surface)
    project_translate = projector.getParam('translate')
    project_translate.setValueAtTime(0., 1., 0)
    project_translate.setValueAtTime(.7, 2., 0)
    projected_one = render(renderer, 'projection-reference', 1)
    projected_two = render(renderer, 'projection-parallax', 2)
    assert projected_two.pixelColor(48,32).red() > projected_one.pixelColor(48,32).red()+20
    projection.getParam('freeze').setValue(False)
    live = render(renderer, 'projection-live', 2)
    assert live.pixelColor(48,32).red() < projected_two.pixelColor(48,32).red()-20
    projection.getParam('freeze').setValue(True)
    renderer.getParam('renderer').setValue('CPU')
    crop = app.createNode('net.sf.openfx.CropPlugin')
    assert crop.connectInput(0, gradient)
    crop.getParam('bottomLeft').set(24., 0.)
    crop.getParam('size').set(48., 64.)
    replace(projection, 0, crop)
    cropped_cpu = render(renderer, 'cropped-projection-cpu')
    assert cropped_cpu.pixelColor(48,32).getRgb() == projected_one.pixelColor(48,32).getRgb()
    assert cropped_cpu.pixelColor(8,32).alpha() == 0, 'Cropped source was stretched across the projection'
    renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    cropped_gpu = render(renderer, 'cropped-projection-vulkan')
    assert max(abs(a-b) for a,b in zip(bytes(cropped_cpu.constBits()), bytes(cropped_gpu.constBits()))) <= 1
    replace(projection, 0, gradient)
    renderer.getParam('renderer').setValue('CPU')
    assert renderer.connectInput(2, blue)
    background = render(renderer, 'background')
    assert background.pixelColor(0, 0).alpha() == 255
    image_card, outer_renderer = node('Card'), node('Render3D')
    assert image_card.connectInput(0, renderer), 'Render3D output was rejected as a normal image texture'
    image_card.getParam('size').set(4., 4.)
    assert outer_renderer.connectInput(0, image_card) and outer_renderer.connectInput(1, projector)
    outer_renderer.getParam('renderer').setValue('CPU')
    assert render(outer_renderer, 'rendered-image-texture').pixelColor(48,32).alpha() == 255
    group = node('Group')
    group_input = group.getNode('Input1') or app.createNode('fr.inria.built-in.Input', -1, group)
    group_output = group.getNode('Output1') or app.createNode('fr.inria.built-in.Output', -1, group)
    grouped_card = app.createNode('fr.inria.built-in.Card', -1, group)
    grouped_card.getParam('size').set(4., 4.)
    assert group.connectInput(0, blue) and grouped_card.connectInput(0, group_input)
    replace(group_output, 0, grouped_card)
    replace(renderer, 0, group)
    grouped = render(renderer, 'grouped-geometry')
    assert grouped.pixelColor(48,32).blue() > 250 and grouped.pixelColor(48,32).red() < 2
    nested = node('Group')
    nested_input = nested.getNode('Input1')
    nested_output = nested.getNode('Output1')
    nested_transform = app.createNode('fr.inria.built-in.TransformGeo', -1, nested)
    # Empty group input can be wired before its external geometry source.
    assert nested_transform.connectInput(0, nested_input)
    replace(nested_output, 0, nested_transform)
    assert nested.connectInput(0, group)
    replace(renderer, 0, nested)
    nested_image = render(renderer, 'nested-geometry')
    assert bytes(nested_image.constBits()) == bytes(grouped.constBits())
    camera_group = node('Group')
    grouped_projection = app.createNode('fr.inria.built-in.Project3D', -1, camera_group)
    assert grouped_projection.connectInput(1, camera_group.getNode('Input1'))
    assert camera_group.connectInput(0, camera)
    camera_group.disconnectInput(0)
    assert not camera_group.connectInput(0, blue), 'Retyping a group invalidated its internal projection camera'
    assert camera_group.connectInput(0, camera)
    replace(renderer, 0, surface)
    # Typed Dot and Group serialization must preserve the 3D connections.
    dot = app.createNode('fr.inria.built-in.Dot')
    assert dot.connectInput(0, camera_group)
    replace(renderer, 1, dot)
    roundtrip_reference = render(renderer, 'roundtrip-before', 2)
    project = output / 'native-3d.ntp'
    assert app.saveProjectAs(project.as_posix())
    loaded = app.loadProject(project.as_posix())
    assert loaded is not None
    restored = loaded.getNode('MainRender3D')
    assert restored and restored.getInput(1).getInput(0).getInput(0).getPluginID() == 'fr.inria.built-in.Camera'
    destination = output / 'roundtrip-after.png'
    writer = loaded.createWriter(destination.as_posix())
    assert writer.connectInput(0, restored)
    loaded.render(writer, 2, 2)
    image = QImage(str(destination)).convertToFormat(QImage.Format_RGBA8888)
    assert bytes(image.constBits()) == bytes(roundtrip_reference.constBits()), 'Project reload changed rendered pixels'
    depth_near, depth_rear, depth_scene, depth_camera, depth_renderer = [node(kind) for kind in ('Card','Card','Scene','Camera','Render3D')]
    depth_renderer.setScriptName('DepthRender3D')
    depth_red=app.createReader((output/'red.png').as_posix())
    depth_blue=app.createReader((output/'blue.png').as_posix())
    assert depth_near.connectInput(0, depth_red) and depth_rear.connectInput(0, depth_blue)
    depth_near.getParam('size').set(1.2, 1.2)
    depth_rear.getParam('size').set(4., 4.)
    depth_rear.getParam('translate').set(0., 0., -2.)
    assert depth_scene.connectInput(0, depth_rear) and depth_scene.connectInput(1, depth_near)
    assert depth_renderer.connectInput(0, depth_scene) and depth_renderer.connectInput(1, depth_camera)
    layers = [(layer.getLayerName(), list(layer.getComponentsNames())) for layer in depth_renderer.getAvailableLayers(-1)]
    assert ('Depth',['Z']) in layers, layers
    depth_renderer.getParam('renderer').setValue('CPU')
    cpu_z = depth_layers(app, depth_renderer, 'depth-layers-cpu')
    assert abs(depth_sample(cpu_z)-5)<1e-6
    assert abs(depth_sample(cpu_z, 24)-7)<1e-6
    assert depth_sample(cpu_z, 0, 0)==0
    depth_renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    gpu_z = depth_layers(app, depth_renderer, 'depth-layers-vulkan')
    assert abs(depth_sample(gpu_z)-5)<1e-6 and abs(depth_sample(gpu_z,24)-7)<1e-6
    compared = subprocess.run([oiio_tool(), '-v', str(cpu_z), '--ch', 'Depth.Z', str(gpu_z), '--ch', 'Depth.Z', '--diff'], capture_output=True, text=True, check=True)
    assert 'Max error  = 0' in compared.stdout, compared.stdout+compared.stderr
    depth_renderer.getParam('output').setValue('DepthPreview')
    preview = render(depth_renderer, 'depth-preview')
    assert preview.pixelColor(48,32).red()>preview.pixelColor(24,32).red()>0
    assert len(set(preview.pixelColor(48,32).getRgb()[:3]))==1
    assert depth_renderer.connectInput(2, depth_blue)
    background_preview=render(depth_renderer, 'depth-preview-background')
    assert bytes(background_preview.constBits()) == bytes(preview.constBits()), 'Background contaminated depth preview'
    assert depth_sample(depth_layers(app, depth_renderer, 'depth-preview-raw-layer'))==5
    depth_renderer.getParam('depthRange').set(4., 8.)
    assert render(depth_renderer, 'depth-preview-range').pixelColor(48,32).red()>preview.pixelColor(48,32).red()
    depth_camera.getParam('translate').setValueAtTime(5., 1., 2)
    depth_camera.getParam('translate').setValueAtTime(6., 2., 2)
    assert abs(depth_sample(depth_layers(app, depth_renderer, 'depth-animated', 2))-6)<1e-6
    depth_project=output/'depth-scene.ntp'
    assert app.saveProjectAs(depth_project.as_posix())
    depth_loaded=app.loadProject(depth_project.as_posix())
    assert depth_loaded is not None
    depth_restored=depth_loaded.getNode('DepthRender3D')
    assert depth_restored.getParam('output').getValue()==1
    assert depth_restored.getParam('depthRange').getValue(0)==4 and depth_restored.getParam('depthRange').getValue(1)==8
    assert abs(depth_sample(depth_layers(depth_loaded,depth_restored,'depth-reloaded',2))-6)<1e-6
    report['depth_output'] = {'layers': layers, 'center': 5., 'rear': 7., 'animated_reloaded': 6., 'float_exr': True, 'float_cpu_gpu_max_difference': 0, 'preview': True}
    # Exercise ufbx through real native nodes, image cache and float EXR output.
    mesh, mesh_camera, mesh_scene, mesh_renderer = [node(kind) for kind in ('ReadGeo','Camera','Scene','Render3D')]
    mesh.setScriptName('ImportedFbx'); mesh_renderer.setScriptName('FbxRender3D')
    fbx = Path('Tests/fixtures/scene3d/colored-cube.fbx').resolve()
    mesh.getParam('file').setValue(fbx.as_posix())
    mesh_camera.getParam('translate').set(0., 0., 6.)
    assert mesh_scene.connectInput(0,mesh) and mesh_renderer.connectInput(0,mesh_scene) and mesh_renderer.connectInput(1,mesh_camera)
    assert not mesh.connectInput(1,mesh_camera), 'ReadGeo accepted a Camera as parent'
    mesh_renderer.getParam('renderer').setValue('CPU')
    mesh_color=render(mesh_renderer,'fbx-color-cpu')
    mesh_cpu_float=depth_layers(app,mesh_renderer,'fbx-color-depth-cpu')
    mesh_renderer.getParam('renderer').setValue('Auto')
    mesh_auto=render(mesh_renderer,'fbx-color-auto')
    assert max(abs(a-b) for a,b in zip(bytes(mesh_color.constBits()),bytes(mesh_auto.constBits())))<=1
    mesh_renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    mesh_gpu_float=depth_layers(app,mesh_renderer,'fbx-color-depth-vulkan')
    mesh_diff=subprocess.run([oiio_tool(),'-v',str(mesh_cpu_float),str(mesh_gpu_float),'--diff'],capture_output=True,text=True,check=True)
    mesh_max_difference=float(re.search(r'Max error\s+=\s+([-+0-9.eE]+)',mesh_diff.stdout).group(1))
    assert mesh_max_difference<.0005,mesh_diff.stdout+mesh_diff.stderr
    assert mesh_color.pixelColor(48,32).red()>mesh_color.pixelColor(48,32).blue()
    assert abs(depth_sample(depth_layers(app,mesh_renderer,'fbx-depth'))-5)<1e-6
    reload_path=output/'fbx-reload.fbx';shutil.copyfile(fbx,reload_path)
    mesh.getParam('file').setValue(reload_path.as_posix())
    red_material=render(mesh_renderer,'fbx-before-file-reload')
    timestamp=reload_path.stat()
    reload_path.write_text(reload_path.read_text().replace('0.8,0.2,0.06','0.2,0.8,0.06'))
    os.utime(reload_path,ns=(timestamp.st_atime_ns,timestamp.st_mtime_ns))
    mesh.getParam('file').reloadFile()
    green_material=render(mesh_renderer,'fbx-after-file-reload')
    assert green_material.pixelColor(48,32).green()>green_material.pixelColor(48,32).red(), 'ReadGeo Reload retained stale geometry/materials'
    mesh.getParam('file').setValue(Path('Tests/fixtures/scene3d/blender-cube-binary.fbx').resolve().as_posix())
    assert not render(mesh_renderer,'fbx-binary').isNull()
    assert abs(depth_sample(depth_layers(app,mesh_renderer,'fbx-binary-depth'))-5)<1e-6
    mesh.getParam('file').setValue(fbx.as_posix())
    mesh_texture=app.createReader((output/'gradient.png').as_posix())
    assert mesh.connectInput(0,mesh_texture)
    uv_image=render(mesh_renderer,'fbx-uv-image')
    assert uv_image.pixelColor(35,32).red()<uv_image.pixelColor(60,32).red()
    mesh_axis=node('Axis'); mesh_axis.getParam('translate').set(0.,0.,.2)
    assert mesh.connectInput(1,mesh_axis)
    assert abs(depth_sample(depth_layers(app,mesh_renderer,'fbx-axis-depth'))-4.8)<1e-5
    mesh_transform=node('TransformGeo');mesh_transform.getParam('translate').set(0.,0.,.3)
    assert mesh_transform.connectInput(0,mesh); replace(mesh_scene,0,mesh_transform)
    assert abs(depth_sample(depth_layers(app,mesh_renderer,'fbx-transform-depth'))-4.5)<1e-5
    mesh_projection, mesh_projector=node('Project3D'),node('Camera')
    mesh_projector.getParam('translate').set(0.,0.,6.)
    mesh_projector.getParam('translate').setValueAtTime(0.,1.,0)
    assert mesh_projection.connectInput(0,mesh_texture) and mesh_projection.connectInput(1,mesh_projector)
    replace(mesh,0,mesh_projection)
    projected_mesh=render(mesh_renderer,'fbx-projected')
    projected_float=depth_layers(app,mesh_renderer,'fbx-projected-float')
    mesh_projector.getParam('translate').setValueAtTime(1.,2.,0)
    frozen_mesh=render(mesh_renderer,'fbx-projected-frozen',2)
    frozen_float=depth_layers(app,mesh_renderer,'fbx-projected-frozen-float',2)
    frozen_diff=subprocess.run([oiio_tool(),'-v',str(projected_float),str(frozen_float),'--diff'],capture_output=True,text=True,check=True)
    assert 'Max error  = 0' in frozen_diff.stdout, frozen_diff.stdout+frozen_diff.stderr
    mesh_projection.getParam('freeze').setValue(False)
    live_mesh=render(mesh_renderer,'fbx-projected-live',2)
    assert bytes(live_mesh.constBits())!=bytes(frozen_mesh.constBits())
    mesh.getParam('rotate').setValueAtTime(0.,1.,1)
    mesh.getParam('rotate').setValueAtTime(30.,2.,1)
    mesh_animated=render(mesh_renderer,'fbx-animated',2)
    mesh_project=output/'fbx-scene.ntp'
    assert app.saveProjectAs(mesh_project.as_posix())
    mesh_loaded=app.loadProject(mesh_project.as_posix()); assert mesh_loaded
    mesh_restored=mesh_loaded.getNode('FbxRender3D')
    mesh_roundtrip=render(mesh_restored,'fbx-reloaded',2,mesh_loaded)
    assert bytes(mesh_roundtrip.constBits())==bytes(mesh_animated.constBits())
    report['fbx_mesh']={'ascii':True,'binary':True,'backend':'CPU' if cpu_only else 'Vulkan','auto_matches_cpu':True,'cpu_vulkan_float_max_difference':mesh_max_difference,'file_reload':True,'uv':True,'projection':True,'axis_transform':True,'depth':True,'animation_roundtrip':True}
    # FBX animation must pass through the native image cache at the project FPS,
    # including repeated times, retiming, empty visibility frames and project IO.
    app.getProjectParam('frameRange').set(1, 49)
    app.getProjectParam('frameRate').setValue(24.)
    animated, animation_camera, animation_renderer = [node(kind) for kind in ('ReadGeo','Camera','Render3D')]
    animated.setScriptName('AnimatedFbx'); animation_renderer.setScriptName('AnimatedFbxRender3D')
    animated.getParam('file').setValue(Path('Tests/fixtures/scene3d/animated-cube.fbx').resolve().as_posix())
    info=animated.getParam('animationInfo').getValue()
    assert all(name in info for name in ('Source FPS: 24','Move','Scale','Hide')), info
    animation_camera.getParam('translate').set(0.,0.,6.)
    assert animation_renderer.connectInput(0,animated) and animation_renderer.connectInput(1,animation_camera)
    animation_renderer.getParam('renderer').setValue('CPU')
    static_one=render(animation_renderer,'fbx-static-frame1',1)
    static_last=render(animation_renderer,'fbx-static-frame25',25)
    # The PNG writer dithers at the requested frame, so compare static geometry
    # in linear float space rather than requiring identical 8-bit codes.
    static_float_one=depth_layers(app,animation_renderer,'fbx-static-float1',1)
    static_float_last=depth_layers(app,animation_renderer,'fbx-static-float25',25)
    static_diff=subprocess.run([oiio_tool(),'-v',str(static_float_one),str(static_float_last),'--diff'],capture_output=True,text=True,check=True)
    assert 'Max error  = 0' in static_diff.stdout,static_diff.stdout+static_diff.stderr
    animated.getParam('readAnimation').setValue(True)
    def animated_depth(name, frame, expected):
        actual=depth_sample(depth_layers(app,animation_renderer,'fbx-animation-'+name,frame))
        assert abs(actual-expected)<1e-5, (name,frame,actual,expected)
    animated_depth('begin',1,5.)
    animated_depth('halfway',13,4.5)
    animated_depth('end',25,4.)
    animated_depth('begin-again',1,5.)
    app.getProjectParam('frameRate').setValue(48.)
    animated_depth('fps48',13,4.75)
    app.getProjectParam('frameRate').setValue(24.)
    animated_depth('fps24-again',13,4.5)
    animated.getParam('animationStart').setValue(13.)
    animated_depth('start-frame',13,5.)
    animated.getParam('animationSpeed').setValue(2.)
    animated_depth('double-speed',19,4.5)
    animated.getParam('animationOffset').setValue(.25)
    animated_depth('time-offset',13,4.75)
    animated.getParam('animationSpeed').setValue(0.)
    animated_depth('hold',25,4.75)
    animated.getParam('animationStart').setValue(1.)
    animated.getParam('animationSpeed').setValue(-1.)
    animated.getParam('animationOffset').setValue(1.)
    animated_depth('reverse-begin',1,4.)
    animated_depth('reverse-end',25,5.)
    animated.getParam('animationSpeed').setValue(1.)
    animated.getParam('animationOffset').setValue(0.)
    animated.getParam('animationTake').setValue('Scale')
    scale_begin=render(animation_renderer,'fbx-scale-begin',1)
    scale_end=render(animation_renderer,'fbx-scale-end',25)
    assert sum(scale_end.pixelColor(x,y).alpha()>0 for y in range(64) for x in range(96))>sum(scale_begin.pixelColor(x,y).alpha()>0 for y in range(64) for x in range(96)), 'Named FBX take did not change geometry'
    animated.getParam('animationTake').setValue('Hide')
    animated_depth('visible',1,5.)
    animated_depth('hidden',25,0.)
    hidden_image=render(animation_renderer,'fbx-hidden-cpu',25)
    assert not any(bytes(hidden_image.constBits())), 'Hidden FBX retained color pixels'
    animation_renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    hidden_gpu=render(animation_renderer,'fbx-hidden-vulkan',25)
    assert bytes(hidden_gpu.constBits())==bytes(hidden_image.constBits())
    animated.getParam('animationTake').setValue('Move')
    animation_renderer.getParam('renderer').setValue('CPU')
    animation_cpu=depth_layers(app,animation_renderer,'fbx-animation-color-depth-cpu',13)
    animation_renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    animation_gpu=depth_layers(app,animation_renderer,'fbx-animation-color-depth-vulkan',13)
    animation_diff=subprocess.run([oiio_tool(),'-v',str(animation_cpu),str(animation_gpu),'--diff'],capture_output=True,text=True,check=True)
    animation_error=float(re.search(r'Max error\s+=\s+([-+0-9.eE]+)',animation_diff.stdout).group(1))
    assert animation_error<.0005,animation_diff.stdout+animation_diff.stderr
    # Imported deformation, rather than native ReadGeo transform keys.
    for source, scale, end_frame, skin_transform in (('animated-skin.fbx',40.,9,True),('animated-blend-shape.fbx',100.,24,False)):
        animated.getParam('file').setValue(Path('Tests/fixtures/scene3d',source).resolve().as_posix())
        animated.getParam('animationTake').setValue('')
        animated.getParam('applySkinTransform').setValue(skin_transform)
        animated.getParam('scale').set(scale,scale,scale)
        animated.getParam('rotate').set(8.,17.,3.)
        first=render(animation_renderer,source+'-first',1)
        last=render(animation_renderer,source+'-last',end_frame)
        assert any(first.pixelColor(x,y).alpha() for y in range(64) for x in range(96)), source
        first_float=depth_layers(app,animation_renderer,source+'-first-float',1)
        last_float=depth_layers(app,animation_renderer,source+'-last-float',end_frame)
        deformation_diff=subprocess.run([oiio_tool(),'-v',str(first_float),str(last_float),'--diff'],capture_output=True,text=True)
        assert deformation_diff.returncode in (0,1),deformation_diff.stdout+deformation_diff.stderr
        deformation_error=float(re.search(r'Max error\s+=\s+([-+0-9.eE]+)',deformation_diff.stdout).group(1))
        assert deformation_error>.001, source+' deformation did not animate'
        animation_renderer.getParam('renderer').setValue('CPU')
        cpu_deformed=depth_layers(app,animation_renderer,source+'-last-float-cpu',end_frame)
        deformation_compare=subprocess.run([oiio_tool(),'-v',str(cpu_deformed),str(last_float),'--fail','.0005','--diff'],capture_output=True,text=True,check=True)
        deformation_gpu_error=float(re.search(r'Max error\s+=\s+([-+0-9.eE]+)',deformation_compare.stdout).group(1))
        assert deformation_gpu_error<.0005, deformation_compare.stdout+deformation_compare.stderr
        animation_error=max(animation_error,deformation_gpu_error)
        animation_renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
    # Save the animated skin options and retiming, then render exactly the same pose.
    animated.getParam('file').setValue(Path('Tests/fixtures/scene3d/animated-skin.fbx').resolve().as_posix())
    animated.getParam('applySkinTransform').setValue(True)
    animated.getParam('animationStart').setValue(5.)
    animated.getParam('animationSpeed').setValue(.5)
    animated.getParam('animationOffset').setValue(1./24.)
    animated.getParam('scale').set(40.,40.,40.)
    saved_pose=render(animation_renderer,'fbx-take-roundtrip-before',13)
    animation_project=output/'fbx-animation.ntp'
    assert app.saveProjectAs(animation_project.as_posix())
    animation_loaded=app.loadProject(animation_project.as_posix()); assert animation_loaded
    restored_animation=animation_loaded.getNode('AnimatedFbx')
    for name,expected in (('readAnimation',True),('applySkinTransform',True),('animationStart',5.),('animationSpeed',.5),('animationOffset',1./24.)):
        assert restored_animation.getParam(name).getValue()==expected, name
    assert 'Source FPS:' in restored_animation.getParam('animationInfo').getValue()
    restored_pose=render(animation_loaded.getNode('AnimatedFbxRender3D'),'fbx-take-roundtrip-after',13,animation_loaded)
    assert bytes(restored_pose.constBits())==bytes(saved_pose.constBits()), 'FBX animation project reload changed pixels'
    report['fbx_animation']={'static_compatibility':True,'timeline':True,'project_fps':True,'retiming':True,'takes':True,'visibility':True,'skinning':True,'blend_shapes':True,'project_roundtrip':True,'cpu_vulkan_float_max_difference':animation_error}
    imported_camera,reference_camera,camera_geometry,camera_renderer=[node(kind) for kind in ('ReadCamera','Camera','ReadGeo','Render3D')]
    imported_camera.setScriptName('ImportedFbxCamera');camera_renderer.setScriptName('ImportedCameraRender3D')
    imported_camera.getParam('file').setValue(Path('Tests/fixtures/scene3d/animated-camera.fbx').resolve().as_posix())
    assert imported_camera.getParam('readAnimation').getValue()
    assert all(name in imported_camera.getParam('cameraInfo').getValue() for name in ('Rig/MainCamera','Rig/OrthoCamera','Rig/FovCamera'))
    assert not imported_camera.connectInput(0,reference_camera) and not imported_camera.connectInput(0,gradient)
    camera_geometry.getParam('file').setValue(fbx.as_posix())
    assert camera_renderer.connectInput(0,camera_geometry) and camera_renderer.connectInput(1,imported_camera)
    reference_camera.getParam('haperture').setValue(36.)
    reference_camera.getParam('near').setValue(.1);reference_camera.getParam('far').setValue(1000.)
    imported_camera.getParam('animationTake').setValue('CameraMove')
    camera_renderer.getParam('renderer').setValue('CPU' if cpu_only else 'Vulkan')
    def float_equal(first,last,tolerance=.00002):
        compared=subprocess.run([oiio_tool(),'-v',str(first),str(last),'--fail',str(tolerance),'--diff'],capture_output=True,text=True,check=True)
        maximum=float(re.search(r'Max error\s+=\s+([-+0-9.eE]+)',compared.stdout).group(1))
        assert maximum<tolerance,compared.stdout+compared.stderr
        return maximum
    camera_poses={}
    for frame,fraction in ((1,0.),(13,.5),(25,1.)):
        reference_camera.getParam('translate').set(.6*fraction,0.,6.)
        reference_camera.getParam('rotate').set(0.,12*fraction,0.)
        reference_camera.getParam('focal').setValue(35+35*fraction)
        replace(camera_renderer,1,imported_camera)
        imported=depth_layers(app,camera_renderer,f'camera-imported-{frame}',frame);camera_poses[frame]=imported
        replace(camera_renderer,1,reference_camera)
        native=depth_layers(app,camera_renderer,f'camera-native-{frame}',frame)
        float_equal(imported,native)
    replace(camera_renderer,1,imported_camera)
    app.getProjectParam('frameRate').setValue(48.)
    float_equal(camera_poses[13],depth_layers(app,camera_renderer,'camera-fps48',25))
    app.getProjectParam('frameRate').setValue(24.)
    imported_camera.getParam('animationSpeed').setValue(0.);imported_camera.getParam('animationOffset').setValue(.5)
    float_equal(camera_poses[13],depth_layers(app,camera_renderer,'camera-held',25))
    imported_camera.getParam('animationSpeed').setValue(-1.);imported_camera.getParam('animationOffset').setValue(1.)
    float_equal(camera_poses[13],depth_layers(app,camera_renderer,'camera-reverse',13))
    imported_camera.getParam('animationSpeed').setValue(1.);imported_camera.getParam('animationOffset').setValue(0.)
    imported_camera.getParam('animationTake').setValue('Hold')
    float_equal(camera_poses[1],depth_layers(app,camera_renderer,'camera-take-hold',25))
    imported_camera.getParam('readAnimation').setValue(False)
    imported_camera.getParam('cameraName').setValue('Rig/OrthoCamera')
    imported_camera.getParam('focalMultiplier').setValue(2.)
    reference_camera.getParam('translate').set(2.,0.,6.);reference_camera.getParam('rotate').set(0.,0.,0.)
    reference_camera.getParam('orthographic').setValue(True);reference_camera.getParam('orthoWidth').setValue(2.)
    ortho_imported=depth_layers(app,camera_renderer,'camera-ortho-imported')
    replace(camera_renderer,1,reference_camera)
    float_equal(ortho_imported,depth_layers(app,camera_renderer,'camera-ortho-native'))
    replace(camera_renderer,1,imported_camera)
    imported_camera.getParam('cameraName').setValue('MainCamera');imported_camera.getParam('focalMultiplier').setValue(1.)
    imported_camera.getParam('readAnimation').setValue(True);imported_camera.getParam('animationTake').setValue('CameraMove')
    imported_camera.getParam('animationStart').setValue(5.);imported_camera.getParam('animationSpeed').setValue(.5)
    imported_camera.getParam('animationOffset').setValue(.125)
    camera_parent=node('Axis');camera_parent.getParam('translate').set(.2,.1,0.)
    assert imported_camera.connectInput(0,camera_parent)
    before_camera=render(camera_renderer,'camera-roundtrip-before',13)
    saved_camera_values={name:imported_camera.getParam(name).getValue() for name in ('file','cameraName','readAnimation','animationTake','animationStart','animationSpeed','animationOffset','focalMultiplier')}
    camera_project=output/'fbx-camera.ntp';assert app.saveProjectAs(camera_project.as_posix())
    camera_loaded=app.loadProject(camera_project.as_posix());assert camera_loaded
    restored_camera=camera_loaded.getNode('ImportedFbxCamera')
    assert restored_camera.getPluginID()=='fr.inria.built-in.ReadCamera'
    for name,expected in saved_camera_values.items():
        assert restored_camera.getParam(name).getValue()==expected,name
    after_camera=render(camera_loaded.getNode('ImportedCameraRender3D'),'camera-roundtrip-after',13,camera_loaded)
    assert bytes(before_camera.constBits())==bytes(after_camera.constBits()),'ReadCamera project reload changed pixels'
    report['fbx_camera']={'native_color_depth_match':True,'timeline':True,'fps_cache':True,'retiming':True,'takes':True,'orthographic_multiplier':True,'typed_connections':True,'axis_parent':True,'project_roundtrip':True}
    report.update(ok=True, typed_connections=True, camera_animation=True, fixed_projection=True,
                  axis_transform=True, grouped_geometry=True, cropped_projection=True, larger_scene_fallback=True,
                  rendered_image_texture=True, cpu_only=cpu_only, project_roundtrip=True)
    if os.environ.get('NATRON_SCENE3D_RELOAD_GUI') == '1':
        gui_result = json.loads(Path('build/scene3d-gui-result.json').read_text(encoding='utf-8'))
        assert gui_result['ok'] and gui_result['stages']['multi_saved'], 'Run Scene3DGuiSmoke first'
        edited = app.loadProject(gui_result['multi_project'])
        assert edited is not None
        for name, expected in gui_result['multi_values'].items():
            restored_card = edited.getNode(name)
            assert restored_card and restored_card.getPluginID() == 'fr.inria.built-in.Card'
            translate = restored_card.getParam('translate')
            assert [translate.getNumKeys(d) for d in range(3)] == [3, 3, 0]
            assert all(abs(translate.getValueAtTime(5., d)-expected[d])<1e-8 for d in range(3))
            assert translate.getValueAtTime(10., 0) == .1 and translate.getValueAtTime(10., 1) == .1
        edited_renderer = edited.getNode('MultiRender3D')
        assert edited_renderer and edited_renderer.getInput(0).getPluginID() == 'fr.inria.built-in.Scene'
        edited_renderer.getParam('renderer').setValue('CPU')
        reference = render(edited_renderer, 'gui-multi-frame1', 1, edited)
        edited_cpu = render(edited_renderer, 'gui-multi-frame5-cpu', 5, edited)
        assert bytes(reference.constBits()) != bytes(edited_cpu.constBits()), 'Saved bulk animation did not affect the headless render'
        assert any(edited_cpu.pixelColor(x,y).alpha()>0 for y in range(64) for x in range(96))
        edited_renderer.getParam('renderer').setValue('Auto' if cpu_only else 'Vulkan')
        edited_gpu = render(edited_renderer, 'gui-multi-frame5-vulkan', 5, edited)
        difference = max(abs(a-b) for a,b in zip(bytes(edited_cpu.constBits()), bytes(edited_gpu.constBits())))
        assert difference <= 1, difference
        report['gui_multi_project_reload'] = {'ok': True, 'cpu_vulkan_max_byte_difference': difference}
except Exception:
    report['ok'] = False
    report['exception'] = traceback.format_exc()
finally:
    (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
assert report['ok'], report.get('exception')
