"""Open the environment example, verify its 3D Viewer and save the layout."""
import json
import os
import traceback
from pathlib import Path
from PySide6.QtCore import QMetaObject,QTimer,Qt
from PySide6.QtWidgets import QApplication,QMainWindow
import NatronGui

directory=Path.cwd()/'build/scene3d-environment-demo'
report={'ok':False};retained={};application=None

def widgets():
    values=QApplication.allWidgets();retained.update((id(w),w) for w in values);return values

def finish(error=None):
    if error:report.update(error=str(error),traceback=traceback.format_exc())
    report['ok']=error is None
    (directory/'gui-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    if application:
        viewer=application.getViewer('EnvironmentViewer')
        if viewer:viewer.pause()
    QTimer.singleShot(1000,lambda:QApplication.exit(0))

def guarded(function):
    def run():
        try:function()
        except Exception as error:finish(error)
    return run

def setup():
    global application
    loaded=app.loadProject((directory/'environment.ntp').as_posix());assert loaded
    application=NatronGui.PyGuiApplication().getGuiInstance(loaded.getAppID())
    light=application.getNode('EnvironmentLight');assert light.getParam('lightType').getValue()==3
    assert light.getInput(1).getScriptName()=='StudioHDR'
    viewer=application.getNode('EnvironmentViewer') or application.createNode('fr.inria.built-in.Viewer')
    viewer.setScriptName('EnvironmentViewer')
    if not viewer.getInput(0):assert viewer.connectInput(0,application.getNode('EnvironmentRender3D'))
    positions={'SurfaceWhite':(-240,-400),'StudioHDR':(420,-410),
               'SmoothGoldMaterial':(-480,-260),'SmoothGold':(-480,-80),
               'RoughGoldMaterial':(-210,-260),'RoughGold':(-210,-80),
               'BluePlasticMaterial':(70,-260),'BluePlastic':(70,-80),
               'GroundMaterial':(340,-260),'Ground':(340,-80),
               'EnvironmentLight':(600,-230),'ShadowLight':(600,-60),
               'EnvironmentScene':(40,170),'RenderCamera':(-240,290),'EnvironmentRender3D':(40,290),
               'EnvironmentViewer':(40,430),'ColorWriter':(-160,550),'DepthWriter':(220,550)}
    for name,position in positions.items():
        effect=application.getNode(name)
        if effect:effect.setPosition(*position)
    application.getViewer('EnvironmentViewer').seek(13)
    for window in widgets():
        if isinstance(window,QMainWindow):window.showNormal();window.resize(1450,1000)
    QTimer.singleShot(2500,guarded(check))

def check():
    toggle=next(w for w in widgets() if w.objectName()=='Viewer3DToggle' and w.isVisible());toggle.setChecked(True)
    camera=next(w for w in widgets() if w.objectName()=='Viewer3DCamera' and w.isVisible());camera.setChecked(True)
    plot=next(w for w in widgets() if w.objectName()=='Scene3DViewer' and w.isVisible())
    def refresh():
        assert QMetaObject.invokeMethod(plot,'refreshScene',Qt.DirectConnection)
        assert not plot.property('renderError'),plot.property('renderError')
    refresh();expected='CPU' if os.environ.get('NATRON_VULKAN_DISABLED')=='1' else 'Vulkan'
    assert plot.property('rendererBackend')==expected
    frames=[]
    for number in (1,13,25):
        application.getViewer('EnvironmentViewer').seek(number);refresh()
        image=plot.property('capturedFrame');assert image.width()==640 and image.height()==360
        frames.append(bytes(image.constBits()));assert image.save(str(directory/f'viewer-frame{number}.png'))
    assert frames[0]!=frames[1] and frames[1]!=frames[2]
    application.getViewer('EnvironmentViewer').seek(13);refresh()
    before=bytes(plot.property('capturedFrame').constBits())
    light=application.getNode('EnvironmentLight');light.getParam('intensity').setValue(0.);refresh()
    assert bytes(plot.property('capturedFrame').constBits())!=before,'Viewer did not apply the environment image'
    light.getParam('intensity').setValue(1.);refresh();assert bytes(plot.property('capturedFrame').constBits())==before
    roughness=application.getNode('RoughGoldMaterial').getParam('roughness');roughness.setValue(.08);refresh()
    assert bytes(plot.property('capturedFrame').constBits())!=before,'Viewer ignored environment reflection roughness'
    roughness.setValue(.55);refresh();assert bytes(plot.property('capturedFrame').constBits())==before
    application.setSelection([light]);assert QMetaObject.invokeMethod(plot,'frameSelection',Qt.DirectConnection);refresh()
    assert plot.property('selectedNode')=='EnvironmentLight' and len(plot.property('gizmoHandles'))==3
    camera.setChecked(True);refresh()
    assert toggle.parentWidget().parentWidget().grab().save(str(directory/'viewer-layout.png'))
    assert application.saveProjectAs((directory/'environment.ntp').as_posix())
    report.update(backend=expected,frames=[1,13,25],environment_image=True,roughness=True,light_gizmo=True,saved=True)
    finish()

QTimer.singleShot(0,guarded(setup))
