// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/CameraTracker.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTimer>
#include <QtConcurrentRun>
#include <QtEndian>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <sstream>
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/NativeScene3D.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlaySupport.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoCommand.h"
#ifdef NATRON_HAS_USD
#include "Scene/ReconstructionUsd.h"
#endif

NATRON_NAMESPACE_ENTER
namespace {
struct Domain {int width=0,height=0,x=0,y=0;double par=1;};
struct Work {
    std::atomic<bool> cancel{false};
    std::atomic<double> progress{0};
    std::mutex mutex;
    std::string message;
};
struct Reply {
    Scene::FeatureTracks tracks;
    Scene::CameraReconstruction solution;
    Domain domain;
    std::string usd,error;
    bool solving=false;
};
std::string pack(const QJsonObject& object)
{
    return qCompress(QJsonDocument(object).toJson(QJsonDocument::Compact),6).toBase64().toStdString();
}
QJsonObject unpack(const std::string& text)
{
    if(text.empty())return {};
    if(text.size()>128*1024*1024)throw std::runtime_error("Stored CameraTracker analysis is too large");
    const auto compressed=QByteArray::fromBase64(QByteArray::fromStdString(text));
    if(compressed.size()<4 || qFromBigEndian<quint32>(compressed.constData())>128*1024*1024)throw std::runtime_error("Invalid CameraTracker analysis size");
    const auto bytes=qUncompress(compressed);
    if(bytes.isEmpty() || bytes.size()>128*1024*1024)throw std::runtime_error("Invalid CameraTracker analysis data");
    const auto object=QJsonDocument::fromJson(bytes).object();
    const int version=object.value(QStringLiteral("version")).toInt();
    if(version!=1 && version!=2)throw std::runtime_error("Unsupported CameraTracker analysis version");
    return object;
}
QJsonArray array(const std::array<double,16>& value){QJsonArray result;for(double v:value)result.append(v);return result;}
QString sourceSignature(const EffectInstancePtr& source,std::map<const EffectInstance*,QString>& cache)
{
    if(!source)return {};
    const auto found=cache.find(source.get());if(found!=cache.end())return found->second;
    cache[source.get()]=QStringLiteral("cycle");
    // Rendering hashes include project creation time. Parameter ages and the
    // upstream graph persist across save/reload and still invalidate on edits.
    QJsonArray values{QString::fromStdString(source->getPluginID()),source->getMajorVersion(),source->getMinorVersion(),
        QString::fromStdString(source->getNode()->getScriptName()),QString::number(source->getNode()->getKnobsAge())};
    for(int i=0;i<source->getNInputs();++i)values.append(sourceSignature(source->getInput(i),cache));
    auto result=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(values).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
    cache[source.get()]=result;return result;
}
class AnalysisUndo : public UndoCommand {
    KnobStringWPtr _knob;
    std::string _before,_after;
public:
    AnalysisUndo(const KnobStringPtr& knob,std::string before,std::string after,const std::string& text)
        :UndoCommand(text),_knob(knob),_before(std::move(before)),_after(std::move(after)){}
    void redo()override{if(auto k=_knob.lock())k->setValue(_after);}
    void undo()override{if(auto k=_knob.lock())k->setValue(_before);}
};
ImagePtr fetchPlate(const EffectInstancePtr& effect,double time,ViewIdx view)
{
        ParallelRenderArgsSetter tls(time,view,false,false,AbortableRenderInfoPtr(),effect->getNode(),0,
            effect->getApp()->getTimeLine().get(),NodePtr(),true,false,RenderStatsPtr());
        RectD rod;
        if(effect->getRegionOfDefinition_public(effect->getHash(),time,RenderScale::identity,view,&rod,nullptr)!=eStatusOK || rod.isNull() || rod.isInfinite())
            throw std::runtime_error("CameraTracker needs a finite source image");
        const RectI bounds=rod.toPixelEnclosing(0,effect->getAspectRatio(-1));
        EffectInstance::RenderRoIArgs args(time,RenderScale::identity,0,view,false,bounds,rod,{ImagePlaneDesc::getRGBAComponents()},eImageBitDepthFloat,true,
                                         nullptr,eStorageModeRAM,time);
        std::map<ImagePlaneDesc,ImagePtr> planes;
        if(effect->renderRoI(args,&planes)!=EffectInstance::eRenderRoIRetCodeOk || planes.empty())throw std::runtime_error("Cannot read plate or tracking mask at frame "+std::to_string(time));
        return planes.begin()->second;
}
Scene::TrackingImage fetchImage(const EffectInstancePtr& source,const EffectInstancePtr& mask,double time,ViewIdx view,
                                int maximumWidth,Domain& domain)
{
    const auto image=fetchPlate(source,time,view);
    const RectI format=source->getOutputFormat();
    if(format.isNull() || format.width()>16384 || format.height()>16384)throw std::runtime_error("Unsupported tracking image format");
    domain={format.width(),format.height(),format.x1,format.y1,source->getAspectRatio(-1)};
    const auto matte=mask?fetchPlate(mask,time,view):ImagePtr();
    Scene::TrackingImage result;
    result.width=std::min(maximumWidth,domain.width);
    result.height=std::max(1,int(std::lround(double(domain.height)*result.width/domain.width)));
    result.pixels.resize(size_t(result.width)*result.height);result.mask.resize(result.pixels.size());
    const Image::ReadAccess pixels(image.get());
    std::unique_ptr<Image::ReadAccess> maskPixels;if(matte)maskPixels=std::make_unique<Image::ReadAccess>(matte.get());
    const unsigned channels=image->getComponentsCount(),maskChannels=matte?matte->getComponentsCount():0;
    for(int y=0;y<result.height;++y)for(int x=0;x<result.width;++x) {
        const int sx=domain.x+std::min(domain.width-1,int((x+.5)*domain.width/result.width));
        const int sy=domain.y+domain.height-1-std::min(domain.height-1,int((y+.5)*domain.height/result.height));
        const auto p=reinterpret_cast<const float*>(pixels.pixelAt(sx,sy));const size_t i=size_t(y)*result.width+x;
        if(!p)continue;
        const float alpha=channels==4?p[3]:1;
        const double scale=source->getPremult()==eImagePremultiplicationPremultiplied && alpha>1e-6?1./alpha:1;
        result.pixels[i]=channels>=3?float((.2126*p[0]+.7152*p[1]+.0722*p[2])*scale):p[0];
        result.mask[i]=alpha>.01?1.f:0.f;
        if(maskPixels){const int mx=int(std::floor((sx+.5)*domain.par/mask->getAspectRatio(-1)));const auto m=reinterpret_cast<const float*>(maskPixels->pixelAt(mx,sy));result.mask[i]*=m?(maskChannels==4?m[3]:m[0]):0;}
    }
    return result;
}
}
struct CameraTracker::Private {
    KnobIntPtr first,last,features,spacing,length,width,key1,key2;
    KnobDoublePtr focal,sensor,fx,fy,cx,cy,rms,correlation;
    KnobDoublePtr k1,k2;
    KnobChoicePtr calibrationMode;
    KnobBoolPtr pixels,autoKeys,busy,showTracks,showIds,refinePrincipal;
    KnobButtonPtr track,solve,cancel,create;
    KnobStringPtr status,trackData,solveData,excluded;
    QFutureWatcher<Reply>* watcher=nullptr;
    QTimer* timer=nullptr;
    std::shared_ptr<Work> work;
    mutable std::mutex mutex;
    std::shared_ptr<const Scene::FeatureTracks> tracks;
    std::shared_ptr<const Scene::CameraReconstruction> result;
    Domain domain;
    std::string trackSignature,solveSignature,usd,error;
    uint64_t generation=0;
    mutable QTemporaryDir files;
};
CameraTracker::CameraTracker(NodePtr node):EffectInstance(node),_imp(new Private){setSupportsRenderScaleMaybe(eSupportsYes);}
CameraTracker::~CameraTracker(){if(_imp->work)_imp->work->cancel=true;}
std::string CameraTracker::getPluginDescription()const{return "Track features and solve camera motion, sparse landmarks, focal length and Brown radial lens distortion. For unknown cameras select Estimate focal + radial. Auto calibration uses the plate pixel aspect and a constant lens. Enable Refine principal point for cropped/decentered plates; the additional model is kept only when validation observations improve. Use a static, non-planar scene with translation and rotation. Create USD scene connects an undistorted plate when needed. Physical focal length requires a known sensor width; the estimate is reported in pixels. Analysis and accepted results are saved with the project.";}
void CameraTracker::addAcceptedComponents(int,std::list<ImagePlaneDesc>* c){c->push_back(ImagePlaneDesc::getRGBAComponents());}
void CameraTracker::addSupportedBitDepth(std::list<ImageBitDepthEnum>* d)const{d->push_back(eImageBitDepthFloat);}
bool CameraTracker::isIdentity(double time,const RenderScale&,const RectI&,ViewIdx view,double* t,ViewIdx* v,int* input){*t=time;*v=view;*input=0;return bool(getInput(0));}
void CameraTracker::initializeKnobs()
{
    auto page=AppManager::createKnob<KnobPage>(this,tr("Camera tracking"));page->setName("cameraTracking");
    auto integer=[&](const char* name,const char* label,int value){auto k=AppManager::createKnob<KnobInt>(this,tr(label));k->setName(name);k->setDefaultValue(value);k->setAnimationEnabled(false);page->addKnob(k);return k;};
    auto number=[&](const char* name,const char* label,double value){auto k=AppManager::createKnob<KnobDouble>(this,tr(label));k->setName(name);k->setDefaultValue(value);k->setAnimationEnabled(false);page->addKnob(k);return k;};
    auto boolean=[&](const char* name,const char* label,bool value){auto k=AppManager::createKnob<KnobBool>(this,tr(label));k->setName(name);k->setDefaultValue(value);k->setAnimationEnabled(false);page->addKnob(k);return k;};
    auto button=[&](const char* name,const char* label){auto k=AppManager::createKnob<KnobButton>(this,tr(label));k->setName(name);page->addKnob(k);return k;};
    Format format;getApp()->getProject()->getProjectDefaultFormat(&format);double first=1,last=50;getApp()->getProject()->getFrameRange(&first,&last);
    _imp->first=integer("firstFrame","First frame",int(first));_imp->last=integer("lastFrame","Last frame",int(last));
    _imp->features=integer("maximumFeatures","Maximum features",300);_imp->spacing=integer("featureSeparation","Feature separation (pixels)",12);
    _imp->length=integer("minimumTrackLength","Minimum track length",6);_imp->width=integer("analysisWidth","Maximum analysis width",960);
    _imp->correlation=number("minimumCorrelation","Minimum tracking correlation",.85);
    _imp->track=button("track","Track features");_imp->cancel=button("cancelAnalysis","Cancel analysis");
    _imp->calibrationMode=AppManager::createKnob<KnobChoice>(this,tr("Camera calibration"));_imp->calibrationMode->setName("calibrationMode");_imp->calibrationMode->setAnimationEnabled(false);
    _imp->calibrationMode->populateChoices({ChoiceOption("Known","Known camera","Use the supplied focal/sensor or pixel intrinsics."),ChoiceOption("AutoFocal","Estimate focal","Unknown camera: fixed pixel aspect, no distortion. Principal point starts at the image center; refinement is optional."),ChoiceOption("AutoLens","Estimate focal + radial","Unknown camera: estimate focal length and Brown radial k1/k2. Requires depth variation, translation and rotation; no zoom.")});page->addKnob(_imp->calibrationMode);
    _imp->refinePrincipal=boolean("refinePrincipalPoint","Refine principal point",false);
    _imp->refinePrincipal->setHintToolTip(tr("Automatic modes: compare a freely estimated principal point against the centered model using withheld observations. Keep it only when validation improves. Requires rotation, translation and image coverage. Bounds are 35..65 percent of image dimensions."));
    _imp->pixels=boolean("pixelIntrinsics","Use calibrated pixel intrinsics",false);
    _imp->focal=number("focalLength","Focal length (mm)",35);_imp->sensor=number("sensorWidth","Sensor width (mm)",36);
    _imp->fx=number("focalPixelsX","Focal X (pixels)",format.width()*35./36);_imp->fy=number("focalPixelsY","Focal Y (pixels)",format.width()*35./36*format.getPixelAspectRatio());
    _imp->cx=number("principalPointX","Principal X (pixel-edge coordinates)",format.width()*.5);_imp->cy=number("principalPointY","Principal Y (pixel-edge coordinates)",format.height()*.5);
    _imp->k1=number("radialK1","Known radial k1",0);_imp->k2=number("radialK2","Known radial k2",0);
    _imp->autoKeys=boolean("automaticKeyframes","Automatic solve keyframes",true);
    _imp->key1=integer("keyframe1","First solve keyframe",int(first));_imp->key2=integer("keyframe2","Second solve keyframe",int(last));
    _imp->rms=number("maximumReprojectionError","Maximum solve RMS (pixels)",1.5);
    _imp->excluded=AppManager::createKnob<KnobString>(this,tr("Exclude track IDs"));_imp->excluded->setName("excludedTracks");_imp->excluded->setAnimationEnabled(false);page->addKnob(_imp->excluded);
    _imp->solve=button("solve","Solve camera");_imp->create=button("createScene","Create USD scene");
    _imp->showTracks=boolean("showTracks","Show tracks and reprojections",true);
    _imp->showIds=boolean("showTrackIds","Show track IDs",false);
    _imp->status=AppManager::createKnob<KnobString>(this,tr("Analysis status"));_imp->status->setName("analysisStatus");_imp->status->setAsMultiLine();_imp->status->setAllDimensionsEnabled(false);_imp->status->setAnimationEnabled(false);_imp->status->setIsPersistent(false);_imp->status->setEvaluateOnChange(false);page->addKnob(_imp->status);
    auto storage=[&](const char* name){auto k=AppManager::createKnob<KnobString>(this,tr("Analysis data"));k->setName(name);k->setAnimationEnabled(false);k->setSecret(true);page->addKnob(k);return k;};
    _imp->trackData=storage("trackingData");_imp->solveData=storage("reconstructionData");
    _imp->busy=boolean("analysisRunning","Analysis running",false);_imp->busy->setSecret(true);_imp->busy->setIsPersistent(false);_imp->busy->setEvaluateOnChange(false);
    _imp->watcher=new QFutureWatcher<Reply>(this);_imp->timer=new QTimer(this);_imp->timer->setInterval(100);
    connect(_imp->timer,&QTimer::timeout,this,[this]{updateStatus();});_imp->timer->start();
    updateStatus();
}
std::string CameraTracker::configuration(bool solving)const
{
    QJsonObject object;
    object.insert(QStringLiteral("fps"),getApp()->getProjectFrameRate());
    const auto source=getInput(0),mask=getInput(1);
    std::map<const EffectInstance*,QString> cache;
    object.insert(QStringLiteral("source"),sourceSignature(source,cache));
    object.insert(QStringLiteral("mask"),sourceSignature(mask,cache));
    for(const auto& k:{_imp->first,_imp->last,_imp->features,_imp->spacing,_imp->length,_imp->width})object.insert(QString::fromStdString(k->getName()),k->getValue());
    object.insert(QStringLiteral("correlation"),_imp->correlation->getValue());
    if(solving){for(const auto& k:{_imp->focal,_imp->sensor,_imp->fx,_imp->fy,_imp->cx,_imp->cy,_imp->rms})object.insert(QString::fromStdString(k->getName()),k->getValue());
        if(_imp->calibrationMode->getValue()!=0)object.insert(QStringLiteral("calibrationMode"),_imp->calibrationMode->getValue());
        if(_imp->calibrationMode->getValue()!=0 && _imp->refinePrincipal->getValue())object.insert(QStringLiteral("refinePrincipalPoint"),true);
        if(_imp->k1->getValue()!=0 || _imp->k2->getValue()!=0){object.insert(QStringLiteral("k1"),_imp->k1->getValue());object.insert(QStringLiteral("k2"),_imp->k2->getValue());}
        object.insert(QStringLiteral("pixels"),_imp->pixels->getValue());object.insert(QStringLiteral("auto"),_imp->autoKeys->getValue());
        object.insert(QStringLiteral("key1"),_imp->key1->getValue());object.insert(QStringLiteral("key2"),_imp->key2->getValue());
        object.insert(QStringLiteral("exclude"),QString::fromStdString(_imp->excluded->getValue()));
        object.insert(QStringLiteral("tracks"),QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromStdString(_imp->trackData->getValue()),QCryptographicHash::Sha256).toHex()));}
    return QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
}
void CameraTracker::startAnalysis(bool solving)
{
    if(_imp->watcher->isRunning())return;
    const auto source=getInput(0),mask=getInput(1);
    if(!source){_imp->error="Connect a plate to Source";updateStatus();return;}
    if(solving && (!_imp->tracks || _imp->trackSignature!=configuration(false))){_imp->error="Track the current source and settings before solving";updateStatus();return;}
    const int first=_imp->first->getValue(),last=_imp->last->getValue(),maximumWidth=_imp->width->getValue();
    if(first>=last || int64_t(last)-first>2000 || maximumWidth<64 || maximumWidth>4096){_imp->error="Use a range of 2..2001 frames and analysis width 64..4096";updateStatus();return;}
    Scene::FeatureTrackingOptions tracking;tracking.maximumFeatures=_imp->features->getValue();tracking.minimumDistance=_imp->spacing->getValue();tracking.minimumTrackLength=_imp->length->getValue();tracking.minimumCorrelation=_imp->correlation->getValue();
    const auto signature=configuration(solving);const auto generation=++_imp->generation;
    const auto work=std::make_shared<Work>();_imp->work=work;_imp->error.clear();_imp->busy->setValue(true);
    const auto capturedTracks=_imp->tracks;const auto capturedDomain=_imp->domain;
    Scene::CameraCalibration calibration;calibration.width=capturedDomain.width;calibration.height=capturedDomain.height;
    calibration.fx=_imp->pixels->getValue()?_imp->fx->getValue():calibration.width*_imp->focal->getValue()/_imp->sensor->getValue();
    calibration.fy=_imp->pixels->getValue()?_imp->fy->getValue():calibration.fx*capturedDomain.par;
    calibration.cx=_imp->pixels->getValue()?_imp->cx->getValue():calibration.width*.5;calibration.cy=_imp->pixels->getValue()?_imp->cy->getValue():calibration.height*.5;
    calibration.k1=_imp->k1->getValue();calibration.k2=_imp->k2->getValue();
    Scene::TrackedSolveOptions solve;solve.automaticKeyframes=_imp->autoKeys->getValue();solve.solve.keyframes={{double(_imp->key1->getValue()),double(_imp->key2->getValue())}};solve.solve.maximumRmsPixels=_imp->rms->getValue();
    const int mode=_imp->calibrationMode->getValue();solve.solve.intrinsics=Scene::CameraSolveOptions::Intrinsics(mode);
    solve.solve.refinePrincipalPoint=mode!=0 && _imp->refinePrincipal->getValue();
    if(mode!=0){calibration.fx=calibration.width;calibration.fy=calibration.width*capturedDomain.par;calibration.cx=calibration.width*.5;calibration.cy=calibration.height*.5;calibration.k1=calibration.k2=0;}
    std::set<int64_t> excluded;std::istringstream text(_imp->excluded->getValue());int64_t id;while(text>>id)excluded.insert(id);
    const ViewIdx view(0);const auto app=getApp();const auto fps=app->getProjectFrameRate();const auto sourceNode=source->getNode();const auto sourceHash=source->getHash();const auto maskHash=mask?mask->getHash():0;
    disconnect(_imp->watcher,nullptr,this,nullptr);
    connect(_imp->watcher,&QFutureWatcher<Reply>::finished,this,[this,generation,signature,solving]{
        _imp->busy->setValue(false);auto reply=_imp->watcher->result();
        if(generation!=_imp->generation || signature!=configuration(solving)){_imp->error="Analysis discarded because the source or settings changed";updateStatus();return;}
        if(!reply.error.empty()){_imp->error=reply.error;updateStatus();return;}
        QJsonObject object{{QStringLiteral("version"),1},{QStringLiteral("signature"),QString::fromStdString(signature)}};
        if(solving){QJsonArray cameras,points;
            object.insert(QStringLiteral("version"),2);
            for(const auto& c:reply.solution.cameras)cameras.append(QJsonObject{{QStringLiteral("frame"),c.frame},{QStringLiteral("matrix"),array(c.transform)}});
            for(const auto& p:reply.solution.points)points.append(QJsonArray{QString::number(p.trackId),p.position[0],p.position[1],p.position[2],double(p.observations),p.rmsPixels,p.parallaxDegrees});
            object.insert(QStringLiteral("cameras"),cameras);object.insert(QStringLiteral("points"),points);object.insert(QStringLiteral("rms"),reply.solution.rmsPixels);
            const auto& c=reply.solution.calibration;
            object.insert(QStringLiteral("usd"),QString::fromStdString(reply.usd));object.insert(QStringLiteral("calibration"),QJsonArray{c.fx,c.fy,c.cx,c.cy,c.k1,c.k2});
            object.insert(QStringLiteral("imageSize"),QJsonArray{c.width,c.height});object.insert(QStringLiteral("estimatedIntrinsics"),reply.solution.estimatedIntrinsics);object.insert(QStringLiteral("calibrationCoverage"),reply.solution.calibrationCoverage);
            object.insert(QStringLiteral("calibrationAmbiguous"),reply.solution.calibrationAmbiguous);object.insert(QStringLiteral("focalRange"),QJsonArray{reply.solution.focalMinimum,reply.solution.focalMaximum});
            object.insert(QStringLiteral("estimatedPrincipalPoint"),reply.solution.estimatedPrincipalPoint);object.insert(QStringLiteral("principalPointValidationGain"),reply.solution.principalPointValidationGain);
        }else{QJsonArray observations,frames;for(double f:reply.tracks.frames)frames.append(f);
            for(const auto& o:reply.tracks.observations)observations.append(QJsonArray{o.frame,QString::number(o.trackId),o.x,o.y,o.weight});
            object.insert(QStringLiteral("observations"),observations);object.insert(QStringLiteral("frames"),frames);object.insert(QStringLiteral("domain"),QJsonArray{reply.domain.width,reply.domain.height,reply.domain.x,reply.domain.y,reply.domain.par});}
        const auto knob=solving?_imp->solveData:_imp->trackData;
        getNode()->pushUndoCommand(std::make_shared<AnalysisUndo>(knob,knob->getValue(),pack(object),solving?"Solve camera":"Track camera features"));
        restoreAnalysis();updateStatus();redrawOverlayInteract();
    });
    _imp->watcher->setFuture(QtConcurrent::run([=]()mutable{
        (void)sourceNode;(void)app; // retain both while the image provider is running
        Reply reply;reply.solving=solving;reply.domain=capturedDomain;
        try{
            Scene::CameraSolveOptions control;control.cancelled=[work]{return work->cancel.load();};
            control.progress=[work](double value,const std::string& message){work->progress=value;std::lock_guard<std::mutex> lock(work->mutex);work->message=message;};
            if(solving){auto tracks=*capturedTracks;for(auto& o:tracks.observations)if(excluded.count(o.trackId))o.enabled=false;
                solve.solve.cancelled=control.cancelled;solve.solve.progress=control.progress;reply.solution=Scene::solveTrackedCamera(calibration,tracks,solve);
#ifdef NATRON_HAS_USD
                Scene::ReconstructionUsdOptions usdOptions;usdOptions.framesPerSecond=fps;usdOptions.imagePixelAspect=capturedDomain.par;reply.usd=Scene::reconstructionUsdLayer(reply.solution,usdOptions);
#else
                throw std::runtime_error("Rebuild with OpenUSD to publish a reconstructed scene");
#endif
            }else{
                std::vector<double> frames;for(int64_t f=first;f<=last;++f)frames.push_back(double(f));
                auto provider=[&](double time){if(source->getHash()!=sourceHash || (mask && mask->getHash()!=maskHash))throw std::runtime_error("The plate changed during tracking");
                    Domain domain;auto image=fetchImage(source,mask,time,view,maximumWidth,domain);
                    if(reply.domain.width && (domain.width!=reply.domain.width || domain.height!=reply.domain.height || domain.x!=reply.domain.x || domain.y!=reply.domain.y || domain.par!=reply.domain.par))throw std::runtime_error("Plate format changed during tracking");reply.domain=domain;return image;};
                reply.tracks=Scene::trackFeatures(frames,provider,tracking,control);
                const double sx=double(reply.domain.width)/reply.tracks.width,sy=double(reply.domain.height)/reply.tracks.height;
                for(auto& o:reply.tracks.observations){o.x*=sx;o.y*=sy;}reply.tracks.width=reply.domain.width;reply.tracks.height=reply.domain.height;
                if(reply.tracks.trackCount<12)throw std::runtime_error("Too few stable features; adjust the mask, separation or range");
            }
        }catch(const std::exception& e){reply.error=e.what();}
        return reply;
    }));
    updateStatus();
}
void CameraTracker::restoreAnalysis()
{
    try{
        const auto data=unpack(_imp->trackData->getValue());std::shared_ptr<Scene::FeatureTracks> tracks;
        Domain domain;
        if(!data.isEmpty()){
            const auto d=data.value(QStringLiteral("domain")).toArray();if(d.size()!=5)throw std::runtime_error("Invalid stored tracking domain");domain={d[0].toInt(),d[1].toInt(),d[2].toInt(),d[3].toInt(),d[4].toDouble()};
            tracks=std::make_shared<Scene::FeatureTracks>();tracks->width=domain.width;tracks->height=domain.height;std::set<int64_t> ids;
            for(const auto& f:data.value(QStringLiteral("frames")).toArray())tracks->frames.push_back(f.toDouble());
            for(const auto& entry:data.value(QStringLiteral("observations")).toArray()){const auto a=entry.toArray();if(a.size()!=5)throw std::runtime_error("Invalid stored observation");const auto id=a[1].toString().toLongLong();tracks->observations.push_back({a[0].toDouble(),id,a[2].toDouble(),a[3].toDouble(),a[4].toDouble()});ids.insert(id);}tracks->trackCount=ids.size();
        }
        const auto solved=unpack(_imp->solveData->getValue());std::shared_ptr<Scene::CameraReconstruction> result;std::string usd;
        if(!solved.isEmpty()){
            result=std::make_shared<Scene::CameraReconstruction>();const auto c=solved.value(QStringLiteral("calibration")).toArray();if(c.size()!=4 && c.size()!=6)throw std::runtime_error("Invalid stored calibration");
            const auto size=solved.value(QStringLiteral("imageSize")).toArray();
            result->calibration={size.size()==2?size[0].toInt():domain.width,size.size()==2?size[1].toInt():domain.height,c[0].toDouble(),c[1].toDouble(),c[2].toDouble(),c[3].toDouble(),c.size()==6?c[4].toDouble():0,c.size()==6?c[5].toDouble():0};
            if(!Scene::validLens(result->calibration))throw std::runtime_error("Invalid stored lens calibration");
            result->estimatedIntrinsics=solved.value(QStringLiteral("estimatedIntrinsics")).toBool();result->calibrationCoverage=solved.value(QStringLiteral("calibrationCoverage")).toDouble();
            result->calibrationAmbiguous=solved.value(QStringLiteral("calibrationAmbiguous")).toBool();const auto range=solved.value(QStringLiteral("focalRange")).toArray();if(range.size()==2){result->focalMinimum=range[0].toDouble();result->focalMaximum=range[1].toDouble();}
            result->estimatedPrincipalPoint=solved.value(QStringLiteral("estimatedPrincipalPoint")).toBool();result->principalPointValidationGain=solved.value(QStringLiteral("principalPointValidationGain")).toDouble();
            for(const auto& entry:solved.value(QStringLiteral("cameras")).toArray()){const auto c=entry.toObject();const auto m=c.value(QStringLiteral("matrix")).toArray();if(m.size()!=16)throw std::runtime_error("Invalid stored camera");Scene::SolvedCamera camera;camera.frame=c.value(QStringLiteral("frame")).toDouble();for(int i=0;i<16;++i)camera.transform[i]=m[i].toDouble();result->cameras.push_back(camera);}
            for(const auto& entry:solved.value(QStringLiteral("points")).toArray()){const auto p=entry.toArray();if(p.size()!=7)throw std::runtime_error("Invalid stored landmark");result->points.push_back({p[0].toString().toLongLong(),{p[1].toDouble(),p[2].toDouble(),p[3].toDouble()},size_t(p[4].toDouble()),p[5].toDouble(),p[6].toDouble()});}
            result->rmsPixels=solved.value(QStringLiteral("rms")).toDouble();usd=solved.value(QStringLiteral("usd")).toString().toStdString();
        }
        std::lock_guard<std::mutex> lock(_imp->mutex);_imp->tracks=std::move(tracks);_imp->result=std::move(result);_imp->domain=domain;_imp->usd=std::move(usd);
        _imp->trackSignature=data.value(QStringLiteral("signature")).toString().toStdString();_imp->solveSignature=solved.value(QStringLiteral("signature")).toString().toStdString();
    }catch(const std::exception& e){std::lock_guard<std::mutex> lock(_imp->mutex);_imp->tracks.reset();_imp->result.reset();_imp->usd.clear();_imp->error=e.what();}
}
void CameraTracker::onKnobsLoaded(){restoreAnalysis();updateStatus();}
void CameraTracker::onInputChanged(int){++_imp->generation;if(_imp->work)_imp->work->cancel=true;updateStatus();}
bool CameraTracker::knobChanged(KnobI* knob,ValueChangedReasonEnum,ViewSpec,double,bool)
{
    if(knob==_imp->track.get()){startAnalysis(false);return true;}
    if(knob==_imp->solve.get()){startAnalysis(true);return true;}
    if(knob==_imp->cancel.get()){if(_imp->work)_imp->work->cancel=true;return true;}
    if(knob==_imp->create.get()){createScene();return true;}
    if(knob==_imp->trackData.get() || knob==_imp->solveData.get()){restoreAnalysis();updateStatus();return true;}
    if(knob==_imp->status.get() || knob==_imp->busy.get())return false;
    if(knob==_imp->showTracks.get() || knob==_imp->showIds.get()){redrawOverlayInteract();return false;}
    if(_imp->work && _imp->watcher->isRunning()){++_imp->generation;_imp->work->cancel=true;}
    updateStatus();return false;
}
bool CameraTracker::analysisRunning()const{return _imp->watcher && _imp->watcher->isRunning();}
std::shared_ptr<const Scene::CameraReconstruction> CameraTracker::reconstruction()const{std::lock_guard<std::mutex> lock(_imp->mutex);return _imp->result;}
std::string CameraTracker::usdAssetPath()const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);
    if(_imp->usd.empty())throw std::runtime_error("CameraTracker has no accepted camera solve");
    if(!_imp->files.isValid())throw std::runtime_error("Cannot create reconstruction cache");
    const auto bytes=QByteArray::fromStdString(_imp->usd);const auto hash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
    const auto path=_imp->files.filePath(QString::fromLatin1(hash)+QStringLiteral(".usda"));
    if(!QFileInfo::exists(path)){QSaveFile file(path);if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())throw std::runtime_error("Cannot publish reconstructed USD asset");}
    return path.toStdString();
}
void CameraTracker::updateStatus()
{
    if(!_imp->status || !_imp->watcher)return;
    const bool running=_imp->watcher->isRunning();_imp->track->setEnabled(0,!running);_imp->solve->setEnabled(0,!running && bool(_imp->tracks));_imp->create->setEnabled(0,!running && bool(_imp->result));_imp->cancel->setEnabled(0,running);
    const bool known=_imp->calibrationMode->getValue()==0,pixels=_imp->pixels->getValue();for(const auto& k:{_imp->fx,_imp->fy,_imp->cx,_imp->cy})k->setEnabled(0,known && pixels);
    _imp->pixels->setEnabled(0,known);_imp->focal->setEnabled(0,known && !pixels);_imp->sensor->setEnabled(0,known && !pixels);_imp->k1->setEnabled(0,known);_imp->k2->setEnabled(0,known);
    _imp->refinePrincipal->setEnabled(0,!known);
    _imp->key1->setEnabled(0,!_imp->autoKeys->getValue());_imp->key2->setEnabled(0,!_imp->autoKeys->getValue());
    std::ostringstream text;
    if(running && _imp->work){std::lock_guard<std::mutex> lock(_imp->work->mutex);text<<int(_imp->work->progress*100)<<"%  "<<_imp->work->message;}
    else if(!_imp->error.empty())text<<_imp->error;
    else if(_imp->result){text<<"Solved "<<_imp->result->cameras.size()<<" frames, "<<_imp->result->points.size()<<" sparse points; RMS "<<_imp->result->rmsPixels<<" px. Scale is arbitrary.";if(_imp->solveSignature!=configuration(true))text<<"\nSolve is stale: source, tracking or calibration changed.";}
    else if(_imp->tracks){text<<_imp->tracks->trackCount<<" tracks, "<<_imp->tracks->observations.size()<<" observations.";if(_imp->trackSignature!=configuration(false))text<<"\nTracks are stale: track the current source/settings again.";}
    else text<<"Connect a plate, set the range and calibration mode, then Track features and Solve camera.";
    if(!running && _imp->result){const auto& c=_imp->result->calibration;text<<"\n"<<(_imp->result->estimatedIntrinsics?"Estimated":"Known")<<" focal: "<<c.fx<<" px; radial k1="<<c.k1<<", k2="<<c.k2<<". Principal: ("<<c.cx<<", "<<c.cy<<").";}
    if(!running && _imp->result && _imp->result->estimatedPrincipalPoint)text<<"\nPrincipal point refinement validated (gain "<<_imp->result->principalPointValidationGain<<" px).";
    else if(!running && _imp->result && _imp->result->estimatedIntrinsics && _imp->refinePrincipal->getValue() && _imp->solveSignature==configuration(true))text<<"\nPrincipal point kept fixed: validation did not justify additional parameters.";
    if(!running && _imp->result && _imp->result->calibrationAmbiguous){text<<"\nLens is weakly constrained by the motion or competing models. Add rotation/depth coverage or use known calibration.";
        if(_imp->result->focalMaximum>_imp->result->focalMinimum*1.02)text<<" Focal candidates: "<<_imp->result->focalMinimum<<".."<<_imp->result->focalMaximum<<" px.";}
    const auto value=text.str();if(_imp->status->getValue()!=value)_imp->status->setValue(value);
}
void CameraTracker::drawOverlay(double time,const RenderScale&,ViewIdx)
{
    if(!_imp->showTracks->getValue() || !_imp->tracks)return;
    auto* viewport=getCurrentViewportForOverlays();if(!viewport || !viewport->overlayDrawList())return;
    auto& draw=*viewport->overlayDrawList();OverlayDrawScope scope(draw);draw.color(.2,1,.35);draw.pointSize(4);draw.begin(OverlayPrimitive::Points);
    for(const auto& o:_imp->tracks->observations)if(std::abs(o.frame-time)<1e-6)draw.vertex((_imp->domain.x+o.x)*_imp->domain.par,_imp->domain.y+_imp->domain.height-o.y);
    draw.end();
    if(_imp->showIds->getValue())for(const auto& o:_imp->tracks->observations)if(std::abs(o.frame-time)<1e-6)
        draw.text((_imp->domain.x+o.x)*_imp->domain.par,_imp->domain.y+_imp->domain.height-o.y,std::to_string(o.trackId),0);
    const auto result=_imp->result;if(!result)return;
    const auto camera=std::find_if(result->cameras.begin(),result->cameras.end(),[&](const auto& c){return std::abs(c.frame-time)<1e-6;});if(camera==result->cameras.end())return;
    std::map<int64_t,const Scene::SolvedPoint*> points;for(const auto& p:result->points)points[p.trackId]=&p;
    draw.color(1,.65,.15);draw.pointSize(3);draw.begin(OverlayPrimitive::Points);
    for(const auto& o:_imp->tracks->observations)if(std::abs(o.frame-time)<1e-6 && points.count(o.trackId)){
        const auto& p=points.at(o.trackId)->position;double local[3]{};
        for(int axis=0;axis<3;++axis)for(int world=0;world<3;++world)local[axis]+=camera->transform[axis*4+world]*(p[world]-camera->transform[12+world]);
        if(local[2]>=0)continue;const auto& c=result->calibration;
        const auto pixel=Scene::distortPixel(c,c.fx*local[0]/-local[2]+c.cx,c.fy*-local[1]/-local[2]+c.cy);
        draw.vertex((_imp->domain.x+pixel[0])*_imp->domain.par,_imp->domain.y+_imp->domain.height-pixel[1]);
    }draw.end();
}
void CameraTracker::createScene()
{
    if(!_imp->result || _imp->solveSignature!=configuration(true)){_imp->error="Solve the current plate and settings before creating a scene";updateStatus();return;}
    try{
        double x=0,y=0;getNode()->getPosition(&x,&y);
        auto create=[&](const char* plugin,double px,double py){CreateNodeArgs args(plugin,getNode()->getGroup());args.setProperty<bool>(kCreateNodeArgsPropAutoConnect,false);auto node=getApp()->createNode(args);if(!node)throw std::runtime_error("Cannot create camera reconstruction nodes");node->setPosition(px,py);return node;};
        const auto scene=create("fr.inria.built-in.ReconstructionScene",x+180,y+100);scene->connectInput(getNode(),0);
        const auto setup=create("fr.inria.built-in.SceneSetup",x+180,y+200);setup->connectInput(scene,0);
        const auto camera=create("fr.inria.built-in.SceneCamera",x+400,y+200);camera->connectInput(setup,0);
        auto native=dynamic_cast<NativeScene3D*>(scene->getEffectInstance().get());
        const auto path="/Natron/Assets/n_"+native->sceneId()+"/Source/Camera";
        std::dynamic_pointer_cast<KnobString>(camera->getEffectInstance()->getKnobByName("cameraPath"))->setValue(path);
        auto plate=getInput(0)->getNode();const auto& lens=_imp->result->calibration;
        if(lens.k1!=0 || lens.k2!=0 || _imp->result->estimatedIntrinsics){const auto undistort=create("fr.inria.built-in.CameraTrackerUndistort",x-180,y+180);undistort->connectInput(plate,0);undistort->connectInput(getNode(),1);plate=undistort;}
        const auto render=create("fr.inria.built-in.Render3D",x+180,y+340);render->connectInput(setup,0);render->connectInput(camera,1);render->connectInput(plate,2);
        std::dynamic_pointer_cast<KnobString>(setup->getEffectInstance()->getKnobByName("layoutRenderer"))->setValue(dynamic_cast<NativeScene3D*>(render->getEffectInstance().get())->sceneId());
        if(!appPTR->isBackground()){const auto viewer=create(PLUGINID_NATRON_VIEWER,x+180,y+470);viewer->connectInput(render,0);}
        _imp->error.clear();updateStatus();
    }catch(const std::exception& e){_imp->error=e.what();updateStatus();}
}

void CameraTrackerUndistort::addAcceptedComponents(int,std::list<ImagePlaneDesc>* c){c->push_back(ImagePlaneDesc::getRGBAComponents());}
void CameraTrackerUndistort::addSupportedBitDepth(std::list<ImageBitDepthEnum>* d)const{d->push_back(eImageBitDepthFloat);}
StatusEnum CameraTrackerUndistort::getPreferredMetadata(NodeMetadata& m){
    const auto source=getInput(0);if(!source)return eStatusFailed;
    m.setOutputFormat(source->getOutputFormat());m.setPixelAspectRatio(-1,source->getAspectRatio(-1));
    m.setNComps(-1,4);m.setBitDepth(-1,eImageBitDepthFloat);m.setOutputPremult(source->getPremult());m.setIsFrameVarying(true);return eStatusOK;
}
StatusEnum CameraTrackerUndistort::getRegionOfDefinition(U64,double,const RenderScale&,ViewIdx,RectD* rod){
    const auto source=getInput(0);if(!source)return eStatusFailed;const auto f=source->getOutputFormat();const double par=source->getAspectRatio(-1);
    *rod=RectD(f.x1*par,f.y1,f.x2*par,f.y2);return eStatusOK;
}
StatusEnum CameraTrackerUndistort::render(const RenderActionArgs& args){
    try{
        auto controller=getInput(1);
        for(int i=0;controller && controller->getPluginID()==PLUGINID_NATRON_DOT && i<1024;++i)controller=controller->getInput(0);
        const auto tracker=std::dynamic_pointer_cast<CameraTracker>(controller);const auto source=getInput(0);
        const auto solved=tracker?tracker->reconstruction():nullptr;
        if(!source || !solved)throw std::runtime_error("UndistortPlate requires a Source and an accepted CameraTracker solve");
        const auto& c=solved->calibration;const auto format=source->getOutputFormat();
        if(format.width()!=c.width || format.height()!=c.height)throw std::runtime_error("Plate dimensions differ from the accepted lens calibration");
        const auto image=fetchPlate(source,args.time,args.view);const Image::ReadAccess read(image.get());const unsigned components=image->getComponentsCount();
        const double scale=std::ldexp(1.,args.mappedScale.toMipmapLevel());
        for(const auto& plane:args.outputPlanes){Image::WriteAccess write(plane.second.get());const unsigned channels=plane.second->getComponentsCount();
            const auto roi=args.roi.intersect(write.bounds());
            for(int y=roi.y1;y<roi.y2;++y){if(aborted())return eStatusOK;for(int x=roi.x1;x<roi.x2;++x){
                const auto pixel=Scene::distortPixel(c,(x+.5)*scale-format.x1,format.y1+format.height()-(y+.5)*scale);
                const double sx=format.x1+pixel[0]-.5,sy=format.y1+format.height()-pixel[1]-.5;
                const int ix=int(std::floor(sx)),iy=int(std::floor(sy));const double dx=sx-ix,dy=sy-iy;
                float color[4]{};
                for(int row=0;row<2;++row)for(int col=0;col<2;++col){const auto p=reinterpret_cast<const float*>(read.pixelAt(ix+col,iy+row));if(!p)continue;
                    const double w=(col?dx:1-dx)*(row?dy:1-dy);
                    for(unsigned ch=0;ch<4;++ch)color[ch]+=float(w*(ch<components?p[ch]:(ch==3?1:0)));
                }
                auto* out=reinterpret_cast<float*>(write.pixelAt(x,y));if(channels==1)out[0]=color[3];else std::copy_n(color,std::min(channels,4u),out);
            }}
        }
        return eStatusOK;
    }catch(const std::exception& e){if(aborted())return eStatusOK;setPersistentMessage(eMessageTypeError,e.what());return eStatusFailed;}
}
NATRON_NAMESPACE_EXIT
