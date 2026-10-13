"""Open the lighting demo, prepare its Viewer and save the usable project."""
import json
import os
import traceback
from pathlib import Path
from PySide6.QtCore import QMetaObject,QTimer,Qt
from PySide6.QtWidgets import QApplication,QMainWindow
import NatronGui

directory=Path.cwd()/'build/scene3d-lighting-demo'
report={'ok':False};retained={};application=None

def widgets():
    values=QApplication.allWidgets();retained.update((id(w),w) for w in values);return values

def finish(error=None):
    if error:report.update(error=str(error),traceback=traceback.format_exc())
    report['ok']=error is None
    (directory/'gui-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    # Stop pending 2D Viewer requests and let their GUI callbacks drain before
    # quitting the event loop; synchronous 3D checks can finish ahead of them.
    if application:
        viewer=application.getViewer('LightingViewer')
        if viewer:viewer.pause()
    QTimer.singleShot(1000,lambda: QApplication.exit(0))

def guarded(function):
    def run():
        try:function()
        except Exception as error:finish(error)
    return run

def setup():
    global application
    loaded=app.loadProject((directory/'lighting.ntp').as_posix());assert loaded
    application=NatronGui.PyGuiApplication().getGuiInstance(loaded.getAppID())
    assert application.getNode('SurfaceWhite'), 'Regenerate the current lighting example first'
    assert application.getNode('GoldMetalMaterial').getParam('roughness').getValue()==.5
    material=application.getNode('BluePlasticMaterial')
    assert [material.getInput(i).getScriptName() for i in (1,2,3)]==['NormalMap','PackedORM','PackedORM']
    assert material.getParam('roughnessChannel').getValue()==1 and material.getParam('metallicChannel').getValue()==2
    viewer=application.getNode('LightingViewer') or application.createNode('fr.inria.built-in.Viewer')
    viewer.setScriptName('LightingViewer')
    if not viewer.getInput(0):assert viewer.connectInput(0,application.getNode('LightingRender3D'))
    positions={'NormalMap':(-640,-390),'PackedORM':(-480,-390),'SurfaceWhite':(-220,-390),'BluePlasticMaterial':(-350,-220),'BluePlastic':(-350,-60),
               'GoldMetalMaterial':(-100,-220),'GoldMetal':(-100,-60),
               'GroundMaterial':(150,-220),'Ground':(150,-60),'KeyLight':(400,-160),'FillLight':(400,-10),
               'LightingScene':(30,150),'RenderCamera':(-250,270),'LightingRender3D':(30,270),
               'LightingViewer':(30,410),'ColorWriter':(-250,550),'DepthWriter':(30,550),'ColorDepthWriter':(280,550)}
    for name,position in positions.items():
        effect=application.getNode(name)
        if effect:effect.setPosition(*position)
    application.getViewer('LightingViewer').seek(13)
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
    refresh()
    expected='CPU' if os.environ.get('NATRON_VULKAN_DISABLED')=='1' else 'Vulkan'
    assert plot.property('rendererBackend')==expected
    assert sum(item['kind']=='Light' for item in plot.property('sceneObjects'))==2
    frames=[]
    for number in (1,13,25):
        application.getViewer('LightingViewer').seek(number);refresh()
        image=plot.property('capturedFrame');assert image.width()==640 and image.height()==400
        frames.append(bytes(image.constBits()))
        assert image.save(str(directory/f'viewer-frame{number}.png'))
    assert frames[0]!=frames[1] and frames[1]!=frames[2]
    application.getViewer('LightingViewer').seek(13);refresh()
    application.setSelection([application.getNode('KeyLight')])
    assert QMetaObject.invokeMethod(plot,'frameSelection',Qt.DirectConnection);refresh()
    assert plot.property('selectedNode')=='KeyLight' and len(plot.property('gizmoHandles'))==3
    assert plot.grab().save(str(directory/'light-gizmo.png'))
    camera.setChecked(True);refresh()
    mapped=bytes(plot.property('capturedFrame').constBits())
    material=application.getNode('BluePlasticMaterial');material.getParam('normalFlipY').setValue(True);refresh()
    assert bytes(plot.property('capturedFrame').constBits())!=mapped,'Viewer ignored the native normal-map control'
    material.getParam('normalFlipY').setValue(False);refresh()
    assert bytes(plot.property('capturedFrame').constBits())==mapped
    assert plot.grab().save(str(directory/'viewer.png'))
    assert toggle.parentWidget().parentWidget().grab().save(str(directory/'viewer-layout.png'))
    assert application.saveProjectAs((directory/'lighting.ntp').as_posix())
    report.update(material_maps=True,backend=expected,lights=2,frames=[1,13,25],light_gizmo=True,saved=True)
    finish()

QTimer.singleShot(0,guarded(setup))
