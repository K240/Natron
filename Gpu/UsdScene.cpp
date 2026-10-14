// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdScene.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#ifdef NATRON_HAS_USD
#include "Gpu/UsdTexture.h"
#include <pxr/usd/ar/resolverContextBinder.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usd/editContext.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/pcp/errors.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdLux/blackbody.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#endif

namespace Natron { namespace Gpu { namespace Cards {
#ifdef NATRON_HAS_USD
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
// USD stages are immutable after opening except explicit reload/session variant
// edits. Serializing accesses also protects shared Sdf layers across nodes.
std::mutex usdMutex;
std::atomic<uint64_t> usdRevision{0};
constexpr double pi=3.14159265358979323846;
void warn(std::vector<std::string>& out,const std::string& s) {
    if(std::find(out.begin(),out.end(),s)==out.end())out.push_back(s);
}
std::string errors(TfErrorMark& mark) {
    std::string result;for(const auto& e:mark){if(!result.empty())result+='\n';result+=e.GetCommentary();}
    mark.Clear();return result;
}
template<class T> T value(const UsdAttribute& a,UsdTimeCode time,T fallback) {
    T result;return a && a.Get(&result,time)?result:fallback;
}
Vec3 vec(const GfVec3f& p){return {p[0],p[1],p[2]};}
Matrix matrix(const GfMatrix4d& usd) {
    Matrix m;for(int r=0;r<4;++r)for(int c=0;c<4;++c)m.m[c*4+r]=usd[c][r];return m;
}
Matrix stageBasis(const UsdStageRefPtr& stage,bool units=true) {
    const double meters=UsdGeomGetStageMetersPerUnit(stage);
    if(!std::isfinite(meters) || meters<=0)throw std::runtime_error("USD metersPerUnit must be positive.");
    const auto axis=UsdGeomGetStageUpAxis(stage);
    if(axis!=UsdGeomTokens->y && axis!=UsdGeomTokens->z)throw std::runtime_error("Unsupported USD upAxis.");
    const double s=units?meters:1;
    return Matrix::transform({},axis==UsdGeomTokens->z?Vec3{-90,0,0}:Vec3{},{s,s,s});
}
template<class T> struct Primvar {
    VtArray<T> data;TfToken interpolation;
    T get(int point,int face,int corner,T fallback) const {
        const int i=interpolation==UsdGeomTokens->constant?0:interpolation==UsdGeomTokens->uniform?face:
            interpolation==UsdGeomTokens->faceVarying?corner:point;
        if(i<0 || size_t(i)>=data.size())return fallback;return data[i];
    }
};
template<class T> Primvar<T> primvar(const UsdGeomPrimvar& pv,UsdTimeCode time) {
    Primvar<T> r;if(pv){if(!pv.ComputeFlattened(&r.data,time) && pv.GetAttr().HasAuthoredValueOpinion())throw std::runtime_error("Invalid USD primvar: "+pv.GetAttr().GetPath().GetString());r.interpolation=pv.GetInterpolation();}return r;
}
template<class T> T inputValue(const UsdShadeShader& shader,const char* name,UsdTimeCode time,T fallback,
                              std::vector<std::string>& warnings) {
    const auto input=shader.GetInput(TfToken(name));
    if(input){const auto attrs=input.GetValueProducingAttributes();T result;if(attrs.size()==1 && attrs[0].Get(&result,time))return result;}
    if(input && input.HasConnectedSource())warn(warnings,shader.GetPath().GetString()+": connected material inputs are not evaluated ("+name+").");
    T result;return input && input.Get(&result,time)?result:fallback;
}
struct Surface {Material material;UsdTextureBinding image;std::array<UsdTextureBinding,3> maps;};
Surface surface(const UsdShadeMaterial& usd,UsdTimeCode time,UsdTextureCache& textures,std::vector<std::string>& warnings) {
    Surface result;auto& m=result.material;if(!usd)return result;
    const auto shader=usd.ComputeSurfaceSource();
    if(!shader || value(shader.GetIdAttr(),time,TfToken())!=TfToken("UsdPreviewSurface")) {
        warn(warnings,usd.GetPath().GetString()+": unsupported surface shader; using unlit displayColor.");return result;
    }
    result.image=usdDiffuseTexture(shader,time,textures,warnings);
    const char* mapNames[]{"normal","roughness","metallic"};
    for(int i=0;i<3;++i)result.maps[i]=usdDataTexture(shader,mapNames[i],time,textures,warnings);
    m.lit=true;const auto color=result.image.texture?GfVec3f(1):inputValue(shader,"diffuseColor",time,GfVec3f(.18f),warnings);
    m.baseColor={color[0],color[1],color[2],result.image.opacityConnected?1.f:inputValue(shader,"opacity",time,1.f,warnings)};
    m.emission=vec(inputValue(shader,"emissiveColor",time,GfVec3f(0),warnings));
    m.metallic=result.maps[2].texture?0.f:inputValue(shader,"metallic",time,0.f,warnings);m.roughness=result.maps[1].texture?.5f:inputValue(shader,"roughness",time,.5f,warnings);
    m.ior=inputValue(shader,"ior",time,1.5f,warnings);
    if(inputValue(shader,"useSpecularWorkflow",time,0,warnings)!=0)
        warn(warnings,shader.GetPath().GetString()+": specular workflow is approximated by metallic/roughness.");
    for(const char* name:{"normal","displacement","occlusion","clearcoat","clearcoatRoughness","opacityThreshold"}) {
        const auto i=shader.GetInput(TfToken(name));
        if(i && !(std::string(name)=="normal" && result.maps[0].texture) && (i.HasConnectedSource() || i.GetAttr().HasAuthoredValueOpinion()))
            warn(warnings,shader.GetPath().GetString()+": unsupported surface input "+name+".");
    }
    return result;
}
// Ear clipping in the polygon's dominant plane preserves concave faces. Bad
// or self-intersecting polygons fail explicitly instead of generating a fan.
std::vector<std::array<int,3>> triangulate(const std::vector<Vec3>& p) {
    std::vector<std::array<int,3>> result;if(p.size()<3)return result;
    Vec3 n;for(size_t i=0;i<p.size();++i){const auto a=p[i],b=p[(i+1)%p.size()];n.x+=(a.y-b.y)*(a.z+b.z);n.y+=(a.z-b.z)*(a.x+b.x);n.z+=(a.x-b.x)*(a.y+b.y);}
    int drop=std::abs(n.x)>std::abs(n.y)?0:1;if(std::abs(n.z)>(drop==0?std::abs(n.x):std::abs(n.y)))drop=2;
    auto xy=[&](int i){return drop==0?std::array<double,2>{p[i].y,p[i].z}:drop==1?std::array<double,2>{p[i].x,p[i].z}:std::array<double,2>{p[i].x,p[i].y};};
    auto cross=[&](int a,int b,int c){const auto x=xy(a),y=xy(b),z=xy(c);return (y[0]-x[0])*(z[1]-x[1])-(y[1]-x[1])*(z[0]-x[0]);};
    double area=0,extent=0;for(size_t i=0;i<p.size();++i){auto a=xy(int(i)),b=xy(int((i+1)%p.size()));area+=a[0]*b[1]-b[0]*a[1];extent=std::max(extent,std::max(std::abs(a[0]-b[0]),std::abs(a[1]-b[1])));}
    const double eps=extent*extent*1e-12;if(std::abs(area)<=eps)return result;
    const double sign=area>0?1:-1;std::vector<int> remaining(p.size());std::iota(remaining.begin(),remaining.end(),0);
    while(remaining.size()>3) {
        bool found=false;
        for(size_t i=0;i<remaining.size();++i) {
            int a=remaining[(i+remaining.size()-1)%remaining.size()],b=remaining[i],c=remaining[(i+1)%remaining.size()];
            if(sign*cross(a,b,c)<=eps)continue;
            bool inside=false;for(int v:remaining)if(v!=a && v!=b && v!=c)
                if(sign*cross(a,b,v)>=-eps && sign*cross(b,c,v)>=-eps && sign*cross(c,a,v)>=-eps){inside=true;break;}
            if(inside)continue;result.push_back({a,b,c});remaining.erase(remaining.begin()+i);found=true;break;
        }
        if(!found) {
            // Collinear corners are safe to remove, retaining other corners.
            for(size_t i=0;i<remaining.size();++i)if(std::abs(cross(remaining[(i+remaining.size()-1)%remaining.size()],remaining[i],remaining[(i+1)%remaining.size()]))<=eps){remaining.erase(remaining.begin()+i);found=true;break;}
            if(!found)throw std::runtime_error("USD polygon cannot be triangulated (self-intersection or invalid topology).");
        }
    }
    if(remaining.size()==3)result.push_back({remaining[0],remaining[1],remaining[2]});return result;
}
std::vector<Card> mesh(const UsdGeomMesh& usd,UsdTimeCode time,UsdGeomXformCache& transforms,const Matrix& basis,
          UsdTextureCache& textures,std::vector<std::string>& warnings) {
    const auto path=usd.GetPath().GetString();
    const auto points=value(usd.GetPointsAttr(),time,VtVec3fArray());
    const auto counts=value(usd.GetFaceVertexCountsAttr(),time,VtIntArray());
    const auto indices=value(usd.GetFaceVertexIndicesAttr(),time,VtIntArray());
    const auto holes=value(usd.GetHoleIndicesAttr(),time,VtIntArray());
    if(value(usd.GetSubdivisionSchemeAttr(),time,UsdGeomTokens->catmullClark)!=UsdGeomTokens->none)
        warn(warnings,path+": subdivision is rendered as the polygon control mesh.");
    if(usd.GetPrim().HasAPI(TfToken("SkelBindingAPI")))warn(warnings,path+": USD skeleton deformation is not evaluated.");
    const auto primvars=UsdGeomPrimvarsAPI(usd);
    auto normals=primvar<GfVec3f>(primvars.FindPrimvarWithInheritance(TfToken("normals")),time);
    if(normals.data.empty()){normals.data=value(usd.GetNormalsAttr(),time,VtVec3fArray());normals.interpolation=usd.GetNormalsInterpolation();}
    const auto uv=primvar<GfVec2f>(primvars.FindPrimvarWithInheritance(TfToken("st")),time);
    const auto colors=primvar<GfVec3f>(primvars.FindPrimvarWithInheritance(TfToken("displayColor")),time);
    const auto opacity=primvar<float>(primvars.FindPrimvarWithInheritance(TfToken("displayOpacity")),time);
    auto validate=[&](const auto& pv) {
        if(pv.data.empty())return;
        const auto interpolation=pv.interpolation;
        size_t count=0;
        if(interpolation==UsdGeomTokens->constant)count=1;
        else if(interpolation==UsdGeomTokens->uniform)count=counts.size();
        else if(interpolation==UsdGeomTokens->faceVarying)count=indices.size();
        else if(interpolation==UsdGeomTokens->vertex || interpolation==UsdGeomTokens->varying)count=points.size();
        else throw std::runtime_error(path+": unsupported primvar interpolation.");
        if(pv.data.size()!=count)throw std::runtime_error(path+": invalid primvar element count.");
    };
    validate(normals);validate(uv);validate(colors);validate(opacity);
    for(const auto& p:points)for(int i=0;i<3;++i)if(!std::isfinite(p[i]))throw std::runtime_error(path+": nonfinite mesh point.");
    const bool left=value(usd.GetOrientationAttr(),time,UsdGeomTokens->rightHanded)==UsdGeomTokens->leftHanded;
    UsdShadeMaterialBindingAPI binding(usd.GetPrim());
    const auto material=surface(binding.ComputeBoundMaterial(),time,textures,warnings);
    std::vector<Surface> materials(counts.size(),material);std::vector<bool> assigned(counts.size());
    for(const auto& subset:binding.GetMaterialBindSubsets()) {
        const auto bound=surface(UsdShadeMaterialBindingAPI(subset.GetPrim()).ComputeBoundMaterial(),time,textures,warnings);
        for(int face:value(subset.GetIndicesAttr(),time,VtIntArray())) {
            if(face<0 || size_t(face)>=counts.size() || assigned[face])throw std::runtime_error(path+": invalid/overlapping material subset.");
            assigned[face]=true;materials[face]=bound;
        }
    }
    // Keep distinct image samplers in separate renderer objects. Untextured
    // face materials stay in one mesh; the Viewer still edits the entire node.
    struct Group {std::shared_ptr<Mesh> mesh=std::make_shared<Mesh>();std::shared_ptr<const Texture> texture;std::array<MaterialMap,3> maps;};
    std::vector<Group> groups;
    std::map<TfToken,Primvar<GfVec2f>> textureUvs;
    size_t begin=0;
    for(size_t face=0;face<counts.size();++face) {
        const int count=counts[face];if(count<3 || size_t(count)>indices.size()-std::min(begin,indices.size()))throw std::runtime_error(path+": invalid face topology.");
        std::vector<Vec3> polygon;polygon.reserve(count);
        for(int k=0;k<count;++k){const int i=indices[begin+k];if(i<0 || size_t(i)>=points.size())throw std::runtime_error(path+": point index outside points array.");polygon.push_back(vec(points[i]));}
        const auto& faceSurface=materials[face];const auto& image=faceSurface.image;
        auto mappedUv=[&](const UsdTextureBinding& image) -> const Primvar<GfVec2f>* {
            if(!image.texture)return &uv;
            auto found=textureUvs.find(image.uvName);
            if(found==textureUvs.end()) {
                auto pv=image.uvName.IsEmpty()?Primvar<GfVec2f>():primvar<GfVec2f>(primvars.FindPrimvarWithInheritance(image.uvName),time);validate(pv);
                if(!image.uvName.IsEmpty() && pv.data.empty())warn(warnings,path+": missing texture UV primvar "+image.uvName.GetString()+"; using reader fallback.");
                found=textureUvs.emplace(image.uvName,std::move(pv)).first;
            }
            return &found->second;
        };
        const auto faceUv=mappedUv(image);
        std::array<const Primvar<GfVec2f>*,3> mapUvs;
        for(int j=0;j<3;++j)mapUvs[j]=mappedUv(faceSurface.maps[j]);
        auto group=std::find_if(groups.begin(),groups.end(),[&](const Group& g){
            if(g.texture!=image.texture)return false;
            for(int j=0;j<3;++j)if(g.maps[j].texture!=faceSurface.maps[j].texture || g.maps[j].channel!=faceSurface.maps[j].channel)return false;
            return true;
        });
        if(group==groups.end()){
            groups.push_back(Group());group=groups.end()-1;group->texture=image.texture;
            for(int j=0;j<3;++j)group->maps[j]={faceSurface.maps[j].texture,faceSurface.maps[j].channel};
        }
        if(std::find(holes.begin(),holes.end(),int(face))==holes.end())for(auto tri:triangulate(polygon)) {
            if(left)std::swap(tri[1],tri[2]);MeshTriangle t;t.color={1,1,1,1};t.material=faceSurface.material;
            for(int k=0;k<3;++k){const int corner=int(begin)+tri[k],i=indices[corner];auto& v=t.vertices[k];v.position=polygon[tri[k]];
                v.normal=vec(normals.get(i,int(face),corner,GfVec3f(0)));auto tex=faceUv->get(i,int(face),corner,image.texture?image.uvFallback:GfVec2f(.5f));v.uv={tex[0],tex[1]};
                for(int j=0;j<3;++j){const auto q=mapUvs[j]->get(i,int(face),corner,faceSurface.maps[j].uvFallback);v.mapUvs[j]={q[0],q[1]};}
                const auto c=colors.get(i,int(face),corner,GfVec3f(1));v.color={c[0],c[1],c[2],opacity.get(i,int(face),corner,1.f)};}
            group->mesh->triangles.push_back(t);
        }
        begin+=count;
    }
    if(begin!=indices.size())throw std::runtime_error(path+": face counts do not match indices.");
    std::vector<Card> cards;const auto transform=basis*matrix(transforms.GetLocalToWorldTransform(usd.GetPrim()));
    for(auto& group:groups)if(!group.mesh->triangles.empty()) {
        group.mesh->objectCount=1;group.mesh->buildAcceleration();Card card;card.mesh=group.mesh;card.texture=group.texture;card.maps=group.maps;card.transform=transform;cards.push_back(std::move(card));
    }
    return cards;
}
Camera readCamera(const UsdGeomCamera& usd,UsdTimeCode time,const UsdStageRefPtr& stage) {
    UsdGeomXformCache cache(time);const double meters=UsdGeomGetStageMetersPerUnit(stage);Camera c;
    c.transform=stageBasis(stage)*matrix(cache.GetLocalToWorldTransform(usd.GetPrim()));
    // Remove the stage unit scale from camera directions, keeping authored scale.
    for(int col=0;col<3;++col)for(int row=0;row<3;++row)c.transform.m[col*4+row]/=meters;
    c.focal=value(usd.GetFocalLengthAttr(),time,50.f)*meters*100;
    c.aperture=value(usd.GetHorizontalApertureAttr(),time,20.955f)*meters*100;
    c.offsetX=value(usd.GetHorizontalApertureOffsetAttr(),time,0.f)/value(usd.GetHorizontalApertureAttr(),time,20.955f);
    c.offsetY=value(usd.GetVerticalApertureOffsetAttr(),time,0.f)/value(usd.GetHorizontalApertureAttr(),time,20.955f);
    c.orthoWidth=c.aperture/1000;c.orthographic=value(usd.GetProjectionAttr(),time,UsdGeomTokens->perspective)==UsdGeomTokens->orthographic;
    const auto clip=value(usd.GetClippingRangeAttr(),time,GfVec2f(1,1000000));c.nearClip=clip[0]*meters;c.farClip=clip[1]*meters;return c;
}
Light readLight(const UsdPrim& prim,UsdTimeCode time,UsdGeomXformCache& cache,const Matrix& basis,
                double meters,UsdTextureCache& textures,std::vector<std::string>& warnings) {
    UsdLuxLightAPI api(prim);Light light;light.transform=basis*matrix(cache.GetLocalToWorldTransform(prim));
    light.color=vec(value(api.GetColorAttr(),time,GfVec3f(1)));
    if(value(api.GetEnableColorTemperatureAttr(),time,false)){const auto rgb=UsdLuxBlackbodyTemperatureAsRgb(value(api.GetColorTemperatureAttr(),time,6500.f));light.color={light.color.x*rgb[0],light.color.y*rgb[1],light.color.z*rgb[2]};}
    light.intensity=value(api.GetIntensityAttr(),time,1.f);light.exposure=value(api.GetExposureAttr(),time,0.f);
    const bool normalize=value(api.GetNormalizeAttr(),time,false);const std::string path=prim.GetPath().GetString();
    if(prim.IsA<UsdLuxDomeLight>()) {
        UsdLuxDomeLight dome(prim);light.type=LightType::Environment;
        const auto format=value(dome.GetTextureFormatAttr(),time,TfToken("automatic"));
        if(format!=TfToken("automatic") && format!=TfToken("latlong")) {
            warn(warnings,path+": unsupported dome texture format "+format.GetString()+"; environment disabled.");light.intensity=0;
        }else light.environment=usdEnvironmentTexture(prim,time,textures,warnings);
        if(dome.GetPortalsRel().HasAuthoredTargets())warn(warnings,path+": dome portals are not used to guide environment sampling.");
    }else if(prim.IsA<UsdLuxDistantLight>()) {
        const double angle=value(UsdLuxDistantLight(prim).GetAngleAttr(),time,.53f)*pi/360;
        const double size=angle==0?1:angle<=pi/2?pi*std::pow(std::sin(angle),2):pi*(2-std::pow(std::sin(angle),2));
        if(!normalize)light.intensity*=size;
        if(angle>0)warn(warnings,path+": distant angular spread uses integrated intensity and hard shadows.");
    }else {
        light.type=LightType::Point;const double radius=value(UsdLuxSphereLight(prim).GetRadiusAttr(),time,.5f)*meters;
        const double sx=std::sqrt(std::pow(light.transform.m[0]/meters,2)+std::pow(light.transform.m[1]/meters,2)+std::pow(light.transform.m[2]/meters,2));
        light.intensity*=normalize?.25:pi*radius*radius*sx*sx;
        warn(warnings,path+": sphere light approximated by a point source; area/soft shadows are not evaluated.");
        if(prim.HasAPI<UsdLuxShapingAPI>()) {
            UsdLuxShapingAPI shaping(prim);const double angle=value(shaping.GetShapingConeAngleAttr(),time,90.f);
            if(angle>0 && angle<90){light.type=LightType::Spot;light.coneAngle=angle;light.coneSoftness=value(shaping.GetShapingConeSoftnessAttr(),time,0.f);}
            if(value(shaping.GetShapingFocusAttr(),time,0.f)!=0 || shaping.GetShapingIesFileAttr().HasAuthoredValueOpinion())warn(warnings,path+": shaping focus and IES profiles are not evaluated.");
        }
    }
    if(prim.HasAPI<UsdLuxShadowAPI>())light.shadows=value(UsdLuxShadowAPI(prim).GetShadowEnableAttr(),time,true);
    if(value(api.GetDiffuseAttr(),time,1.f)!=1 || value(api.GetSpecularAttr(),time,1.f)!=1)warn(warnings,path+": per-lobe light multipliers are not evaluated.");
    if(api.GetLightLinkCollectionAPI().GetIncludesRel().HasAuthoredTargets() || api.GetShadowLinkCollectionAPI().GetIncludesRel().HasAuthoredTargets())warn(warnings,path+": light/shadow linking is not evaluated.");
    return light;
}
}
struct UsdSceneAsset::Impl {
    uint64_t revision=++usdRevision;
    UsdStageRefPtr stage;std::string variants;std::vector<std::string> cameraNames,meshNames,lightNames;mutable UsdTextureCache textures;
    void metadata(){cameraNames.clear();meshNames.clear();lightNames.clear();for(const auto& p:UsdPrimRange::Stage(stage,UsdTraverseInstanceProxies())) {
        if(p.IsA<UsdGeomCamera>())cameraNames.push_back(p.GetPath().GetString());
        if(p.IsA<UsdGeomMesh>())meshNames.push_back(p.GetPath().GetString());
        if(p.HasAPI<UsdLuxLightAPI>())lightNames.push_back(p.GetPath().GetString());
    }}
    void selectVariants() {
        UsdEditContext edit(stage,stage->GetSessionLayer());std::istringstream lines(variants);std::string line;
        while(std::getline(lines,line)){const auto comment=line.find('#');if(comment!=std::string::npos)line.resize(comment);
            std::istringstream words(line);std::string path,choice,extra;if(!(words>>path))continue;
            if(!(words>>choice) || (words>>extra))throw std::runtime_error("USD variants: expected /Prim variantSet=selection per line.");
            const size_t equals=choice.find('=');if(equals==std::string::npos)throw std::runtime_error("USD variant is missing '='.");
            if(!SdfPath::IsValidPathString(path) || !SdfPath(path).IsAbsoluteRootOrPrimPath())throw std::runtime_error("Invalid USD variant prim path: "+path);
            const auto prim=stage->GetPrimAtPath(SdfPath(path));if(!prim)throw std::runtime_error("USD variant prim does not exist: "+path);
            const auto setName=choice.substr(0,equals),selection=choice.substr(equals+1);auto sets=prim.GetVariantSets();
            if(!sets.HasVariantSet(setName))throw std::runtime_error("Unknown USD variant set: "+setName);auto set=sets.GetVariantSet(setName);const auto names=set.GetVariantNames();
            if(std::find(names.begin(),names.end(),selection)==names.end() || !set.SetVariantSelection(selection))throw std::runtime_error("Unknown USD variant selection: "+selection);
        }
    }
    UsdTimeCode time(double seconds,bool animated) const {
        if(!std::isfinite(seconds))throw std::runtime_error("USD source time must be finite.");
        return UsdTimeCode(stage->GetStartTimeCode()+(animated?seconds*stage->GetTimeCodesPerSecond():0));
    }
};
bool UsdSceneAsset::available(){return true;}
std::shared_ptr<UsdSceneAsset> UsdSceneAsset::load(const std::string& filename,const std::string& variants,bool payloads) {
    std::lock_guard<std::mutex> lock(usdMutex);TfErrorMark mark;auto impl=std::make_unique<Impl>();
    if(!PlugRegistry::GetInstance().GetPluginWithName("sdf") || !PlugRegistry::GetInstance().GetPluginWithName("usdGeom"))
        throw std::runtime_error("OpenUSD plugin resources are missing. Run with tools/run-windows-qt6.ps1, or configure PXR_PLUGINPATH_NAME for the USD resources directory.");
    // A private session layer prevents variant selections leaking between nodes.
    const auto root=SdfLayer::FindOrOpen(filename);
    if(root)impl->stage=UsdStage::Open(root,SdfLayer::CreateAnonymous("NatronUsdSession"),payloads?UsdStage::LoadAll:UsdStage::LoadNone);
    const auto message=errors(mark);if(!impl->stage)throw std::runtime_error("Cannot open USD: "+filename+"\n"+message);
    // Force reload: timestamp resolution must not hide edits made within the
    // same second. Never reload anonymous private session layers.
    for(const auto& layer:impl->stage->GetUsedLayers())if(!layer->IsAnonymous() && !layer->Reload(true))
        throw std::runtime_error("Cannot reload USD layer: "+layer->GetIdentifier());
    impl->stage->Reload();
    stageBasis(impl->stage);impl->variants=variants;impl->selectVariants();impl->metadata();return std::shared_ptr<UsdSceneAsset>(new UsdSceneAsset(std::move(impl)));
}
UsdScenePose UsdSceneAsset::evaluate(double seconds,bool animated,const std::string& path) const {
    std::lock_guard<std::mutex> lock(usdMutex);UsdScenePose pose;const auto time=_impl->time(seconds,animated);auto stage=_impl->stage;
    ArResolverContextBinder resolverContext(stage->GetPathResolverContext());
    for(const auto& error:stage->GetCompositionErrors())warn(pose.warnings,error->ToString());
    UsdPrim root=stage->GetPseudoRoot();if(!path.empty() && path!="/") {
        if(!SdfPath::IsValidPathString(path) || !SdfPath(path).IsAbsoluteRootOrPrimPath())throw std::runtime_error("Invalid USD prim path: "+path);
        root=stage->GetPrimAtPath(SdfPath(path));if(!root)throw std::runtime_error("USD prim does not exist: "+path);
    }
    const auto basis=stageBasis(stage);UsdGeomXformCache cache(time);auto range=UsdPrimRange(root,UsdTraverseInstanceProxies());
    for(auto it=range.begin();it!=range.end();++it) {
        const auto prim=*it;const std::string name=prim.GetPath().GetString();UsdGeomImageable imageable(prim);
        if(imageable && (imageable.ComputeVisibility(time)==UsdGeomTokens->invisible || imageable.ComputePurpose()==UsdGeomTokens->guide || imageable.ComputePurpose()==UsdGeomTokens->proxy)){it.PruneChildren();continue;}
        const std::string type=prim.GetTypeName().GetString();
        if(type=="PointInstancer"){warn(pose.warnings,name+": PointInstancer is not evaluated.");it.PruneChildren();continue;}
        if(prim.IsA<UsdGeomMesh>()) {
            auto cards=mesh(UsdGeomMesh(prim),time,cache,basis,_impl->textures,pose.warnings);
            if(!cards.empty())pose.meshPaths.push_back(name);
            for(auto& card:cards)pose.cards.push_back(std::move(card));
        }else if(prim.IsA<UsdLuxDistantLight>() || prim.IsA<UsdLuxSphereLight>() || prim.IsA<UsdLuxDomeLight>())pose.lights.push_back(readLight(prim,time,cache,basis,UsdGeomGetStageMetersPerUnit(stage),_impl->textures,pose.warnings));
        else if(prim.HasAPI<UsdLuxLightAPI>())warn(pose.warnings,name+": unsupported light type "+type+".");
        else if(prim.IsA<UsdGeomCamera>()) {
        }else if(type=="Cube" || type=="Sphere" || type=="Cylinder" || type=="Cone" || type=="Capsule" || type=="BasisCurves" || type=="NurbsPatch" || type=="Points")warn(pose.warnings,name+": unsupported geometry type "+type+" (convert to Mesh).");
    }
    return pose;
}
Camera UsdSceneAsset::camera(const std::string& path,double seconds,bool animated) const {
    std::lock_guard<std::mutex> lock(usdMutex);UsdPrim chosen;
    for(const auto& name:_impl->cameraNames)if(path.empty() || name==path || SdfPath(name).GetName()==path) {
        if(chosen && !path.empty())throw std::runtime_error("Ambiguous USD camera: "+path);
        chosen=_impl->stage->GetPrimAtPath(SdfPath(name));if(path.empty())break;
    }
    if(!chosen)throw std::runtime_error("USD camera not found: "+path);return readCamera(UsdGeomCamera(chosen),_impl->time(seconds,animated),_impl->stage);
}
const std::vector<std::string>& UsdSceneAsset::cameras() const{return _impl->cameraNames;}
const std::vector<std::string>& UsdSceneAsset::meshPaths() const{return _impl->meshNames;}
size_t UsdSceneAsset::lightCount(const std::string& path) const {
    if(path.empty() || path=="/")return _impl->lightNames.size();
    return std::count_if(_impl->lightNames.begin(),_impl->lightNames.end(),[&](const std::string& name){return name==path || name.rfind(path+"/",0)==0;});
}
double UsdSceneAsset::startTime() const{return _impl->stage->GetStartTimeCode();}
double UsdSceneAsset::endTime() const{return _impl->stage->GetEndTimeCode();}
double UsdSceneAsset::timeCodesPerSecond() const{return _impl->stage->GetTimeCodesPerSecond();}
double UsdSceneAsset::frameRate() const{return _impl->stage->GetFramesPerSecond();}
double UsdSceneAsset::metersPerUnit() const{return UsdGeomGetStageMetersPerUnit(_impl->stage);}
bool UsdSceneAsset::zUp() const{return UsdGeomGetStageUpAxis(_impl->stage)==UsdGeomTokens->z;}
uint64_t UsdSceneAsset::revision() const{return _impl->revision;}
void UsdSceneAsset::reload(){std::lock_guard<std::mutex> lock(usdMutex);_impl->textures.entries.clear();_impl->textures.images.clear();for(const auto& layer:_impl->stage->GetUsedLayers())if(!layer->IsAnonymous() && !layer->Reload(true))throw std::runtime_error("Cannot reload USD layer: "+layer->GetIdentifier());_impl->stage->Reload();_impl->selectVariants();_impl->metadata();_impl->revision=++usdRevision;}
#else
struct UsdSceneAsset::Impl {};
bool UsdSceneAsset::available(){return false;}
namespace {void unavailable(){throw std::runtime_error("OpenUSD support is not enabled in this build (NATRON_ENABLE_USD).");}}
std::shared_ptr<UsdSceneAsset> UsdSceneAsset::load(const std::string&,const std::string&,bool){unavailable();return {};}
UsdScenePose UsdSceneAsset::evaluate(double,bool,const std::string&) const{unavailable();return {};}
Camera UsdSceneAsset::camera(const std::string&,double,bool) const{unavailable();return {};}
const std::vector<std::string>& UsdSceneAsset::cameras() const{static const std::vector<std::string> empty;return empty;}
const std::vector<std::string>& UsdSceneAsset::meshPaths() const{static const std::vector<std::string> empty;return empty;}
size_t UsdSceneAsset::lightCount(const std::string&) const{return 0;}
double UsdSceneAsset::startTime() const{return 0;}
double UsdSceneAsset::endTime() const{return 0;}
double UsdSceneAsset::timeCodesPerSecond() const{return 24;}
double UsdSceneAsset::frameRate() const{return 24;}
double UsdSceneAsset::metersPerUnit() const{return 1;}
bool UsdSceneAsset::zUp() const{return false;}
uint64_t UsdSceneAsset::revision() const{return 0;}
void UsdSceneAsset::reload(){unavailable();}
#endif
UsdSceneAsset::UsdSceneAsset(std::unique_ptr<Impl> impl):_impl(std::move(impl)){}
UsdSceneAsset::~UsdSceneAsset()=default;
}}}
