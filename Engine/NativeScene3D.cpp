// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/NativeScene3D.h"
#include "Gpu/Compositing.h"
#include "Engine/CameraTracker.h"
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <QDateTime>
#include <QFileInfo>
#include <QCoreApplication>
#include <QThread>
#include "Gpu/FbxMesh.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/VulkanImageContextPool.h"
#include "Engine/VulkanImageStorage.h"

NATRON_NAMESPACE_ENTER
using Gpu::Cards::Vec3;
using Gpu::Cards::Matrix;
namespace {
template<class K> auto sceneValueAt(const std::shared_ptr<K>& knob,double time,int dimension,ViewIdx view)
    ->decltype(knob->getValueAtTime(time,dimension,view)) {
    // Instance-safe 2D renders can defer engine values while the editor already
    // has newer GUI values. Snapshot the editor's static values for 3D preview;
    // keep time sampling and render-thread engine values for final rendering.
    if(QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread() && !knob->getMaster(dimension).second && !knob->isAnimated(dimension,view) && knob->getExpression(dimension).empty())
        return knob->getValue(dimension,view);
    return knob->getValueAtTime(time,dimension,view);
}
const char* names[]={"Card","Camera","Axis","TransformGeo","Scene","Project3D","Render3D","ReadGeo","ReadCamera","Light","PBRMaterial","ReadUSD","ReadUSDCamera","USDTransform","USDVisibility","BindMaterial","USDCompositing","ReconstructionScene","SceneCamera","SceneSetup","ShadowCatcher","Occluder","ReflectionCatcher"};
bool usdEditKind(Scene3DKind k){return k==Scene3DKind::USDTransform || k==Scene3DKind::USDVisibility || k==Scene3DKind::BindMaterial || k==Scene3DKind::USDCompositing;}
std::vector<std::string> targetPaths(const KnobStringWPtr& knob,ViewIdx view) {
    std::vector<std::string> paths;std::istringstream text(knob.lock()->getValue(0,view));std::string path;
    while(text>>path)if(std::find(paths.begin(),paths.end(),path)==paths.end())paths.push_back(path);
    if(paths.empty())throw std::runtime_error("USD edit requires at least one absolute Prim path.");return paths;
}
bool geometryLeaf(Scene3DKind kind){return kind==Scene3DKind::Card || kind==Scene3DKind::ReadGeo || kind==Scene3DKind::ReadUSD;}
bool cameraKind(Scene3DKind kind){return kind==Scene3DKind::Camera || kind==Scene3DKind::ReadCamera || kind==Scene3DKind::ReadUSDCamera || kind==Scene3DKind::SceneCamera;}
EffectInstancePtr resolve(EffectInstancePtr effect) {
    // getInput() already redirects Group Input/Output connections.
    for(int i=0;effect && i<1024;++i) {
        const auto id=effect->getPluginID();
        if(id!=PLUGINID_NATRON_DOT && id!=PLUGINID_NATRON_INPUT && id!=PLUGINID_NATRON_OUTPUT) break;
        effect=effect->getInput(0);
    }
    return effect;
}
NativeScene3D* input3D(const NativeScene3D* node,int index) {
    return dynamic_cast<NativeScene3D*>(resolve(node->getInput(index)).get());
}
Vec3 value3(const KnobDoubleWPtr& weak,double time,ViewIdx view,Vec3 fallback={}) {
    const auto knob=weak.lock();
    return knob?Vec3{sceneValueAt(knob,time,0,view),sceneValueAt(knob,time,1,view),sceneValueAt(knob,time,2,view)}:fallback;
}
Vec3 color3(const KnobColorWPtr& weak,double time,ViewIdx view) {
    const auto knob=weak.lock();return {sceneValueAt(knob,time,0,view),sceneValueAt(knob,time,1,view),sceneValueAt(knob,time,2,view)};
}
std::shared_ptr<Gpu::Cards::Texture> fetchTexture(const EffectInstancePtr& source,double time,ViewIdx view,bool draft,const Scene3DMapSource* map=nullptr) {
    if(!source)return {};
    ParallelRenderArgsSetter tls(time,view,false,false,AbortableRenderInfoPtr(),source->getNode(),0,
        source->getApp()->getTimeLine().get(),NodePtr(),false,draft,RenderStatsPtr());
    RectD rod;
    if(source->getRegionOfDefinition_public(source->getHash(),time,RenderScale::identity,view,&rod,nullptr)!=eStatusOK || rod.isNull())
        throw std::runtime_error("Cannot evaluate the 3D texture's region of definition.");
    if(rod.isInfinite()) throw std::runtime_error("3D textures require finite image bounds; insert a Crop or Reformat.");
    const RectI bounds=rod.toPixelEnclosing(0,source->getAspectRatio(-1));
    ImagePlaneDesc rgba=ImagePlaneDesc::getRGBAComponents();
    if(map) {
        ImagePlaneDesc preferred,paired;source->getMetadataComponents(-1,&preferred,&paired);
        if(preferred.getNumComponents()==1)rgba=preferred;
    }
    EffectInstance::RenderRoIArgs args(time,RenderScale::identity,0,view,false,bounds,rod,{rgba},eImageBitDepthFloat,false,
        nullptr,eStorageModeRAM,time);
    std::map<ImagePlaneDesc,ImagePtr> planes;
    if(source->renderRoI(args,&planes)!=EffectInstance::eRenderRoIRetCodeOk || planes.empty())
        throw std::runtime_error("Cannot render an image used by the 3D scene.");
    const auto image=planes.begin()->second;
    if(!image || image->getStorageMode()!=eStorageModeRAM || image->getBitDepth()!=eImageBitDepthFloat)
        throw std::runtime_error("3D texture is not a CPU float image.");
    // Data maps sample the complete source format so a cropped data window
    // does not stretch or repeat its remaining pixels over the entire mesh.
    const RectI format=source->getOutputFormat();
    const RectI textureBounds=map && format.width()>0 && format.height()>0?format:bounds;
    auto result=std::make_shared<Gpu::Cards::Texture>();result->width=textureBounds.width();result->height=textureBounds.height();
    if(map){result->materialSampling=result->dataSampling=true;result->wrapS=result->wrapT=map->wrap;}
    if(!map && format.width()>0 && format.height()>0) {
        result->imageUvScale={double(format.width())/bounds.width(),double(format.height())/bounds.height()};
        result->imageUvOffset={double(format.x1-bounds.x1)/bounds.width(),double(format.y1-bounds.y1)/bounds.height()};
    }
    result->pixels.resize(size_t(result->width)*result->height*4);
    const Image::ReadAccess pixels(image.get());const unsigned channels=image->getComponentsCount();
    const auto premult=source->getPremult();
    const RectI copyBounds=bounds.intersect(textureBounds);
    for(int y=copyBounds.y1;y<copyBounds.y2;++y)for(int x=copyBounds.x1;x<copyBounds.x2;++x) {
        const float* p=reinterpret_cast<const float*>(pixels.pixelAt(x,y));
        float* to=result->pixels.data()+((size_t(y-textureBounds.y1)*result->width)+(x-textureBounds.x1))*4;
        if(!p)continue;
        if(map) {
            if(channels==1)std::fill_n(to,4,p[0]);
            else {
                const float alpha=channels==4?p[3]:1.f;
                // Data channels are graph values, not associated color.
                // Readers should supply raw/linear, unpremultiplied pixels.
                for(unsigned c=0;c<3;++c)to[c]=p[c];
                to[3]=alpha;
            }
            if(map->normalEncoding>=0) {
                if(map->normalEncoding==0)for(int c=0;c<3;++c)to[c]=2*to[c]-1;
                if(map->flipY)to[1]=-to[1];
            }
            continue;
        }
        if(channels==1){to[3]=p[0];continue;}
        const float alpha=channels==4 && premult!=eImagePremultiplicationOpaque?p[3]:1.f;
        for(unsigned c=0;c<3;++c)to[c]=p[c]*(premult==eImagePremultiplicationUnPremultiplied?alpha:1.f);
        to[3]=alpha;
    }
    return result;
}
}
NativeScene3D::NativeScene3D(NodePtr n,Scene3DKind k):EffectInstance(n),_kind(k) {setSupportsRenderScaleMaybe(eSupportsYes);}
std::string NativeScene3D::getPluginLabel() const {return names[int(_kind)];}
std::string NativeScene3D::getPluginID() const {return std::string("fr.inria.built-in.")+names[int(_kind)];}
std::string NativeScene3D::getPluginDescription() const {
    switch(_kind) {
    case Scene3DKind::ShadowCatcher:return "Receive CG shadows on proxy geometry while keeping the real plate visible. Connect a Card, mesh or USD asset. Shadow density and linear tint affect the independent Shadow RGBA output. Add scene lights and connect the plate to Render3D background. The surface also occludes CG behind it.";
    case Scene3DKind::ReflectionCatcher:return "Receive added CG reflections on a plate proxy. Reflection strength scales the signed radiance difference; roughness spreads the reflected image. Connect the real plate to Render3D background. Enable Catch shadows to receive both on the same surface. USD / hdNVP uses path tracing; native renderers use a CPU single-reflection approximation.";
    case Scene3DKind::Occluder:return "Use proxy geometry to hide CG behind a real plate object. Connect a Card, mesh or USD asset, then adjust the upstream geometry in the 3D Viewer. The plate stays visible through the proxy. Camera, shadow and reflection visibility are independent; Occlusion is an independent matte output.";
    case Scene3DKind::ReconstructionScene:return "Publish the accepted CameraTracker solve as a linked USD camera and guide point cloud. Connect CameraTracker's image output; analysis data is saved in that node.";
    case Scene3DKind::SceneCamera:return "Read a camera from the connected composed scene for Render3D or Project3D. The source camera remains in its USD hierarchy.";
    case Scene3DKind::SceneSetup:return "Align a reconstruction, set ground/origin/metric scale, and place USD assets or projected cards. Camera, landmarks and placed geometry share the scene frame. Select landmarks in the 3D Viewer's Points mode.";
    case Scene3DKind::ReadUSD:return "Read a composed OpenUSD stage into the shared Scene and Render3D. Supports Mesh hierarchy/instances, time samples, primvars, PreviewSurface values and base-color/normal/roughness/metallic maps, distant, approximated sphere and latitude-longitude dome lights. References, sublayers, variants and payload loading use OpenUSD. Unsupported shaders/textures, subdivision, area lights and deformation are reported in Source info. USD cameras use ReadUSDCamera.";
    case Scene3DKind::Light:return "Distant, point, spot or environment light in the shared 3D Scene. Distant/spot lights shine down local -Z. Intensity is multiplied by 2^Exposure; point/spot lights use inverse-square attenuation. Environment lights integrate diffuse and GGX reflections from an optional latitude-longitude image (linear straight RGB; alpha ignored), or uniform Color. Rotation and an Axis orient the environment. Shadows respect geometry and texture opacity. Connect to Scene; input 0 optionally parents the light with an Axis.";
    case Scene3DKind::PBRMaterial:return "GGX metallic/roughness surface for Card, ReadGeo or ReadUSD. The optional image or Project3D input supplies base color; Base color multiplies that image or the mesh's imported colors. Without Scene lights only emission is visible. Existing image/Project3D connections remain unlit. Direct and environment lighting are supported. Optional normal, roughness and metallic image inputs use mesh UVs. Connected scalar maps replace the numeric values; choose their RGBA channels. Set map Readers to raw/linear data and unpremultiplied output. Normal maps default to 0..1 RGB with optional Y inversion.";
    case Scene3DKind::Card:return "Place an image or Project3D material on a 3D card. Right-handed coordinates; rotation is XYZ in degrees.";
    case Scene3DKind::ReadGeo:return "Load ASCII/binary FBX meshes with ufbx, normalized to right-handed Y-up meters. Enable Read animation to evaluate FBX transforms, skinning and blend shapes at the timeline's time. An optional image uses mesh UVs; Project3D projects onto the mesh. Vulkan/CPU rendering supports color and Depth.Z. Imported lights and external texture files are not evaluated.";
    case Scene3DKind::Camera:return "Perspective or orthographic camera looking down local -Z. Focal length and horizontal aperture are in millimeters.";
    case Scene3DKind::ReadUSDCamera:return "Read a camera from an OpenUSD stage. Same composed hierarchy, session variants and timing as ReadUSD. Units are normalized to meters; horizontal framing is preserved. Connect to Render3D or Project3D. Film offsets are not evaluated.";
    case Scene3DKind::ReadCamera:return "Read an FBX camera with ufbx, including parent transforms, lens animation, clipping and orthographic projection. Connect to Render3D or Project3D. Read animation follows the project frame rate. Transform offsets and an optional Axis parent remain editable in the 3D Viewer. Horizontal framing is preserved; the output or projected image defines the vertical aspect.";
    case Scene3DKind::Axis:return "Parent transform for cards, cameras and other axes. Local transform order: scale, XYZ rotation, translation.";
    case Scene3DKind::TransformGeo:return "Transform all geometry in the incoming geometry or scene, optionally relative to an Axis.";
    case Scene3DKind::Scene:return "Combine cards, meshes, lights and nested scenes in a shared 3D space. Transparent intersections are sorted per pixel.";
    case Scene3DKind::Project3D:return "Project an image using a separate camera. Freeze projection and image at a reference frame to create parallax.";
    case Scene3DKind::USDTransform:return "Author a world-space transform delta on selected composed USD Prims. Translation uses Y-up meters. Source files are read-only.";
    case Scene3DKind::USDVisibility:return "Author visibility on selected composed USD Prims in this branch's private edit layer.";
    case Scene3DKind::BindMaterial:return "Bind an image, Project3D or PBRMaterial to composed USD Prim paths. The binding overrides descendant source bindings in this branch only.";
    case Scene3DKind::USDCompositing:return "Author camera, shadow and reflection visibility and holdout semantics on composed USD Prims.";
    default:return "Render geometry through a Camera into linear premultiplied RGBA and a float Depth.Z layer at the project format. Depth is the nearest visible surface's camera-space -Z, with zero for empty pixels. Auto uses Vulkan for up to 16 Card/ReadGeo objects within the device's mesh-buffer limits, and falls back to CPU for larger scenes or unavailable hardware. Connect to a Viewer and select 3D to edit the scene.";
    }
}
void NativeScene3D::getPluginGrouping(std::list<std::string>* groups) const {groups->push_back("3D");}
int NativeScene3D::getNInputs() const {
    switch(_kind) {
    case Scene3DKind::ReconstructionScene:case Scene3DKind::SceneCamera:case Scene3DKind::SceneSetup:case Scene3DKind::ShadowCatcher:case Scene3DKind::ReflectionCatcher:case Scene3DKind::Occluder:return 1;
    case Scene3DKind::Axis:case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:return 1;
    case Scene3DKind::Light:return 2;
    case Scene3DKind::PBRMaterial:return 4;
    case Scene3DKind::Scene:return 16;
    case Scene3DKind::Render3D:return 3;
    case Scene3DKind::USDTransform:case Scene3DKind::USDVisibility:case Scene3DKind::USDCompositing:return 1;
    default:return 2;
    }
}
bool NativeScene3D::isInputOptional(int input) const {return _kind!=Scene3DKind::Render3D || input==2;}
std::string NativeScene3D::getInputLabel(int input) const {
    switch(_kind) {
    case Scene3DKind::ReconstructionScene:return "CameraTracker";
    case Scene3DKind::SceneCamera:return "scene";
    case Scene3DKind::SceneSetup:return "scene";
    case Scene3DKind::ShadowCatcher:case Scene3DKind::ReflectionCatcher:case Scene3DKind::Occluder:return "proxy geometry / scene";
    case Scene3DKind::ReadUSD:case Scene3DKind::Card:case Scene3DKind::ReadGeo:return input==0?"image / material":"axis";
    case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:case Scene3DKind::Axis:return "axis";
    case Scene3DKind::Light:return input==0?"axis":"environment image";
    case Scene3DKind::PBRMaterial:return input==0?"image / material":input==1?"normal":input==2?"roughness":"metallic";
    case Scene3DKind::TransformGeo:return input==0?"geo / scene":"axis";
    case Scene3DKind::Project3D:return input==0?"image":"projection camera";
    case Scene3DKind::Render3D:return input==0?"scene":input==1?"camera":"background";
    case Scene3DKind::BindMaterial:return input==0?"scene":"material";
    case Scene3DKind::USDTransform:case Scene3DKind::USDVisibility:case Scene3DKind::USDCompositing:return "scene";
    default:return "object "+std::to_string(input+1);
    }
}
void NativeScene3D::addAcceptedComponents(int input,std::list<ImagePlaneDesc>* comps) {
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    if(input==-1 && _kind==Scene3DKind::Render3D){comps->push_back(depthPlane());comps->push_back(albedoPlane());comps->push_back(normalPlane());comps->push_back(positionPlane());comps->push_back(shadowPlane());comps->push_back(occlusionPlane());comps->push_back(reflectionPlane());}
}
const ImagePlaneDesc& NativeScene3D::depthPlane() {
    static const ImagePlaneDesc plane("Depth","Depth","Z",std::vector<std::string>{"Z"});return plane;
}
const ImagePlaneDesc& NativeScene3D::albedoPlane(){static const ImagePlaneDesc p("Albedo","Albedo","RGB",{"R","G","B"});return p;}
const ImagePlaneDesc& NativeScene3D::normalPlane(){static const ImagePlaneDesc p("N","N","XYZ",{"X","Y","Z"});return p;}
const ImagePlaneDesc& NativeScene3D::positionPlane(){static const ImagePlaneDesc p("P","P","XYZ",{"X","Y","Z"});return p;}
const ImagePlaneDesc& NativeScene3D::shadowPlane(){static const ImagePlaneDesc p("Shadow","Shadow","RGBA",{"R","G","B","A"});return p;}
const ImagePlaneDesc& NativeScene3D::reflectionPlane(){static const ImagePlaneDesc p("Reflection","Reflection","RGB",{"R","G","B"});return p;}
const ImagePlaneDesc& NativeScene3D::occlusionPlane(){static const ImagePlaneDesc p("Occlusion","Occlusion","R",{"R"});return p;}
void NativeScene3D::getComponentsNeededAndProduced(double time,ViewIdx view,ComponentsNeededMap* comps,
                                                   double* passTime,int* passView,int* passInput) {
    if(_kind!=Scene3DKind::Render3D){EffectInstance::getComponentsNeededAndProduced(time,view,comps,passTime,passView,passInput);return;}
    *passTime=time;*passView=view.value();*passInput=-1;
    (*comps)[-1]={ImagePlaneDesc::getRGBAComponents(),depthPlane(),shadowPlane(),occlusionPlane(),reflectionPlane()};
    if(rendererBackendAt(time,view)==3){(*comps)[-1].push_back(albedoPlane());(*comps)[-1].push_back(normalPlane());(*comps)[-1].push_back(positionPlane());}
    // Geometry and camera inputs are traversed by sceneAt, not image rendering.
    if(getInput(2))(*comps)[2]={ImagePlaneDesc::getRGBAComponents()};
}
bool NativeScene3D::depthPreviewAt(double time,ViewIdx view) const {
    const auto knob=_output.lock();return knob && sceneValueAt(knob,time,0,view)==1;
}
Gpu::Cards::RenderPass NativeScene3D::displayPassAt(double time,ViewIdx view)const {return _output.lock()?Gpu::Cards::RenderPass(sceneValueAt(_output.lock(),time,0,view)):Gpu::Cards::RenderPass::Color;}
std::shared_ptr<const Gpu::Cards::Texture> NativeScene3D::backgroundAt(double time,ViewIdx view,bool draft)const {auto input=getInput(2);return input?fetchTexture(input,time,view,draft):nullptr;}
Gpu::Cards::Compositing NativeScene3D::compositingAt(double time,ViewIdx view)const {
    Gpu::Cards::Compositing r;r.role=_kind==Scene3DKind::ShadowCatcher?Gpu::Cards::GeometryRole::ShadowCatcher:_kind==Scene3DKind::Occluder?Gpu::Cards::GeometryRole::Occluder:_kind==Scene3DKind::ReflectionCatcher?Gpu::Cards::GeometryRole::ReflectionCatcher:Gpu::Cards::GeometryRole(sceneValueAt(_compositingRole.lock(),time,0,view));
    if((_catchShadows.lock() && sceneValueAt(_catchShadows.lock(),time,0,view)) || (_catchReflections.lock() && sceneValueAt(_catchReflections.lock(),time,0,view)))r.role=Gpu::Cards::GeometryRole::ShadowReflectionCatcher;
    r.cameraVisible=sceneValueAt(_cameraVisible.lock(),time,0,view);r.shadowVisible=sceneValueAt(_shadowVisible.lock(),time,0,view);r.reflectionVisible=sceneValueAt(_reflectionVisible.lock(),time,0,view);
    r.holdout=_holdout.lock() && sceneValueAt(_holdout.lock(),time,0,view);r.shadowDensity=sceneValueAt(_shadowDensity.lock(),time,0,view);r.shadowTint=color3(_shadowTint,time,view);
    r.reflectionStrength=sceneValueAt(_reflectionStrength.lock(),time,0,view);r.reflectionRoughness=sceneValueAt(_reflectionRoughness.lock(),time,0,view);Gpu::Cards::validateCompositing(r);return r;
}
std::array<double,2> NativeScene3D::depthPreviewRangeAt(double time,ViewIdx view) const {
    const auto knob=_depthRange.lock();return knob?std::array<double,2>{sceneValueAt(knob,time,0,view),sceneValueAt(knob,time,1,view)}:std::array<double,2>{0,10};
}
void NativeScene3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const {depths->push_back(eImageBitDepthFloat);}
NodeDataType NativeScene3D::getOutputDataType() const {
    switch(_kind) {
    case Scene3DKind::ReconstructionScene:return NodeDataType::Scene;
    case Scene3DKind::SceneSetup:case Scene3DKind::ShadowCatcher:case Scene3DKind::ReflectionCatcher:case Scene3DKind::Occluder:return NodeDataType::Scene;
    case Scene3DKind::SceneCamera:return NodeDataType::Camera;
    case Scene3DKind::Card:case Scene3DKind::ReadGeo:case Scene3DKind::TransformGeo:return NodeDataType::Geometry;
    case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:return NodeDataType::Camera;
    case Scene3DKind::Axis:return NodeDataType::Transform;
    case Scene3DKind::ReadUSD:case Scene3DKind::Scene:case Scene3DKind::USDTransform:case Scene3DKind::USDVisibility:case Scene3DKind::BindMaterial:case Scene3DKind::USDCompositing:return NodeDataType::Scene;
    case Scene3DKind::Project3D:case Scene3DKind::PBRMaterial:return NodeDataType::Material;
    case Scene3DKind::Light:return NodeDataType::Light;
    default:return NodeDataType::Image;
    }
}
bool NativeScene3D::acceptsInputDataType(int i,NodeDataType t) const {
    if(t==NodeDataType::Unknown)return true;
    const bool geo=t==NodeDataType::Geometry || t==NodeDataType::Scene;
    switch(_kind) {
    case Scene3DKind::ReconstructionScene:return i==0 && t==NodeDataType::Image;
    case Scene3DKind::SceneCamera:return i==0 && geo;
    case Scene3DKind::SceneSetup:case Scene3DKind::ShadowCatcher:case Scene3DKind::ReflectionCatcher:case Scene3DKind::Occluder:return i==0 && geo;
    case Scene3DKind::ReadUSD:case Scene3DKind::Card:case Scene3DKind::ReadGeo:return i==0?(t==NodeDataType::Image || t==NodeDataType::Material):t==NodeDataType::Transform;
    case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:case Scene3DKind::Axis:return t==NodeDataType::Transform;
    case Scene3DKind::Light:return i==0?t==NodeDataType::Transform:i==1 && t==NodeDataType::Image;
    case Scene3DKind::PBRMaterial:return i>=0 && i<4 && (t==NodeDataType::Image || (i==0 && t==NodeDataType::Material));
    case Scene3DKind::TransformGeo:return i==0?geo:t==NodeDataType::Transform;
    case Scene3DKind::Scene:return geo || t==NodeDataType::Light || t==NodeDataType::Transform;
    case Scene3DKind::Project3D:return i==0?t==NodeDataType::Image:t==NodeDataType::Camera;
    case Scene3DKind::BindMaterial:return i==0?geo:t==NodeDataType::Material || t==NodeDataType::Image;
    case Scene3DKind::USDTransform:case Scene3DKind::USDVisibility:case Scene3DKind::USDCompositing:return i==0 && geo;
    default:return i==0?geo:i==1?t==NodeDataType::Camera:t==NodeDataType::Image;
    }
}
void NativeScene3D::initializeKnobs() {
    auto page=AppManager::createKnob<KnobPage>(this,tr("Controls"));page->setName("controls");
    auto identity=AppManager::createKnob<KnobString>(this,tr("USD identity"));identity->setName("usdIdentity");identity->setAnimationEnabled(false);identity->setSecret(true);identity->setEvaluateOnChange(false);
    identity->setValue(QUuid::createUuid().toString(QUuid::Id128).toStdString());page->addKnob(identity);_sceneId=identity;
    if(_kind==Scene3DKind::ReconstructionScene)return;
    if(_kind==Scene3DKind::SceneSetup){initializeLayoutKnobs(page);return;}
    if(_kind==Scene3DKind::SceneCamera){auto path=AppManager::createKnob<KnobString>(this,tr("Camera prim path"));path->setName("cameraPath");path->setAnimationEnabled(false);path->setDefaultValue("/Reconstruction/Camera");page->addKnob(path);_usdCamera=path;return;}
    auto number=[&](const char* name,const char* label,int dimensions,double value) {
        auto knob=AppManager::createKnob<KnobDouble>(this,tr(label),dimensions);knob->setName(name);
        for(int i=0;i<dimensions;++i)knob->setDefaultValue(value,i);page->addKnob(knob);return knob;
    };
    if(geometryLeaf(_kind) || cameraKind(_kind) || _kind==Scene3DKind::Axis || _kind==Scene3DKind::TransformGeo || _kind==Scene3DKind::Light || _kind==Scene3DKind::USDTransform) {
        _translate=number("translate",(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?"Translate offset":"Translate",3,0);
        _rotate=number("rotate",(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?"Rotate offset":"Rotate",3,0);
        _scale=number("scale",(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?"Scale multiplier":"Scale",3,1);
        if(_kind==Scene3DKind::Camera)_translate.lock()->setDefaultValue(5,2);
    }
    auto color=[&](const char* name,const char* label,double value) {
        auto knob=AppManager::createKnob<KnobColor>(this,tr(label),3);knob->setName(name);
        for(int i=0;i<3;++i)knob->setDefaultValue(value,i);page->addKnob(knob);return knob;
    };
    if(_kind==Scene3DKind::USDCompositing || _kind==Scene3DKind::ShadowCatcher || _kind==Scene3DKind::ReflectionCatcher || _kind==Scene3DKind::Occluder){
        if(_kind==Scene3DKind::USDCompositing){auto role=AppManager::createKnob<KnobChoice>(this,tr("Geometry role"));role->setName("compositingRole");role->populateChoices({ChoiceOption("Surface"),ChoiceOption("ShadowCatcher","Shadow catcher","Receive CG shadows without showing the surface."),ChoiceOption("Occluder","Occluder","Hide CG behind a plate object."),ChoiceOption("ReflectionCatcher","Reflection catcher","Receive added CG reflections."),ChoiceOption("ShadowReflectionCatcher","Shadow and reflection catcher","Receive shadows and reflections on the same proxy.")});page->addKnob(role);_compositingRole=role;}
        _shadowDensity=number("shadowDensity","Shadow density",1,1);_shadowDensity.lock()->setMinimum(0);_shadowDensity.lock()->setMaximum(10);_shadowDensity.lock()->setDisplayMaximum(2);
        _shadowTint=color("shadowTint","Shadow tint (linear)",0);
        _reflectionStrength=number("reflectionStrength","Reflection strength",1,.25);_reflectionStrength.lock()->setMinimum(0);_reflectionStrength.lock()->setMaximum(10);_reflectionStrength.lock()->setDisplayMaximum(1);
        _reflectionRoughness=number("reflectionRoughness","Reflection roughness",1,.2);_reflectionRoughness.lock()->setMinimum(0);_reflectionRoughness.lock()->setMaximum(1);
        _reflectionStrength.lock()->setHintToolTip(tr("Scales the added model's signed reflection contribution. 0 preserves the plate exactly; 1 uses a white reflector. Match scene lighting to the plate. The Reflection RGB layer can contain negative values when CG blocks an environment reflection."));
        _reflectionRoughness.lock()->setHintToolTip(tr("0 is a sharp mirror; larger values spread and soften reflections using the surface BRDF. Increase path samples for rough reflections in USD / hdNVP."));
        _shadowDensity.lock()->setHintToolTip(tr("0 removes received shadows while retaining the proxy's occlusion. 1 applies the measured lighting loss; larger values strengthen it. Shadow tint is scene-linear RGB."));
        if(_kind!=Scene3DKind::USDCompositing){
            auto flag=[&](const char* name,const char* label,bool value){auto k=AppManager::createKnob<KnobBool>(this,tr(label));k->setName(name);k->setDefaultValue(value);page->addKnob(k);return k;};
            if(_kind==Scene3DKind::ReflectionCatcher)_catchShadows=flag("catchShadows","Catch shadows",false);
            if(_kind==Scene3DKind::ShadowCatcher)_catchReflections=flag("catchReflections","Catch reflections",false);
            _cameraVisible=flag("cameraVisible","Occlude CG / receive lighting",true);_shadowVisible=flag("shadowVisible","Cast shadows",true);_reflectionVisible=flag("reflectionVisible","Visible to secondary rays",false);
            updateCompositingControls();
            return;
        }
        updateCompositingControls();
    }
    if(usdEditKind(_kind)) {
        if(_kind==Scene3DKind::USDTransform) {
            _usdPivot=number("pivot","Pivot",3,0);
            _usdPivot.lock()->setHintToolTip(tr("World-space rotation and scale pivot in Y-up meters. Outline initializes it at the selected Prims' bounds. Zero preserves existing USDTransform behavior."));
        }
        auto targets=AppManager::createKnob<KnobString>(this,tr("USD Prim paths"));targets->setName("primPaths");targets->setAsMultiLine();targets->setAnimationEnabled(false);
        targets->setHintToolTip(tr("Absolute paths in the composed stage, separated by whitespace. Edits affect only this node's downstream branch. Instance child edits create a private override of that instance."));page->addKnob(targets);_usdTargets=targets;
        auto flag=[&](const char* name,const char* label,bool value){auto k=AppManager::createKnob<KnobBool>(this,tr(label));k->setName(name);k->setDefaultValue(value);page->addKnob(k);return k;};
        if(_kind==Scene3DKind::USDVisibility)_visible=flag("visible","Visible",true);
        if(_kind==Scene3DKind::USDCompositing){_cameraVisible=flag("cameraVisible","Camera visibility",true);_shadowVisible=flag("shadowVisible","Shadow visibility",true);_reflectionVisible=flag("reflectionVisible","Reflection visibility",true);_holdout=flag("holdout","Holdout",false);}
    }
    if(_kind==Scene3DKind::PBRMaterial) {
        _baseColor=color("baseColor","Base color multiplier",1);_emission=color("emission","Emission",0);
        _baseColor.lock()->setHintToolTip(tr("Linear RGB multiplier for the input image, projection or imported mesh colors. This is a straight color, not premultiplied."));
        _metallic=number("metallic","Metallic",1,0);_roughness=number("roughness","Roughness",1,.5);_ior=number("ior","Index of refraction",1,1.5);
        _materialOpacity=number("opacity","Opacity",1,1);
        for(const auto& knob:{_metallic.lock(),_roughness.lock(),_materialOpacity.lock()}){knob->setMinimum(0);knob->setMaximum(1);}
        _ior.lock()->setMinimum(1);
        _roughness.lock()->setHintToolTip(tr("Used when the roughness input is disconnected. A connected map replaces this value using Roughness channel."));
        _metallic.lock()->setHintToolTip(tr("Used when the metallic input is disconnected. A connected map replaces this value using Metallic channel."));
        auto choice=[&](const char* name,const char* label,const std::vector<ChoiceOption>& options,int initial=0) {
            auto knob=AppManager::createKnob<KnobChoice>(this,tr(label));knob->setName(name);knob->populateChoices(options);
            knob->setDefaultValue(initial);page->addKnob(knob);return knob;
        };
        _normalEncoding=choice("normalEncoding","Normal encoding",{
            ChoiceOption("Unsigned","0..1 RGB","Decode image RGB as 2 * RGB - 1."),
            ChoiceOption("Signed","-1..1 RGB","Use already decoded tangent-space RGB vectors.")});
        auto flip=AppManager::createKnob<KnobBool>(this,tr("Invert normal Y"));flip->setName("normalFlipY");page->addKnob(flip);_normalFlipY=flip;
        flip->setHintToolTip(tr("Negate the tangent-space Y component. Positive Y normally follows increasing V."));
        const std::vector<ChoiceOption> channels{ChoiceOption("R"),ChoiceOption("G"),ChoiceOption("B"),ChoiceOption("A")};
        _roughnessChannel=choice("roughnessChannel","Roughness channel",channels);
        _metallicChannel=choice("metallicChannel","Metallic channel",channels);
        _mapWrap=choice("mapWrap","Map wrapping",{ChoiceOption("Black"),ChoiceOption("Clamp"),ChoiceOption("Repeat"),ChoiceOption("Mirror")},2);
        _mapWrap.lock()->setHintToolTip(tr("Wrapping for all three data maps in mesh UV space. Each map uses its source image format; cropped regions are zero data. Map alpha does not change surface opacity. Read data images as raw/linear with unpremultiplied output."));
    }
    if(_kind==Scene3DKind::Light) {
        auto type=AppManager::createKnob<KnobChoice>(this,tr("Light type"));type->setName("lightType");
        type->populateChoices({ChoiceOption("Distant","","Parallel rays along local -Z."),ChoiceOption("Point","","Omnidirectional point source with inverse-square attenuation."),ChoiceOption("Spot","","Cone around local -Z with inverse-square attenuation."),ChoiceOption("Environment","","Diffuse and GGX image-based lighting from the environment image input, or uniform Color.")});page->addKnob(type);_lightType=type;
        auto envSamples=AppManager::createKnob<KnobChoice>(this,tr("Environment samples per lobe"));envSamples->setName("environmentSamples");
        envSamples->populateChoices({ChoiceOption("16"),ChoiceOption("32"),ChoiceOption("64")});envSamples->setDefaultValue(1);page->addKnob(envSamples);_environmentSamples=envSamples;
        envSamples->setHintToolTip(tr("Samples for each of the diffuse and GGX reflection lobes. More samples reduce integration artifacts, especially with small bright lights in an HDR image. Applies to Environment lights. Supply straight linear radiance; alpha is ignored. Image input 1 supports Natron Reader formats including EXR/HDR. Rotate to orient the image; translation and scale do not affect its illumination."));
        _translate.lock()->setDefaultValue(5,2);_lightColor=color("color","Color",1);
        _intensity=number("intensity","Intensity",1,1);_intensity.lock()->setMinimum(0);_exposure=number("exposure","Exposure (stops)",1,0);
        _coneAngle=number("coneAngle","Cone half angle (degrees)",1,30);_coneAngle.lock()->setMinimum(.001);_coneAngle.lock()->setMaximum(89.999);
        _coneSoftness=number("coneSoftness","Cone softness",1,.2);_coneSoftness.lock()->setMinimum(0);_coneSoftness.lock()->setMaximum(1);
        auto shadows=AppManager::createKnob<KnobBool>(this,tr("Cast shadows"));shadows->setName("shadows");shadows->setDefaultValue(true);page->addKnob(shadows);_shadows=shadows;
        _shadowBias=number("shadowBias","Shadow bias",1,.0001);_shadowBias.lock()->setMinimum(.0000001);
        _intensity.lock()->setHintToolTip(tr("Linear light strength multiplied by 2^Exposure. Point/spot strength is divided by squared distance in world units. These are native renderer units, not photometric USD units."));
    }
    if(_kind==Scene3DKind::Card) {
        _size=number("size","Size",2,2);_opacity=number("opacity","Opacity",1,1);
    }
    if(_kind==Scene3DKind::ReadGeo || _kind==Scene3DKind::ReadCamera) {
        auto file=AppManager::createKnob<KnobFile>(this,tr("FBX file"));file->setName("file");file->setAnimationEnabled(false);
        file->setHintToolTip((_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?tr("ASCII/binary FBX containing cameras. Imported units are meters, right-handed Y-up, looking down local -Z. Camera-only files are supported. Connect to Render3D or Project3D."):
            tr("FBX mesh file. Imported units are meters, right-handed Y-up. Connect an image for UV texturing or Project3D for camera projection. Enable Read animation to play FBX transforms and deformation. Supports Vulkan and CPU rendering."));
        page->addKnob(file);_file=file;
        if(_kind==Scene3DKind::ReadGeo)_opacity=number("opacity","Opacity",1,1);
        else {
            auto selection=AppManager::createKnob<KnobString>(this,tr("FBX camera"));selection->setName("cameraName");selection->setAnimationEnabled(false);
            selection->setHintToolTip(tr("Camera hierarchy path shown in FBX cameras, or a unique node name. Leave blank for the first camera in the file."));page->addKnob(selection);_cameraName=selection;
            auto cameras=AppManager::createKnob<KnobString>(this,tr("FBX cameras"));cameras->setName("cameraInfo");cameras->setAsMultiLine();cameras->setAllDimensionsEnabled(false);
            cameras->setAnimationEnabled(false);cameras->setIsPersistent(false);cameras->setEvaluateOnChange(false);page->addKnob(cameras);_cameraInfo=cameras;
            _focalMultiplier=number("focalMultiplier","Focal multiplier",1,1);_focalMultiplier.lock()->setMinimum(.0001);
            _focalMultiplier.lock()->setHintToolTip(tr("Multiply the imported focal length; for orthographic cameras divide the imported width. 1 preserves the imported horizontal framing. Can be animated."));
        }
        auto animation=AppManager::createKnob<KnobBool>(this,tr("Read animation"));animation->setName("readAnimation");animation->setAnimationEnabled(false);
        animation->setDefaultValue(_kind==Scene3DKind::ReadCamera);
        animation->setHintToolTip((_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?tr("Evaluate FBX camera and parent transforms, lens and projection settings at the timeline's time. Disabled uses the unanimated file pose."):
            tr("Evaluate the FBX take, including node transforms, skinning and blend shapes. Disabled preserves the unanimated file pose."));
        page->addKnob(animation);_readAnimation=animation;
        if(_kind==Scene3DKind::ReadGeo) {
            auto skin=AppManager::createKnob<KnobBool>(this,tr("Apply mesh transform to skin"));skin->setName("applySkinTransform");skin->setAnimationEnabled(false);
            skin->setHintToolTip(tr("Enable for exports that animate the skinned mesh node independently of its bones, such as Maya mesh transform animation. Applies the mesh transform relative to its bind pose after skinning. Leave disabled when bone transforms already place the mesh in world space. Requires a mesh bind pose."));
            page->addKnob(skin);_applySkinTransform=skin;
        }
        auto take=AppManager::createKnob<KnobString>(this,tr("Animation take"));take->setName("animationTake");take->setAnimationEnabled(false);
        take->setHintToolTip(tr("Exact FBX take name. Leave blank to use the file's default take. Available names and source time ranges appear below."));page->addKnob(take);_animationTake=take;
        auto start=number("animationStart","Start frame",1,1);start->setAnimationEnabled(false);_animationStart=start;
        start->setHintToolTip(tr("Timeline frame aligned to the selected FBX take's beginning. Playback uses the project frame rate."));
        auto speed=number("animationSpeed","Playback speed",1,1);_animationSpeed=speed;
        speed->setHintToolTip(tr("FBX playback speed. 1 is real time; 0 holds a pose; negative values play backwards. Can be animated."));
        auto offset=number("animationOffset","Time offset (seconds)",1,0);_animationOffset=offset;
        offset->setHintToolTip(tr("Offset from the take's beginning, in seconds. Source time = (timeline frame - Start frame) / project FPS * Playback speed + Time offset. Can be animated."));
        auto info=AppManager::createKnob<KnobString>(this,tr("FBX takes"));info->setName("animationInfo");info->setAsMultiLine();info->setAllDimensionsEnabled(false);
        info->setAnimationEnabled(false);info->setIsPersistent(false);info->setEvaluateOnChange(false);page->addKnob(info);_animationInfo=info;
    }
    if(_kind==Scene3DKind::ReadUSD || _kind==Scene3DKind::ReadUSDCamera) {
        if(_kind==Scene3DKind::ReadUSD) {
            auto mode=AppManager::createKnob<KnobChoice>(this,tr("USD assembly"));mode->setName("usdAssembly");mode->setAnimationEnabled(false);
            mode->populateChoices({ChoiceOption("Shot layers","","Compose the complete shot at its original paths. Scene input order sets layer strength; overlapping paths intentionally combine opinions."),ChoiceOption("Asset reference","","Reference the Prim subtree (or defaultPrim when blank) under this node's stable namespace. Include all material and relationship dependencies inside the chosen subtree.")});page->addKnob(mode);_usdAssemblyMode=mode;
        }
        auto file=AppManager::createKnob<KnobFile>(this,tr("USD file"));file->setName("file");file->setAnimationEnabled(false);page->addKnob(file);_file=file;
        file->setHintToolTip(tr("OpenUSD .usd/.usda/.usdc/.usdz stage. References and sublayers resolve relative to their USD layers. Reload refreshes referenced layers. Coordinates are normalized to right-handed Y-up meters."));
        auto variants=AppManager::createKnob<KnobString>(this,tr("Variant selections"));variants->setName("variants");variants->setAsMultiLine();variants->setAnimationEnabled(false);
        variants->setHintToolTip(tr("One /Prim variantSet=selection per line. These overrides live in a private session layer and do not edit the source file."));page->addKnob(variants);_variants=variants;
        auto payloads=AppManager::createKnob<KnobBool>(this,tr("Load payloads"));payloads->setName("loadPayloads");payloads->setDefaultValue(true);payloads->setAnimationEnabled(false);page->addKnob(payloads);_loadPayloads=payloads;
        auto path=AppManager::createKnob<KnobString>(this,tr(_kind==Scene3DKind::ReadUSD?"Prim subtree":"USD camera"));path->setName(_kind==Scene3DKind::ReadUSD?"primPath":"cameraName");path->setAnimationEnabled(false);page->addKnob(path);
        if(_kind==Scene3DKind::ReadUSD) {_primPath=path;_opacity=number("opacity","Opacity",1,1);path->setHintToolTip(tr("Absolute prim path. Blank or / reads the whole stage. The node transform moves the selected stage subtree as one object."));}
        else {_cameraName=path;_focalMultiplier=number("focalMultiplier","Focal multiplier",1,1);_focalMultiplier.lock()->setMinimum(.0001);path->setHintToolTip(tr("Absolute camera path or unique camera name. Blank selects the first camera listed in Source info."));}
        auto animation=AppManager::createKnob<KnobBool>(this,tr("Read animation"));animation->setName("readAnimation");animation->setDefaultValue(true);animation->setAnimationEnabled(false);page->addKnob(animation);_readAnimation=animation;
        auto start=number("animationStart","Start frame",1,1);start->setAnimationEnabled(false);_animationStart=start;
        _animationSpeed=number("animationSpeed","Playback speed",1,1);_animationOffset=number("animationOffset","Time offset (seconds)",1,0);
        start->setHintToolTip(tr("Frame aligned to the USD startTimeCode. Source time code = startTimeCode + ((frame - Start frame) / project FPS * speed + offset seconds) * timeCodesPerSecond. Disabled animation holds startTimeCode."));
        auto info=AppManager::createKnob<KnobString>(this,tr("Source info"));info->setName("sourceInfo");info->setAsMultiLine();info->setAllDimensionsEnabled(false);info->setAnimationEnabled(false);info->setIsPersistent(false);info->setEvaluateOnChange(false);page->addKnob(info);_animationInfo=info;
    }
    if(_kind==Scene3DKind::Camera) {
        _focal=number("focal","Focal length (mm)",1,50);_aperture=number("haperture","Horizontal aperture (mm)",1,36);
        _near=number("near","Near clip",1,.001);_far=number("far","Far clip",1,100000);
        _orthoWidth=number("orthoWidth","Orthographic width",1,4);
        auto knob=AppManager::createKnob<KnobBool>(this,tr("Orthographic"));knob->setName("orthographic");page->addKnob(knob);_orthographic=knob;
    }
    if(_kind==Scene3DKind::Project3D) {
        auto frozen=AppManager::createKnob<KnobBool>(this,tr("Freeze projection camera"));frozen->setName("freeze");frozen->setDefaultValue(true);
        frozen->setHintToolTip(tr("Evaluate the projection camera at the reference frame. The render camera remains animated."));page->addKnob(frozen);_freeze=frozen;
        auto imageTiming=AppManager::createKnob<KnobChoice>(this,tr("Image timing"));imageTiming->setName("imageTimeMode");imageTiming->populateChoices({ChoiceOption("FollowCamera","Follow camera","Preserve the original Project3D timing: freeze the image when the projection camera is frozen."),ChoiceOption("Frozen","Reference frame","Sample the image at the reference frame independently of camera timing."),ChoiceOption("Live","Current frame","Sample the current image independently of camera timing.")});imageTiming->setDefaultValue(0);page->addKnob(imageTiming);_imageTiming=imageTiming;
        auto reference=AppManager::createKnob<KnobInt>(this,tr("Reference frame"));reference->setName("referenceFrame");reference->setDefaultValue(1);page->addKnob(reference);_reference=reference;
    }
    if(_kind==Scene3DKind::Render3D) {
        auto usdCamera=AppManager::createKnob<KnobString>(this,tr("USD render camera"));usdCamera->setName("usdCamera");usdCamera->setAnimationEnabled(false);usdCamera->setHintToolTip(tr("Absolute Camera Prim path in the composed scene. Blank uses the camera input. Setting this does not change the Viewer's inspection camera."));page->addKnob(usdCamera);_usdCamera=usdCamera;
        auto backend=AppManager::createKnob<KnobChoice>(this,tr("Renderer"));backend->setName("renderer");
        backend->populateChoices({ChoiceOption("Auto","","Use Vulkan when available; fall back to CPU for larger scenes or device limits."),ChoiceOption("CPU","","CPU card/mesh PBR renderer with direct and environment lighting."),ChoiceOption("Vulkan","","Require Vulkan. Supports up to 16 geometry objects, 16 lights and 16 distinct texture samplers in lit scenes, within device mesh-buffer limits."),ChoiceOption("hdNVP","USD / hdNVP","Path trace the composed USD stage through the persistent Hydra/hdNVP host, including Project3D and USD edit opinions. Build tools/build-hdnvp-host.ps1 first.")});page->addKnob(backend);_backend=backend;
        auto pathSamples=AppManager::createKnob<KnobInt>(this,tr("Path tracing samples"));pathSamples->setName("pathSamples");pathSamples->setMinimum(1);pathSamples->setMaximum(1048576);pathSamples->setDefaultValue(16);page->addKnob(pathSamples);_pathSamples=pathSamples;
        auto pathBounces=AppManager::createKnob<KnobInt>(this,tr("Path tracing bounces"));pathBounces->setName("pathBounces");pathBounces->setMinimum(0);pathBounces->setMaximum(64);pathBounces->setDefaultValue(4);page->addKnob(pathBounces);_pathBounces=pathBounces;
        auto pathDenoiser=AppManager::createKnob<KnobChoice>(this,tr("Denoiser"));pathDenoiser->setName("pathDenoiser");
        pathDenoiser->populateChoices({ChoiceOption("Auto","","USD / hdNVP: use DLSS Ray Reconstruction for 3D/draft previews when supported, otherwise OIDN. Use OIDN for completed renders."),
                                      ChoiceOption("OIDN","Open Image Denoise","Denoise the completed scene-linear color using albedo and normal guides. Preserve alpha and Depth.Z."),
                                      ChoiceOption("DLSS","DLSS Ray Reconstruction","Use temporal GPU reconstruction at native resolution. Requires a supported NVIDIA GPU and at least 32 x 32 pixels."),
                                      ChoiceOption("None","Off","Return the original path traced color without denoising.")});
        pathDenoiser->setHintToolTip(tr("Denoising for the USD / hdNVP renderer. Auto uses OIDN for completed renders and DLSS RR (or OIDN fallback) for 3D/draft previews. Low sample counts can lose fine detail."));
        page->addKnob(pathDenoiser);_pathDenoiser=pathDenoiser;
        auto aa=AppManager::createKnob<KnobChoice>(this,tr("Antialiasing"));aa->setName("antialiasing");
        aa->populateChoices({ChoiceOption("1x"),ChoiceOption("2x2"),ChoiceOption("4x4")});aa->setDefaultValue(1);page->addKnob(aa);_samples=aa;
        auto output=AppManager::createKnob<KnobChoice>(this,tr("Output"));output->setName("output");
        output->populateChoices({ChoiceOption("Color","","Premultiplied RGBA. Depth.Z remains available as a separate float layer."),
                                 ChoiceOption("DepthPreview","Depth preview","Near-white / far-black visualization. Depth.Z retains its raw values."),
                                 ChoiceOption("ShadowPreview","Shadow mask","Display the shadow coverage. Shadow RGBA remains available as a separate float layer."),
                                 ChoiceOption("OcclusionPreview","Occlusion mask","Display the visible proxy coverage. Occlusion.R remains available as a separate float layer."),
                                 ChoiceOption("ReflectionPreview","Reflection contribution","Signed reflection preview: neutral gray is unchanged, brighter adds radiance, darker removes it. Reflection RGB remains scene-linear data.")});page->addKnob(output);_output=output;
        auto range=number("depthRange","Depth preview range",2,0);range->setDefaultValue(10,1);
        range->setDimensionName(0,"Near");range->setDimensionName(1,"Far");
        range->setHintToolTip(tr("Near and far camera-space depths for the white-to-black preview. Requires 0 <= near < far. Does not change clipping or Depth.Z."));
        range->setMinimum(0,0);range->setMinimum(0,1);_depthRange=range;
    }
}
void NativeScene3D::purgeCaches() {
    _usdFrame.reset();_usdFrameKey.clear();
    {std::lock_guard<std::mutex> lock(_meshMutex);_usdAsset.reset();_usdPose.reset();_usdPoseAsset.reset();_mesh.reset();_fbxAsset.reset();_meshAsset.reset();_cameraAsset.reset();_meshPath.clear();_meshStamp=_meshSize=-1;}
    EffectInstance::purgeCaches();
}
int NativeScene3D::rendererBackendAt(double time,ViewIdx view) const {return _backend.lock()?sceneValueAt(_backend.lock(),time,0,view):0;}
Gpu::Cards::HdNvpSettings NativeScene3D::hdNvpSettingsAt(double time,ViewIdx view,bool draft) const {
    Gpu::Cards::HdNvpSettings result;result.samples=sceneValueAt(_pathSamples.lock(),time,0,view);result.bounces=sceneValueAt(_pathBounces.lock(),time,0,view);
    result.denoiser=static_cast<Gpu::Cards::HdNvpDenoiser>(sceneValueAt(_pathDenoiser.lock(),time,0,view));result.interactive=draft;
    if(draft)result.samples=std::min(result.samples,4);return result;
}
std::string NativeScene3D::sceneId()const{return _sceneId.lock()->getValue();}
void NativeScene3D::onKnobsLoaded() {
    bool duplicate=sceneId().empty();NodesList nodes;getApp()->getProject()->getNodes_recursive(nodes,false);
    for(const auto& node:nodes)if(node!=getNode())if(auto native=dynamic_cast<NativeScene3D*>(node->getEffectInstance().get()))if(native->_sceneId.lock() && native->sceneId()==sceneId()){duplicate=true;break;}
    if(duplicate)_sceneId.lock()->setValue(QUuid::createUuid().toString(QUuid::Id128).toStdString());
    if(_file.lock())refreshAnimationInfo();
    if(_kind==Scene3DKind::SceneSetup)updateLayoutStatus();
    updateCompositingControls();
}
void NativeScene3D::updateCompositingControls() {
    if(!_shadowDensity.lock() || !_reflectionRoughness.lock())return;
    const int role=_compositingRole.lock()?_compositingRole.lock()->getValue():0;
    const bool shadow=_kind==Scene3DKind::ShadowCatcher || ( _catchShadows.lock() && _catchShadows.lock()->getValue()) || Gpu::Cards::catchesShadows(Gpu::Cards::GeometryRole(role));
    const bool reflection=_kind==Scene3DKind::ReflectionCatcher || (_catchReflections.lock() && _catchReflections.lock()->getValue()) || Gpu::Cards::catchesReflections(Gpu::Cards::GeometryRole(role));
    _shadowDensity.lock()->setSecret(!shadow);_shadowTint.lock()->setSecret(!shadow);
    _reflectionStrength.lock()->setSecret(!reflection);_reflectionRoughness.lock()->setSecret(!reflection);
}
bool NativeScene3D::knobChanged(KnobI* knob,ValueChangedReasonEnum,ViewSpec,double,bool) {
    if(knob==_compositingRole.lock().get() || knob==_catchShadows.lock().get() || knob==_catchReflections.lock().get()){updateCompositingControls();return true;}
    if(_kind==Scene3DKind::SceneSetup)return layoutKnobChanged(knob);
    if(!_file.lock() || (knob!=_file.lock().get() && knob!=_variants.lock().get() && knob!=_loadPayloads.lock().get() && knob!=_primPath.lock().get()))return false;
    refreshAnimationInfo();return true;
}
void NativeScene3D::refreshAnimationInfo() {
    const auto info=_animationInfo.lock();if(!info)return;
    const auto cameraInfo=_cameraInfo.lock();
    if(_file.lock()->getValue().empty()){info->setValue("");if(cameraInfo)cameraInfo->setValue("");return;}
    try {
        if(_kind==Scene3DKind::ReadUSD || _kind==Scene3DKind::ReadUSDCamera) {
            const auto asset=usdAssetAt(ViewIdx(0));std::ostringstream text;
            text<<"Source FPS: "<<asset->frameRate()<<"  timeCodesPerSecond: "<<asset->timeCodesPerSecond()<<"\nTime codes: "<<asset->startTime()<<" .. "<<asset->endTime();
            for(const auto& path:asset->cameras())text<<"\nCamera: "<<path;
            if(_kind==Scene3DKind::ReadUSD) {
                const auto subtree=_primPath.lock()->getValue();std::vector<std::string> meshes;
                for(const auto& path:asset->meshPaths())if(subtree.empty() || subtree=="/" || path==subtree || path.rfind(subtree+"/",0)==0)meshes.push_back(path);
                text<<"\nMeshes: "<<meshes.size()<<"  Lights: "<<asset->lightCount(subtree);
                for(const auto& path:meshes)text<<"\nMesh: "<<path;
                text<<"\nUSD composition loaded. Meshes/materials are evaluated by the selected renderer.";
            }else text<<"\nHorizontal camera framing; film offsets are evaluated.";
            info->setValue(text.str());return;
        }
        const auto asset=fbxAssetAt(ViewIdx(0));std::ostringstream text;text<<"Source FPS: "<<asset->frameRate();
        if(asset->takes().empty())text<<"\nNo animation takes.";
        for(const auto& take:asset->takes())text<<'\n'<<take.name<<"  ["<<take.begin<<" .. "<<take.end<<" seconds]";
        info->setValue(text.str());
        if(cameraInfo) {
            std::ostringstream names;
            if(asset->cameras().empty())names<<"FBX contains no cameras.";
            for(const auto& name:asset->cameras()){if(names.tellp()>0)names<<'\n';names<<name;}
            cameraInfo->setValue(names.str());
        }
    }catch(const std::exception& error){info->setValue(error.what());if(cameraInfo)cameraInfo->setValue(error.what());}
}
std::shared_ptr<Gpu::Cards::UsdSceneAsset> NativeScene3D::usdAssetAt(ViewIdx view) const {

    std::string path=_file.lock()->getValue(0,view);getApp()->getProject()->canonicalizePath(path);

    if(path.empty())throw std::runtime_error(getPluginLabel()+" requires a USD filename.");
    const QFileInfo info(QString::fromUtf8(path.c_str()));if(!info.isFile())throw std::runtime_error("Cannot open USD: "+path);
    const auto variants=_variants.lock()->getValue(0,view);const bool payloads=_loadPayloads.lock()->getValue(0,view);
    const qint64 stamp=info.lastModified().toMSecsSinceEpoch(),size=info.size();std::lock_guard<std::mutex> lock(_meshMutex);
    if(!_usdAsset || path!=_meshPath || stamp!=_meshStamp || size!=_meshSize || variants!=_usdVariantSelection || payloads!=_usdPayloads) {
        auto asset=Gpu::Cards::UsdSceneAsset::load(path,variants,payloads);_usdAsset=std::move(asset);_usdPose.reset();_usdPoseAsset.reset();
        _meshPath=path;_meshStamp=stamp;_meshSize=size;_usdVariantSelection=variants;_usdPayloads=payloads;
    }
    return _usdAsset;
}
std::shared_ptr<const Gpu::Cards::UsdScenePose> NativeScene3D::usdPoseAt(double time,ViewIdx view) const {
    const auto asset=usdAssetAt(view);const bool animated=_readAnimation.lock()->getValue(0,view);const double seconds=animated?fbxSecondsAt(time,view):0;
    const auto path=_primPath.lock()->getValue(0,view);std::lock_guard<std::mutex> lock(_meshMutex);
    if(!_usdPose || _usdPoseAsset!=asset || seconds!=_usdTime || animated!=_usdAnimated || path!=_usdPrimPath) {
        _usdPose=std::make_shared<Gpu::Cards::UsdScenePose>(asset->evaluate(seconds,animated,path));_usdPoseAsset=asset;_usdTime=seconds;_usdAnimated=animated;_usdPrimPath=path;
    }
    return _usdPose;
}
std::shared_ptr<Gpu::Cards::FbxMeshAsset> NativeScene3D::fbxAssetAt(ViewIdx view) const {
    std::string path=_file.lock()->getValue(0,view);getApp()->getProject()->canonicalizePath(path);
    if(path.empty())throw std::runtime_error(getPluginLabel()+" requires an FBX filename.");
    const QFileInfo info(QString::fromUtf8(path.c_str()));
    if(!info.isFile())throw std::runtime_error("Cannot open FBX: "+path);
    const qint64 stamp=info.lastModified().toMSecsSinceEpoch(),size=info.size();
    std::lock_guard<std::mutex> lock(_meshMutex);
    if(!_fbxAsset || path!=_meshPath || stamp!=_meshStamp || size!=_meshSize) {
        auto loaded=Gpu::Cards::FbxMeshAsset::load(path,_kind!=Scene3DKind::ReadCamera);_fbxAsset=std::move(loaded);_mesh.reset();_meshAsset.reset();_cameraAsset.reset();_meshPath=path;_meshStamp=stamp;_meshSize=size;
    }
    return _fbxAsset;
}
std::shared_ptr<const Gpu::Cards::Mesh> NativeScene3D::meshAt(double time,ViewIdx view) const {
    const auto asset=fbxAssetAt(view);
    if(!_readAnimation.lock()->getValue(0,view))return asset->staticMesh();
    const double seconds=fbxSecondsAt(time,view);
    const std::string take=_animationTake.lock()->getValue(0,view);
    const bool skinTransform=_applySkinTransform.lock()->getValue(0,view);
    std::lock_guard<std::mutex> lock(_meshMutex);
    if(!_mesh || _meshAsset!=asset || _meshTime!=seconds || _meshTake!=take || _meshSkinTransform!=skinTransform) {
        auto evaluated=asset->evaluate(seconds,take,skinTransform);_mesh=std::move(evaluated);_meshAsset=asset;_meshTime=seconds;_meshTake=take;_meshSkinTransform=skinTransform;
    }
    return _mesh;
}
double NativeScene3D::fbxSecondsAt(double time,ViewIdx view) const {
    const double fps=getApp()->getProject()->getProjectFrameRate();
    if(!std::isfinite(fps) || fps<=0)throw std::runtime_error("3D source animation requires a positive project frame rate.");
    return (time-_animationStart.lock()->getValue(0,view))/fps*sceneValueAt(_animationSpeed.lock(),time,0,view)+sceneValueAt(_animationOffset.lock(),time,0,view);
}
Gpu::Cards::Camera NativeScene3D::importedCameraAt(double time,ViewIdx view) const {
    const auto asset=fbxAssetAt(view);const bool animated=_readAnimation.lock()->getValue(0,view);
    const double seconds=animated?fbxSecondsAt(time,view):0;
    const std::string name=_cameraName.lock()->getValue(0,view),take=_animationTake.lock()->getValue(0,view);
    std::lock_guard<std::mutex> lock(_meshMutex);
    if(_cameraAsset!=asset || _cameraTime!=seconds || _cameraSelection!=name || _cameraTake!=take || _cameraAnimated!=animated) {
        const auto camera=asset->camera(name,animated,seconds,take);
        _importedCamera=camera;_cameraAsset=asset;_cameraTime=seconds;_cameraSelection=name;_cameraTake=take;_cameraAnimated=animated;
    }
    return _importedCamera;
}
Matrix NativeScene3D::transformAt(double time,ViewIdx view) const {
    if(_kind==Scene3DKind::SceneCamera)return cameraAt(time,view).transform;
    if(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)return cameraAt(time,view).transform;
    Matrix local=Matrix::transform(value3(_translate,time,view),value3(_rotate,time,view),value3(_scale,time,view,{1,1,1}));
    const int parentInput=(geometryLeaf(_kind) || _kind==Scene3DKind::TransformGeo)?1:0;
    if(auto parent=input3D(this,parentInput))local=parent->transformAt(time,view)*local;
    return local;
}
Gpu::Cards::Material NativeScene3D::materialAt(double time,ViewIdx view) const {
    Gpu::Cards::Material m;m.lit=true;const auto base=color3(_baseColor,time,view);
    m.baseColor={float(base.x),float(base.y),float(base.z),float(sceneValueAt(_materialOpacity.lock(),time,0,view))};
    m.emission=color3(_emission,time,view);m.metallic=sceneValueAt(_metallic.lock(),time,0,view);
    m.roughness=sceneValueAt(_roughness.lock(),time,0,view);m.ior=sceneValueAt(_ior.lock(),time,0,view);return m;
}
Gpu::Cards::Light NativeScene3D::lightAt(double time,ViewIdx view) const {
    Gpu::Cards::Light light;light.transform=transformAt(time,view);light.type=Gpu::Cards::LightType(sceneValueAt(_lightType.lock(),time,0,view));
    light.color=color3(_lightColor,time,view);light.intensity=sceneValueAt(_intensity.lock(),time,0,view);light.exposure=sceneValueAt(_exposure.lock(),time,0,view);
    light.coneAngle=sceneValueAt(_coneAngle.lock(),time,0,view);light.coneSoftness=sceneValueAt(_coneSoftness.lock(),time,0,view);
    light.shadows=sceneValueAt(_shadows.lock(),time,0,view);light.shadowBias=sceneValueAt(_shadowBias.lock(),time,0,view);
    light.environmentSamples=16u<<sceneValueAt(_environmentSamples.lock(),time,0,view);return light;
}
Gpu::Cards::Camera NativeScene3D::cameraAt(double time,ViewIdx view) const {
    Gpu::Cards::Camera camera;
    if(_kind==Scene3DKind::SceneCamera){
        auto source=input3D(this,0);if(!source)throw std::runtime_error("SceneCamera needs a scene input");
        const auto scene=source->sceneAt(time,view,true);Gpu::Cards::UsdSceneBridge bridge;Gpu::Cards::Settings settings;
        settings.lights=scene.lights;settings.edits=scene.usdEdits;std::vector<Gpu::Cards::Card> cards;for(const auto& item:scene.items)cards.push_back(item.card);
        bridge.exportScene(cards,scene.camera,settings,scene.usdSources);return bridge.sceneValue()->camera(_usdCamera.lock()->getValue(0,view),true);
    }
    if(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera) {
        camera=_kind==Scene3DKind::ReadCamera?importedCameraAt(time,view):usdAssetAt(view)->camera(_cameraName.lock()->getValue(0,view),_readAnimation.lock()->getValue(0,view)?fbxSecondsAt(time,view):0,_readAnimation.lock()->getValue(0,view));
        const Vec3 position=camera.transform.point({})+value3(_translate,time,view);
        Matrix basis=camera.transform;basis.m[12]=basis.m[13]=basis.m[14]=0;
        camera.transform=Matrix::transform(position,value3(_rotate,time,view),value3(_scale,time,view,{1,1,1}))*basis;
        if(auto parent=input3D(this,0))camera.transform=parent->transformAt(time,view)*camera.transform;
        const double focalMultiplier=sceneValueAt(_focalMultiplier.lock(),time,0,view);
        if(!std::isfinite(focalMultiplier) || focalMultiplier<=0)throw std::runtime_error(getPluginLabel()+" focal multiplier must be positive.");
        camera.focal*=focalMultiplier;camera.orthoWidth/=focalMultiplier;return camera;
    }
    if(!cameraKind(_kind))throw std::runtime_error("A Camera connection is required.");
    camera.transform=transformAt(time,view);
    camera.focal=sceneValueAt(_focal.lock(),time,0,view);camera.aperture=sceneValueAt(_aperture.lock(),time,0,view);
    camera.nearClip=sceneValueAt(_near.lock(),time,0,view);camera.farClip=sceneValueAt(_far.lock(),time,0,view);
    camera.orthoWidth=sceneValueAt(_orthoWidth.lock(),time,0,view);camera.orthographic=sceneValueAt(_orthographic.lock(),time,0,view);
    return camera;
}
Scene3DObject NativeScene3D::editorObjectAt(double time,ViewIdx view) const {
    if(_kind==Scene3DKind::USDTransform) {
        Scene3DObject object;object.kind=_kind;object.owner=getNode();object.time=time;
        object.transform=Matrix::transform(value3(_usdPivot,time,view)+value3(_translate,time,view),value3(_rotate,time,view),value3(_scale,time,view,{1,1,1}));
        object.primPaths=targetPaths(_usdTargets,view);return object;
    }
    Scene3DObject object;object.kind=_kind;object.owner=getNode();object.time=time;object.transform=transformAt(time,view);
    const int parentInput=(geometryLeaf(_kind) || _kind==Scene3DKind::TransformGeo)?1:0;
    if(auto parent=input3D(this,parentInput))object.editParentTransform=parent->transformAt(time,view);
    if(cameraKind(_kind) || _kind==Scene3DKind::SceneCamera)object.camera=cameraAt(time,view);
    if(_kind==Scene3DKind::Light)object.light=lightAt(time,view);
    return object;
}
void NativeScene3D::appendParentObjects(Scene3DSnapshot& scene,double time,ViewIdx view,bool worldSpace) const {
    if(_kind==Scene3DKind::SceneCamera)return;
    const int parentInput=(geometryLeaf(_kind) || _kind==Scene3DKind::TransformGeo)?1:0;
    auto parent=input3D(this,parentInput);
    for(int i=0;parent && i<1024;++i) {
        auto object=parent->editorObjectAt(time,view);object.worldSpace=worldSpace;scene.objects.push_back(object);
        parent=input3D(parent,0);
    }
}
Scene3DSnapshot NativeScene3D::sceneAt(double time,ViewIdx view,bool preserveUsd) const {
    Scene3DSnapshot scene;
    if(_kind==Scene3DKind::ShadowCatcher || _kind==Scene3DKind::ReflectionCatcher || _kind==Scene3DKind::Occluder){
        auto input=input3D(this,0);if(!input)throw std::runtime_error("Connect proxy geometry to the compositing node");
        scene=input->sceneAt(time,view,preserveUsd);const auto comp=compositingAt(time,view);
        for(auto& item:scene.items)item.card.compositing=comp;
        if(!scene.usdSources.empty()){
            Gpu::Cards::UsdSceneBridge bridge;Gpu::Cards::Settings settings;bridge.exportScene({},scene.camera,settings,scene.usdSources);
            auto value=bridge.sceneValue();const auto roots=value->children().prims;
            Gpu::Cards::UsdPrimEdit edit;edit.kind=Gpu::Cards::UsdPrimEdit::Kind::Compositing;edit.identity=sceneId();
            edit.cameraVisible=comp.cameraVisible;edit.shadowVisible=comp.shadowVisible;edit.reflectionVisible=comp.reflectionVisible;edit.holdout=comp.holdout;edit.role=comp.role;edit.shadowDensity=comp.shadowDensity;edit.shadowTint=comp.shadowTint;edit.reflectionStrength=comp.reflectionStrength;edit.reflectionRoughness=comp.reflectionRoughness;
            for(const auto& source:scene.usdSources){if(!source.mountPath.empty())edit.paths.push_back(source.mountPath);else for(const auto& root:roots)if(root.ownerId==source.ownerId)edit.paths.push_back(root.path);}
            if(!edit.paths.empty())scene.usdEdits.push_back(std::move(edit));
        }return scene;
    }
    if(_kind==Scene3DKind::SceneSetup)return layoutSceneAt(time,view,preserveUsd);
    if(_kind==Scene3DKind::ReconstructionScene){
        auto tracker=std::dynamic_pointer_cast<CameraTracker>(resolve(getInput(0)));
        if(!tracker)throw std::runtime_error("ReconstructionScene needs a CameraTracker input");
        const auto result=tracker->reconstruction();if(!result)throw std::runtime_error("CameraTracker has no accepted solve");
        Gpu::Cards::UsdSceneSource source;source.filename=tracker->usdAssetPath();source.ownerId=sceneId();source.label=getNode()->getLabel();
        source.mountPath="/Natron/Assets/n_"+sceneId();source.timeCode=time;source.timeCodesPerSecond=getApp()->getProjectFrameRate();
        source.revision=std::hash<std::string>{}(source.filename);scene.usdSources.push_back(std::move(source));
        scene.reconstructions.push_back({getNode(),result,QFileInfo(QString::fromStdString(tracker->usdAssetPath())).completeBaseName().toStdString()});
        scene.reconstructions.back().imagePixelAspect=tracker->getAspectRatio(-1);
        for(const auto& point:result->points)scene.guidePoints.push_back({{point.position[0],point.position[1],point.position[2]},point.trackId,getNode(),getNode(),point.rmsPixels});
        return scene;
    }
    if(_kind==Scene3DKind::Render3D) {
        auto geometry=input3D(this,0);auto camera=input3D(this,1);
        if(!geometry || (!camera && _usdCamera.lock()->getValue(0,view).empty()))throw std::runtime_error("Render3D requires a Scene and a Camera input or USD render camera path.");
        scene=geometry->sceneAt(time,view,preserveUsd || rendererBackendAt(time,view)==3);
        if(camera) {
            scene.camera=camera->cameraAt(time,view);scene.camera.ownerId=camera->sceneId();scene.camera.label=camera->getNode()->getLabel();
            auto object=camera->editorObjectAt(time,view);object.worldSpace=true;object.renderCamera=_usdCamera.lock()->getValue(0,view).empty() || _usdCamera.lock()->getValue(0,view)=="/Natron/Camera";
            Format format;getApp()->getProject()->getProjectDefaultFormat(&format);
            object.cameraAspect=double(format.width())*format.getPixelAspectRatio()/std::max(1,format.height());
            scene.objects.push_back(object);camera->appendParentObjects(scene,time,view,true);
        }
        // Shared axes/projectors may be reached through several cards. Keep
        // distinct transformed instances and distinct projection sample times.
        std::vector<Scene3DObject> unique;
        for(const auto& candidate:scene.objects) {
            auto duplicate=std::find_if(unique.begin(),unique.end(),[&](const Scene3DObject& other){
                return candidate.itemIndex<0 && other.owner==candidate.owner && other.time==candidate.time &&
                    other.transform.m==candidate.transform.m && other.worldSpace==candidate.worldSpace && other.cameraAspect==candidate.cameraAspect && other.primPaths==candidate.primPaths;
            });
            if(duplicate==unique.end())unique.push_back(candidate);
            else {duplicate->renderCamera |= candidate.renderCamera;duplicate->projectionCamera |= candidate.projectionCamera;}
        }
        scene.objects=std::move(unique);
        const auto path=_usdCamera.lock()->getValue(0,view);
        if(!path.empty() && path!="/Natron/Camera") {
            if(!preserveUsd && rendererBackendAt(time,view)!=3)throw std::runtime_error("A composed USD render camera requires USD / hdNVP.");
            Gpu::Cards::UsdSceneBridge bridge;Gpu::Cards::Settings settings;settings.lights=scene.lights;settings.edits=scene.usdEdits;
            std::vector<Gpu::Cards::Card> cards;for(const auto& item:scene.items)cards.push_back(item.card);
            bridge.exportScene(cards,scene.camera,settings,scene.usdSources);scene.camera=bridge.sceneValue()->camera(path);
        }
    } else if(_kind==Scene3DKind::Scene) {
        for(int i=0;i<getNInputs();++i)if(auto child=input3D(this,i)) {
            auto part=child->sceneAt(time,view,preserveUsd);const int offset=int(scene.items.size());
            scene.guidePoints.insert(scene.guidePoints.end(),part.guidePoints.begin(),part.guidePoints.end());
            scene.reconstructions.insert(scene.reconstructions.end(),part.reconstructions.begin(),part.reconstructions.end());
            scene.usdSources.insert(scene.usdSources.end(),part.usdSources.begin(),part.usdSources.end());
            // Input zero is the strongest branch, matching USD source arcs.
            scene.usdEdits.insert(scene.usdEdits.begin(),part.usdEdits.begin(),part.usdEdits.end());
            scene.items.insert(scene.items.end(),part.items.begin(),part.items.end());
            const size_t lightOffset=scene.lights.size();
            scene.lights.insert(scene.lights.end(),part.lights.begin(),part.lights.end());
            for(auto source:part.environmentSources){source.lightIndex+=lightOffset;scene.environmentSources.push_back(std::move(source));}
            for(auto& object:part.objects){if(object.itemIndex>=0)object.itemIndex+=offset;scene.objects.push_back(object);}
        }
    } else if(usdEditKind(_kind)) {
        if(!preserveUsd)throw std::runtime_error("USD edit nodes require the USD / hdNVP renderer.");
        if(auto child=input3D(this,0))scene=child->sceneAt(time,view,true);else throw std::runtime_error("USD edit requires a scene input.");
        const auto paths=targetPaths(_usdTargets,view);
        if(_kind==Scene3DKind::BindMaterial) {
            Scene3DItem item;item.owner=getNode();item.card.ownerId=sceneId();item.card.label=getNode()->getLabel();item.card.bindingPaths=paths;
            item.imageSource=resolve(getInput(1));item.imageTime=item.mapTime=time;
            if(!item.imageSource)throw std::runtime_error("BindMaterial requires an image or material input.");
            auto material=dynamic_cast<NativeScene3D*>(item.imageSource.get());
            if(material && material->kind()==Scene3DKind::PBRMaterial) {
                item.card.material=material->materialAt(time,view);item.card.overrideMaterial=true;
                for(int j=0;j<3;++j){auto& map=item.mapSources[j];map.source=resolve(material->getInput(j+1));map.channel=j==0?0u:unsigned(sceneValueAt((j==1?material->_roughnessChannel:material->_metallicChannel).lock(),time,0,view));map.normalEncoding=j==0?sceneValueAt(material->_normalEncoding.lock(),time,0,view):-1;map.flipY=j==0 && sceneValueAt(material->_normalFlipY.lock(),time,0,view);map.wrap=Gpu::Cards::TextureAddress(sceneValueAt(material->_mapWrap.lock(),time,0,view));}
                for(int i=0;material && material->kind()==Scene3DKind::PBRMaterial && i<1024;++i){item.imageSource=resolve(material->getInput(0));material=dynamic_cast<NativeScene3D*>(item.imageSource.get());}
            }
            if(material && material->kind()==Scene3DKind::Project3D) {
                const double projectionTime=sceneValueAt(material->_freeze.lock(),time,0,view)?sceneValueAt(material->_reference.lock(),time,0,view):time;
                auto projector=input3D(material,1);if(!projector)throw std::runtime_error("Project3D requires a projection Camera.");
                item.card.projected=true;item.card.projector=projector->cameraAt(projectionTime,view);
                const int timing=sceneValueAt(material->_imageTiming.lock(),time,0,view);const bool frozen=timing==1 || (timing==0 && sceneValueAt(material->_freeze.lock(),time,0,view));
                item.imageSource=resolve(material->getInput(0));item.imageTime=frozen?sceneValueAt(material->_reference.lock(),time,0,view):time;
                item.card.projectionFrozen=sceneValueAt(material->_freeze.lock(),time,0,view);item.card.projectionReferenceTime=sceneValueAt(material->_reference.lock(),time,0,view);item.card.projectionImageTime=item.imageTime;
            } else if(material && material->getOutputDataType()!=NodeDataType::Image)throw std::runtime_error("BindMaterial requires an image, Project3D or PBRMaterial.");
            if(item.imageSource){const auto format=item.imageSource->getOutputFormat();if(format.height()>0)item.card.projectionAspect=double(format.width())*item.imageSource->getAspectRatio(-1)/format.height();}
            scene.items.push_back(std::move(item));
            Gpu::Cards::UsdPrimEdit binding;binding.kind=Gpu::Cards::UsdPrimEdit::Kind::Material;binding.identity=sceneId();binding.paths=paths;binding.materialPath="/Natron/Materials/n_"+sceneId();scene.usdEdits.push_back(std::move(binding));
        } else {
            Gpu::Cards::UsdPrimEdit edit;edit.identity=sceneId();edit.paths=paths;
            if(_kind==Scene3DKind::USDTransform){
                edit.kind=Gpu::Cards::UsdPrimEdit::Kind::Transform;const auto pivot=value3(_usdPivot,time,view);
                edit.transform=Matrix::transform(value3(_translate,time,view)+pivot,value3(_rotate,time,view),value3(_scale,time,view,{1,1,1}))*Matrix::transform(pivot*(-1),{});
                scene.objects.push_back(editorObjectAt(time,view));
            }
            else if(_kind==Scene3DKind::USDVisibility){edit.kind=Gpu::Cards::UsdPrimEdit::Kind::Visibility;edit.visible=sceneValueAt(_visible.lock(),time,0,view);}
            else {const auto comp=compositingAt(time,view);edit.kind=Gpu::Cards::UsdPrimEdit::Kind::Compositing;edit.cameraVisible=comp.cameraVisible;edit.shadowVisible=comp.shadowVisible;edit.reflectionVisible=comp.reflectionVisible;edit.holdout=comp.holdout;edit.role=comp.role;edit.shadowDensity=comp.shadowDensity;edit.shadowTint=comp.shadowTint;edit.reflectionStrength=comp.reflectionStrength;edit.reflectionRoughness=comp.reflectionRoughness;}
            scene.usdEdits.push_back(std::move(edit));
        }
    } else if(_kind==Scene3DKind::TransformGeo) {
        if(auto child=input3D(this,0))scene=child->sceneAt(time,view,preserveUsd);
        const Matrix matrix=transformAt(time,view);
        for(auto& edit:scene.usdEdits)if(edit.kind==Gpu::Cards::UsdPrimEdit::Kind::Transform)edit.transform=matrix*edit.transform*matrix.inverse();
        for(auto& source:scene.usdSources) {
            if(source.mountPath.empty() && matrix.m!=Matrix().m)throw std::runtime_error("TransformGeo on a complete shot requires namespace normalization; choose Asset reference with a dependency-complete Prim subtree.");
            source.transform=matrix*source.transform;
            if(!source.mountPath.empty()) {
                const auto previous=source.mountPath;source.mountPath+="_t_"+sceneId();
                auto remap=[&](std::string& path){if(path==previous || path.compare(0,previous.size()+1,previous+"/")==0)path.replace(0,previous.size(),source.mountPath);};
                for(auto& edit:scene.usdEdits)for(auto& path:edit.paths)remap(path);
                for(auto& object:scene.objects)for(auto& path:object.primPaths)remap(path);
                for(auto& item:scene.items)for(auto& path:item.card.bindingPaths)remap(path);
            }
        }
        for(auto& point:scene.guidePoints)point.position=matrix.point(point.position);
        for(auto& light:scene.lights){light.transform=matrix*light.transform;light.instanceId=(light.instanceId.empty()?light.ownerId:light.instanceId)+"_t_"+sceneId();}
        for(auto& item:scene.items) {
            item.card.transform=matrix*item.card.transform;
            const auto previous="/Natron/Geometry/n_"+(item.card.instanceId.empty()?item.card.ownerId:item.card.instanceId);
            item.card.instanceId=(item.card.instanceId.empty()?item.card.ownerId:item.card.instanceId)+"_t_"+sceneId();
            const auto renamed="/Natron/Geometry/n_"+item.card.instanceId;
            for(auto& edit:scene.usdEdits)for(auto& path:edit.paths)if(path==previous || path.compare(0,previous.size()+1,previous+"/")==0)path.replace(0,previous.size(),renamed);
            item.editParentTransform=matrix*item.editParentTransform;
        }
        for(auto& object:scene.objects)if(!object.worldSpace) {
            object.transform=matrix*object.transform;object.editParentTransform=matrix*object.editParentTransform;
            if(object.kind==Scene3DKind::Light)object.light.transform=object.transform;
        }
        scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);
    } else if(_kind==Scene3DKind::Axis) {
        scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);
    } else if(_kind==Scene3DKind::Light) {
        scene.lights.push_back(lightAt(time,view));scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);
        scene.lights.back().ownerId=sceneId();scene.lights.back().label=getNode()->getLabel();
        if(scene.lights[0].type==Gpu::Cards::LightType::Environment)if(auto source=resolve(getInput(1))) {
            if(source->getOutputDataType()!=NodeDataType::Image)throw std::runtime_error("Environment light requires an image input.");
            scene.environmentSources.push_back({0,source,time});
        }
    } else if(geometryLeaf(_kind)) {
        std::vector<Gpu::Cards::Card> sourceCards;
        const Matrix matrix=transformAt(time,view);
        if(_kind==Scene3DKind::ReadUSD && preserveUsd) {
            const auto subtree=_primPath.lock()->getValue(0,view);
            const bool assetMode=_usdAssemblyMode.lock()->getValue(0,view)==1;
            if(sceneValueAt(_opacity.lock(),time,0,view)!=1 || resolve(getInput(0)))
                throw std::runtime_error("ReadUSD opacity and material overrides need the USD edit adapter. Keep the source materials for now.");
            if(!assetMode && (matrix.m!=Matrix().m || (!subtree.empty() && subtree!="/")))
                throw std::runtime_error("ReadUSD transforms and subtree selection need Asset reference mode with a dependency-complete root. Shot layers preserve the original paths.");
            const auto asset=usdAssetAt(view);Gpu::Cards::UsdSceneSource source;
            source.filename=_file.lock()->getValue(0,view);getApp()->getProject()->canonicalizePath(source.filename);
            source.variants=_variants.lock()->getValue(0,view);source.loadPayloads=_loadPayloads.lock()->getValue(0,view);
            source.timeCode=asset->startTime()+(_readAnimation.lock()->getValue(0,view)?fbxSecondsAt(time,view)*asset->timeCodesPerSecond():0);
            source.metersPerUnit=asset->metersPerUnit();source.zUp=asset->zUp();source.timeCodesPerSecond=asset->timeCodesPerSecond();
            source.revision=asset->revision();
            source.ownerId=sceneId();source.label=getNode()->getLabel();source.transform=matrix;
            if(assetMode){source.mountPath="/Natron/Assets/n_"+sceneId();source.primPath=subtree=="/"?std::string():subtree;}
            scene.usdSources.push_back(std::move(source));scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);return scene;
        }
        if(_kind==Scene3DKind::ReadUSD) {
            const auto pose=usdPoseAt(time,view);sourceCards=pose->cards;
            for(auto light:pose->lights){light.transform=matrix*light.transform;scene.lights.push_back(light);}
        }else sourceCards.emplace_back();
        size_t sourceIndex=0;for(const auto& sourceCard:sourceCards) {
        Scene3DItem item;item.owner=getNode();item.card=sourceCard;item.card.transform=matrix*item.card.transform;item.imageTime=time;
        item.card.ownerId=sceneId();item.card.label=getNode()->getLabel();
        if(sourceCards.size()>1)item.card.instanceId=sceneId()+"_part_"+std::to_string(sourceIndex);++sourceIndex;
        if(auto parent=input3D(this,1))item.editParentTransform=parent->transformAt(time,view);
        if(_kind==Scene3DKind::ReadGeo)item.card.mesh=meshAt(time,view);
        else if(_kind==Scene3DKind::Card) {item.card.width=sceneValueAt(_size.lock(),time,0,view);item.card.height=sceneValueAt(_size.lock(),time,1,view);}
        item.card.opacity=sceneValueAt(_opacity.lock(),time,0,view);item.imageSource=resolve(getInput(0));
        // An explicit image/material assignment replaces imported texture
        // bindings, including an untextured native PBRMaterial override.
        if(item.imageSource){item.card.texture.reset();item.card.maps={};}
        auto material=dynamic_cast<NativeScene3D*>(item.imageSource.get());
        if(material && material->kind()==Scene3DKind::PBRMaterial) {
            item.card.material=material->materialAt(time,view);item.card.overrideMaterial=true;item.mapTime=time;
            // The nearest material owns all surface controls and map inputs.
            for(int j=0;j<3;++j) {
                auto& map=item.mapSources[j];map.source=resolve(material->getInput(j+1));
                if(map.source && map.source->getOutputDataType()!=NodeDataType::Image)throw std::runtime_error("PBRMaterial data maps require image inputs.");
                map.channel=j==0?0u:unsigned(sceneValueAt((j==1?material->_roughnessChannel:material->_metallicChannel).lock(),time,0,view));
                map.normalEncoding=j==0?sceneValueAt(material->_normalEncoding.lock(),time,0,view):-1;
                map.flipY=j==0 && sceneValueAt(material->_normalFlipY.lock(),time,0,view);
                map.wrap=Gpu::Cards::TextureAddress(sceneValueAt(material->_mapWrap.lock(),time,0,view));
            }
            // The material nearest geometry supplies the surface; unwrap any
            // upstream material overrides to find its image or projection.
            for(int i=0;material && material->kind()==Scene3DKind::PBRMaterial && i<1024;++i) {
                item.imageSource=resolve(material->getInput(0));material=dynamic_cast<NativeScene3D*>(item.imageSource.get());
            }
        }
        NativeScene3D* projector=nullptr;double projectionTime=time;
        if(material && material->kind()==Scene3DKind::Project3D) {
            projectionTime=sceneValueAt(material->_freeze.lock(),time,0,view)?sceneValueAt(material->_reference.lock(),time,0,view):time;
            projector=input3D(material,1);
            if(!projector)throw std::runtime_error("Project3D requires a projection Camera.");
            item.card.projected=true;item.card.projector=projector->cameraAt(projectionTime,view);
            const int timing=sceneValueAt(material->_imageTiming.lock(),time,0,view);const bool frozen=timing==1 || (timing==0 && sceneValueAt(material->_freeze.lock(),time,0,view));
            item.imageSource=resolve(material->getInput(0));item.imageTime=frozen?sceneValueAt(material->_reference.lock(),time,0,view):time;
            item.card.projectionFrozen=sceneValueAt(material->_freeze.lock(),time,0,view);item.card.projectionReferenceTime=sceneValueAt(material->_reference.lock(),time,0,view);item.card.projectionImageTime=item.imageTime;
        }
        else if(material && material->getOutputDataType()!=NodeDataType::Image)
            throw std::runtime_error("Geometry requires an image or Project3D material.");
        if(item.imageSource) {
            const RectI format=item.imageSource->getOutputFormat();
            if(format.height()>0)item.card.projectionAspect=double(format.width())*item.imageSource->getAspectRatio(-1)/format.height();
        }
        scene.items.push_back(item);
        auto object=editorObjectAt(time,view);object.itemIndex=int(scene.items.size())-1;scene.objects.push_back(object);appendParentObjects(scene,time,view);
        if(projector) {
            auto projection=projector->editorObjectAt(projectionTime,view);projection.worldSpace=true;projection.projectionCamera=true;
            projection.cameraAspect=item.card.projectionAspect;scene.objects.push_back(projection);
            projector->appendParentObjects(scene,projectionTime,view,true);
        }
        }
        if(sourceCards.empty()) {scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);}
    }
    return scene;
}
void NativeScene3D::textureLights(Scene3DSnapshot& scene,ViewIdx view,bool draft) const {
    std::map<std::pair<EffectInstancePtr,double>,std::shared_ptr<const Gpu::Cards::Texture>> images;
    for(const auto& binding:scene.environmentSources) {
        const auto key=std::make_pair(binding.source,binding.time);auto found=images.find(key);
        if(found==images.end()) {
            Scene3DMapSource source;source.source=binding.source;
            auto texture=fetchTexture(binding.source,binding.time,view,draft,&source);
            if(!texture)throw std::runtime_error("Cannot fetch environment light image.");
            texture->wrapS=Gpu::Cards::TextureAddress::Repeat;texture->wrapT=Gpu::Cards::TextureAddress::Clamp;
            found=images.emplace(key,texture).first;
        }
        scene.lights.at(binding.lightIndex).environment=found->second;
    }
}
std::vector<Gpu::Cards::Card> NativeScene3D::texturedScene(const Scene3DSnapshot& scene,ViewIdx view,bool draft) const {
    std::vector<Gpu::Cards::Card> cards;cards.reserve(scene.items.size());
    std::map<std::pair<EffectInstancePtr,double>,std::shared_ptr<Gpu::Cards::Texture>> textures;
    using MapKey=std::tuple<EffectInstancePtr,double,int,bool,Gpu::Cards::TextureAddress>;
    std::map<MapKey,std::shared_ptr<Gpu::Cards::Texture>> dataTextures;
    for(const auto& item:scene.items) {
        auto card=item.card;
        if(item.imageSource) {
            const auto key=std::make_pair(item.imageSource,item.imageTime);
            auto found=textures.find(key);
            if(found==textures.end())found=textures.emplace(key,fetchTexture(item.imageSource,item.imageTime,view,draft)).first;
            card.texture=found->second;
        }
        for(int j=0;j<3;++j) {
            const auto& map=item.mapSources[j];if(!map.source)continue;
            const MapKey key{map.source,item.mapTime,map.normalEncoding,map.flipY,map.wrap};
            auto found=dataTextures.find(key);
            if(found==dataTextures.end())found=dataTextures.emplace(key,fetchTexture(map.source,item.mapTime,view,draft,&map)).first;
            card.maps[j]={found->second,map.channel,true};
        }
        cards.push_back(std::move(card));
    }
    return cards;
}
StatusEnum NativeScene3D::getRegionOfDefinition(U64,double,const RenderScale&,ViewIdx,RectD* rod) {
    Format format;getApp()->getProject()->getProjectDefaultFormat(&format);*rod=format.toCanonicalFormat();return eStatusOK;
}
StatusEnum NativeScene3D::getPreferredMetadata(NodeMetadata& metadata) {
    Format format;getApp()->getProject()->getProjectDefaultFormat(&format);
    metadata.setOutputFormat(format);metadata.setPixelAspectRatio(-1,format.getPixelAspectRatio());
    metadata.setOutputPremult(eImagePremultiplicationPremultiplied);metadata.setNComps(-1,4);metadata.setBitDepth(-1,eImageBitDepthFloat);
    metadata.setIsFrameVarying(true);return eStatusOK;
}
StatusEnum NativeScene3D::render(const RenderActionArgs& args) {
    if(_kind!=Scene3DKind::Render3D)return eStatusFailed;
    const bool trace=qEnvironmentVariableIsSet("NATRON_HDNVP_TRACE");
    if(trace)fprintf(stderr,"Natron Render3D begin: time=%g planes=%zu aborted=%d\n",args.time,args.outputPlanes.size(),int(aborted()));
    if(aborted())return eStatusOK;
    try {
        // A Reader may adopt the project format on its first pixel fetch.
        // Keep the format that defined this request's output planes, so that
        // lazy texture evaluation cannot leave part of those planes unwritten.
        Format format;getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectI output=format.toCanonicalFormat().toPixelEnclosing(args.mappedScale.toMipmapLevel(),format.getPixelAspectRatio());
        auto scene=sceneAt(args.time,args.view);textureLights(scene,args.view,args.draftMode);auto cards=texturedScene(scene,args.view,args.draftMode);
        Gpu::Cards::Settings settings;settings.width=output.width();settings.height=output.height();settings.pixelAspect=format.getPixelAspectRatio();
        settings.lights=scene.lights;
        settings.edits=scene.usdEdits;
        settings.transparentBackground=bool(getInput(2));
        settings.samples=1<<sceneValueAt(_samples.lock(),args.time,0,args.view);
        const int backend=sceneValueAt(_backend.lock(),args.time,0,args.view);
        std::shared_ptr<const Gpu::Cards::HdNvpFrame> completed;
        if(backend==3) {
            if(!_usdBridge)_usdBridge=std::make_unique<Gpu::Cards::UsdSceneBridge>();
            // Natron can request beauty and utility planes separately. Reuse
            // the same converged GPU frame rather than path tracing each AOV.
            std::ostringstream key;key.precision(17);key<<getNode()->getHashValue()<<'/'<<args.time<<'/'<<args.view.value()<<'/'<<args.draftMode<<'/'<<output.width()<<'/'<<output.height()<<'/'<<format.getPixelAspectRatio();
            for(const auto& source:scene.usdSources)key<<'/'<<source.filename<<'/'<<source.revision<<'/'<<source.timeCode;
            if(!_usdFrame || _usdFrameKey!=key.str()) {
                auto frame=std::make_shared<Gpu::Cards::HdNvpFrame>(_usdBridge->render(cards,scene.camera,settings,hdNvpSettingsAt(args.time,args.view,args.draftMode),[this]{return aborted();},scene.usdSources));
                if(aborted())return eStatusOK;
                _usdFrame=std::move(frame);_usdFrameKey=key.str();
            }
            completed=_usdFrame;
        }
        const Gpu::Cards::HdNvpFrame emptyFrame;const auto& usdFrame=completed?*completed:emptyFrame;
        auto renderPass=[&](Gpu::Cards::RenderPass pass) {
            settings.pass=pass;std::vector<float> pixels;
            if(backend==3) {
                if(pass==Gpu::Cards::RenderPass::Color)return usdFrame.color;
                if(pass==Gpu::Cards::RenderPass::Shadow)return usdFrame.shadow.empty()?std::vector<float>(size_t(settings.width)*settings.height*4):usdFrame.shadow;
                if(pass==Gpu::Cards::RenderPass::Reflection)return usdFrame.reflection.empty()?std::vector<float>(size_t(settings.width)*settings.height*4):usdFrame.reflection;
                if(pass==Gpu::Cards::RenderPass::Occlusion){pixels.resize(size_t(settings.width)*settings.height*4);for(size_t i=0;i<usdFrame.occlusion.size();++i)for(int c=0;c<4;++c)pixels[i*4+c]=usdFrame.occlusion[i];return pixels;}
                pixels.resize(usdFrame.depth.size()*4);
                for(size_t i=0;i<usdFrame.depth.size();++i){for(int c=0;c<3;++c)pixels[i*4+c]=usdFrame.depth[i];pixels[i*4+3]=usdFrame.depth[i]>0?1.f:0.f;}
                return pixels;
            }
            if(pass==Gpu::Cards::RenderPass::Shadow && std::none_of(cards.begin(),cards.end(),[](const auto& c){return Gpu::Cards::catchesShadows(c.compositing.role);}))return std::vector<float>(size_t(settings.width)*settings.height*4);
            if(pass==Gpu::Cards::RenderPass::Occlusion && std::none_of(cards.begin(),cards.end(),[](const auto& c){return c.compositing.role==Gpu::Cards::GeometryRole::Occluder || c.compositing.holdout;}))return std::vector<float>(size_t(settings.width)*settings.height*4);
            if(pass==Gpu::Cards::RenderPass::Reflection){if(std::none_of(cards.begin(),cards.end(),[](const auto& c){return Gpu::Cards::catchesReflections(c.compositing.role) && c.compositing.reflectionStrength>0;}))return std::vector<float>(size_t(settings.width)*settings.height*4);return Gpu::Cards::renderCpu(cards,scene.camera,settings,[this]{return aborted();});}
            if(backend!=1) {
                try {
                    const auto context=sharedVulkanImageContext();
                    if(!context)throw std::runtime_error("Vulkan is unavailable.");
                    if(!_renderer || _rendererDevice!=context->device()) {
                        _renderer.reset();_rendererDevice=context->device();_renderer=std::make_unique<Gpu::Cards::Renderer>(_rendererDevice);
                    }
                    pixels=_renderer->render(cards,scene.camera,settings);
                }catch(const std::exception&) {if(backend==2)throw;}
            }
            if(pixels.empty())pixels=Gpu::Cards::renderCpu(cards,scene.camera,settings,[this]{return aborted();});
            return pixels;
        };
        const auto displayPass=displayPassAt(args.time,args.view);const bool preview=displayPass!=Gpu::Cards::RenderPass::Color;
        const bool needColor=std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first.isColorPlane();});
        const bool needDepth=(needColor && displayPass==Gpu::Cards::RenderPass::Depth) || std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first==depthPlane();});
        const bool needShadow=(needColor && (displayPass==Gpu::Cards::RenderPass::Color || displayPass==Gpu::Cards::RenderPass::Shadow)) || std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first==shadowPlane();});
        const bool needOcclusion=(needColor && displayPass==Gpu::Cards::RenderPass::Occlusion) || std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first==occlusionPlane();});
        const bool needReflection=(needColor && (displayPass==Gpu::Cards::RenderPass::Color || displayPass==Gpu::Cards::RenderPass::Reflection)) || std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first==reflectionPlane();});
        std::vector<float> pixels,depth,shadow,occlusion,reflection;
        if(needDepth)depth=renderPass(Gpu::Cards::RenderPass::Depth);
        if(needShadow)shadow=renderPass(Gpu::Cards::RenderPass::Shadow);
        if(needOcclusion)occlusion=renderPass(Gpu::Cards::RenderPass::Occlusion);
        if(needReflection)reflection=renderPass(Gpu::Cards::RenderPass::Reflection);
        if(needColor) {
            if(displayPass==Gpu::Cards::RenderPass::Depth) {
                pixels=depth;const auto range=depthPreviewRangeAt(args.time,args.view);
                Gpu::Cards::makeDepthPreview(pixels,range[0],range[1]);
            }else if(displayPass==Gpu::Cards::RenderPass::Reflection){pixels=reflection;Gpu::Cards::makeReflectionPreview(pixels);}
            else if(preview){pixels=displayPass==Gpu::Cards::RenderPass::Shadow?shadow:occlusion;for(size_t i=0;i<pixels.size();i+=4){const float a=displayPass==Gpu::Cards::RenderPass::Shadow?pixels[i+3]:pixels[i];for(int c=0;c<3;++c)pixels[i+c]=a;pixels[i+3]=1;}}
            else pixels=renderPass(Gpu::Cards::RenderPass::Color);
        }
        if(needColor && !preview){const auto plate=backgroundAt(args.time,args.view,args.draftMode);Gpu::Cards::compositePlate(pixels,shadow,plate.get(),settings.width,settings.height,reflection);}
        if(aborted())return eStatusOK;
        for(const auto& plane:args.outputPlanes) {
            const auto image=plane.second;Image::WriteAccess write(image.get());
            const bool z=plane.first==depthPlane();const unsigned channels=image->getComponentsCount();
            const RectI roi=args.roi.intersect(output).intersect(write.bounds());
            for(int y=roi.y1;y<roi.y2;++y)for(int x=roi.x1;x<roi.x2;++x) {
                float* to=reinterpret_cast<float*>(write.pixelAt(x,y));
                const size_t at=((size_t(y-output.y1)*settings.width)+(x-output.x1))*4;
                if(z)to[0]=depth[at];
                else if(plane.first==shadowPlane())std::copy_n(shadow.data()+at,std::min(channels,4u),to);
                else if(plane.first==occlusionPlane())to[0]=occlusion[at];
                else if(plane.first==reflectionPlane())std::copy_n(reflection.data()+at,std::min(channels,3u),to);
                else if(plane.first==albedoPlane() || plane.first==normalPlane() || plane.first==positionPlane()) {
                    if(backend!=3)throw std::runtime_error("Albedo, N and P outputs require USD / hdNVP.");
                    const auto& values=plane.first==albedoPlane()?usdFrame.albedo:plane.first==normalPlane()?usdFrame.normal:usdFrame.position;
                    std::copy_n(values.data()+at/4*3,std::min(channels,3u),to);
                } else if(plane.first.isColorPlane()) {
                    const float* from=pixels.data()+at;
                    if(channels==1)to[0]=from[3];else std::copy_n(from,std::min(channels,4u),to);
                }
            }
        }
        if(trace)fprintf(stderr,"Natron Render3D completed: time=%g aborted=%d\n",args.time,int(aborted()));
        return eStatusOK;
    }catch(const std::exception& e) {
        if(trace)fprintf(stderr,"Natron Render3D stopped: time=%g aborted=%d reason=%s\n",args.time,int(aborted()),e.what());
        // The render wrapper maps OK + its abort flag to an interrupted render.
        // Returning Failed or posting a message here makes a superseded Viewer
        // request appear as a persistent Render3D error.
        if(aborted())return eStatusOK;
        setPersistentMessage(eMessageTypeError,e.what());return eStatusFailed;
    }
}
NATRON_NAMESPACE_EXIT
