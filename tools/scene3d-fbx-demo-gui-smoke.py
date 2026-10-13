"""Load the FBX demo, attach a Viewer, lay out the graph and verify the 3D view."""
import json
import os
import traceback
from pathlib import Path
from PySide6.QtCore import QMetaObject, QTimer, Qt
from PySide6.QtWidgets import QApplication, QMainWindow
import NatronGui

animation_demo=globals().get('animation_demo',False)
camera_demo=globals().get('camera_demo',False)
directory=Path.cwd()/'build'/('scene3d-fbx-camera-demo' if camera_demo else 'scene3d-fbx-animation-demo' if animation_demo else 'scene3d-fbx-demo')
project_file='fbx-camera.ntp' if camera_demo else 'fbx-animation.ntp' if animation_demo else 'fbx-meshes.ntp'
mesh_count=3 if animation_demo else 2
demo_frame=13 if camera_demo else 25 if animation_demo else 24
report={'ok':False}
retained={}
application=None

def widgets():
    values=QApplication.allWidgets();retained.update((id(w),w) for w in values);return values

def finish(error=None):
    if error:report.update(error=str(error),traceback=traceback.format_exc())
    report['ok']=error is None
    (directory/'gui-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    QApplication.exit(0)

def guarded(function):
    def run():
        try:function()
        except Exception as error:finish(error)
    return run

def setup():
    global application
    application=app.loadProject((directory/project_file).as_posix());assert application
    application=NatronGui.PyGuiApplication().getGuiInstance(application.getAppID())
    renderer=application.getNode('FbxRender3D');assert renderer
    viewer=application.getNode('FbxViewer') or application.createNode('fr.inria.built-in.Viewer')
    assert viewer;viewer.setScriptName('FbxViewer')
    connected=viewer.getInput(0)
    if not connected or connected.getScriptName()!=renderer.getScriptName():
        if connected:viewer.disconnectInput(0)
        assert viewer.connectInput(0,renderer)
    positions={'ReferenceCamera':(-440,-320),'Read1':(-230,-320),'FrozenProjection':(-230,-160),
               'MeshAxis':(70,-170),'FbxMaterialCube':(50,0),'ProjectedFbxCube':(-230,0),
               'Constant1':(330,-170),'BackgroundCard':(330,0),'FbxScene':(30,160),
               'AnimatedCamera':(-210,280),'FbxRender3D':(30,280),'FbxViewer':(30,400),
               'Write1':(-230,530),'Write2':(30,530),'Write3':(290,530)}
    if animation_demo:
        positions={'ReferenceCamera':(-380,-320),'ProjectionImage':(-170,-320),'FrozenProjection':(-170,-160),
                   'HierarchyAnimation':(-450,0),'SkinColor':(60,-170),'BoneAnimation':(60,0),
                   'BlendShapeAnimation':(-170,0),'BackgroundColor':(340,-170),'BackgroundCard':(340,0),
                   'FbxScene':(30,160),'RenderCamera':(-210,280),'FbxRender3D':(30,280),'FbxViewer':(30,400),
                   'ColorWriter':(-230,530),'DepthPreviewWriter':(30,530),'ColorDepthWriter':(290,530)}
    for name,position in positions.items():
        effect=application.getNode(name)
        if effect:effect.setPosition(*position)
    application.getViewer('FbxViewer').seek(demo_frame)
    for window in widgets():
        if isinstance(window,QMainWindow):window.showNormal();window.resize(1450,1000)
    QTimer.singleShot(2500,guarded(check))

def check():
    toggle=next(w for w in widgets() if w.objectName()=='Viewer3DToggle' and w.isVisible())
    toggle.setChecked(True)
    camera=next(w for w in widgets() if w.objectName()=='Viewer3DCamera' and w.isVisible())
    camera.setChecked(True)
    plot=next(w for w in widgets() if w.objectName()=='Scene3DViewer' and w.isVisible())
    assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
    assert not plot.property('renderError'),plot.property('renderError')
    backend='CPU' if os.environ.get('NATRON_VULKAN_DISABLED')=='1' else 'Vulkan'
    assert plot.property('rendererBackend')==backend
    assert sum(item['kind']=='ReadGeo' for item in plot.property('sceneObjects'))==mesh_count
    tab=toggle.parentWidget().parentWidget()
    rows=[tab.layout().itemAt(i).widget() for i in range(2)]
    QApplication.processEvents()
    report['toolbar_rows']=[{'height':row.height(),'hint':max(row.sizeHint().height(),row.minimumSizeHint().height())} for row in rows]
    report['viewport_height']=plot.height()
    assert all(row.height()<=max(row.sizeHint().height(),row.minimumSizeHint().height())+2 for row in rows),report['toolbar_rows']
    assert tab.grab().save(str(directory/'viewer-layout.png'))
    heights=[]
    original_size=tab.window().size()
    for size in ((1450,850),(1450,1150)):
        tab.window().resize(*size)
        for mode in (False,True):
            toggle.setChecked(mode);QApplication.processEvents()
            assert all(row.height()<=max(row.sizeHint().height(),row.minimumSizeHint().height())+2 for row in rows)
            heights.append({'3d':mode,'tab':tab.height(),'toolbar':sum(row.height() for row in rows),'viewport':plot.height() if mode else None})
    tab.window().resize(original_size);toggle.setChecked(True);QApplication.processEvents()
    assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
    report['layout_resize_2d_3d']=heights
    assert plot.grab().save(str(directory/'viewer.png'))
    if animation_demo or camera_demo:
        frames=[]
        test_frames=(1,13,25) if camera_demo else (1,25,49)
        for number in test_frames:
            application.getViewer('FbxViewer').seek(number)
            assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
            assert not plot.property('renderError'),plot.property('renderError')
            captured=plot.property('capturedFrame');frames.append(bytes(captured.constBits()))
            assert captured.save(str(directory/f'viewer-frame{number}.png'))
        assert frames[0]!=frames[1] and frames[1]!=frames[2], 'Saved FBX animation did not follow the playhead'
        if camera_demo:
            for name in ('AnimatedCamera','ReferenceCamera'):
                assert application.getNode(name).getPluginID()=='fr.inria.built-in.ReadCamera'
            assert application.getNode('AnimatedCamera').getParam('readAnimation').getValue()
            assert not application.getNode('ReferenceCamera').getParam('readAnimation').getValue()
        else:
            for name in ('HierarchyAnimation','BoneAnimation','BlendShapeAnimation'):
                assert application.getNode(name).getParam('readAnimation').getValue()
        application.getViewer('FbxViewer').seek(demo_frame)
        assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
        report['animation_frames']=list(test_frames)
    assert application.saveProjectAs((directory/project_file).as_posix())
    report.update(meshes=mesh_count,backend=plot.property('rendererBackend'),viewer='FbxViewer',frame=demo_frame,saved=True)
    finish()

QTimer.singleShot(0,guarded(setup))
