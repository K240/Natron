// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/NativeScene3D.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <tuple>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <QDateTime>
#include <QFileInfo>
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
const char* names[]={"Card","Camera","Axis","TransformGeo","Scene","Project3D","Render3D","ReadGeo","ReadCamera","Light","PBRMaterial","ReadUSD","ReadUSDCamera"};
bool geometryLeaf(Scene3DKind kind){return kind==Scene3DKind::Card || kind==Scene3DKind::ReadGeo || kind==Scene3DKind::ReadUSD;}
bool cameraKind(Scene3DKind kind){return kind==Scene3DKind::Camera || kind==Scene3DKind::ReadCamera || kind==Scene3DKind::ReadUSDCamera;}
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
    return knob?Vec3{knob->getValueAtTime(time,0,view),knob->getValueAtTime(time,1,view),knob->getValueAtTime(time,2,view)}:fallback;
}
Vec3 color3(const KnobColorWPtr& weak,double time,ViewIdx view) {
    const auto knob=weak.lock();return {knob->getValueAtTime(time,0,view),knob->getValueAtTime(time,1,view),knob->getValueAtTime(time,2,view)};
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
    default:return "Render geometry through a Camera into linear premultiplied RGBA and a float Depth.Z layer at the project format. Depth is the nearest visible surface's camera-space -Z, with zero for empty pixels. Auto uses Vulkan for up to 16 Card/ReadGeo objects within the device's mesh-buffer limits, and falls back to CPU for larger scenes or unavailable hardware. Connect to a Viewer and select 3D to edit the scene.";
    }
}
void NativeScene3D::getPluginGrouping(std::list<std::string>* groups) const {groups->push_back("3D");}
int NativeScene3D::getNInputs() const {
    switch(_kind) {
    case Scene3DKind::Axis:case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:return 1;
    case Scene3DKind::Light:return 2;
    case Scene3DKind::PBRMaterial:return 4;
    case Scene3DKind::Scene:return 16;
    case Scene3DKind::Render3D:return 3;
    default:return 2;
    }
}
bool NativeScene3D::isInputOptional(int input) const {return _kind!=Scene3DKind::Render3D || input==2;}
std::string NativeScene3D::getInputLabel(int input) const {
    switch(_kind) {
    case Scene3DKind::ReadUSD:case Scene3DKind::Card:case Scene3DKind::ReadGeo:return input==0?"image / material":"axis";
    case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:case Scene3DKind::Axis:return "axis";
    case Scene3DKind::Light:return input==0?"axis":"environment image";
    case Scene3DKind::PBRMaterial:return input==0?"image / material":input==1?"normal":input==2?"roughness":"metallic";
    case Scene3DKind::TransformGeo:return input==0?"geo / scene":"axis";
    case Scene3DKind::Project3D:return input==0?"image":"projection camera";
    case Scene3DKind::Render3D:return input==0?"scene":input==1?"camera":"background";
    default:return "object "+std::to_string(input+1);
    }
}
void NativeScene3D::addAcceptedComponents(int input,std::list<ImagePlaneDesc>* comps) {
    comps->push_back(ImagePlaneDesc::getRGBAComponents());
    if(input==-1 && _kind==Scene3DKind::Render3D)comps->push_back(depthPlane());
}
const ImagePlaneDesc& NativeScene3D::depthPlane() {
    static const ImagePlaneDesc plane("Depth","Depth","Z",std::vector<std::string>{"Z"});return plane;
}
void NativeScene3D::getComponentsNeededAndProduced(double time,ViewIdx view,ComponentsNeededMap* comps,
                                                   double* passTime,int* passView,int* passInput) {
    if(_kind!=Scene3DKind::Render3D){EffectInstance::getComponentsNeededAndProduced(time,view,comps,passTime,passView,passInput);return;}
    *passTime=time;*passView=view.value();*passInput=-1;
    (*comps)[-1]={ImagePlaneDesc::getRGBAComponents(),depthPlane()};
    // Geometry and camera inputs are traversed by sceneAt, not image rendering.
    if(getInput(2))(*comps)[2]={ImagePlaneDesc::getRGBAComponents()};
}
bool NativeScene3D::depthPreviewAt(double time,ViewIdx view) const {
    const auto knob=_output.lock();return knob && knob->getValueAtTime(time,0,view)==1;
}
std::array<double,2> NativeScene3D::depthPreviewRangeAt(double time,ViewIdx view) const {
    const auto knob=_depthRange.lock();return knob?std::array<double,2>{knob->getValueAtTime(time,0,view),knob->getValueAtTime(time,1,view)}:std::array<double,2>{0,10};
}
void NativeScene3D::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const {depths->push_back(eImageBitDepthFloat);}
NodeDataType NativeScene3D::getOutputDataType() const {
    switch(_kind) {
    case Scene3DKind::Card:case Scene3DKind::ReadGeo:case Scene3DKind::TransformGeo:return NodeDataType::Geometry;
    case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:return NodeDataType::Camera;
    case Scene3DKind::Axis:return NodeDataType::Transform;
    case Scene3DKind::ReadUSD:case Scene3DKind::Scene:return NodeDataType::Scene;
    case Scene3DKind::Project3D:case Scene3DKind::PBRMaterial:return NodeDataType::Material;
    case Scene3DKind::Light:return NodeDataType::Light;
    default:return NodeDataType::Image;
    }
}
bool NativeScene3D::acceptsInputDataType(int i,NodeDataType t) const {
    if(t==NodeDataType::Unknown)return true;
    const bool geo=t==NodeDataType::Geometry || t==NodeDataType::Scene;
    switch(_kind) {
    case Scene3DKind::ReadUSD:case Scene3DKind::Card:case Scene3DKind::ReadGeo:return i==0?(t==NodeDataType::Image || t==NodeDataType::Material):t==NodeDataType::Transform;
    case Scene3DKind::Camera:case Scene3DKind::ReadUSDCamera:case Scene3DKind::ReadCamera:case Scene3DKind::Axis:return t==NodeDataType::Transform;
    case Scene3DKind::Light:return i==0?t==NodeDataType::Transform:i==1 && t==NodeDataType::Image;
    case Scene3DKind::PBRMaterial:return i>=0 && i<4 && (t==NodeDataType::Image || (i==0 && t==NodeDataType::Material));
    case Scene3DKind::TransformGeo:return i==0?geo:t==NodeDataType::Transform;
    case Scene3DKind::Scene:return geo || t==NodeDataType::Light;
    case Scene3DKind::Project3D:return i==0?t==NodeDataType::Image:t==NodeDataType::Camera;
    default:return i==0?geo:i==1?t==NodeDataType::Camera:t==NodeDataType::Image;
    }
}
void NativeScene3D::initializeKnobs() {
    auto page=AppManager::createKnob<KnobPage>(this,tr("Controls"));page->setName("controls");
    auto number=[&](const char* name,const char* label,int dimensions,double value) {
        auto knob=AppManager::createKnob<KnobDouble>(this,tr(label),dimensions);knob->setName(name);
        for(int i=0;i<dimensions;++i)knob->setDefaultValue(value,i);page->addKnob(knob);return knob;
    };
    if(geometryLeaf(_kind) || cameraKind(_kind) || _kind==Scene3DKind::Axis || _kind==Scene3DKind::TransformGeo || _kind==Scene3DKind::Light) {
        _translate=number("translate",(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?"Translate offset":"Translate",3,0);
        _rotate=number("rotate",(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?"Rotate offset":"Rotate",3,0);
        _scale=number("scale",(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)?"Scale multiplier":"Scale",3,1);
        if(_kind==Scene3DKind::Camera)_translate.lock()->setDefaultValue(5,2);
    }
    auto color=[&](const char* name,const char* label,double value) {
        auto knob=AppManager::createKnob<KnobColor>(this,tr(label),3);knob->setName(name);
        for(int i=0;i<3;++i)knob->setDefaultValue(value,i);page->addKnob(knob);return knob;
    };
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
        auto frozen=AppManager::createKnob<KnobBool>(this,tr("Freeze projection and image"));frozen->setName("freeze");frozen->setDefaultValue(true);
        frozen->setHintToolTip(tr("Evaluate the projection camera and image at the reference frame. The render camera remains animated."));page->addKnob(frozen);_freeze=frozen;
        auto reference=AppManager::createKnob<KnobInt>(this,tr("Reference frame"));reference->setName("referenceFrame");reference->setDefaultValue(1);page->addKnob(reference);_reference=reference;
    }
    if(_kind==Scene3DKind::Render3D) {
        auto backend=AppManager::createKnob<KnobChoice>(this,tr("Renderer"));backend->setName("renderer");
        backend->populateChoices({ChoiceOption("Auto","","Use Vulkan when available; fall back to CPU for larger scenes or device limits."),ChoiceOption("CPU","","CPU card/mesh PBR renderer with direct and environment lighting."),ChoiceOption("Vulkan","","Require Vulkan. Supports up to 16 geometry objects, 16 lights and 16 distinct texture samplers in lit scenes, within device mesh-buffer limits."),ChoiceOption("hdNVP","USD / hdNVP","Path trace a USD scene through the persistent Hydra/hdNVP host. Build tools/build-hdnvp-host.ps1 first. Camera projection needs its USD adapter and is currently unavailable on this backend.")});page->addKnob(backend);_backend=backend;
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
                                 ChoiceOption("DepthPreview","Depth preview","Near-white / far-black visualization. Depth.Z retains its raw values.")});page->addKnob(output);_output=output;
        auto range=number("depthRange","Depth preview range",2,0);range->setDefaultValue(10,1);
        range->setDimensionName(0,"Near");range->setDimensionName(1,"Far");
        range->setHintToolTip(tr("Near and far camera-space depths for the white-to-black preview. Requires 0 <= near < far. Does not change clipping or Depth.Z."));
        range->setMinimum(0,0);range->setMinimum(0,1);_depthRange=range;
    }
}
void NativeScene3D::purgeCaches() {
    {std::lock_guard<std::mutex> lock(_meshMutex);_usdAsset.reset();_usdPose.reset();_usdPoseAsset.reset();_mesh.reset();_fbxAsset.reset();_meshAsset.reset();_cameraAsset.reset();_meshPath.clear();_meshStamp=_meshSize=-1;}
    EffectInstance::purgeCaches();
}
int NativeScene3D::rendererBackendAt(double time,ViewIdx view) const {return _backend.lock()?_backend.lock()->getValueAtTime(time,0,view):0;}
Gpu::Cards::HdNvpSettings NativeScene3D::hdNvpSettingsAt(double time,ViewIdx view,bool draft) const {
    Gpu::Cards::HdNvpSettings result;result.samples=_pathSamples.lock()->getValueAtTime(time,0,view);result.bounces=_pathBounces.lock()->getValueAtTime(time,0,view);
    result.denoiser=static_cast<Gpu::Cards::HdNvpDenoiser>(_pathDenoiser.lock()->getValueAtTime(time,0,view));result.interactive=draft;
    if(draft)result.samples=std::min(result.samples,4);return result;
}
void NativeScene3D::onKnobsLoaded() {if(_file.lock())refreshAnimationInfo();}
bool NativeScene3D::knobChanged(KnobI* knob,ValueChangedReasonEnum,ViewSpec,double,bool) {
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
            }else text<<"\nHorizontal camera framing; film offsets are not evaluated.";
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
    return (time-_animationStart.lock()->getValue(0,view))/fps*_animationSpeed.lock()->getValueAtTime(time,0,view)+_animationOffset.lock()->getValueAtTime(time,0,view);
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
    if(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera)return cameraAt(time,view).transform;
    Matrix local=Matrix::transform(value3(_translate,time,view),value3(_rotate,time,view),value3(_scale,time,view,{1,1,1}));
    const int parentInput=(geometryLeaf(_kind) || _kind==Scene3DKind::TransformGeo)?1:0;
    if(auto parent=input3D(this,parentInput))local=parent->transformAt(time,view)*local;
    return local;
}
Gpu::Cards::Material NativeScene3D::materialAt(double time,ViewIdx view) const {
    Gpu::Cards::Material m;m.lit=true;const auto base=color3(_baseColor,time,view);
    m.baseColor={float(base.x),float(base.y),float(base.z),float(_materialOpacity.lock()->getValueAtTime(time,0,view))};
    m.emission=color3(_emission,time,view);m.metallic=_metallic.lock()->getValueAtTime(time,0,view);
    m.roughness=_roughness.lock()->getValueAtTime(time,0,view);m.ior=_ior.lock()->getValueAtTime(time,0,view);return m;
}
Gpu::Cards::Light NativeScene3D::lightAt(double time,ViewIdx view) const {
    Gpu::Cards::Light light;light.transform=transformAt(time,view);light.type=Gpu::Cards::LightType(_lightType.lock()->getValueAtTime(time,0,view));
    light.color=color3(_lightColor,time,view);light.intensity=_intensity.lock()->getValueAtTime(time,0,view);light.exposure=_exposure.lock()->getValueAtTime(time,0,view);
    light.coneAngle=_coneAngle.lock()->getValueAtTime(time,0,view);light.coneSoftness=_coneSoftness.lock()->getValueAtTime(time,0,view);
    light.shadows=_shadows.lock()->getValueAtTime(time,0,view);light.shadowBias=_shadowBias.lock()->getValueAtTime(time,0,view);
    light.environmentSamples=16u<<_environmentSamples.lock()->getValueAtTime(time,0,view);return light;
}
Gpu::Cards::Camera NativeScene3D::cameraAt(double time,ViewIdx view) const {
    Gpu::Cards::Camera camera;
    if(_kind==Scene3DKind::ReadCamera || _kind==Scene3DKind::ReadUSDCamera) {
        camera=_kind==Scene3DKind::ReadCamera?importedCameraAt(time,view):usdAssetAt(view)->camera(_cameraName.lock()->getValue(0,view),_readAnimation.lock()->getValue(0,view)?fbxSecondsAt(time,view):0,_readAnimation.lock()->getValue(0,view));
        const Vec3 position=camera.transform.point({})+value3(_translate,time,view);
        Matrix basis=camera.transform;basis.m[12]=basis.m[13]=basis.m[14]=0;
        camera.transform=Matrix::transform(position,value3(_rotate,time,view),value3(_scale,time,view,{1,1,1}))*basis;
        if(auto parent=input3D(this,0))camera.transform=parent->transformAt(time,view)*camera.transform;
        const double focalMultiplier=_focalMultiplier.lock()->getValueAtTime(time,0,view);
        if(!std::isfinite(focalMultiplier) || focalMultiplier<=0)throw std::runtime_error(getPluginLabel()+" focal multiplier must be positive.");
        camera.focal*=focalMultiplier;camera.orthoWidth/=focalMultiplier;return camera;
    }
    if(!cameraKind(_kind))throw std::runtime_error("A Camera connection is required.");
    camera.transform=transformAt(time,view);
    camera.focal=_focal.lock()->getValueAtTime(time,0,view);camera.aperture=_aperture.lock()->getValueAtTime(time,0,view);
    camera.nearClip=_near.lock()->getValueAtTime(time,0,view);camera.farClip=_far.lock()->getValueAtTime(time,0,view);
    camera.orthoWidth=_orthoWidth.lock()->getValueAtTime(time,0,view);camera.orthographic=_orthographic.lock()->getValueAtTime(time,0,view);
    return camera;
}
Scene3DObject NativeScene3D::editorObjectAt(double time,ViewIdx view) const {
    Scene3DObject object;object.kind=_kind;object.owner=getNode();object.time=time;object.transform=transformAt(time,view);
    const int parentInput=(geometryLeaf(_kind) || _kind==Scene3DKind::TransformGeo)?1:0;
    if(auto parent=input3D(this,parentInput))object.editParentTransform=parent->transformAt(time,view);
    if(cameraKind(_kind))object.camera=cameraAt(time,view);
    if(_kind==Scene3DKind::Light)object.light=lightAt(time,view);
    return object;
}
void NativeScene3D::appendParentObjects(Scene3DSnapshot& scene,double time,ViewIdx view,bool worldSpace) const {
    const int parentInput=(geometryLeaf(_kind) || _kind==Scene3DKind::TransformGeo)?1:0;
    auto parent=input3D(this,parentInput);
    for(int i=0;parent && i<1024;++i) {
        auto object=parent->editorObjectAt(time,view);object.worldSpace=worldSpace;scene.objects.push_back(object);
        parent=input3D(parent,0);
    }
}
Scene3DSnapshot NativeScene3D::sceneAt(double time,ViewIdx view,bool preserveUsd) const {
    Scene3DSnapshot scene;
    if(_kind==Scene3DKind::Render3D) {
        auto geometry=input3D(this,0);auto camera=input3D(this,1);
        if(!geometry || !camera)throw std::runtime_error("Render3D requires a Scene (or Card) and Camera.");
        scene=geometry->sceneAt(time,view,preserveUsd || rendererBackendAt(time,view)==3);scene.camera=camera->cameraAt(time,view);
        auto object=camera->editorObjectAt(time,view);object.worldSpace=true;object.renderCamera=true;
        Format format;getApp()->getProject()->getProjectDefaultFormat(&format);
        object.cameraAspect=double(format.width())*format.getPixelAspectRatio()/std::max(1,format.height());
        scene.objects.push_back(object);camera->appendParentObjects(scene,time,view,true);
        // Shared axes/projectors may be reached through several cards. Keep
        // distinct transformed instances and distinct projection sample times.
        std::vector<Scene3DObject> unique;
        for(const auto& candidate:scene.objects) {
            auto duplicate=std::find_if(unique.begin(),unique.end(),[&](const Scene3DObject& other){
                return candidate.itemIndex<0 && other.owner==candidate.owner && other.time==candidate.time &&
                    other.transform.m==candidate.transform.m && other.worldSpace==candidate.worldSpace && other.cameraAspect==candidate.cameraAspect;
            });
            if(duplicate==unique.end())unique.push_back(candidate);
            else {duplicate->renderCamera |= candidate.renderCamera;duplicate->projectionCamera |= candidate.projectionCamera;}
        }
        scene.objects=std::move(unique);
    } else if(_kind==Scene3DKind::Scene) {
        for(int i=0;i<getNInputs();++i)if(auto child=input3D(this,i)) {
            auto part=child->sceneAt(time,view,preserveUsd);const int offset=int(scene.items.size());
            scene.usdSources.insert(scene.usdSources.end(),part.usdSources.begin(),part.usdSources.end());
            scene.items.insert(scene.items.end(),part.items.begin(),part.items.end());
            const size_t lightOffset=scene.lights.size();
            scene.lights.insert(scene.lights.end(),part.lights.begin(),part.lights.end());
            for(auto source:part.environmentSources){source.lightIndex+=lightOffset;scene.environmentSources.push_back(std::move(source));}
            for(auto& object:part.objects){if(object.itemIndex>=0)object.itemIndex+=offset;scene.objects.push_back(object);}
        }
    } else if(_kind==Scene3DKind::TransformGeo) {
        if(auto child=input3D(this,0))scene=child->sceneAt(time,view,preserveUsd);
        const Matrix matrix=transformAt(time,view);
        if(!scene.usdSources.empty() && matrix.m!=Matrix().m)throw std::runtime_error("USD / hdNVP stage transforms require the USD composition adapter. Keep TransformGeo at identity for now.");
        for(auto& light:scene.lights)light.transform=matrix*light.transform;
        for(auto& item:scene.items) {
            item.card.transform=matrix*item.card.transform;
            item.editParentTransform=matrix*item.editParentTransform;
        }
        for(auto& object:scene.objects)if(!object.worldSpace) {
            object.transform=matrix*object.transform;object.editParentTransform=matrix*object.editParentTransform;
            if(object.kind==Scene3DKind::Light)object.light.transform=object.transform;
        }
        scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);
    } else if(_kind==Scene3DKind::Light) {
        scene.lights.push_back(lightAt(time,view));scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);
        if(scene.lights[0].type==Gpu::Cards::LightType::Environment)if(auto source=resolve(getInput(1))) {
            if(source->getOutputDataType()!=NodeDataType::Image)throw std::runtime_error("Environment light requires an image input.");
            scene.environmentSources.push_back({0,source,time});
        }
    } else if(geometryLeaf(_kind)) {
        std::vector<Gpu::Cards::Card> sourceCards;
        const Matrix matrix=transformAt(time,view);
        if(_kind==Scene3DKind::ReadUSD && preserveUsd) {
            const auto subtree=_primPath.lock()->getValue(0,view);
            if(matrix.m!=Matrix().m || _opacity.lock()->getValueAtTime(time,0,view)!=1 || resolve(getInput(0)) || (!subtree.empty() && subtree!="/"))
                throw std::runtime_error("USD / hdNVP currently reads the complete stage with its original materials. ReadUSD transform, opacity, material and subtree overrides require the USD composition adapter.");
            const auto asset=usdAssetAt(view);Gpu::Cards::UsdSceneSource source;
            source.filename=_file.lock()->getValue(0,view);getApp()->getProject()->canonicalizePath(source.filename);
            source.variants=_variants.lock()->getValue(0,view);source.loadPayloads=_loadPayloads.lock()->getValue(0,view);
            source.timeCode=asset->startTime()+(_readAnimation.lock()->getValue(0,view)?fbxSecondsAt(time,view)*asset->timeCodesPerSecond():0);
            source.metersPerUnit=asset->metersPerUnit();source.zUp=asset->zUp();source.timeCodesPerSecond=asset->timeCodesPerSecond();
            source.revision=asset->revision();
            scene.usdSources.push_back(std::move(source));scene.objects.push_back(editorObjectAt(time,view));appendParentObjects(scene,time,view);return scene;
        }
        if(_kind==Scene3DKind::ReadUSD) {
            const auto pose=usdPoseAt(time,view);sourceCards=pose->cards;
            for(auto light:pose->lights){light.transform=matrix*light.transform;scene.lights.push_back(light);}
        }else sourceCards.emplace_back();
        for(const auto& sourceCard:sourceCards) {
        Scene3DItem item;item.owner=getNode();item.card=sourceCard;item.card.transform=matrix*item.card.transform;item.imageTime=time;
        if(auto parent=input3D(this,1))item.editParentTransform=parent->transformAt(time,view);
        if(_kind==Scene3DKind::ReadGeo)item.card.mesh=meshAt(time,view);
        else if(_kind==Scene3DKind::Card) {item.card.width=_size.lock()->getValueAtTime(time,0,view);item.card.height=_size.lock()->getValueAtTime(time,1,view);}
        item.card.opacity=_opacity.lock()->getValueAtTime(time,0,view);item.imageSource=resolve(getInput(0));
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
                map.channel=j==0?0u:unsigned((j==1?material->_roughnessChannel:material->_metallicChannel).lock()->getValueAtTime(time,0,view));
                map.normalEncoding=j==0?material->_normalEncoding.lock()->getValueAtTime(time,0,view):-1;
                map.flipY=j==0 && material->_normalFlipY.lock()->getValueAtTime(time,0,view);
                map.wrap=Gpu::Cards::TextureAddress(material->_mapWrap.lock()->getValueAtTime(time,0,view));
            }
            // The material nearest geometry supplies the surface; unwrap any
            // upstream material overrides to find its image or projection.
            for(int i=0;material && material->kind()==Scene3DKind::PBRMaterial && i<1024;++i) {
                item.imageSource=resolve(material->getInput(0));material=dynamic_cast<NativeScene3D*>(item.imageSource.get());
            }
        }
        NativeScene3D* projector=nullptr;double projectionTime=time;
        if(material && material->kind()==Scene3DKind::Project3D) {
            projectionTime=material->_freeze.lock()->getValueAtTime(time,0,view)?material->_reference.lock()->getValueAtTime(time,0,view):time;
            projector=input3D(material,1);
            if(!projector)throw std::runtime_error("Project3D requires a projection Camera.");
            item.card.projected=true;item.card.projector=projector->cameraAt(projectionTime,view);
            item.imageSource=resolve(material->getInput(0));item.imageTime=projectionTime;
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
    try {
        // A Reader may adopt the project format on its first pixel fetch.
        // Keep the format that defined this request's output planes, so that
        // lazy texture evaluation cannot leave part of those planes unwritten.
        Format format;getApp()->getProject()->getProjectDefaultFormat(&format);
        const RectI output=format.toCanonicalFormat().toPixelEnclosing(args.mappedScale.toMipmapLevel(),format.getPixelAspectRatio());
        auto scene=sceneAt(args.time,args.view);textureLights(scene,args.view,args.draftMode);auto cards=texturedScene(scene,args.view,args.draftMode);
        Gpu::Cards::Settings settings;settings.width=output.width();settings.height=output.height();settings.pixelAspect=format.getPixelAspectRatio();
        settings.lights=scene.lights;
        settings.samples=1<<_samples.lock()->getValueAtTime(args.time,0,args.view);
        const int backend=_backend.lock()->getValueAtTime(args.time,0,args.view);
        Gpu::Cards::HdNvpFrame usdFrame;
        if(backend==3) {
            if(!_usdBridge)_usdBridge=std::make_unique<Gpu::Cards::UsdSceneBridge>();
            usdFrame=_usdBridge->render(cards,scene.camera,settings,hdNvpSettingsAt(args.time,args.view,args.draftMode),[this]{return aborted();},scene.usdSources);
        }
        auto renderPass=[&](Gpu::Cards::RenderPass pass) {
            settings.pass=pass;std::vector<float> pixels;
            if(backend==3) {
                if(pass==Gpu::Cards::RenderPass::Color)return usdFrame.color;
                pixels.resize(usdFrame.depth.size()*4);
                for(size_t i=0;i<usdFrame.depth.size();++i){for(int c=0;c<3;++c)pixels[i*4+c]=usdFrame.depth[i];pixels[i*4+3]=usdFrame.depth[i]>0?1.f:0.f;}
                return pixels;
            }
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
        const bool preview=depthPreviewAt(args.time,args.view);
        const bool needColor=std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first.isColorPlane();});
        const bool needDepth=(needColor && preview) || std::any_of(args.outputPlanes.begin(),args.outputPlanes.end(),[](const auto& plane){return plane.first==depthPlane();});
        std::vector<float> pixels,depth;
        if(needDepth)depth=renderPass(Gpu::Cards::RenderPass::Depth);
        if(needColor) {
            if(preview) {
                pixels=depth;const auto range=depthPreviewRangeAt(args.time,args.view);
                Gpu::Cards::makeDepthPreview(pixels,range[0],range[1]);
            }else pixels=renderPass(Gpu::Cards::RenderPass::Color);
        }
        if(needColor && !preview)if(const auto background=getInput(2)) {
            auto texture=fetchTexture(background,args.time,args.view,args.draftMode);
            if(texture)for(int y=0;y<settings.height;++y)for(int x=0;x<settings.width;++x) {
                const auto bg=texture->sample((x+.5)/settings.width,(y+.5)/settings.height);
                float* p=pixels.data()+(size_t(y)*settings.width+x)*4;const float weight=1-p[3];
                for(int c=0;c<4;++c)p[c]+=bg[c]*weight;
            }
        }
        for(const auto& plane:args.outputPlanes) {
            const auto image=plane.second;Image::WriteAccess write(image.get());
            const bool z=plane.first==depthPlane();const unsigned channels=image->getComponentsCount();
            const RectI roi=args.roi.intersect(output).intersect(write.bounds());
            for(int y=roi.y1;y<roi.y2;++y)for(int x=roi.x1;x<roi.x2;++x) {
                float* to=reinterpret_cast<float*>(write.pixelAt(x,y));
                const size_t at=((size_t(y-output.y1)*settings.width)+(x-output.x1))*4;
                if(z)to[0]=depth[at];
                else if(plane.first.isColorPlane()) {
                    const float* from=pixels.data()+at;
                    if(channels==1)to[0]=from[3];else std::copy_n(from,std::min(channels,4u),to);
                }
            }
        }
        return eStatusOK;
    }catch(const std::exception& e) {
        setPersistentMessage(eMessageTypeError,e.what());return eStatusFailed;
    }
}
NATRON_NAMESPACE_EXIT
