// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdSceneBridge.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>
#include <cstdio>
#include <map>
#include <sstream>
#include <condition_variable>
#include <deque>
#include <future>
#include <thread>
#include <mutex>
#include <stdexcept>
#ifdef NATRON_HAS_USD
#define QT_NO_KEYWORDS
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdLux/distantLight.h>
#include <pxr/usd/usdLux/domeLight.h>
#include <pxr/usd/usdLux/lightAPI.h>
#include <pxr/usd/usdLux/shapingAPI.h>
#include <pxr/usd/usdLux/shadowAPI.h>
#include <pxr/usd/usdLux/sphereLight.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/connectableAPI.h>
PXR_NAMESPACE_USING_DIRECTIVE
#endif
namespace Natron { namespace Gpu { namespace Cards {
#ifdef NATRON_HAS_USD
namespace {
template<class T> void put(const UsdAttribute& a,const T& value) {T old;if(!a.Get(&old) || old!=value)a.Set(value);}
GfMatrix4d matrix(const Matrix& m) {GfMatrix4d result;for(int r=0;r<4;++r)for(int c=0;c<4;++c)result[r][c]=m.m[r*4+c];return result;}
void transform(const UsdGeomXformable& prim,const Matrix& m) {
    auto attr=prim.GetPrim().GetAttribute(TfToken("xformOp:transform"));
    if(!attr)attr=prim.AddTransformOp().GetAttr();put(attr,matrix(m));
}
const char* wrap(TextureAddress a) {switch(a){case TextureAddress::Clamp:return "clamp";case TextureAddress::Repeat:return "repeat";case TextureAddress::Mirror:return "mirror";default:return "black";}}
QByteArray rgbaBytes(const Texture& texture,bool radiance) {
    const auto& data=texture.pixelData();
    if(texture.width<=0 || texture.height<=0 || data.size()!=size_t(texture.width)*texture.height*4)throw std::runtime_error("Invalid USD texture");
    const size_t bytes=data.size()*sizeof(float);if(bytes>size_t(INT_MAX))throw std::runtime_error("USD texture exceeds the transfer limit");
    QByteArray out(qsizetype(bytes),Qt::Uninitialized);
    auto* to=reinterpret_cast<float*>(out.data());
    for(int y=0;y<texture.height;++y)for(int x=0;x<texture.width;++x) {
        const float* p=data.data()+(size_t(texture.height-1-y)*texture.width+x)*4;
        float* q=to+(size_t(y)*texture.width+x)*4;
        for(int c=0;c<4;++c)if(!std::isfinite(p[c]))throw std::runtime_error("Non-finite USD texture");
        for(int c=0;c<3;++c)q[c]=radiance || texture.materialSampling || texture.dataSampling?p[c]:(p[3]>0?p[c]/p[3]:0);
        q[3]=radiance?1:p[3];
    }
    return out;
}
// Uncompressed IEEE float TIFF preserves HDR, signed data maps and independent
// alpha without requiring another image library in Natron's USD exporter.
void writeTiff(const QString& path,int width,int height,const QByteArray& pixels) {
    QSaveFile file(path);if(!file.open(QIODevice::WriteOnly))throw std::runtime_error("Cannot write USD texture");
    QDataStream s(&file);s.setByteOrder(QDataStream::LittleEndian);
    constexpr quint16 count=12;constexpr quint32 bits=8+2+count*12+4,sampleFormat=bits+8,data=sampleFormat+8;
    s.writeRawData("II",2);s<<quint16(42)<<quint32(8)<<count;
    auto tag=[&](quint16 id,quint16 type,quint32 n,quint32 value){s<<id<<type<<n<<value;};
    tag(256,4,1,width);tag(257,4,1,height);tag(258,3,4,bits);tag(259,3,1,1);tag(262,3,1,2);
    tag(273,4,1,data);tag(277,3,1,4);tag(278,4,1,height);tag(279,4,1,quint32(pixels.size()));
    tag(284,3,1,1);tag(338,3,1,2);tag(339,3,4,sampleFormat);s<<quint32(0);
    for(int i=0;i<4;++i)s<<quint16(32);for(int i=0;i<4;++i)s<<quint16(3);
    s.writeRawData(pixels.constData(),int(pixels.size()));
    if(s.status()!=QDataStream::Ok || !file.commit())throw std::runtime_error("Cannot finish USD texture");
}
}
struct UsdSceneBridge::Impl {
    std::mutex mutex;QTemporaryDir directory;UsdStageRefPtr stage;
    std::string sourceKey;
    std::unique_ptr<QProcess> worker;QString workerExecutable;QByteArray pending,diagnostics;
    std::mutex ioMutex;std::condition_variable ioWake;std::deque<std::function<void()>> tasks;std::thread ioThread;bool stopping=false;
    Impl() {if(!directory.isValid())throw std::runtime_error("Cannot create the USD bridge directory");stage=UsdStage::CreateInMemory("NatronScene.usda");
        UsdGeomSetStageUpAxis(stage,UsdGeomTokens->y);UsdGeomSetStageMetersPerUnit(stage,1.);UsdGeomXform::Define(stage,SdfPath("/Natron"));stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/Natron")));
        ioThread=std::thread([this]{
            for(;;){std::function<void()> task;{std::unique_lock<std::mutex> lock(ioMutex);ioWake.wait(lock,[this]{return stopping || !tasks.empty();});
                if(stopping && tasks.empty())break;task=std::move(tasks.front());tasks.pop_front();}task();}
            if(worker && worker->state()!=QProcess::NotRunning){worker->write("{\"quit\":true}\n");worker->closeWriteChannel();if(!worker->waitForFinished(2000)){worker->kill();worker->waitForFinished(2000);}}
            worker.reset();
        });
    }
    ~Impl(){{std::lock_guard<std::mutex> lock(ioMutex);stopping=true;}ioWake.notify_one();ioThread.join();}
    template<class F> auto invoke(F&& f)->decltype(f()) {
        auto task=std::make_shared<std::packaged_task<decltype(f())()>>(std::forward<F>(f));auto result=task->get_future();
        {std::lock_guard<std::mutex> lock(ioMutex);tasks.push_back([task]{(*task)();});}ioWake.notify_one();return result.get();
    }
    SdfAssetPath texture(const std::shared_ptr<const Texture>& source,bool radiance=false) {
        const auto bytes=rgbaBytes(*source,radiance);QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(bytes);hash.addData(QByteArray::number(source->width)+":"+QByteArray::number(source->height));
        const QString path=directory.filePath(QStringLiteral("texture-")+QString::fromLatin1(hash.result().toHex())+QStringLiteral(".tif"));
        if(!QFileInfo::exists(path))writeTiff(path,source->width,source->height,bytes);
        return SdfAssetPath(path.toStdString());
    }
    UsdShadeShader sampler(const SdfPath& parent,const char* name,const std::shared_ptr<const Texture>& tex,const char* uv) {
        auto st=UsdShadeShader::Define(stage,parent.AppendChild(TfToken(std::string(name)+"UV")));
        put(st.CreateIdAttr(),TfToken("UsdPrimvarReader_float2"));put(st.CreateInput(TfToken("varname"),SdfValueTypeNames->Token).GetAttr(),TfToken(uv));
        auto reader=UsdShadeShader::Define(stage,parent.AppendChild(TfToken(name)));
        put(reader.CreateIdAttr(),TfToken("UsdUVTexture"));put(reader.CreateInput(TfToken("file"),SdfValueTypeNames->Asset).GetAttr(),texture(tex));
        put(reader.CreateInput(TfToken("sourceColorSpace"),SdfValueTypeNames->Token).GetAttr(),TfToken("raw"));
        put(reader.CreateInput(TfToken("wrapS"),SdfValueTypeNames->Token).GetAttr(),TfToken(wrap(tex->wrapS)));
        put(reader.CreateInput(TfToken("wrapT"),SdfValueTypeNames->Token).GetAttr(),TfToken(wrap(tex->wrapT)));
        reader.CreateInput(TfToken("st"),SdfValueTypeNames->Float2).ConnectToSource(st.ConnectableAPI(),TfToken("result"));
        return reader;
    }
    void material(const UsdPrim& target,const Card& card,const Material& surface,const SdfPath& path) {
        auto mat=UsdShadeMaterial::Define(stage,path);auto shader=UsdShadeShader::Define(stage,path.AppendChild(TfToken("Surface")));
        put(shader.CreateIdAttr(),TfToken("UsdPreviewSurface"));
        const GfVec3f base(surface.baseColor[0],surface.baseColor[1],surface.baseColor[2]);
        const float opacity=float(card.opacity)*surface.baseColor[3];
        auto input=shader.CreateInput(TfToken(surface.lit?"diffuseColor":"emissiveColor"),SdfValueTypeNames->Color3f);
        if(card.texture) {
            auto image=sampler(path,"Image",card.texture,"colorSt");put(image.CreateInput(TfToken("scale"),SdfValueTypeNames->Float4).GetAttr(),GfVec4f(base[0],base[1],base[2],opacity));
            input.ConnectToSource(image.ConnectableAPI(),TfToken("rgb"));
            shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float).ConnectToSource(image.ConnectableAPI(),TfToken("a"));
        } else {input.DisconnectSource();put(input.GetAttr(),base);auto a=shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float);a.DisconnectSource();put(a.GetAttr(),opacity);}
        auto inactive=shader.CreateInput(TfToken(surface.lit?"emissiveColor":"diffuseColor"),SdfValueTypeNames->Color3f);inactive.DisconnectSource();
        put(inactive.GetAttr(),surface.lit?GfVec3f(surface.emission.x,surface.emission.y,surface.emission.z):GfVec3f(0));
        put(shader.CreateInput(TfToken("ior"),SdfValueTypeNames->Float).GetAttr(),float(surface.ior));
        put(shader.CreateInput(TfToken("useSpecularWorkflow"),SdfValueTypeNames->Int).GetAttr(),surface.lit?0:1);
        put(shader.CreateInput(TfToken("specularColor"),SdfValueTypeNames->Color3f).GetAttr(),GfVec3f(0));
        const char* inputs[]{"normal","roughness","metallic"};const char* channels[]{"r","g","b","a"};
        for(int j=0;j<3;++j) {
            auto v=shader.CreateInput(TfToken(inputs[j]),j==0?SdfValueTypeNames->Normal3f:SdfValueTypeNames->Float);
            if(card.maps[j].texture) {auto map=sampler(path,inputs[j],card.maps[j].texture,("map"+std::to_string(j)).c_str());
                v.ConnectToSource(map.ConnectableAPI(),TfToken(j==0?"rgb":channels[std::min(3u,card.maps[j].channel)]));}
            else {v.DisconnectSource();if(j==0)put(v.GetAttr(),GfVec3f(0,0,1));else put(v.GetAttr(),float(j==1?surface.roughness:surface.metallic));}
        }
        shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token);mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),TfToken("surface"));
        UsdShadeMaterialBindingAPI::Apply(target).Bind(mat);
        target.AddAppliedSchema(TfToken("NatronCompositingAPI"));
        put(target.CreateAttribute(TfToken("natron:unlit"),SdfValueTypeNames->Bool),!surface.lit);
    }
    QString exportScene(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,const std::vector<UsdSceneSource>& sources) {
        if(settings.width<1 || settings.height<1 || !std::isfinite(settings.pixelAspect) || settings.pixelAspect<=0)throw std::runtime_error("Invalid USD render dimensions");
        if(sources.size()>1)throw std::runtime_error("USD / hdNVP currently supports one composed ReadUSD stage per Render3D. Combine additional USD assets through references in that stage.");
        const double meters=sources.empty()?1:sources.front().metersPerUnit;
        if(!std::isfinite(meters) || meters<=0)throw std::runtime_error("Invalid USD metersPerUnit");
        const auto axis=!sources.empty() && sources.front().zUp?UsdGeomTokens->z:UsdGeomTokens->y;
        const std::vector<std::string> layers=sources.empty()?std::vector<std::string>{}:std::vector<std::string>{sources.front().filename};
        const auto key=sources.empty()?std::string():sources.front().filename+"\n"+sources.front().variants;
        if(key!=sourceKey) {
            if(!sources.empty()) {
                const auto original=UsdStage::Open(sources.front().filename,UsdStage::LoadNone);
                if(!original)throw std::runtime_error("Cannot open composed USD stage");
                if(original->GetPrimAtPath(SdfPath("/Natron")))throw std::runtime_error("The source USD uses the reserved /Natron renderer namespace");
            }
            std::vector<SdfPath> overrides;for(const auto& prim:stage->GetRootLayer()->GetRootPrims())if(prim->GetName()!="Natron")overrides.push_back(prim->GetPath());
            for(const auto& path:overrides)stage->RemovePrim(path);sourceKey=key;
        }
        if(stage->GetRootLayer()->GetSubLayerPaths()!=layers) {
            stage->GetRootLayer()->SetSubLayerPaths(layers);
        }
        UsdGeomSetStageUpAxis(stage,axis);UsdGeomSetStageMetersPerUnit(stage,meters);
        stage->SetTimeCodesPerSecond(sources.empty()?24:sources.front().timeCodesPerSecond);
        const Matrix basis=Matrix::transform({},axis==UsdGeomTokens->z?Vec3{-90,0,0}:Vec3{},{meters,meters,meters});
        transform(UsdGeomXformable(stage->GetPrimAtPath(SdfPath("/Natron"))),basis.inverse());
        // The root is a stronger, private layer over the original USD. No mesh,
        // material, reference, instance, light or asset path is flattened.
        for(const auto& source:sources) {
            std::istringstream lines(source.variants);std::string line;
            while(std::getline(lines,line)) {
                const auto comment=line.find('#');if(comment!=std::string::npos)line.resize(comment);
                std::istringstream words(line);std::string path,choice,extra;if(!(words>>path))continue;
                if(!(words>>choice) || words>>extra)throw std::runtime_error("Invalid USD variant selection");
                const auto equal=choice.find('=');if(equal==std::string::npos)throw std::runtime_error("Invalid USD variant selection");
                auto prim=stage->GetPrimAtPath(SdfPath(path));if(!prim)throw std::runtime_error("USD variant prim is missing: "+path);
                auto set=prim.GetVariantSet(choice.substr(0,equal));const auto names=set.GetVariantNames();const auto selection=choice.substr(equal+1);
                if(std::find(names.begin(),names.end(),selection)==names.end() || !set.SetVariantSelection(selection))throw std::runtime_error("Unknown USD variant selection");
            }
        }
        const double aspect=double(settings.width)*settings.pixelAspect/settings.height;
        auto c=UsdGeomCamera::Define(stage,SdfPath("/Natron/Camera"));
        auto cameraTransform=basis.inverse()*camera.transform;
        // Convert camera position to source units while retaining a unit
        // rotation basis. Scale belongs to geometry, not the camera's view.
        for(int col=0;col<3;++col)for(int row=0;row<3;++row)cameraTransform.m[col*4+row]*=meters;
        transform(c,cameraTransform);c.SetResetXformStack(true);
        put(c.CreateProjectionAttr(),camera.orthographic?UsdGeomTokens->orthographic:UsdGeomTokens->perspective);
        put(c.CreateFocalLengthAttr(),float(camera.focal*.01/meters));
        const double aperture=(camera.orthographic?camera.orthoWidth*10:camera.aperture*.01)/meters;
        put(c.CreateHorizontalApertureAttr(),float(aperture));put(c.CreateVerticalApertureAttr(),float(aperture/aspect));
        put(c.CreateClippingRangeAttr(),GfVec2f(camera.nearClip/meters,camera.farClip/meters));
        UsdGeomXform::Define(stage,SdfPath("/Natron/Geometry"));UsdGeomXform::Define(stage,SdfPath("/Natron/Lights"));
        size_t i=0;
        for(const auto& card:cards) {
            if(card.projected)throw std::runtime_error("hdNVP camera projection requires the Natron projection adapter; use the existing renderer for this scene.");
            const auto path=SdfPath("/Natron/Geometry/Object"+std::to_string(i++));auto mesh=UsdGeomMesh::Define(stage,path);transform(mesh,card.transform);
            VtVec3fArray points,normals;VtIntArray counts,indices;std::array<VtVec2fArray,4> uvs;
            auto vertex=[&](const MeshVertex& v){indices.push_back(int(points.size()));points.emplace_back(v.position.x,v.position.y,v.position.z);normals.emplace_back(v.normal.x,v.normal.y,v.normal.z);
                uvs[0].emplace_back(v.uv[0],v.uv[1]);for(int j=0;j<3;++j)uvs[j+1].emplace_back(v.mapUvs[j][0],v.mapUvs[j][1]);};
            if(card.mesh)for(const auto& t:card.mesh->triangles){counts.push_back(3);for(const auto& v:t.vertices)vertex(v);}
            else {counts.push_back(4);const std::array<std::array<double,2>,4> corners{{{0,0},{1,0},{1,1},{0,1}}};
                for(auto uv:corners){MeshVertex v;v.position={(uv[0]-.5)*card.width,(uv[1]-.5)*card.height,0};v.normal={0,0,1};v.uv=uv;for(auto& map:v.mapUvs)map=uv;vertex(v);}}
            auto mapUvs=[](VtVec2fArray& coords,const std::shared_ptr<const Texture>& texture){if(!texture)return;
                const auto& t=*texture;const auto& m=t.uvTransform;for(auto& uv:coords){const double u=uv[0],v=uv[1];
                    uv=GfVec2f((m[0]*u+m[1]*v+m[2])*t.imageUvScale[0]+t.imageUvOffset[0],(m[3]*u+m[4]*v+m[5])*t.imageUvScale[1]+t.imageUvOffset[1]);}};
            auto colorUvs=uvs[0];mapUvs(colorUvs,card.texture);
            for(int j=0;j<3;++j){if(card.maps[j].useMeshUv)uvs[j+1]=uvs[0];mapUvs(uvs[j+1],card.maps[j].texture);}
            put(mesh.CreatePointsAttr(),points);put(mesh.CreateNormalsAttr(),normals);mesh.SetNormalsInterpolation(UsdGeomTokens->vertex);
            put(mesh.CreateFaceVertexCountsAttr(),counts);put(mesh.CreateFaceVertexIndicesAttr(),indices);
            put(mesh.CreateSubdivisionSchemeAttr(),UsdGeomTokens->none);put(mesh.CreateDoubleSidedAttr(),true);
            UsdGeomPrimvarsAPI pv(mesh);for(int j=0;j<4;++j)put(pv.CreatePrimvar(TfToken(j==0?"st":"map"+std::to_string(j-1)),SdfValueTypeNames->TexCoord2fArray,UsdGeomTokens->vertex).GetAttr(),uvs[j]);
            put(pv.CreatePrimvar(TfToken("colorSt"),SdfValueTypeNames->TexCoord2fArray,UsdGeomTokens->vertex).GetAttr(),colorUvs);
            material(mesh.GetPrim(),card,card.material,path.AppendChild(TfToken("Material")));
            size_t groupCount=0;
            if(card.mesh) {
                std::map<std::array<double,12>,size_t> groups;std::vector<Material> materials;std::vector<VtIntArray> faces;
                for(size_t face=0;face<card.mesh->triangles.size();++face) {
                    const auto& triangle=card.mesh->triangles[face];auto surface=card.overrideMaterial?card.material:triangle.material;
                    for(const auto& v:triangle.vertices)if(v.color!=triangle.vertices[0].color)throw std::runtime_error("Native mesh vertex color gradients require the USD primvar adapter; import the original USD directly for now.");
                    for(int c=0;c<4;++c)surface.baseColor[c]*=triangle.vertices[0].color[c]*((c==3 || !card.texture)?triangle.color[c]:1.f);
                    const std::array<double,12> key{double(surface.lit),surface.baseColor[0],surface.baseColor[1],surface.baseColor[2],surface.baseColor[3],surface.emission.x,surface.emission.y,surface.emission.z,surface.metallic,surface.roughness,surface.ior,0};
                    auto group=groups.emplace(key,materials.size());if(group.second){materials.push_back(surface);faces.emplace_back();}faces[group.first->second].push_back(int(face));
                }
                groupCount=materials.size();
                for(size_t group=0;group<groupCount;++group) {
                    auto subset=UsdGeomSubset::Define(stage,path.AppendChild(TfToken("Faces"+std::to_string(group))));
                    put(subset.CreateElementTypeAttr(),UsdGeomTokens->face);put(subset.CreateFamilyNameAttr(),TfToken("materialBind"));put(subset.CreateIndicesAttr(),faces[group]);
                    material(subset.GetPrim(),card,materials[group],path.AppendChild(TfToken("MaterialGroup"+std::to_string(group))));
                }
                UsdGeomSubset::SetFamilyType(mesh,TfToken("materialBind"),UsdGeomTokens->partition);
            }
            for(const auto& child:mesh.GetPrim().GetChildren()) {
                const auto name=child.GetName().GetString();const size_t prefix=name.rfind("Faces",0)==0?5:name.rfind("MaterialGroup",0)==0?13:0;
                if(prefix && std::stoull(name.substr(prefix))>=groupCount)stage->RemovePrim(child.GetPath());
            }
        }
        for(const auto& child:stage->GetPrimAtPath(SdfPath("/Natron/Geometry")).GetChildren()) {
            const auto name=child.GetName().GetString();if(name.rfind("Object",0)==0 && std::stoull(name.substr(6))>=i)stage->RemovePrim(child.GetPath());
        }
        i=0;for(const auto& light:settings.lights) {
            const auto path=SdfPath("/Natron/Lights/Light"+std::to_string(i++));UsdPrim prim;
            if(light.type==LightType::Environment) {auto dome=UsdLuxDomeLight::Define(stage,path);prim=dome.GetPrim();
                if(light.environment)put(dome.CreateTextureFileAttr(),texture(light.environment,true));else put(dome.CreateTextureFileAttr(),SdfAssetPath());
                put(dome.CreateTextureFormatAttr(),TfToken("latlong"));
                dome.GetTextureFileAttr().SetColorSpace(TfToken("raw"));
            } else if(light.type==LightType::Distant)prim=UsdLuxDistantLight::Define(stage,path).GetPrim();
            else {auto sphere=UsdLuxSphereLight::Define(stage,path);prim=sphere.GetPrim();put(sphere.CreateRadiusAttr(),0.f);put(sphere.CreateTreatAsPointAttr(),true);
                if(light.type==LightType::Spot){auto shaping=UsdLuxShapingAPI::Apply(prim);put(shaping.CreateShapingConeAngleAttr(),float(light.coneAngle));put(shaping.CreateShapingConeSoftnessAttr(),float(light.coneSoftness));}}
            transform(UsdGeomXformable(prim),light.transform);UsdLuxLightAPI api(prim);
            put(api.CreateNormalizeAttr(),light.type==LightType::Distant);
            put(api.CreateColorAttr(),GfVec3f(light.color.x,light.color.y,light.color.z));put(api.CreateIntensityAttr(),float(light.intensity));put(api.CreateExposureAttr(),float(light.exposure));
            put(UsdLuxShadowAPI::Apply(prim).CreateShadowEnableAttr(),light.shadows);
        }
        for(const auto& child:stage->GetPrimAtPath(SdfPath("/Natron/Lights")).GetChildren())if(std::stoull(child.GetName().GetString().substr(5))>=i)stage->RemovePrim(child.GetPath());
        const QString path=directory.filePath(QStringLiteral("scene.usda"));if(!stage->GetRootLayer()->Export(path.toStdString()))throw std::runtime_error("Cannot export USD scene");return path;
    }
};
std::string UsdSceneBridge::hostPath() {
    const auto env=qEnvironmentVariable("NATRON_HDNVP_HOST");if(!env.isEmpty())return env.toStdString();
    const QDir app(QCoreApplication::applicationDirPath());
    for(const auto& relative:{QStringLiteral("../hdnvp-host/runtime/NatronHdNvpHost.exe"),QStringLiteral("../../hdnvp-host/runtime/NatronHdNvpHost.exe"),QStringLiteral("hdnvp/NatronHdNvpHost.exe")}) {
        const QString path=app.absoluteFilePath(relative);if(QFileInfo::exists(path))return QDir::cleanPath(path).toStdString();
    }
    return {};
}
bool UsdSceneBridge::available(){return QFileInfo::exists(QString::fromStdString(hostPath()));}
UsdSceneBridge::UsdSceneBridge():_impl(new Impl){}
UsdSceneBridge::~UsdSceneBridge()=default;
std::string UsdSceneBridge::exportScene(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,const std::vector<UsdSceneSource>& sources) {
    std::lock_guard<std::mutex> lock(_impl->mutex);return _impl->exportScene(cards,camera,settings,sources).toStdString();
}
HdNvpFrame UsdSceneBridge::render(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,const HdNvpSettings& quality,const std::function<bool()>& abort,const std::vector<UsdSceneSource>& sources) {
    std::lock_guard<std::mutex> lock(_impl->mutex);const QString host=QString::fromStdString(hostPath());
    if(host.isEmpty() || !QFileInfo::exists(host))throw std::runtime_error("Build the hdNVP host with tools/build-hdnvp-host.ps1 or set NATRON_HDNVP_HOST.");
    const QString scene=_impl->exportScene(cards,camera,settings,sources),output=_impl->directory.filePath(QStringLiteral("aovs"));
    // Natron renders from different pool threads. QProcess and its notifiers
    // stay on one dedicated thread for the entire lifetime of this bridge.
    return _impl->invoke([&]() {
    if(!_impl->worker || _impl->worker->state()==QProcess::NotRunning || _impl->workerExecutable!=host) {
        _impl->worker=std::make_unique<QProcess>();_impl->workerExecutable=host;_impl->pending.clear();_impl->diagnostics.clear();
        auto env=QProcessEnvironment::systemEnvironment();const QString runtime=QFileInfo(host).absolutePath();
        env.insert(QStringLiteral("PATH"),runtime+QStringLiteral(";")+env.value(QStringLiteral("PATH")));env.insert(QStringLiteral("PXR_MTLX_STDLIB_SEARCH_PATHS"),runtime+QStringLiteral("/libraries"));
        _impl->worker->setProcessEnvironment(env);_impl->worker->start(host,{runtime});
        if(!_impl->worker->waitForStarted(10000))throw std::runtime_error("Cannot start the hdNVP host");
    }
    QString denoiser;
    switch(quality.denoiser) {
        case HdNvpDenoiser::Auto:denoiser=QStringLiteral("auto");break;
        case HdNvpDenoiser::Oidn:denoiser=QStringLiteral("oidn");break;
        case HdNvpDenoiser::Dlss:denoiser=QStringLiteral("dlss");break;
        case HdNvpDenoiser::None:denoiser=QStringLiteral("none");break;
        default:throw std::runtime_error("Invalid hdNVP denoiser");
    }
    const QJsonObject request{{QStringLiteral("scene"),scene},{QStringLiteral("output"),output},{QStringLiteral("time"),sources.empty()?1:sources.front().timeCode},{QStringLiteral("loadPayloads"),sources.empty() || sources.front().loadPayloads},{QStringLiteral("width"),settings.width},{QStringLiteral("height"),settings.height},{QStringLiteral("samples"),quality.samples},{QStringLiteral("bounces"),quality.bounces},{QStringLiteral("denoiser"),denoiser},{QStringLiteral("interactive"),quality.interactive}};
    auto message=request;message.insert(QStringLiteral("sourceRevision"),QString::number(sources.empty()?0:sources.front().revision));
    _impl->worker->write(QJsonDocument(message).toJson(QJsonDocument::Compact)+'\n');QElapsedTimer timer;timer.start();QJsonObject response;
    for(;;) {
        if((abort && abort()) || timer.elapsed()>310000) {_impl->worker->kill();_impl->worker->waitForFinished(2000);throw std::runtime_error("hdNVP rendering cancelled or timed out");}
        _impl->worker->waitForReadyRead(25);_impl->pending+=_impl->worker->readAllStandardOutput();
        const auto diagnostics=_impl->worker->readAllStandardError();_impl->diagnostics+=diagnostics;
        if(qEnvironmentVariableIsSet("NATRON_HDNVP_TRACE"))fprintf(stderr,"%s",diagnostics.constData());
        if(_impl->diagnostics.size()>16384)_impl->diagnostics=_impl->diagnostics.right(16384);
        int newline;while((newline=int(_impl->pending.indexOf('\n')))>=0) {const auto line=_impl->pending.left(newline);_impl->pending.remove(0,newline+1);
            if(line.startsWith("NATRON_HDNVP ")){response=QJsonDocument::fromJson(line.mid(13)).object();break;}}
        if(!response.isEmpty())break;
        if(_impl->worker->state()==QProcess::NotRunning)throw std::runtime_error("hdNVP host stopped: "+_impl->diagnostics.toStdString());
    }
    if(!response.value(QStringLiteral("ok")).toBool())throw std::runtime_error("hdNVP: "+response.value(QStringLiteral("error")).toString().toStdString());
    HdNvpFrame frame;frame.report=QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString();
    auto read=[&](const char* name,auto& values,size_t count){QFile file(QDir(output).filePath(QString::fromLatin1(name)));if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Missing hdNVP AOV");
        const auto data=file.readAll();if(size_t(data.size())!=count*sizeof(values[0]))throw std::runtime_error("Invalid hdNVP AOV size");values.resize(count);std::memcpy(values.data(),data.constData(),data.size());};
    const size_t pixels=size_t(settings.width)*settings.height;read("color.raw",frame.color,pixels*4);read("depth.raw",frame.depth,pixels);read("primId.raw",frame.primIds,pixels);read("instanceId.raw",frame.instanceIds,pixels);
    if(!sources.empty())for(auto& depth:frame.depth)depth*=float(sources.front().metersPerUnit);
    return frame;
    });
}
#else
struct UsdSceneBridge::Impl {};
UsdSceneBridge::UsdSceneBridge():_impl(new Impl){}UsdSceneBridge::~UsdSceneBridge()=default;
bool UsdSceneBridge::available(){return false;}std::string UsdSceneBridge::hostPath(){return {};}
std::string UsdSceneBridge::exportScene(const std::vector<Card>&,const Camera&,const Settings&,const std::vector<UsdSceneSource>&){throw std::runtime_error("OpenUSD is unavailable");}
HdNvpFrame UsdSceneBridge::render(const std::vector<Card>&,const Camera&,const Settings&,const HdNvpSettings&,const std::function<bool()>&,const std::vector<UsdSceneSource>&){throw std::runtime_error("OpenUSD is unavailable");}
#endif
}}}
