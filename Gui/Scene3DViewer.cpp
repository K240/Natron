// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Gui/Scene3DViewer.h"
#include <algorithm>
#include <cmath>
#include <set>
#include <map>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QPointer>
#include <QtConcurrentRun>
#include <QUndoStack>
#include "Engine/AppInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Lut.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Format.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewerInstance.h"
#include "Engine/VulkanImageContextPool.h"
#include "Engine/VulkanImageStorage.h"
#include "Gui/ViewerTab.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeGraph.h"
#include "Gui/NodeSettingsPanel.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER
using namespace Gpu::Cards;
namespace {
NodePtr findRenderer(const NodePtr& root,std::set<Node*>& seen) {
    if(!root || !seen.insert(root.get()).second)return {};
    if(auto native=dynamic_cast<NativeScene3D*>(root->getEffectInstance().get()))
        if(native->kind()==Scene3DKind::Render3D)return root;
    const auto effect=root->getEffectInstance();
    for(int i=0;i<effect->getNInputs();++i)if(const auto input=effect->getInput(i))
        if(auto found=findRenderer(input->getNode(),seen))return found;
    return {};
}
double lineDistance(QPointF p,QPointF a,QPointF b) {
    const QPointF d=b-a;const double length=d.x()*d.x()+d.y()*d.y();
    if(length<1e-5)return 1e6;
    const double t=std::clamp(((p-a).x()*d.x()+(p-a).y()*d.y())/length,0.,1.);
    return QLineF(p,a+d*t).length();
}
constexpr double pi=3.14159265358979323846;
double component(Vec3 p,int axis){return axis==0?p.x:axis==1?p.y:p.z;}
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Vec3 normalized(Vec3 v){const double length=std::sqrt(dot(v,v));return length>1e-12?v*(1/length):Vec3{};}
Vec3 unitAxis(int axis){return axis==0?Vec3{1,0,0}:axis==1?Vec3{0,1,0}:Vec3{0,0,1};}
Matrix orientation(const Matrix& matrix) {
    // Gizmos describe orientation without inheriting object scale or shear.
    const Vec3 x=normalized(matrix.vector(unitAxis(0)));
    const Vec3 rawY=matrix.vector(unitAxis(1));const Vec3 y=normalized(rawY-x*dot(x,rawY));
    Vec3 z=cross(x,y);if(dot(z,matrix.vector(unitAxis(2)))<0)z=z*(-1);
    Matrix result;result.m={x.x,x.y,x.z,0, y.x,y.y,y.z,0, z.x,z.y,z.z,0, 0,0,0,1};return result;
}
struct Ray {Vec3 origin,direction;};
QString objectKind(Scene3DKind kind) {
    const char* names[]={"Card","Camera","Axis","TransformGeo","Scene","Project3D","Render3D","ReadGeo","ReadCamera","Light","PBRMaterial","ReadUSD","ReadUSDCamera"};
    return QString::fromLatin1(names[int(kind)]);
}
bool geometryLeaf(Scene3DKind kind){return kind==Scene3DKind::Card || kind==Scene3DKind::ReadGeo || kind==Scene3DKind::ReadUSD;}
bool cameraKind(Scene3DKind kind){return kind==Scene3DKind::Camera || kind==Scene3DKind::ReadCamera || kind==Scene3DKind::ReadUSDCamera;}
std::vector<Vec3> geometryCorners(const Card& card) {
    if(!card.mesh)return {{-card.width*.5,-card.height*.5,0},{card.width*.5,-card.height*.5,0},{card.width*.5,card.height*.5,0},{-card.width*.5,card.height*.5,0}};
    const auto& lo=card.mesh->bounds.minimum;const auto& hi=card.mesh->bounds.maximum;
    return {{lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},
            {lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z}};
}
Ray rayAt(QPointF position,const QRectF& imageRect,const Camera& camera) {
    const double nx=2*(position.x()-imageRect.left())/imageRect.width()-1+2*camera.offsetX;
    const double ny=1-2*(position.y()-imageRect.top())/imageRect.height()+2*camera.offsetY*imageRect.width()/imageRect.height();
    const double tx=camera.orthographic?camera.orthoWidth*.5:camera.aperture/(2*camera.focal),ty=tx*imageRect.height()/imageRect.width();
    return {camera.transform.point(camera.orthographic?Vec3{nx*tx,ny*ty,0}:Vec3{}),
            camera.transform.vector(camera.orthographic?Vec3{0,0,-1}:Vec3{nx*tx,ny*ty,-1})};
}
}
Scene3DViewer::Scene3DViewer(ViewerTab* tab):QWidget(tab),_tab(tab) {
    setObjectName("Scene3DViewer");setFocusPolicy(Qt::StrongFocus);setMouseTracking(true);setMinimumSize(200,150);
    setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Expanding);
    auto timer=new QTimer(this);timer->setInterval(150);
    connect(timer,&QTimer::timeout,this,[this]{if(isVisible())refreshScene();});timer->start();
}
Scene3DViewer::~Scene3DViewer(){if(_previewCancelled)_previewCancelled->store(true);endDrag();}
NodePtr Scene3DViewer::rendererForViewer(ViewerTab* tab) {
    if(!tab || !tab->getInternalNode())return {};
    int a=-1,b=-1;tab->getActiveInputs(&a,&b);
    const auto input=a>=0?tab->getInternalNode()->getInput(a):EffectInstancePtr();
    std::set<Node*> seen;return input?findRenderer(input->getNode(),seen):NodePtr();
}
QString Scene3DViewer::stageBranch()const {
    if(auto source=_source.lock())return QString::fromStdString(source->getLabel())+tr(" · frame %1 · view %2").arg(_time).arg(_view);
    return tr("Connect Render3D with the USD / hdNVP backend.");
}
QStringList Scene3DViewer::selectedPrimPaths()const {QStringList result;for(const auto& p:_primSelection)result<<QString::fromStdString(p.path);return result;}
void Scene3DViewer::setPrimSelection(const std::vector<UsdPrimInfo>& prims) {
    if(prims.empty()){_primSelection.clear();update();return;}
    endDrag();_primSelection=prims;_selection.clear();_selected.reset();_selectedObject=-1;
    // Reveal owning nodes while keeping prim-level selection independent.
    std::map<NodeGraph*,NodesGuiList> graphs;std::set<Node*> seen;
    for(const auto& prim:prims)for(const auto& object:_scene.objects) {
        auto native=dynamic_cast<NativeScene3D*>(object.owner->getEffectInstance().get());
        if(native && native->sceneId()==prim.ownerId && seen.insert(object.owner.get()).second) {
            if(auto gui=std::dynamic_pointer_cast<NodeGui>(object.owner->getNodeGui()))graphs[gui->getDagGui()].push_back(gui);
            const auto base=(native->kind()==Scene3DKind::Light?"/Natron/Lights/n_":"/Natron/Geometry/n_")+native->sceneId();
            if(prim.path==base && (native->kind()==Scene3DKind::Card || native->kind()==Scene3DKind::ReadGeo || native->kind()==Scene3DKind::Light)) {
                _selection.push_back(object.owner);_selected=object.owner;
            }
        }
    }
    for(const auto& pair:graphs)pair.first->setSelection(pair.second);
    _graphSelection.clear();for(const int index:graphSelectionIndices())_graphSelection.push_back(_scene.objects[index].owner);
    updateHandles();update();
}
void Scene3DViewer::endDrag() {
    if(_dragUndoStack){_dragUndoStack->endMacro();_dragUndoStack.reset();}
    _dragAxis=-1;_dragKnob.reset();_dragTargets.clear();_marquee=false;
}
void Scene3DViewer::setGizmoMode(int mode) {
    mode=std::clamp(mode,0,2);if(mode==_gizmoMode)return;
    endDrag();_gizmoMode=mode;updateHandles();Q_EMIT gizmoModeChanged(mode);update();
}
QPolygonF Scene3DViewer::gizmoHandles() const {
    QPolygonF result;if(_handlesVisible)for(const auto& point:_handleEnd)result<<point;return result;
}
QVariantList Scene3DViewer::gizmoRings() const {
    QVariantList result;if(_handlesVisible)for(const auto& ring:_rotationRings)result.push_back(QVariant::fromValue(ring));return result;
}
QString Scene3DViewer::selectedNode() const {
    const auto node=_selected.lock();return node?QString::fromStdString(node->getScriptName_mt_safe()):QString();
}
QStringList Scene3DViewer::selectedNodes() const {
    QStringList result;for(const auto& weak:_selection)if(auto node=weak.lock())result<<QString::fromStdString(node->getScriptName_mt_safe());return result;
}
QVariantList Scene3DViewer::sceneObjects() const {
    QVariantList result;
    for(size_t i=0;i<_scene.objects.size();++i) {
        const auto& object=_scene.objects[i];const Vec3 center=object.transform.point({});QVariantMap info;
        info[QStringLiteral("node")]=QString::fromStdString(object.owner->getScriptName_mt_safe());info[QStringLiteral("kind")]=objectKind(object.kind);
        info[QStringLiteral("time")]=object.time;info[QStringLiteral("editable")]=object.time==_time;info[QStringLiteral("renderCamera")]=object.renderCamera;
        info[QStringLiteral("projectionCamera")]=object.projectionCamera;info[QStringLiteral("worldOrigin")]=QVariantList{center.x,center.y,center.z};
        info[QStringLiteral("cameraAspect")]=object.cameraAspect;
        if(i<_objectDisplay.size()) {
            info[QStringLiteral("icon")]=_objectDisplay[i].icon;info[QStringLiteral("visible")]=_objectDisplay[i].visible;
            info[QStringLiteral("frustum")]=QVariant::fromValue(_objectDisplay[i].farPlane);
        }
        result.push_back(info);
    }
    return result;
}
void Scene3DViewer::setLookThroughCamera(bool value) {
    const bool wasPrim=!_lookCameraPath.isEmpty();
    _lookCameraPath.clear();
    if(value==_lookThrough && !wasPrim)return;
    endDrag();_lookThrough=value;_dirty=true;Q_EMIT lookThroughCameraChanged(value);refreshScene();
}
void Scene3DViewer::lookThroughPrim(const QString& path,const Camera& camera) {
    if(_lookThrough && _lookCameraPath==path && _lookCamera.transform.m==camera.transform.m && _lookCamera.focal==camera.focal && _lookCamera.aperture==camera.aperture && _lookCamera.orthographic==camera.orthographic && _lookCamera.orthoWidth==camera.orthoWidth && _lookCamera.nearClip==camera.nearClip && _lookCamera.farClip==camera.farClip && _lookCamera.offsetX==camera.offsetX && _lookCamera.offsetY==camera.offsetY)return;
    endDrag();_lookCameraPath=path;_lookCamera=camera;_lookThrough=true;_dirty=true;Q_EMIT lookThroughCameraChanged(true);refreshScene();
}
void Scene3DViewer::resetView(){endDrag();_yaw=25;_pitch=-15;_distance=7;_pivot={};_dirty=true;refreshScene();}
void Scene3DViewer::frameSelection() {
    endDrag();refreshScene();if(!_snapshotValid)return;
    const auto objects=selectedObjects();
    const bool includeScene=objects.empty() || std::any_of(objects.begin(),objects.end(),[](const Scene3DObject* object){return object->itemIndex<0;});
    std::vector<Vec3> points;
    for(const auto& prim:_primSelection)if(prim.hasBounds)for(int i=0;i<8;++i)points.push_back({i&1?prim.maximum.x:prim.minimum.x,i&2?prim.maximum.y:prim.minimum.y,i&4?prim.maximum.z:prim.minimum.z});
    for(const auto& item:_scene.items)if(_primSelection.empty() && (includeScene || isSelected(item.owner))) {
        const auto& card=item.card;
        if(!card.bindingPaths.empty())continue;
        if(card.opacity<=0 || (card.mesh && card.mesh->triangles.empty()))continue;
        for(const Vec3 p:geometryCorners(card))points.push_back(card.transform.point(p));
    }
    // Geometry may be offset from its node origin (especially deformed FBX).
    // Only non-geometry selections need their editor origins included in the fit.
    const bool geometryEmpty=points.empty();
    for(const auto object:objects)if(_primSelection.empty() && (object->itemIndex<0 || geometryEmpty))points.push_back(object->transform.point({}));
    if(points.empty()){_pivot={};_distance=7;}
    else {
        Vec3 lo=points.front(),hi=lo;
        for(const Vec3 p:points) {
            lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
            hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
        }
        _pivot=lo+(hi-lo)*.5;
        if(dot(hi-lo,hi-lo)<1e-24)_distance=7;
        else {
            const Camera inspection;
            const int renderWidth=std::min(640,std::max(1,width()));
            const int renderHeight=std::max(1,int(double(renderWidth)*height()/std::max(1,width())));
            const double tx=inspection.aperture/(2*inspection.focal),ty=tx*renderHeight/renderWidth;
            const Matrix inverseRotation=Matrix::transform({}, {_pitch,_yaw,0}).inverse();
            _distance=inspection.nearClip*2;
            for(const Vec3 p:points) {
                const Vec3 q=inverseRotation.vector(p-_pivot);
                // Fit both perspective axes at each corner's actual depth.
                _distance=std::max(_distance,q.z+std::max(std::abs(q.x)/tx,std::abs(q.y)/ty)*1.1);
                _distance=std::max(_distance,q.z+inspection.nearClip*2);
            }
        }
    }
    if(_lookThrough){_lookThrough=false;Q_EMIT lookThroughCameraChanged(false);}
    _dirty=true;refreshScene();
}
void Scene3DViewer::refreshScene() {
    if(!_tab->getInternalNode())return;
    const auto viewer=_tab->getInternalNode();
    const auto source=rendererForViewer(_tab);
    if(!source){endDrag();_source.reset();_graphSelection.clear();_selection.clear();_handlesVisible=false;_snapshotValid=false;_scene={};_objectDisplay.clear();_selected.reset();_selectedObject=-1;_error=tr("Connect Render3D to this Viewer to inspect a 3D scene.");_frame={};if(_stageValue){_stageValue.reset();Q_EMIT stageValueChanged(true);}update();return;}
    const double time=source->getApp()->getTimeLine()->currentFrame();
    const U64 hash=source->getHashValue();
    const int view=_tab->getCurrentView().value(),lut=viewer->getLutType();
    const double gain=_tab->getGain(),gamma=_tab->getGamma();
    auto renderNode=dynamic_cast<NativeScene3D*>(source->getEffectInstance().get());
    const auto quality=renderNode->hdNvpSettingsAt(time,_tab->getCurrentView(),true);
    const auto settingsKey=QStringLiteral("%1/%2/%3/%4").arg(renderNode->rendererBackendAt(time,_tab->getCurrentView())).arg(quality.samples).arg(quality.bounces).arg(int(quality.denoiser));
    if(settingsKey!=_renderSettingsKey)_dirty=true;
    QString labels;for(const auto& object:_scene.objects)labels+=QString::fromStdString(object.owner->getLabel())+QStringLiteral(";");
    const bool labelsChanged=labels!=_sceneLabels;
    if(labelsChanged)_dirty=true;
    if(_previewRunning) {
        if(_source.lock()!=source || hash!=_hash || time!=_time || view!=_view || lut!=_lut || gain!=_gain || gamma!=_gamma)_dirty=true;
        if(_usdBridge && !_dirty){auto value=_usdBridge->sceneValue();if(value && value!=_previewValueBefore && value!=_stageValue){_stageValue=value;Q_EMIT stageValueChanged(_outlineReset);_outlineReset=false;}}
        if(_dirty && _previewCancelled)_previewCancelled->store(true);
        return;
    }
    if((_dragAxis>=0 || _marquee) && (time!=_dragTime || view!=_view || source!=_source.lock()))endDrag();
    if(!_dirty && _source.lock()==source && hash==_hash && time==_time && view==_view && lut==_lut && gain==_gain && gamma==_gamma) {
        if(syncGraphSelection()){updateHandles();update();}return;
    }
    const bool sourceChanged=_source.lock()!=source;
    const bool reload=!_snapshotValid || sourceChanged || hash!=_hash || time!=_time || view!=_view || labelsChanged;
    if(sourceChanged && _snapshotValid)_primSelection.clear();
    _source=source;_hash=hash;_time=time;_dirty=false;
    _view=view;_lut=lut;_gain=gain;_gamma=gamma;
    _renderSettingsKey=settingsKey;
    try {
        auto native=dynamic_cast<NativeScene3D*>(source->getEffectInstance().get());
        if(reload){
            if(_marquee)endDrag();
            _scene=native->sceneAt(time,_tab->getCurrentView());_cards=native->texturedScene(_scene,_tab->getCurrentView(),true);
            native->textureLights(_scene,_tab->getCurrentView(),true);
            _snapshotValid=true;
            _sceneLabels.clear();for(const auto& object:_scene.objects)_sceneLabels+=QString::fromStdString(object.owner->getLabel())+QStringLiteral(";");
            const auto oldSize=_selection.size();
            _selection.erase(std::remove_if(_selection.begin(),_selection.end(),[&](const NodeWPtr& weak){
                const auto node=weak.lock();return !node || std::none_of(_scene.objects.begin(),_scene.objects.end(),[&](const Scene3DObject& object){return object.owner==node;});
            }),_selection.end());
            if(_selection.size()!=oldSize)endDrag();
            if(!isSelected(_selected.lock())){_selected=_selection.empty()?NodeWPtr():_selection.back();_selectedObject=-1;}
            Q_EMIT sceneSnapshotChanged();
        }
        _camera=_lookThrough?(_lookCameraPath.isEmpty()?_scene.camera:_lookCamera):Camera();
        if(!_lookThrough) {
            _camera.label="Viewer inspection camera";
            _camera.orthographic=false;
            const Matrix rotation=Matrix::transform({}, {_pitch,_yaw,0});
            _camera.transform=rotation;const auto t=_pivot+rotation.vector({0,0,_distance});
            _camera.transform.m[12]=t.x;_camera.transform.m[13]=t.y;_camera.transform.m[14]=t.z;
        }
        _settings.width=std::min(640,std::max(1,width()));_settings.height=std::max(1,int(double(_settings.width)*height()/std::max(1,width())));_settings.samples=1;
        if(_lookThrough) {
            Format format;source->getApp()->getProject()->getProjectDefaultFormat(&format);
            _settings.height=std::max(1,int(double(_settings.width)*format.height()/(format.width()*format.getPixelAspectRatio())));
        }
        const bool depthPreview=native->depthPreviewAt(time,_tab->getCurrentView());
        _settings.pass=depthPreview?RenderPass::Depth:RenderPass::Color;
        _settings.lights=_scene.lights;
        _settings.edits=_scene.usdEdits;
        _settings.stageCamera=std::make_shared<Camera>(_scene.camera);
        Format stageFormat;source->getApp()->getProject()->getProjectDefaultFormat(&stageFormat);
        _settings.stageCameraAspect=double(stageFormat.width())*stageFormat.getPixelAspectRatio()/std::max(1,stageFormat.height());
        std::vector<float> pixels;
        if(native->rendererBackendAt(time,_tab->getCurrentView())==3) {
            if(!_usdBridge)_usdBridge=std::make_shared<UsdSceneBridge>();
            if(!_previewWatcher){
                _previewWatcher=new QFutureWatcher<PreviewReply>(this);
                connect(_previewWatcher,&QFutureWatcher<PreviewReply>::finished,this,[this]{
                    _previewRunning=false;auto reply=_previewWatcher->result();
                    auto source=_source.lock();
                    if(!source || source->getHashValue()!=_hash || source->getApp()->getTimeLine()->currentFrame()!=_time || _dirty){_dirty=true;if(isVisible())refreshScene();return;}
                    if(auto value=_usdBridge->sceneValue())if(value!=_stageValue){_stageValue=value;Q_EMIT stageValueChanged(_outlineReset);_outlineReset=false;}
                    if(!reply.error.isEmpty()){_error=reply.error;update();return;}
                    setProperty("hdNvpReport",QString::fromStdString(reply.frame.report));
                    _pickFrame={};_pickFrame.revision=reply.frame.revision;_pickFrame.primIds=std::move(reply.frame.primIds);_pickFrame.instanceIds=std::move(reply.frame.instanceIds);_pickFrame.picks=std::move(reply.frame.picks);
                    auto pixels=std::move(reply.frame.color);
                    if(_settings.pass==RenderPass::Depth)for(size_t i=0;i<reply.frame.depth.size();++i){for(int c=0;c<3;++c)pixels[i*4+c]=reply.frame.depth[i];pixels[i*4+3]=reply.frame.depth[i]>0?1.f:0.f;}
                    _error.clear();applyPixels(std::move(pixels));
                });
            }
            const auto bridge=_usdBridge;const auto cards=_cards;const auto camera=_camera;const auto settings=_settings;const auto sources=_scene.usdSources;
            const auto quality=native->hdNvpSettingsAt(time,_tab->getCurrentView(),true);
            _previewValueBefore=_usdBridge->sceneValue();
            _outlineReset=reload;
            _backend=QStringLiteral("USD / hdNVP");
            if(_frame.isNull())_imageRect=rect();
            _previewRunning=true;_error.clear();updateObjects();syncGraphSelection();updateHandles();update();
            _previewCancelled=std::make_shared<std::atomic<bool>>(false);const auto cancelled=_previewCancelled;
            const QPointer<Scene3DViewer> target(this);
            const auto pending=std::make_shared<std::atomic<bool>>(false);
            _previewWatcher->setFuture(QtConcurrent::run([bridge,cards,camera,settings,sources,quality,cancelled,target,pending]{PreviewReply reply;try{
                reply.frame=bridge->render(cards,camera,settings,quality,[cancelled]{return cancelled->load();},sources,[target,cancelled,settings,pending](const HdNvpFrame& frame){
                    if(!target || pending->exchange(true))return;
                    auto pixels=frame.color;if(settings.pass==RenderPass::Depth)for(size_t i=0;i<frame.depth.size();++i){for(int c=0;c<3;++c)pixels[i*4+c]=frame.depth[i];pixels[i*4+3]=frame.depth[i]>0?1.f:0.f;}
                    QMetaObject::invokeMethod(target,[target,cancelled,pending,pixels=std::move(pixels)]()mutable{pending->store(false);if(target && !cancelled->load() && !target->_dirty){target->setProperty("progressiveFrames",target->property("progressiveFrames").toInt()+1);target->applyPixels(std::move(pixels));}},Qt::QueuedConnection);
                });
            }catch(const std::exception& e){reply.error=QString::fromUtf8(e.what());}return reply;}));
            return;
        } else try {
            const auto context=sharedVulkanImageContext();if(!context)throw std::runtime_error("Vulkan is unavailable.");
            if(!_renderer || _device!=context->device()) {_renderer.reset();_device=context->device();_renderer=std::make_unique<Renderer>(_device);}
            pixels=_renderer->render(_cards,_camera,_settings);_backend=QStringLiteral("Vulkan");
        }catch(const std::exception&) {pixels=renderCpu(_cards,_camera,_settings);_backend=QStringLiteral("CPU");}
        if(_stageValue){_stageValue.reset();Q_EMIT stageValueChanged(true);}
        applyPixels(std::move(pixels));
    }catch(const std::exception& e){endDrag();_handlesVisible=false;_snapshotValid=false;_objectDisplay.clear();_scene={};_cards.clear();_selectedObject=-1;_error=QString::fromUtf8(e.what());_frame={};if(_stageValue){_stageValue.reset();Q_EMIT stageValueChanged(true);}}
    update();
}
void Scene3DViewer::applyPixels(std::vector<float> pixels) {
        if(_settings.pass==RenderPass::Depth)if(auto source=_source.lock())if(auto native=dynamic_cast<NativeScene3D*>(source->getEffectInstance().get())) {
            const auto range=native->depthPreviewRangeAt(_time,_tab->getCurrentView());makeDepthPreview(pixels,range[0],range[1]);
        }
        _frame=QImage(_settings.width,_settings.height,QImage::Format_RGBA8888);
        const auto displayLut=ViewerInstance::lutFromColorspace(ViewerColorSpaceEnum(_lut));
        for(int y=0;y<_settings.height;++y) {
            auto row=_frame.scanLine(_settings.height-1-y);
            for(int x=0;x<_settings.width;++x) {
                const auto p=pixels.data()+(size_t(y)*_settings.width+x)*4;
                // Same scene-linear content as the render; display over a neutral backdrop.
                for(int c=0;c<3;++c) {
                    const float linear=std::max(0.f,float((p[c]+.055f*(1-p[3]))*_gain));
                    const float encoded=displayLut?displayLut->toColorSpaceFloatFromLinearFloat(linear):linear;
                    const float display=_gamma>0?std::pow(encoded,float(1/_gamma)):0.f;
                    row[x*4+c]=uint8_t(std::clamp(display*255.f,0.f,255.f));
                }
                row[x*4+3]=255;
            }
        }
        const QSizeF imageSize=_frame.size();const double scale=std::min(width()/imageSize.width(),height()/imageSize.height());
        const QSizeF fit=imageSize*scale;_imageRect=QRectF(QPointF((width()-fit.width())*.5,(height()-fit.height())*.5),fit);
        _error.clear();updateObjects();syncGraphSelection();updateHandles();
    update();
}
QPointF Scene3DViewer::project(Vec3 world,bool* visible) const {
    const Vec3 q=_camera.transform.inverse().point(world);
    const double aspect=double(_settings.width)/_settings.height;
    const double tx=_camera.orthographic?_camera.orthoWidth*.5:_camera.aperture/(2*_camera.focal),ty=tx/aspect;
    const bool valid=-q.z>_camera.nearClip;
    if(visible)*visible=valid;if(!valid)return {-100000,-100000};
    const double nx=(_camera.orthographic?q.x/tx:q.x/(-q.z*tx))-2*_camera.offsetX,ny=(_camera.orthographic?q.y/ty:q.y/(-q.z*ty))-2*_camera.offsetY*tx/ty;
    return {_imageRect.left()+(nx+1)*.5*_imageRect.width(),_imageRect.bottom()-(ny+1)*.5*_imageRect.height()};
}
const Scene3DObject* Scene3DViewer::selectedObject() const {
    const auto selected=_selected.lock();if(!selected)return nullptr;
    if(_selectedObject>=0 && _selectedObject<int(_scene.objects.size()) && _scene.objects[_selectedObject].owner==selected)return &_scene.objects[_selectedObject];
    for(const auto& object:_scene.objects)if(object.owner==selected && object.time==_time)return &object;
    for(const auto& object:_scene.objects)if(object.owner==selected)return &object;
    return nullptr;
}
void Scene3DViewer::selectObject(int index,bool updateGraph) {
    selectObjects(index<0?std::vector<int>():std::vector<int>{index},updateGraph);
}
bool Scene3DViewer::isSelected(const NodePtr& node) const {
    return node && std::any_of(_selection.begin(),_selection.end(),[&](const NodeWPtr& weak){return weak.lock()==node;});
}
std::vector<const Scene3DObject*> Scene3DViewer::selectedObjects() const {
    std::vector<const Scene3DObject*> result;
    for(const auto& weak:_selection)if(const auto node=weak.lock()) {
        if(node==_selected.lock()){if(const auto object=selectedObject())result.push_back(object);continue;}
        const Scene3DObject* found=nullptr;
        for(const auto& object:_scene.objects)if(object.owner==node){found=&object;if(object.time==_time)break;}
        if(found)result.push_back(found);
    }
    return result;
}
Vec3 Scene3DViewer::selectionCenter() const {
    Vec3 center;const auto objects=selectedObjects();for(const auto object:objects)center=center+object->transform.point({});
    return objects.empty()?Vec3{}:center*(1./objects.size());
}
std::vector<int> Scene3DViewer::graphSelectionIndices() const {
    std::vector<int> indices;std::set<NodeGraph*> graphs;std::set<Node*> seen;
    for(const auto& object:_scene.objects)if(auto gui=std::dynamic_pointer_cast<NodeGui>(object.owner->getNodeGui())) {
        const auto graph=gui->getDagGui();if(!graphs.insert(graph).second)continue;
        for(const auto& selected:graph->getSelectedNodes()) {
            int index=-1;
            for(size_t i=0;i<_scene.objects.size();++i)if(_scene.objects[i].owner==selected->getNode()) {
                index=int(i);if(_scene.objects[i].time==_time)break;
            }
            if(index>=0 && seen.insert(selected->getNode().get()).second)indices.push_back(index);
        }
    }
    return indices;
}
void Scene3DViewer::selectObjects(const std::vector<int>& indices,bool updateGraph) {
    _primSelection.clear();
    endDrag();_selection.clear();_selected.reset();_selectedObject=-1;std::set<Node*> seen;
    std::map<NodeGraph*,NodesGuiList> graphNodes;
    if(updateGraph)for(const auto& object:_scene.objects)if(auto gui=std::dynamic_pointer_cast<NodeGui>(object.owner->getNodeGui()))graphNodes[gui->getDagGui()];
    for(const int index:indices)if(index>=0 && index<int(_scene.objects.size())) {
        const auto node=_scene.objects[index].owner;if(!seen.insert(node.get()).second)continue;
        _selection.push_back(node);_selected=node;_selectedObject=index;
        if(auto gui=std::dynamic_pointer_cast<NodeGui>(node->getNodeGui())) {
            gui->ensurePanelCreated();if(updateGraph)graphNodes[gui->getDagGui()].push_back(gui);
        }
    }
    if(updateGraph) {
        for(const auto& entry:graphNodes)entry.first->setSelection(entry.second);
        _graphSelection.clear();for(const int index:graphSelectionIndices())_graphSelection.push_back(_scene.objects[index].owner);
        if(auto node=_selected.lock())if(auto gui=std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()))gui->setVisibleSettingsPanel(true);
    }
    updateHandles();update();
    QStringList paths;for(const auto& weak:_selection)if(auto node=weak.lock())if(auto native=dynamic_cast<NativeScene3D*>(node->getEffectInstance().get())) {
        if(geometryLeaf(native->kind()) && native->kind()!=Scene3DKind::ReadUSD)paths<<QStringLiteral("/Natron/Geometry/n_")+QString::fromStdString(native->sceneId());
        else if(native->kind()==Scene3DKind::Light)paths<<QStringLiteral("/Natron/Lights/n_")+QString::fromStdString(native->sceneId());
    }Q_EMIT primPathsSelected(paths);
}
bool Scene3DViewer::syncGraphSelection() {
    const auto indices=graphSelectionIndices();bool same=indices.size()==_graphSelection.size();
    if(same)for(size_t i=0;i<indices.size();++i)if(_scene.objects[indices[i]].owner!=_graphSelection[i].lock()){same=false;break;}
    if(same)return false;
    _graphSelection.clear();for(const int index:indices)_graphSelection.push_back(_scene.objects[index].owner);
    selectObjects(indices,false);return true;
}
void Scene3DViewer::updateObjects() {
    _objectDisplay.assign(_scene.objects.size(),{});
    for(size_t i=0;i<_scene.objects.size();++i) {
        const auto& object=_scene.objects[i];auto& display=_objectDisplay[i];bool visible;
        const Vec3 origin=object.transform.point({});display.icon=project(origin,&visible);
        if(object.kind==Scene3DKind::Axis)display.icon+=QPointF(12,-12);
        else if(object.kind==Scene3DKind::TransformGeo)display.icon+=QPointF(-12,-12);
        display.visible=visible && _imageRect.contains(display.icon);
        if(!cameraKind(object.kind))continue;
        const auto& camera=object.camera;const Matrix inverse=camera.transform.inverse();double depth=2;
        for(const auto& item:_scene.items)depth=std::max(depth,-inverse.point(item.card.transform.point({})).z);
        depth=std::min(camera.farClip,std::max(camera.nearClip,depth));
        const double nearDepth=std::min(depth,std::max(camera.nearClip,depth*.15));
        for(int plane=0;plane<2;++plane) {
            const double z=plane?depth:nearDepth;
            const double halfWidth=camera.orthographic?camera.orthoWidth*.5:z*camera.aperture/(2*camera.focal);
            const double halfHeight=halfWidth/object.cameraAspect;
            QPolygonF polygon;bool allVisible=true;
            for(const Vec3 p:std::array<Vec3,4>{{{-halfWidth,-halfHeight,-z},{halfWidth,-halfHeight,-z},{halfWidth,halfHeight,-z},{-halfWidth,halfHeight,-z}}}) {
                polygon<<project(camera.transform.point(p+Vec3{2*halfWidth*camera.offsetX,2*halfWidth*camera.offsetY,0}),&visible);allVisible &= visible;
            }
            if(allVisible)(plane?display.farPlane:display.nearPlane)=polygon;
        }
    }
}
void Scene3DViewer::updateHandles() {
    _handlesVisible=false;for(auto& ring:_rotationRings)ring.clear();
    const auto item=selectedObject();if(!item || _frame.isNull() || item->time!=_time)return;
    const auto objects=selectedObjects();const bool multiple=objects.size()>1;
    if(multiple && std::any_of(objects.begin(),objects.end(),[&](const Scene3DObject* object){return !geometryLeaf(object->kind) || object->time!=_time;}))return;
    const Vec3 center=multiple && _gizmoMode==0?selectionCenter():item->transform.point({});bool visible;
    _handleCenter=project(center,&visible);if(!visible)return;
    _gizmoViewInverse=orientation(_camera.transform).inverse();
    const auto rotate=std::dynamic_pointer_cast<KnobDouble>(item->owner->getKnobByName("rotate"));
    const Vec3 angles=rotate?Vec3{rotate->getValueAtTime(_time,0,_tab->getCurrentView()),rotate->getValueAtTime(_time,1,_tab->getCurrentView()),rotate->getValueAtTime(_time,2,_tab->getCurrentView())}:Vec3{};
    const Matrix parentOrientation=orientation(item->editParentTransform);
    const Matrix basis=multiple && _gizmoMode==0?Matrix():_gizmoMode==2?parentOrientation*Matrix::transform({},angles):parentOrientation;
    for(int axis=0;axis<3;++axis) {
        if(_gizmoMode==1) {
            // Rz * Ry * Rx Euler knobs rotate about different intermediate axes.
            const Vec3 partial=axis==0?Vec3{0,angles.y,angles.z}:axis==1?Vec3{0,0,angles.z}:Vec3{};
            auto& ringBasis=_rotationBases[axis];ringBasis=parentOrientation*Matrix::transform({},partial);
            const Vec3 u=unitAxis((axis+1)%3),v=unitAxis((axis+2)%3);
            for(int i=0;i<=96;++i) {
                const Vec3 direction=ringBasis.vector(u*std::cos(i*2*pi/96)+v*std::sin(i*2*pi/96));
                _rotationRings[axis]<<_handleCenter+projectGizmoVector(direction,55);
            }
            _handleEnd[axis]=_rotationRings[axis][12];
        }else {
            // Move follows the actual parent axes; normalize their lengths so
            // parent scale never stretches the display. Preserve foreshortening.
            const Vec3 direction=_gizmoMode==0 && !multiple?normalized(item->editParentTransform.vector(unitAxis(axis))):basis.vector(unitAxis(axis));
            _handleEnd[axis]=_handleCenter+projectGizmoVector(direction,65);
        }
    }
    _handlesVisible=true;
}
QPointF Scene3DViewer::projectGizmoVector(Vec3 world,double pixels) const {
    // An orthographic overlay centered at the projected pivot stays stable
    // while panning/dollying. Only camera orientation foreshortens its axes.
    const Vec3 view=_gizmoViewInverse.vector(world);return {pixels*view.x,-pixels*view.y};
}
bool Scene3DViewer::rotationAngle(QPointF position,double* angle) const {
    const QPointF p=position-_dragRotationCenter;
    const auto& u=_dragRotationU;const auto& v=_dragRotationV;
    const double determinant=u.x()*v.y()-u.y()*v.x();if(std::abs(determinant)<1e-8)return false;
    const double x=(p.x()*v.y()-p.y()*v.x())/determinant,y=(u.x()*p.y()-u.y()*p.x())/determinant;
    if(x*x+y*y<1e-12)return false;*angle=std::atan2(y,x);return true;
}
bool Scene3DViewer::lineParameter(QPointF position,double* parameter) const {
    const auto ray=rayAt(position,_dragImageRect,_dragCamera);const Vec3 offset=ray.origin-_dragWorldCenter;
    const double aa=dot(_dragWorldAxis,_dragWorldAxis),dd=dot(ray.direction,ray.direction),ad=dot(_dragWorldAxis,ray.direction);
    const double denominator=aa*dd-ad*ad;if(denominator<=aa*dd*1e-8)return false;
    *parameter=(dot(_dragWorldAxis,offset)*dd-ad*dot(ray.direction,offset))/denominator;return true;
}
void Scene3DViewer::paintEvent(QPaintEvent*) {
    QPainter painter(this);painter.fillRect(rect(),QColor(25,25,25));painter.setRenderHint(QPainter::Antialiasing);
    if(!_frame.isNull())painter.drawImage(_imageRect,_frame);
    if(!_error.isEmpty()){painter.setPen(Qt::white);painter.drawText(rect().adjusted(20,20,-20,-20),Qt::AlignCenter|Qt::TextWordWrap,_error);return;}
    painter.save();painter.setClipRect(_imageRect);
    for(const auto& prim:_primSelection)if(prim.hasBounds) {
        std::array<QPointF,8> corners;std::array<bool,8> visible;
        for(int i=0;i<8;++i)corners[i]=project({i&1?prim.maximum.x:prim.minimum.x,i&2?prim.maximum.y:prim.minimum.y,i&4?prim.maximum.z:prim.minimum.z},&visible[i]);
        painter.setPen(QPen(QColor(255,200,70),2));for(int i=0;i<8;++i)for(int j=0;j<3;++j){const int b=i^(1<<j);if(b>i && visible[i] && visible[b])painter.drawLine(corners[i],corners[b]);}
    }
    if(!_lookThrough) {
        painter.setPen(QColor(100,100,100,150));
        for(int i=-10;i<=10;++i) {
            bool a,b;auto p=project({double(i),-1,-10},&a),q=project({double(i),-1,10},&b);if(a&&b)painter.drawLine(p,q);
            p=project({-10,-1,double(i)},&a);q=project({10,-1,double(i)},&b);if(a&&b)painter.drawLine(p,q);
        }
    }
    for(size_t i=0;i<_scene.items.size();++i) {
        const auto& item=_scene.items[i];const bool active=isSelected(item.owner);
        QPolygonF polygon;bool allVisible=true;
        for(auto p:geometryCorners(item.card)) {
            bool visible;polygon<<project(item.card.transform.point(p),&visible);allVisible &= visible;
        }
        if(!allVisible)continue;
        painter.setPen(QPen(active?QColor(255,200,70):QColor(190,190,190),active?2:1));
        if(item.card.mesh) {
            for(int j=0;j<4;++j){painter.drawLine(polygon[j],polygon[(j+1)%4]);painter.drawLine(polygon[j+4],polygon[(j+1)%4+4]);painter.drawLine(polygon[j],polygon[j+4]);}
        }else painter.drawPolygon(polygon);
        if(active)painter.drawText(polygon[3]+QPointF(4,-4),QString::fromStdString(item.owner->getLabel()));
    }
    if(_handlesVisible) {
        const std::array<QColor,3> colors{QColor(255,90,90),QColor(90,230,110),QColor(90,140,255)};
        for(int axis=0;axis<3;++axis) {
            painter.setPen(QPen(colors[axis],_dragAxis==axis?4:2));painter.setBrush(colors[axis]);
            if(_gizmoMode==1) {
                if(_rotationRings[axis].isEmpty())continue;
                painter.drawPolyline(_rotationRings[axis]);painter.drawEllipse(_handleEnd[axis],4,4);
            }else {
                if(QLineF(_handleCenter,_handleEnd[axis]).length()<1)continue;
                painter.drawLine(_handleCenter,_handleEnd[axis]);
                if(_gizmoMode==2)painter.drawRect(QRectF(_handleEnd[axis]-QPointF(4,4),QSizeF(8,8)));
                else painter.drawEllipse(_handleEnd[axis],4,4);
            }
            painter.drawText(_handleEnd[axis]+QPointF(5,-5),QStringLiteral("XYZ").mid(axis,1));
        }
        if(_gizmoMode==2){painter.setPen(Qt::white);painter.setBrush(Qt::white);painter.drawRect(QRectF(_handleCenter-QPointF(4,4),QSizeF(8,8)));}
    }
    for(size_t i=0;i<_scene.objects.size() && i<_objectDisplay.size();++i) {
        const auto& object=_scene.objects[i];const auto& display=_objectDisplay[i];if(geometryLeaf(object.kind))continue;
        const bool active=isSelected(object.owner);const QColor color=active?QColor(255,200,70):cameraKind(object.kind)?
            (object.projectionCamera?QColor(230,140,240):QColor(120,200,250)):object.kind==Scene3DKind::Axis?QColor(100,220,200):QColor(190,160,250);
        painter.setPen(QPen(color,active?2:1,object.time==_time?Qt::SolidLine:Qt::DashLine));painter.setBrush(Qt::NoBrush);
        if(cameraKind(object.kind)) {
            if(!display.nearPlane.isEmpty())painter.drawPolygon(display.nearPlane);
            if(!display.farPlane.isEmpty())painter.drawPolygon(display.farPlane);
            if(display.nearPlane.size()==4 && display.farPlane.size()==4)
                for(int corner=0;corner<4;++corner)painter.drawLine(display.nearPlane[corner],display.farPlane[corner]);
        }
        if(!display.visible)continue;
        const QPointF p=display.icon;
        if(cameraKind(object.kind))painter.drawRect(QRectF(p-QPointF(6,4),QSizeF(12,8)));
        else if(object.kind==Scene3DKind::Light) {
            painter.drawEllipse(p,5,5);
            for(int ray=0;ray<8;++ray) {
                const QPointF direction(std::cos(ray*pi/4),std::sin(ray*pi/4));painter.drawLine(p+direction*7,p+direction*10);
            }
            bool visible;const QPointF tip=project(object.transform.point({0,0,-.6}),&visible);
            if(visible && (object.light.type==LightType::Distant || object.light.type==LightType::Spot))painter.drawLine(p,tip);
        } else {
            painter.drawLine(project(object.transform.point({})),p);
            if(object.kind==Scene3DKind::Axis)painter.drawPolygon(QPolygonF{p+QPointF(0,-5),p+QPointF(5,0),p+QPointF(0,5),p+QPointF(-5,0)});
            else painter.drawRect(QRectF(p-QPointF(5,5),QSizeF(10,10)));
        }
        QString label=QString::fromStdString(object.owner->getLabel());
        if(cameraKind(object.kind))label+=object.projectionCamera?tr(" (projection)"):tr(" (render)");
        if(object.time!=_time)label+=tr(" [frame %1]").arg(object.time);
        if(active)painter.drawText(p+QPointF(9,-6),label);
    }
    painter.restore();
    if(_marquee && QLineF(_dragStart,_marqueeEnd).length()>=4) {
        painter.setPen(QPen(QColor(255,200,70),1,Qt::DashLine));painter.setBrush(QColor(255,200,70,30));
        painter.drawRect(QRectF(_dragStart,_marqueeEnd).normalized());
    }
    painter.setPen(Qt::white);
    QString title=_lookThrough?tr("Render camera"):tr("Perspective");
    if(_previewRunning)title+=tr(" · Rendering...");
    if(!_primSelection.empty())title+=tr(" · %1 prims selected").arg(_primSelection.size());
    if(_settings.pass==Gpu::Cards::RenderPass::Depth)title+=QStringLiteral(" · ")+tr("Depth preview");
    const auto objects=selectedObjects();
    if(objects.size()>1)title+=tr(" · %1 objects selected").arg(objects.size());
    else if(const auto object=selectedObject())title+=QStringLiteral(" · ")+objectKind(object->kind)+QStringLiteral(": ")+QString::fromStdString(object->owner->getLabel());
    painter.drawText(12,20,title);
    if(objects.size()>1)painter.drawText(12,38,_handlesVisible?(_gizmoMode==0?tr("World-space move"):tr("Local rotation / scale at each object's origin")):tr("Select only geometry to manipulate multiple objects"));
    else if(const auto object=selectedObject())if(object->time!=_time)
        painter.drawText(12,38,tr("Fixed projection: edit this camera at reference frame %1").arg(object->time));
    painter.drawText(12,height()-12,tr("W/E/R: tools   Shift: add   Box: select   Alt: orbit   Middle: pan   Wheel: dolly   F: frame"));
}
int Scene3DViewer::hitObject(QPointF position) const {
    if(_previewRunning)return -1;
    if(!_error.isEmpty() || !_imageRect.contains(position))return -1;
    int objectIndex=-1;double iconDistance=9;
    for(size_t i=0;i<_scene.objects.size() && i<_objectDisplay.size();++i) {
        if(geometryLeaf(_scene.objects[i].kind) || !_objectDisplay[i].visible)continue;
        const double distance=QLineF(position,_objectDisplay[i].icon).length();
        if(distance<iconDistance){iconDistance=distance;objectIndex=int(i);}
    }
    const double x=(position.x()-_imageRect.left())/_imageRect.width()*_settings.width;
    const double y=(_imageRect.bottom()-position.y())/_imageRect.height()*_settings.height;
    int index=-1;double depth=1e100;
    for(size_t i=0;i<_cards.size();++i) {
        if(!_cards[i].bindingPaths.empty())continue;
        auto geometry=_cards[i];geometry.projected=false;
        const Hit hit=intersect(geometry,_camera,_settings,x,y);
        if(hit.card>=0 && hit.depth<depth){depth=hit.depth;index=int(i);}
    }
    for(size_t i=0;i<_scene.objects.size();++i)if(index>=0 && _scene.objects[i].itemIndex==index) {
        // Card centers can overlap a parent's offset icon as the viewport grows.
        // Prefer the visible card at its center, while preserving icon picking
        // and nearest-surface picking away from the center.
        if(objectIndex<0 || (i<_objectDisplay.size() && QLineF(position,_objectDisplay[i].icon).length()<iconDistance))objectIndex=int(i);
        break;
    }
    return objectIndex;
}
void Scene3DViewer::selectAt(QPointF position) {
    const auto path=primAt(position);
    if(!path.isEmpty() && _stageValue) {
        try{setPrimSelection({_stageValue->inspect(path.toStdString())});Q_EMIT primPathsSelected({path});}catch(const std::exception&){}return;
    }
    selectObject(hitObject(position),true);
}
QString Scene3DViewer::primAt(QPointF position)const {
    if(_previewRunning || _dirty || !_stageValue || _pickFrame.revision!=_stageValue->revision() || !_imageRect.contains(position))return {};
    const int x=int((position.x()-_imageRect.left())/_imageRect.width()*_settings.width),y=int((_imageRect.bottom()-position.y())/_imageRect.height()*_settings.height);
    if(x<0 || y<0 || x>=_settings.width || y>=_settings.height)return {};
    const auto i=size_t(y)*_settings.width+x;if(i>=_pickFrame.primIds.size() || i>=_pickFrame.instanceIds.size())return {};
    return QString::fromStdString(_pickFrame.pickPath(_pickFrame.primIds[i],_pickFrame.instanceIds[i]));
}
void Scene3DViewer::finishMarquee() {
    if(!_previewRunning && !_dirty && _stageValue && _pickFrame.revision==_stageValue->revision()) {
        QStringList paths=_marqueeAdd?selectedPrimPaths():QStringList();
        const QRectF box=QRectF(_dragStart,_marqueeEnd).normalized().intersected(_imageRect);
        if(QLineF(_dragStart,_marqueeEnd).length()<4) {
            const auto path=primAt(_dragStart);if(!path.isEmpty()){if(paths.contains(path))paths.removeAll(path);else paths<<path;}
        } else {
            const int x1=std::max(0,int((box.left()-_imageRect.left())/_imageRect.width()*_settings.width)),x2=std::min(_settings.width,int(std::ceil((box.right()-_imageRect.left())/_imageRect.width()*_settings.width)));
            const int y1=std::max(0,int((_imageRect.bottom()-box.bottom())/_imageRect.height()*_settings.height)),y2=std::min(_settings.height,int(std::ceil((_imageRect.bottom()-box.top())/_imageRect.height()*_settings.height)));
            std::set<std::pair<int32_t,int32_t>> ids;
            for(int y=y1;y<y2;++y)for(int x=x1;x<x2;++x){const size_t i=size_t(y)*_settings.width+x;ids.emplace(_pickFrame.primIds[i],_pickFrame.instanceIds[i]);}
            for(const auto& id:ids){const auto p=QString::fromStdString(_pickFrame.pickPath(id.first,id.second));if(!p.isEmpty() && !paths.contains(p))paths<<p;}
        }
        std::vector<UsdPrimInfo> prims;try{for(const auto& path:paths)prims.push_back(_stageValue->inspect(path.toStdString()));}catch(const std::exception&){}
        if(!prims.empty() || _marqueeStartHit<0){setPrimSelection(prims);Q_EMIT primPathsSelected(paths);return;}
    }
    std::vector<int> indices;
    if(_marqueeAdd)for(const auto object:selectedObjects())indices.push_back(int(object-_scene.objects.data()));
    if(QLineF(_dragStart,_marqueeEnd).length()<4) {
        if(_marqueeStartHit>=0) {
            const auto owner=_scene.objects[_marqueeStartHit].owner;
            const auto found=std::find_if(indices.begin(),indices.end(),[&](int i){return _scene.objects[i].owner==owner;});
            if(found!=indices.end())indices.erase(found);else indices.push_back(_marqueeStartHit);
        }
    }else {
        QPainterPath box;box.addRect(QRectF(_dragStart,_marqueeEnd).normalized().intersected(_imageRect));
        for(size_t i=0;i<_scene.objects.size();++i)if(geometryLeaf(_scene.objects[i].kind)) {
            const auto& card=_scene.items[_scene.objects[i].itemIndex].card;QPolygonF polygon;bool allVisible=true;
            for(const Vec3 p:geometryCorners(card)) {
                bool visible;polygon<<project(card.transform.point(p),&visible);allVisible &= visible;
            }
            QPainterPath shape;if(card.mesh)shape.addRect(polygon.boundingRect());else {shape.addPolygon(polygon);shape.closeSubpath();}
            if(allVisible && box.intersects(shape))indices.push_back(int(i));
        }
    }
    selectObjects(indices,true);
}
void Scene3DViewer::mousePressEvent(QMouseEvent* event) {
    setFocus();endDrag();refreshScene();_lastMouse=_dragStart=event->position();
    if(!_error.isEmpty()){event->accept();return;}
    if(event->button()==Qt::LeftButton && !(event->modifiers()&Qt::AltModifier)) {
        if(event->modifiers()&Qt::ShiftModifier) {
            _marquee=true;_marqueeAdd=true;_marqueeEnd=_dragStart;_marqueeStartHit=hitObject(_dragStart);_dragTime=_time;
            event->accept();return;
        }
        // Preserve icon picking over a handle's shaft/ring, while an endpoint
        // clicked directly takes precedence. Shift-click bypasses all handles.
        const int hit=hitObject(event->position());double endpointDistance=1e100;
        if(_handlesVisible) {
            if(_gizmoMode==2)endpointDistance=QLineF(event->position(),_handleCenter).length();
            for(int axis=0;axis<3;++axis)if(QLineF(_handleCenter,_handleEnd[axis]).length()>1)
                endpointDistance=std::min(endpointDistance,QLineF(event->position(),_handleEnd[axis]).length());
        }
        if(hit>=0 && !geometryLeaf(_scene.objects[hit].kind) && hit!=_selectedObject &&
            QLineF(event->position(),_objectDisplay[hit].icon).length()<endpointDistance) {
            selectObject(hit,true);event->accept();return;
        }
        if(_handlesVisible && (_gizmoMode==2 || QLineF(event->position(),_handleCenter).length()>10)) {
            double nearest=9;
            if(_gizmoMode==2 && QLineF(event->position(),_handleCenter).length()<9)_dragAxis=3;
            else for(int axis=0;axis<3;++axis) {
                if(_gizmoMode==1) {
                    const auto& ring=_rotationRings[axis];
                    for(int j=1;j<ring.size();++j) {
                        const double distance=lineDistance(event->position(),ring[j-1],ring[j]);
                        if(distance<nearest){nearest=distance;_dragAxis=axis;_dragTangent=ring[j]-ring[j-1];}
                    }
                }else if(QLineF(_handleCenter,_handleEnd[axis]).length()>1) {
                    const double distance=lineDistance(event->position(),_handleCenter,_handleEnd[axis]);
                    if(distance<nearest){nearest=distance;_dragAxis=axis;}
                }
            }
        }
        if(_dragAxis<0) {
            if(hitObject(event->position())>=0 || !primAt(event->position()).isEmpty())selectAt(event->position());
            else {_marquee=true;_marqueeAdd=false;_marqueeEnd=_dragStart;_marqueeStartHit=-1;_dragTime=_time;}
        }
        else if(const auto item=selectedObject()) {
            const char* names[]={"translate","rotate","scale"};
            const auto knob=std::dynamic_pointer_cast<KnobDouble>(item->owner->getKnobByName(names[_gizmoMode]));
            if(!knob){endDrag();event->accept();return;}
            const auto objects=selectedObjects();const bool multiple=objects.size()>1;
            if(multiple)for(const auto object:objects) {
                const auto targetKnob=std::dynamic_pointer_cast<KnobDouble>(object->owner->getKnobByName(names[_gizmoMode]));
                const auto targetGui=std::dynamic_pointer_cast<NodeGui>(object->owner->getNodeGui());
                const auto knobGui=targetGui && targetGui->getSettingPanel()?targetGui->getSettingPanel()->getKnobGui(targetKnob):KnobGuiPtr();
                if(!targetKnob || !knobGui){endDrag();event->accept();return;}
                DragTarget target;target.knob=targetKnob;target.gui=knobGui;target.parentInverse=object->editParentTransform.inverse();
                for(int i=0;i<3;++i)target.values[i]=targetKnob->getValueAtTime(_time,i,_tab->getCurrentView());
                _dragTargets.push_back(target);
            }
            _dragKnob=knob;_dragTime=_time;_dragWorldCenter=multiple && _gizmoMode==0?selectionCenter():item->transform.point({});
            _dragCamera=_camera;_dragImageRect=_imageRect;
            for(int i=0;i<3;++i)_dragValues[i]=knob->getValueAtTime(_time,i,_tab->getCurrentView());
            if(_gizmoMode==0) {
                _dragWorldAxis=multiple?unitAxis(_dragAxis):item->editParentTransform.vector(unitAxis(_dragAxis));
                if(!lineParameter(event->position(),&_dragLineStart))endDrag();
            }else if(_gizmoMode==1) {
                _dragRotation=0;_dragRotationCenter=_handleCenter;
                const auto& basis=_rotationBases[_dragAxis];
                _dragRotationU=projectGizmoVector(basis.vector(unitAxis((_dragAxis+1)%3)),55);
                _dragRotationV=projectGizmoVector(basis.vector(unitAxis((_dragAxis+2)%3)),55);
                const double determinant=_dragRotationU.x()*_dragRotationV.y()-_dragRotationU.y()*_dragRotationV.x();
                _geometricRotation=std::abs(determinant)>.08*55*55 && rotationAngle(event->position(),&_dragAngle);
                const double length=std::sqrt(_dragTangent.x()*_dragTangent.x()+_dragTangent.y()*_dragTangent.y());
                _dragTangent=length>1e-8?_dragTangent/length:QPointF(1,0);
            }else if(_dragAxis<3) {
                _dragTangent=_handleEnd[_dragAxis]-_handleCenter;
                _dragTangent/=QLineF(_handleEnd[_dragAxis],_handleCenter).length();
            }
        }
    }
    event->accept();
}
void Scene3DViewer::mouseMoveEvent(QMouseEvent* event) {
    const QPointF delta=event->position()-_lastMouse;_lastMouse=event->position();
    if(event->buttons()==Qt::NoButton) {
        QString tooltip;double nearest=9;const Scene3DObject* hovered=nullptr;
        for(size_t i=0;i<_scene.objects.size() && i<_objectDisplay.size();++i)if(_objectDisplay[i].visible && QLineF(event->position(),_objectDisplay[i].icon).length()<9) {
            const auto& object=_scene.objects[i];const double distance=QLineF(event->position(),_objectDisplay[i].icon).length();
            if(distance<nearest || (distance==nearest && !geometryLeaf(object.kind))){nearest=distance;hovered=&object;}
        }
        if(hovered) {
            tooltip=objectKind(hovered->kind)+QStringLiteral(": ")+QString::fromStdString(hovered->owner->getLabel());
            if(hovered->time!=_time)tooltip+=tr(" (reference frame %1)").arg(hovered->time);
        }
        setToolTip(tooltip);
    }
    if(_marquee && event->buttons()&Qt::LeftButton) {
        _marqueeEnd=event->position();update();
    }else if(_dragAxis>=0 && event->buttons()&Qt::LeftButton) {
        if(_time!=_dragTime){endDrag();event->accept();return;}
        const auto node=_selected.lock();const auto knob=_dragKnob.lock();
        const auto gui=node?std::dynamic_pointer_cast<NodeGui>(node->getNodeGui()):NodeGuiPtr();
        if(knob && gui && gui->getSettingPanel()) {
            const auto knobGui=gui->getSettingPanel()->getKnobGui(knob);auto values=_dragValues;
            const QPointF total=event->position()-_dragStart;double move=0,factor=1;
            if(_gizmoMode==0) {
                double parameter;if(!lineParameter(event->position(),&parameter)){event->accept();return;}
                move=parameter-_dragLineStart;values[_dragAxis]+=move;
            }else if(_gizmoMode==1) {
                if(_geometricRotation) {
                    double angle;if(!rotationAngle(event->position(),&angle)){event->accept();return;}
                    _dragRotation+=std::remainder(angle-_dragAngle,2*pi)*180/pi;_dragAngle=angle;
                }else _dragRotation+=(delta.x()*_dragTangent.x()+delta.y()*_dragTangent.y())*180/(55*pi);
                values[_dragAxis]+=_dragRotation;
            }else {
                const double distance=_dragAxis==3?total.x()-total.y():total.x()*_dragTangent.x()+total.y()*_dragTangent.y();
                factor=std::exp(std::clamp(distance/65.,-4.,4.));
                for(int i=0;i<3;++i)if(_dragAxis==3 || i==_dragAxis)values[i]*=factor;
            }
            if(knobGui && !_dragTargets.empty()) {
                // Use one command on the primary node's stack for every selected
                // card. Scalar children preserve auto-keying on changed axes only.
                auto command=std::make_unique<QUndoCommand>();
                for(const auto& target:_dragTargets) {
                    const auto targetKnob=target.knob.lock();const auto targetGui=target.gui.lock();
                    if(!targetKnob || !targetGui){endDrag();event->accept();return;}
                    auto targetValues=target.values;
                    const Vec3 localMove=target.parentInverse.vector(_dragWorldAxis*move);
                    for(int i=0;i<3;++i) {
                        if(_gizmoMode==0)targetValues[i]+=component(localMove,i);
                        else if(_gizmoMode==1 && i==_dragAxis)targetValues[i]+=_dragRotation;
                        else if(_gizmoMode==2 && (_dragAxis==3 || i==_dragAxis))targetValues[i]*=factor;
                        const double old=targetKnob->getValueAtTime(_time,i,_tab->getCurrentView());
                        if(std::abs(old-targetValues[i])>1e-10)
                            new KnobUndoCommand<double>(targetGui,old,targetValues[i],i,true,command.get());
                    }
                }
                if(command->childCount()) {
                    if(!_dragUndoStack){_dragUndoStack=gui->getUndoStack();_dragUndoStack->beginMacro(_gizmoMode==0?tr("Move 3D cards"):_gizmoMode==1?tr("Rotate 3D cards"):tr("Scale 3D cards"));}
                    knobGui->pushUndoCommand(command.release());_dirty=true;refreshScene();
                }
            }else if(knobGui) {
                bool changed=false;std::list<double> oldValues,newValues;
                for(int i=0;i<3;++i) {
                    const double old=knob->getValueAtTime(_time,i,_tab->getCurrentView());
                    oldValues.push_back(old);newValues.push_back(values[i]);changed |= std::abs(old-values[i])>1e-10;
                }
                if(changed) {
                    if(!_dragUndoStack){_dragUndoStack=gui->getUndoStack();_dragUndoStack->beginMacro(_gizmoMode==0?tr("Move 3D object"):_gizmoMode==1?tr("Rotate 3D object"):tr("Scale 3D object"));}
                    if(_dragAxis==3)knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui,oldValues,newValues));
                    else knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui,knob->getValueAtTime(_time,_dragAxis,_tab->getCurrentView()),values[_dragAxis],_dragAxis));
                    _dirty=true;refreshScene();
                }
            }
        }else endDrag();
    }else if(!_lookThrough && event->buttons()&Qt::MiddleButton) {
        _pivot=_pivot+Matrix::transform({}, {_pitch,_yaw,0}).vector({-delta.x()*_distance*.002,delta.y()*_distance*.002,0});_dirty=true;refreshScene();
    }else if(!_lookThrough && event->buttons()&Qt::LeftButton && event->modifiers()&Qt::AltModifier) {
        _yaw-=delta.x()*.4;_pitch=std::clamp(_pitch-delta.y()*.4,-89.,89.);_dirty=true;refreshScene();
    }
    event->accept();
}
void Scene3DViewer::mouseReleaseEvent(QMouseEvent* event){
    if(_marquee && event->button()==Qt::LeftButton){_marqueeEnd=event->position();finishMarquee();}
    endDrag();update();event->accept();
}
void Scene3DViewer::wheelEvent(QWheelEvent* event){endDrag();if(!_lookThrough){_distance=std::clamp(_distance*std::exp(-event->angleDelta().y()*.001),Camera().nearClip*2,100000.);_dirty=true;refreshScene();}event->accept();}
void Scene3DViewer::keyPressEvent(QKeyEvent* event) {
    if(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) {
        QWidget::keyPressEvent(event);return;
    }
    if(event->key()==Qt::Key_W || event->key()==Qt::Key_E || event->key()==Qt::Key_R) {
        setGizmoMode(event->key()==Qt::Key_W?0:event->key()==Qt::Key_E?1:2);event->accept();
    }else if(event->key()==Qt::Key_Escape && (_dragAxis>=0 || _marquee)) {
        const auto undo=_dragUndoStack;endDrag();if(undo)undo->undo();_dirty=true;refreshScene();event->accept();
    }else if(event->key()==Qt::Key_F) {
        frameSelection();event->accept();
    }else if(event->key()==Qt::Key_H){setLookThroughCamera(!_lookThrough);event->accept();}
    else QWidget::keyPressEvent(event);
}
void Scene3DViewer::resizeEvent(QResizeEvent* event){endDrag();QWidget::resizeEvent(event);_dirty=true;}
void Scene3DViewer::hideEvent(QHideEvent* event){if(_previewCancelled && _previewRunning){_previewCancelled->store(true);_dirty=true;}endDrag();QWidget::hideEvent(event);}
void Scene3DViewer::focusOutEvent(QFocusEvent* event){endDrag();QWidget::focusOutEvent(event);}
NATRON_NAMESPACE_EXIT
NATRON_NAMESPACE_USING
#include "moc_Scene3DViewer.cpp"
