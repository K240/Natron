"""Open the USD example; exercise shared stage/camera editing and save it."""
import json
import math
import os
import traceback
from pathlib import Path
from PySide6.QtCore import QEvent,QMetaObject,QPointF,QTimer,Qt
from PySide6.QtGui import QAction,QKeyEvent,QMouseEvent
from PySide6.QtWidgets import QApplication,QMainWindow,QDialog,QLabel
import NatronGui

directory=Path.cwd()/'build/scene3d-usd-demo'
report={'ok':False};retained={};application=None;plot=None

def widgets():
    values=QApplication.allWidgets();retained.update((id(w),w) for w in values);return values

def finish(error=None):
    if error:report.update(error=str(error),traceback=traceback.format_exc())
    report['ok']=error is None
    (directory/'gui-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    # Keep Qt's event loop alive while Natron stops pending Viewer renders.
    loaded_id=application.getAppID() if application else None
    if application:application.closeProject()
    if loaded_id!=app.getAppID():
        app.saveProjectAs((directory/'empty-test.ntp').as_posix());app.closeProject()

def guarded(function):
    def run():
        try:function()
        except Exception as error:finish(error)
    return run

def refresh():
    assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
    assert not plot.property('renderError'),plot.property('renderError')

def mouse(kind,position,button,buttons):
    world=QPointF(plot.mapToGlobal(position.toPoint()))
    QApplication.sendEvent(plot,QMouseEvent(kind,position,position,world,button,buttons,Qt.NoModifier))

def drag(start,end):
    mouse(QEvent.MouseButtonPress,start,Qt.LeftButton,Qt.LeftButton)
    for step in range(1,5):mouse(QEvent.MouseMove,start+(end-start)*(step/4),Qt.NoButton,Qt.LeftButton)
    mouse(QEvent.MouseButtonRelease,end,Qt.LeftButton,Qt.NoButton)

def undo():
    actions=[]
    for w in widgets():actions+=w.actions()+w.findChildren(QAction)
    next(a for a in actions if a.isEnabled() and a.text().replace('&','').startswith('Undo')).trigger();refresh()

def setup():
    global application
    report['phase']='load_project';(directory/'gui-progress.json').write_text(json.dumps(report),encoding='utf-8')
    loaded=app.loadProject((directory/'usd.ntp').as_posix());assert loaded
    report['phase']='prepare_viewer';(directory/'gui-progress.json').write_text(json.dumps(report),encoding='utf-8')
    application=NatronGui.PyGuiApplication().getGuiInstance(loaded.getAppID())
    assert application.getNode('UsdStage') and application.getNode('UsdCamera')
    assert Path(application.getNode('UsdStage').getParam('file').getValue()).name=='pbr-maps.usda'
    info=application.getNode('UsdStage').getParam('sourceInfo').getValue()
    assert 'cannot decode texture' not in info and 'unsupported surface input normal' not in info,info
    viewer=application.getNode('UsdViewer') or application.createNode('fr.inria.built-in.Viewer');viewer.setScriptName('UsdViewer')
    if not viewer.getInput(0):assert viewer.connectInput(0,application.getNode('UsdRender3D'))
    for name,position in {'StageAxis':(0,-190),'UsdStage':(0,-40),'UsdScene':(0,100),'UsdCamera':(-260,260),
                          'UsdRender3D':(0,260),'UsdViewer':(0,410),'ColorWriter':(-160,550),'DepthWriter':(160,550)}.items():
        effect=application.getNode(name)
        if effect:effect.setPosition(*position)
    application.getViewer('UsdViewer').seek(13)
    for window in widgets():
        if isinstance(window,QMainWindow):window.showNormal();window.resize(1450,1000)
    QTimer.singleShot(2500,guarded(check))

def check():
    global plot
    toggle=next(w for w in widgets() if w.objectName()=='Viewer3DToggle' and w.isVisible());toggle.setChecked(True)
    camera=next(w for w in widgets() if w.objectName()=='Viewer3DCamera' and w.isVisible());camera.setChecked(True)
    plot=next(w for w in widgets() if w.objectName()=='Scene3DViewer' and w.isVisible());refresh()
    expected='CPU' if os.environ.get('NATRON_VULKAN_DISABLED')=='1' else 'Vulkan'
    assert plot.property('rendererBackend')==expected
    objects=plot.property('sceneObjects')
    assert sum(item['kind']=='ReadUSD' for item in objects)==2
    assert any(item['kind']=='ReadUSDCamera' and item['renderCamera'] for item in objects)
    frames=[]
    for frame in (1,13,25):
        application.getViewer('UsdViewer').seek(frame);refresh();image=plot.property('capturedFrame')
        frames.append(bytes(image.constBits()));assert image.save(str(directory/f'viewer-frame{frame}.png'))
    assert frames[0]!=frames[1] and frames[1]!=frames[2]
    application.getViewer('UsdViewer').seek(13);refresh()
    source=application.getNode('UsdStage');application.setSelection([source]);refresh()
    assert len(plot.property('gizmoHandles'))==3
    translate=source.getParam('translate');old=[translate.getValue(i) for i in range(3)]
    start=plot.property('gizmoHandles')[0];vector=start-plot.property('gizmoCenter');length=math.hypot(vector.x(),vector.y())
    assert length>1;drag(start,start+vector*(22/length));refresh()
    assert abs(translate.getValue(0)-old[0])>.01,'ReadUSD stage move did not change its transform'
    undo();assert all(abs(translate.getValue(i)-old[i])<1e-8 for i in range(3))
    assert QMetaObject.invokeMethod(plot,'frameSelection',Qt.DirectConnection);refresh()
    assert not plot.property('lookThroughCamera')
    assert plot.grab().save(str(directory/'stage-gizmo.png'))
    imported=application.getNode('UsdCamera');application.setSelection([imported]);refresh()
    assert len(plot.property('gizmoHandles'))==3 and plot.property('selectedNode')=='UsdCamera'
    start=plot.property('gizmoHandles')[0];vector=start-plot.property('gizmoCenter');length=math.hypot(vector.x(),vector.y())
    old=imported.getParam('translate').getValue(0);drag(start,start+vector*(15/length));refresh()
    assert abs(imported.getParam('translate').getValue(0)-old)>.01;undo()
    assert abs(imported.getParam('translate').getValue(0)-old)<1e-8
    application.setSelection([source]);camera.setChecked(True);refresh()
    assert plot.grab().save(str(directory/'viewer.png'))
    assert toggle.parentWidget().parentWidget().grab().save(str(directory/'viewer-layout.png'))
    assert application.saveProjectAs((directory/'usd.ntp').as_posix())
    report.update(pbr_maps=True,backend=expected,frames=[1,13,25],stage_gizmo_move_undo=True,camera_gizmo_move_undo=True,frame_selection=True,saved=True)
    finish()

def diagnostics():
    dialogs=[{'title':w.windowTitle(),'labels':[l.text() for l in w.findChildren(QLabel)]} for w in widgets() if isinstance(w,QDialog) and w.isVisible()]
    (directory/'gui-progress.json').write_text(json.dumps({'phase':report.get('phase'),'dialogs':dialogs},indent=2),encoding='utf-8')
progress_timer=QTimer();progress_timer.timeout.connect(diagnostics);progress_timer.start(5000)
QTimer.singleShot(0,guarded(setup))
