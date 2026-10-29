// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdSceneBridge.h"
#include "Gpu/Compositing.h"
#include "Gpu/HdNvpFrameProtocol.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <climits>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <condition_variable>
#include <deque>
#include <future>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
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
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUuid>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/sdf/copyUtils.h>
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
#include <pxr/usd/usdShade/nodeGraph.h>
#include <pxr/usd/usdShade/connectableAPI.h>
PXR_NAMESPACE_USING_DIRECTIVE
#endif
namespace Natron { namespace Gpu { namespace Cards {
std::string HdNvpFrame::pickPath(int32_t prim,int32_t instance)const {
    for(const auto& pick:picks)if(pick.prim==prim && pick.instance==instance)return pick.path;
    return {};
}
#ifdef NATRON_HAS_USD
namespace {
void clearTransparentRgb(std::vector<float>& color) {
    // Denoisers can spread foreground color across a holdout or transparent edge.
    for(size_t i=0;i+3<color.size();i+=4)if(color[i+3]<=0)color[i]=color[i+1]=color[i+2]=0;
}
template<class T> void put(const UsdAttribute& a,const T& value) {T old;if(!a.Get(&old) || old!=value)a.Set(value);}
GfMatrix4d matrix(const Matrix& m) {GfMatrix4d result;for(int r=0;r<4;++r)for(int c=0;c<4;++c)result[r][c]=m.m[r*4+c];return result;}
void transform(const UsdGeomXformable& prim,const Matrix& m) {
    auto attr=prim.GetPrim().GetAttribute(TfToken("xformOp:transform"));
    if(!attr)attr=prim.AddTransformOp().GetAttr();put(attr,matrix(m));
}
const char* wrap(TextureAddress a) {switch(a){case TextureAddress::Clamp:return "clamp";case TextureAddress::Repeat:return "repeat";case TextureAddress::Mirror:return "mirror";default:return "black";}}
bool same(Vec3 a,Vec3 b){return a.x==b.x && a.y==b.y && a.z==b.z;}
bool same(const Camera& a,const Camera& b){return a.transform.m==b.transform.m && a.focal==b.focal && a.aperture==b.aperture && a.apertureAspect==b.apertureAspect && a.orthographic==b.orthographic && a.orthoWidth==b.orthoWidth && a.nearClip==b.nearClip && a.farClip==b.farClip && a.offsetX==b.offsetX && a.offsetY==b.offsetY && a.ownerId==b.ownerId && a.label==b.label;}
bool same(const UsdSceneSource& a,const UsdSceneSource& b){return a.filename==b.filename && a.variants==b.variants && a.ownerId==b.ownerId && a.label==b.label && a.mountPath==b.mountPath && a.primPath==b.primPath && a.transform.m==b.transform.m && a.timeCode==b.timeCode && a.metersPerUnit==b.metersPerUnit && a.timeCodesPerSecond==b.timeCodesPerSecond && a.zUp==b.zUp && a.loadPayloads==b.loadPayloads && a.revision==b.revision;}
bool same(const UsdPrimEdit& a,const UsdPrimEdit& b){return a.kind==b.kind && a.identity==b.identity && a.paths==b.paths && a.transform.m==b.transform.m && a.visible==b.visible && a.materialPath==b.materialPath && a.cameraVisible==b.cameraVisible && a.shadowVisible==b.shadowVisible && a.reflectionVisible==b.reflectionVisible && a.holdout==b.holdout && a.role==b.role && a.shadowDensity==b.shadowDensity && same(a.shadowTint,b.shadowTint) && a.reflectionStrength==b.reflectionStrength && a.reflectionRoughness==b.reflectionRoughness;}
bool same(const Compositing& a,const Compositing& b){return a.role==b.role && a.cameraVisible==b.cameraVisible && a.shadowVisible==b.shadowVisible && a.reflectionVisible==b.reflectionVisible && a.holdout==b.holdout && a.shadowDensity==b.shadowDensity && same(a.shadowTint,b.shadowTint) && a.reflectionStrength==b.reflectionStrength && a.reflectionRoughness==b.reflectionRoughness;}
bool same(const Card& a,const Card& b) {
    if(!same(a.compositing,b.compositing))return false;
    if(a.transform.m!=b.transform.m || a.width!=b.width || a.height!=b.height || a.opacity!=b.opacity || a.texture!=b.texture || a.mesh!=b.mesh || a.projected!=b.projected || a.projectionAspect!=b.projectionAspect || a.projectionFrozen!=b.projectionFrozen || a.projectionReferenceTime!=b.projectionReferenceTime || a.projectionImageTime!=b.projectionImageTime || !same(a.projector,b.projector) || a.label!=b.label || a.ownerId!=b.ownerId || a.instanceId!=b.instanceId || a.overrideMaterial!=b.overrideMaterial || a.bindingPaths!=b.bindingPaths)return false;
    const auto& x=a.material;const auto& y=b.material;if(x.lit!=y.lit || x.baseColor!=y.baseColor || !same(x.emission,y.emission) || x.metallic!=y.metallic || x.roughness!=y.roughness || x.ior!=y.ior)return false;
    for(int i=0;i<3;++i)if(a.maps[i].texture!=b.maps[i].texture || a.maps[i].channel!=b.maps[i].channel || a.maps[i].useMeshUv!=b.maps[i].useMeshUv)return false;return true;
}
bool same(const Light& a,const Light& b){return a.type==b.type && a.transform.m==b.transform.m && same(a.color,b.color) && a.intensity==b.intensity && a.exposure==b.exposure && a.coneAngle==b.coneAngle && a.coneSoftness==b.coneSoftness && a.shadows==b.shadows && a.shadowBias==b.shadowBias && a.ownerId==b.ownerId && a.label==b.label && a.instanceId==b.instanceId && a.environment==b.environment && a.environmentSamples==b.environmentSamples;}
template<class T> bool same(const std::vector<T>& a,const std::vector<T>& b){return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const T& x,const T& y){return same(x,y);});}
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
    std::string geometryText;uint64_t geometryRevision=0;bool probeFiles=false;
    std::string publishedText;
    std::vector<UsdSceneSource> publishedSources;
    std::vector<UsdPrimEdit> publishedEdits;
    std::vector<Card> publishedCards;
    std::vector<Light> publishedLights;
    Camera publishedCamera;
    double publishedAspect=0;
    std::map<SdfPath,Card> cardCache;
    std::mutex mutex;QTemporaryDir directory;UsdStageRefPtr stage;
    std::shared_ptr<const UsdSceneValue> value;uint64_t revision=0;
    std::unique_ptr<QProcess> worker;QString workerExecutable;QByteArray pending,diagnostics;
    std::mutex ioMutex;std::condition_variable ioWake;std::deque<std::function<void()>> tasks;std::thread ioThread;bool stopping=false;
#ifdef _WIN32
    HANDLE mappingHandle=nullptr;void* mappingData=nullptr;size_t mappingCapacity=0;
    std::string mappingName="Local\\NatronHdNvp_"+QUuid::createUuid().toString(QUuid::Id128).toStdString();
    bool mapping(size_t pixels,int width,int height,uint64_t revision) {
        const auto bytes=NatronHdNvp::bytes(pixels);
        if(bytes>size_t(512)*1024*1024)return false;
        if(bytes>mappingCapacity) {
            if(mappingData)UnmapViewOfFile(mappingData);if(mappingHandle)CloseHandle(mappingHandle);mappingData=nullptr;mappingHandle=nullptr;mappingCapacity=0;
            mappingHandle=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,DWORD(bytes),mappingName.c_str());
            if(!mappingHandle || GetLastError()==ERROR_ALREADY_EXISTS)throw std::runtime_error("Cannot allocate isolated hdNVP frame mapping");
            mappingData=MapViewOfFile(mappingHandle,FILE_MAP_ALL_ACCESS,0,0,bytes);if(!mappingData)throw std::runtime_error("Cannot map hdNVP frame memory");mappingCapacity=bytes;
        }
        auto header=new(mappingData) NatronHdNvp::FrameHeader;header->pixels=pixels;header->width=uint32_t(width);header->height=uint32_t(height);header->revision=revision;return true;
    }
#endif
    Impl() {if(!directory.isValid())throw std::runtime_error("Cannot create the USD bridge directory");stage=UsdStage::CreateInMemory("NatronScene.usda");
        UsdGeomSetStageUpAxis(stage,UsdGeomTokens->y);UsdGeomSetStageMetersPerUnit(stage,1.);UsdGeomXform::Define(stage,SdfPath("/Natron"));stage->SetDefaultPrim(stage->GetPrimAtPath(SdfPath("/Natron")));
        ioThread=std::thread([this]{
            for(;;){std::function<void()> task;{std::unique_lock<std::mutex> lock(ioMutex);ioWake.wait(lock,[this]{return stopping || !tasks.empty();});
                if(stopping && tasks.empty())break;task=std::move(tasks.front());tasks.pop_front();}task();}
            if(worker && worker->state()!=QProcess::NotRunning){worker->write("{\"quit\":true}\n");worker->closeWriteChannel();if(!worker->waitForFinished(2000)){worker->kill();worker->waitForFinished(2000);}}
            worker.reset();
        });
    }
    ~Impl(){{std::lock_guard<std::mutex> lock(ioMutex);stopping=true;}ioWake.notify_one();ioThread.join();
#ifdef _WIN32
        if(mappingData)UnmapViewOfFile(mappingData);if(mappingHandle)CloseHandle(mappingHandle);
#endif
    }
    template<class F> auto invoke(F&& f,const std::function<bool()>& abort,const std::shared_ptr<std::atomic<bool>>& cancelled,const std::function<void()>& tick={})->decltype(f()) {
        auto task=std::make_shared<std::packaged_task<decltype(f())()>>(std::forward<F>(f));auto result=task->get_future();
        {std::lock_guard<std::mutex> lock(ioMutex);tasks.push_back([task]{(*task)();});}ioWake.notify_one();
        // Natron's abort predicate uses the calling render thread's TLS. Poll
        // there and pass only an atomic cancellation flag to the process thread.
        std::exception_ptr callbackError;
        while(result.wait_for(std::chrono::milliseconds(25))!=std::future_status::ready) {
            if(abort && !cancelled->load())try {if(abort())cancelled->store(true);}catch(...) {callbackError=std::current_exception();cancelled->store(true);}
            if(tick && !cancelled->load())try{tick();}catch(...){callbackError=std::current_exception();cancelled->store(true);}
        }
        // Always finish the task before releasing references captured by it.
        if(callbackError){try{result.get();}catch(...){}std::rethrow_exception(callbackError);}
        return result.get();
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
        if(std::string(uv)=="st") {
            auto transform=UsdShadeShader::Define(stage,parent.AppendChild(TfToken(std::string(name)+"UVTransform")));
            put(transform.CreateIdAttr(),TfToken("UsdTransform2d"));
            transform.CreateInput(TfToken("in"),SdfValueTypeNames->Float2).ConnectToSource(st.ConnectableAPI(),TfToken("result"));
            put(transform.CreateInput(TfToken("scale"),SdfValueTypeNames->Float2).GetAttr(),GfVec2f(tex->imageUvScale[0],tex->imageUvScale[1]));
            put(transform.CreateInput(TfToken("translation"),SdfValueTypeNames->Float2).GetAttr(),GfVec2f(tex->imageUvOffset[0],tex->imageUvOffset[1]));
            reader.CreateInput(TfToken("st"),SdfValueTypeNames->Float2).ConnectToSource(transform.ConnectableAPI(),TfToken("result"));
        }
        return reader;
    }
    std::pair<UsdShadeOutput,UsdShadeOutput> projection(const SdfPath& materialPath,const Card& card,const GfVec3f& base,float opacity) {
        // MaterialX position is evaluated at each ray hit, including secondary
        // rays. Perspective division therefore does not depend on tessellation.
        const auto root=materialPath.AppendChild(TfToken("Projection"));auto graph=UsdShadeNodeGraph::Define(stage,root);
        auto node=[&](const char* name,const char* id,const SdfValueTypeName& type){auto n=UsdShadeShader::Define(stage,root.AppendChild(TfToken(name)));put(n.CreateIdAttr(),TfToken(id));n.CreateOutput(TfToken("out"),type);return n;};
        auto connect=[](UsdShadeShader to,const char* input,const SdfValueTypeName& type,const UsdShadeShader& from){to.CreateInput(TfToken(input),type).ConnectToSource(from.ConnectableAPI(),TfToken("out"));};
        auto scalar=[&](UsdShadeShader n,const char* input,double value){put(n.CreateInput(TfToken(input),SdfValueTypeNames->Float).GetAttr(),float(value));};
        auto binary=[&](const char* name,const char* id,const UsdShadeShader& a,double b){auto n=node(name,id,SdfValueTypeNames->Float);connect(n,"in1",SdfValueTypeNames->Float,a);scalar(n,"in2",b);return n;};
        const double meters=UsdGeomGetStageMetersPerUnit(stage);
        const auto basis=Matrix::transform({},UsdGeomGetStageUpAxis(stage)==UsdGeomTokens->z?Vec3{-90,0,0}:Vec3{},{meters,meters,meters});
        const auto inverse=card.projector.transform.inverse()*basis;
        auto position=node("Position","ND_position_vector3",SdfValueTypeNames->Vector3f);put(position.CreateInput(TfToken("space"),SdfValueTypeNames->String).GetAttr(),std::string("world"));
        std::array<UsdShadeShader,3> q;
        const char* axes[]{"X","Y","Depth"};
        for(int i=0;i<3;++i) {
            const double sign=i==2?-1:1;
            auto dot=node((std::string(axes[i])+"Dot").c_str(),"ND_dotproduct_vector3",SdfValueTypeNames->Float);connect(dot,"in1",SdfValueTypeNames->Vector3f,position);
            put(dot.CreateInput(TfToken("in2"),SdfValueTypeNames->Vector3f).GetAttr(),GfVec3f(sign*inverse.m[i],sign*inverse.m[4+i],sign*inverse.m[8+i]));
            q[i]=binary(axes[i],"ND_add_float",dot,sign*inverse.m[12+i]);
        }
        auto denominator=binary("SafeDepth","ND_max_float",q[2],std::max(1e-8,card.projector.nearClip));
        const double tx=card.projector.orthographic?card.projector.orthoWidth*.5:card.projector.aperture/(2*card.projector.focal),ty=tx/card.projector.filmAspect(card.projectionAspect);
        std::array<UsdShadeShader,2> uv,gateUv;
        for(int i=0;i<2;++i) {
            auto v=q[i];const auto prefix=i==0?std::string("U"):std::string("V");
            auto division=node((prefix+"Divide").c_str(),"ND_divide_float",SdfValueTypeNames->Float);connect(division,"in1",SdfValueTypeNames->Float,v);connect(division,"in2",SdfValueTypeNames->Float,denominator);
            // Retain the same graph topology when camera projection changes.
            auto mixed=node((prefix+"Projection").c_str(),"ND_mix_float",SdfValueTypeNames->Float);connect(mixed,"fg",SdfValueTypeNames->Float,v);connect(mixed,"bg",SdfValueTypeNames->Float,division);scalar(mixed,"mix",card.projector.orthographic?1:0);v=mixed;
            v=binary((prefix+"Scale").c_str(),"ND_multiply_float",v,1/(2*(i==0?tx:ty)));
            v=binary((prefix+"Center").c_str(),"ND_add_float",v,.5-(i==0?card.projector.offsetX:card.projector.offsetY*card.projector.filmAspect(card.projectionAspect)));
            gateUv[i]=v;
            v=binary((prefix+"DataScale").c_str(),"ND_multiply_float",v,card.texture?card.texture->imageUvScale[i]:1);v=binary((prefix+"DataOffset").c_str(),"ND_add_float",v,card.texture?card.texture->imageUvOffset[i]:0);
            uv[i]=v;
        }
        auto coords=node("Coordinates","ND_combine2_vector2",SdfValueTypeNames->Float2);for(int i=0;i<2;++i)connect(coords,i==0?"in1":"in2",SdfValueTypeNames->Float,uv[i]);
        auto image=node("Image","ND_image_color4",SdfValueTypeNames->Color4f);
        put(image.CreateInput(TfToken("file"),SdfValueTypeNames->Asset).GetAttr(),card.texture?texture(card.texture):SdfAssetPath());image.GetInput(TfToken("file")).GetAttr().SetColorSpace(TfToken("raw"));
        connect(image,"texcoord",SdfValueTypeNames->Float2,coords);put(image.CreateInput(TfToken("uaddressmode"),SdfValueTypeNames->String).GetAttr(),std::string("constant"));put(image.CreateInput(TfToken("vaddressmode"),SdfValueTypeNames->String).GetAttr(),std::string("constant"));
        auto rgb=node("RGB","ND_convert_color4_color3",SdfValueTypeNames->Color3f);connect(rgb,"in",SdfValueTypeNames->Color4f,image);
        auto color=node("Color","ND_multiply_color3",SdfValueTypeNames->Color3f);connect(color,"in1",SdfValueTypeNames->Color3f,rgb);put(color.CreateInput(TfToken("in2"),SdfValueTypeNames->Color3f).GetAttr(),base);
        auto alpha=node("Alpha","ND_extract_color4",SdfValueTypeNames->Float);connect(alpha,"in",SdfValueTypeNames->Color4f,image);put(alpha.CreateInput(TfToken("index"),SdfValueTypeNames->Int).GetAttr(),3);
        auto coverage=binary("Opacity","ND_multiply_float",alpha,opacity);
        auto clipNear=node("Near","ND_ifgreater_float",SdfValueTypeNames->Float);connect(clipNear,"value1",SdfValueTypeNames->Float,q[2]);scalar(clipNear,"value2",card.projector.nearClip);scalar(clipNear,"in1",1);scalar(clipNear,"in2",0);
        auto clipFar=node("Far","ND_ifgreater_float",SdfValueTypeNames->Float);scalar(clipFar,"value1",card.projector.farClip);connect(clipFar,"value2",SdfValueTypeNames->Float,q[2]);connect(clipFar,"in1",SdfValueTypeNames->Float,clipNear);scalar(clipFar,"in2",0);
        auto gate=clipFar;
        for(int i=0;i<2;++i) {
            const auto prefix=i==0?std::string("U"):std::string("V");
            auto lower=node((prefix+"InsideMin").c_str(),"ND_ifgreater_float",SdfValueTypeNames->Float);connect(lower,"value1",SdfValueTypeNames->Float,gateUv[i]);scalar(lower,"value2",0);connect(lower,"in1",SdfValueTypeNames->Float,gate);scalar(lower,"in2",0);
            auto upper=node((prefix+"InsideMax").c_str(),"ND_ifgreater_float",SdfValueTypeNames->Float);scalar(upper,"value1",1);connect(upper,"value2",SdfValueTypeNames->Float,gateUv[i]);connect(upper,"in1",SdfValueTypeNames->Float,lower);scalar(upper,"in2",0);gate=upper;
        }
        auto clipped=node("Coverage","ND_multiply_float",SdfValueTypeNames->Float);connect(clipped,"in1",SdfValueTypeNames->Float,coverage);connect(clipped,"in2",SdfValueTypeNames->Float,gate);
        auto prim=stage->GetPrimAtPath(materialPath);prim.AddAppliedSchema(TfToken("NatronProjectionAPI"));put(prim.CreateAttribute(TfToken("natron:projection:aspect"),SdfValueTypeNames->Double),card.projectionAspect);
        put(prim.CreateAttribute(TfToken("natron:projection:worldToCamera"),SdfValueTypeNames->Matrix4d),matrix(inverse));
        put(prim.CreateAttribute(TfToken("natron:projection:image"),SdfValueTypeNames->Asset),card.texture?texture(card.texture):SdfAssetPath());
        put(prim.CreateAttribute(TfToken("natron:projection:timeMode"),SdfValueTypeNames->Token),TfToken(card.projectionFrozen?"frozen":"live"));
        put(prim.CreateAttribute(TfToken("natron:projection:referenceTime"),SdfValueTypeNames->Double),card.projectionReferenceTime);
        put(prim.CreateAttribute(TfToken("natron:projection:imageTime"),SdfValueTypeNames->Double),card.projectionImageTime);
        const auto cameraPath=materialPath.AppendChild(TfToken("ProjectionCamera"));auto camera=UsdGeomCamera::Define(stage,cameraPath);
        auto cameraMatrix=basis.inverse()*card.projector.transform;for(int col=0;col<3;++col)for(int row=0;row<3;++row)cameraMatrix.m[col*4+row]*=meters;
        transform(camera,cameraMatrix);camera.SetResetXformStack(true);
        const double aperture=(card.projector.orthographic?card.projector.orthoWidth*10:card.projector.aperture*.01)/meters;
        put(camera.CreateProjectionAttr(),card.projector.orthographic?UsdGeomTokens->orthographic:UsdGeomTokens->perspective);
        put(camera.CreateFocalLengthAttr(),float(card.projector.focal*.01/meters));put(camera.CreateHorizontalApertureAttr(),float(aperture));put(camera.CreateVerticalApertureAttr(),float(aperture/card.projector.filmAspect(card.projectionAspect)));
        put(camera.CreateHorizontalApertureOffsetAttr(),float(aperture*card.projector.offsetX));put(camera.CreateVerticalApertureOffsetAttr(),float(aperture*card.projector.offsetY));
        put(camera.CreateClippingRangeAttr(),GfVec2f(card.projector.nearClip/meters,card.projector.farClip/meters));
        prim.CreateRelationship(TfToken("natron:projection:camera")).SetTargets({cameraPath});
        auto colorOutput=graph.CreateOutput(TfToken("color"),SdfValueTypeNames->Color3f);colorOutput.ConnectToSource(color.ConnectableAPI(),TfToken("out"));
        auto opacityOutput=graph.CreateOutput(TfToken("opacity"),SdfValueTypeNames->Float);opacityOutput.ConnectToSource(clipped.ConnectableAPI(),TfToken("out"));
        return {colorOutput,opacityOutput};
    }
    void material(const UsdPrim& target,const Card& card,const Material& surface,const SdfPath& path) {
        auto mat=UsdShadeMaterial::Define(stage,path);auto shader=UsdShadeShader::Define(stage,path.AppendChild(TfToken("Surface")));
        if(!card.projected) {
            stage->RemovePrim(path.AppendChild(TfToken("Projection")));
            stage->RemovePrim(path.AppendChild(TfToken("ProjectionCamera")));
            mat.GetPrim().RemoveAppliedSchema(TfToken("NatronProjectionAPI"));
            for(const char* name:{"natron:projection:aspect","natron:projection:worldToCamera","natron:projection:image","natron:projection:camera","natron:projection:timeMode","natron:projection:referenceTime","natron:projection:imageTime"})mat.GetPrim().RemoveProperty(TfToken(name));
        }
        put(shader.CreateIdAttr(),TfToken("UsdPreviewSurface"));
        const GfVec3f base(surface.baseColor[0],surface.baseColor[1],surface.baseColor[2]);
        const float opacity=float(card.opacity)*surface.baseColor[3];
        auto input=shader.CreateInput(TfToken(surface.lit?"diffuseColor":"emissiveColor"),SdfValueTypeNames->Color3f);
        if(card.projected) {
            const auto projected=projection(path,card,base,opacity);
            input.ConnectToSource(projected.first);shader.CreateInput(TfToken("opacity"),SdfValueTypeNames->Float).ConnectToSource(projected.second);
        } else if(card.texture) {
            auto image=sampler(path,"Image",card.texture,card.bindingPaths.empty()?"colorSt":"st");put(image.CreateInput(TfToken("scale"),SdfValueTypeNames->Float4).GetAttr(),GfVec4f(base[0],base[1],base[2],opacity));
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
            if(card.maps[j].texture) {auto map=sampler(path,inputs[j],card.maps[j].texture,card.bindingPaths.empty()?("map"+std::to_string(j)).c_str():"st");
                v.ConnectToSource(map.ConnectableAPI(),TfToken(j==0?"rgb":channels[std::min(3u,card.maps[j].channel)]));}
            else {v.DisconnectSource();if(j==0)put(v.GetAttr(),GfVec3f(0,0,1));else put(v.GetAttr(),float(j==1?surface.roughness:surface.metallic));}
        }
        shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token);mat.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),TfToken("surface"));
        if(target) {
            UsdShadeMaterialBindingAPI::Apply(target).Bind(mat);
            target.AddAppliedSchema(TfToken("NatronCompositingAPI"));
            put(target.CreateAttribute(TfToken("natron:unlit"),SdfValueTypeNames->Bool),!surface.lit);
        }
    }
    UsdGeomCamera authorCamera(const Camera& camera,const SdfPath& path,double aspect) {
        const double meters=UsdGeomGetStageMetersPerUnit(stage);const auto basis=Matrix::transform({},UsdGeomGetStageUpAxis(stage)==UsdGeomTokens->z?Vec3{-90,0,0}:Vec3{},{meters,meters,meters});
        auto c=UsdGeomCamera::Define(stage,path);c.GetPrim().SetCustomDataByKey(TfToken("natron:owner"),VtValue(camera.ownerId));c.GetPrim().SetDisplayName(camera.label);
        auto cameraTransform=basis.inverse()*camera.transform;for(int col=0;col<3;++col)for(int row=0;row<3;++row)cameraTransform.m[col*4+row]*=meters;
        transform(c,cameraTransform);c.SetResetXformStack(true);put(c.CreateProjectionAttr(),camera.orthographic?UsdGeomTokens->orthographic:UsdGeomTokens->perspective);
        put(c.CreateFocalLengthAttr(),float(camera.focal*.01/meters));const double aperture=(camera.orthographic?camera.orthoWidth*10:camera.aperture*.01)/meters;
        put(c.CreateHorizontalApertureAttr(),float(aperture));put(c.CreateVerticalApertureAttr(),float(aperture/camera.filmAspect(aspect)));
        put(c.CreateHorizontalApertureOffsetAttr(),float(aperture*camera.offsetX));put(c.CreateVerticalApertureOffsetAttr(),float(aperture*camera.offsetY));
        put(c.CreateClippingRangeAttr(),GfVec2f(camera.nearClip/meters,camera.farClip/meters));return c;
    }
    void exportInspectionCamera(const QString& path) {
        // Public scene values intentionally omit this camera. Every private
        // transport assembly, including lighting probes, needs its own copy.
        const auto transport=SdfLayer::FindOrOpen(path.toStdString());transport->Reload();
        const SdfPath cameraPath("/Natron/InspectionCamera");
        if(!SdfCopySpec(stage->GetRootLayer(),cameraPath,transport,cameraPath) || !transport->Export(path.toStdString()))throw std::runtime_error("Cannot export inspection camera");
    }
    QString exportScene(const std::vector<Card>& cards,const Camera& viewCamera,const Settings& settings,const std::vector<UsdSceneSource>& sources) {
        const auto& camera=settings.stageCamera?*settings.stageCamera:viewCamera;
        if(settings.width<1 || settings.height<1 || !std::isfinite(settings.pixelAspect) || settings.pixelAspect<=0)throw std::runtime_error("Invalid USD render dimensions");
        const auto shot=std::find_if(sources.begin(),sources.end(),[](const UsdSceneSource& s){return s.mountPath.empty();});
        const double meters=shot==sources.end()?1:shot->metersPerUnit;
        if(!std::isfinite(meters) || meters<=0)throw std::runtime_error("Invalid USD metersPerUnit");
        const auto axis=shot!=sources.end() && shot->zUp?UsdGeomTokens->z:UsdGeomTokens->y;
        UsdGeomSetStageUpAxis(stage,axis);UsdGeomSetStageMetersPerUnit(stage,meters);
        stage->SetTimeCodesPerSecond(shot==sources.end()?24:shot->timeCodesPerSecond);
        const Matrix basis=Matrix::transform({},axis==UsdGeomTokens->z?Vec3{-90,0,0}:Vec3{},{meters,meters,meters});
        transform(UsdGeomXformable(stage->GetPrimAtPath(SdfPath("/Natron"))),basis.inverse());
        const double aspect=double(settings.width)*settings.pixelAspect/settings.height;
        const double stageAspect=settings.stageCamera && settings.stageCameraAspect>0?settings.stageCameraAspect:aspect;
        authorCamera(camera,SdfPath("/Natron/Camera"),stageAspect);
        if(settings.stageCamera)authorCamera(viewCamera,SdfPath("/Natron/InspectionCamera"),aspect);
        else stage->RemovePrim(SdfPath("/Natron/InspectionCamera"));
        UsdGeomXform::Define(stage,SdfPath("/Natron/Geometry"));UsdGeomXform::Define(stage,SdfPath("/Natron/Lights"));
        UsdGeomXform::Define(stage,SdfPath("/Natron/Materials"));
        size_t i=0;std::set<SdfPath> geometryPaths,lightPaths,materialPaths;
        for(const auto& card:cards) {
            if(!card.bindingPaths.empty()) {
                const SdfPath path("/Natron/Materials/n_"+card.ownerId);materialPaths.insert(path);
                const auto cached=cardCache.find(path);if(cached==cardCache.end() || !same(cached->second,card)){material({},card,card.material,path);cardCache[path]=card;}continue;
            }
            const auto path=SdfPath("/Natron/Geometry/"+(card.ownerId.empty()?"Object"+std::to_string(i):"n_"+(card.instanceId.empty()?card.ownerId:card.instanceId)));++i;
            if(!geometryPaths.insert(path).second)continue;
            const auto cached=cardCache.find(path);if(cached!=cardCache.end() && same(cached->second,card))continue;
            auto mesh=UsdGeomMesh::Define(stage,path);transform(mesh,card.transform);
            mesh.GetPrim().SetCustomDataByKey(TfToken("natron:owner"),VtValue(card.ownerId));mesh.GetPrim().SetDisplayName(card.label);
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
            const auto& comp=card.compositing;validateCompositing(comp);
            put(mesh.GetPrim().CreateAttribute(TfToken("natron:compositingRole"),SdfValueTypeNames->Int),int(comp.role));
            put(mesh.GetPrim().CreateAttribute(TfToken("natron:shadowDensity"),SdfValueTypeNames->Float),float(comp.shadowDensity));
            put(mesh.GetPrim().CreateAttribute(TfToken("natron:shadowTint"),SdfValueTypeNames->Color3f),GfVec3f(comp.shadowTint.x,comp.shadowTint.y,comp.shadowTint.z));
            put(mesh.GetPrim().CreateAttribute(TfToken("natron:reflectionStrength"),SdfValueTypeNames->Float),float(comp.reflectionStrength));
            put(mesh.GetPrim().CreateAttribute(TfToken("natron:reflectionRoughness"),SdfValueTypeNames->Float),float(comp.reflectionRoughness));
            put(pv.CreatePrimvar(TfToken("natron:compositingRole"),SdfValueTypeNames->Int,UsdGeomTokens->constant).GetAttr(),int(comp.role));
            put(pv.CreatePrimvar(TfToken("natron:rayMask"),SdfValueTypeNames->Int,UsdGeomTokens->constant).GetAttr(),(comp.cameraVisible?1:0)|(comp.reflectionVisible?2:0)|(comp.shadowVisible?4:0));
            put(pv.CreatePrimvar(TfToken("natron:holdout"),SdfValueTypeNames->Int,UsdGeomTokens->constant).GetAttr(),comp.holdout || comp.role!=GeometryRole::Surface?1:0);
            put(pv.CreatePrimvar(TfToken("natron:shadowDensity"),SdfValueTypeNames->Float,UsdGeomTokens->constant).GetAttr(),float(comp.shadowDensity));
            put(pv.CreatePrimvar(TfToken("natron:shadowTint"),SdfValueTypeNames->Color3f,UsdGeomTokens->constant).GetAttr(),GfVec3f(comp.shadowTint.x,comp.shadowTint.y,comp.shadowTint.z));
            put(pv.CreatePrimvar(TfToken("natron:reflectionStrength"),SdfValueTypeNames->Float,UsdGeomTokens->constant).GetAttr(),float(comp.reflectionStrength));
            put(pv.CreatePrimvar(TfToken("natron:reflectionRoughness"),SdfValueTypeNames->Float,UsdGeomTokens->constant).GetAttr(),float(comp.reflectionRoughness));
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
            cardCache[path]=card;
        }
        for(const auto& child:stage->GetPrimAtPath(SdfPath("/Natron/Geometry")).GetChildren()) {
            if(!geometryPaths.count(child.GetPath()))stage->RemovePrim(child.GetPath());
        }
        for(const auto& child:stage->GetPrimAtPath(SdfPath("/Natron/Materials")).GetChildren())if(!materialPaths.count(child.GetPath()))stage->RemovePrim(child.GetPath());
        for(auto it=cardCache.begin();it!=cardCache.end();)if(!geometryPaths.count(it->first) && !materialPaths.count(it->first))it=cardCache.erase(it);else ++it;
        i=0;for(const auto& light:settings.lights) {
            const auto path=SdfPath("/Natron/Lights/"+(light.ownerId.empty()?"Light"+std::to_string(i):"n_"+(light.instanceId.empty()?light.ownerId:light.instanceId)));++i;UsdPrim prim;
            if(!lightPaths.insert(path).second)continue;
            if(light.type==LightType::Environment) {auto dome=UsdLuxDomeLight::Define(stage,path);prim=dome.GetPrim();
                if(light.environment)put(dome.CreateTextureFileAttr(),texture(light.environment,true));else put(dome.CreateTextureFileAttr(),SdfAssetPath());
                put(dome.CreateTextureFormatAttr(),TfToken("latlong"));
                dome.GetTextureFileAttr().SetColorSpace(TfToken("raw"));
            } else if(light.type==LightType::Distant)prim=UsdLuxDistantLight::Define(stage,path).GetPrim();
            else {auto sphere=UsdLuxSphereLight::Define(stage,path);prim=sphere.GetPrim();put(sphere.CreateRadiusAttr(),0.f);put(sphere.CreateTreatAsPointAttr(),true);
                if(light.type==LightType::Spot){auto shaping=UsdLuxShapingAPI::Apply(prim);put(shaping.CreateShapingConeAngleAttr(),float(light.coneAngle));put(shaping.CreateShapingConeSoftnessAttr(),float(light.coneSoftness));}}
            transform(UsdGeomXformable(prim),light.transform);UsdLuxLightAPI api(prim);
            prim.SetCustomDataByKey(TfToken("natron:owner"),VtValue(light.ownerId));prim.SetDisplayName(light.label);
            put(api.CreateNormalizeAttr(),light.type==LightType::Distant);
            put(api.CreateColorAttr(),GfVec3f(light.color.x,light.color.y,light.color.z));put(api.CreateIntensityAttr(),float(light.intensity));put(api.CreateExposureAttr(),float(light.exposure));
            put(UsdLuxShadowAPI::Apply(prim).CreateShadowEnableAttr(),light.shadows);
        }
        for(const auto& child:stage->GetPrimAtPath(SdfPath("/Natron/Lights")).GetChildren())if(!lightPaths.count(child.GetPath()))stage->RemovePrim(child.GetPath());
        auto next=std::atomic_load(&value);
        const bool sameInputs=next && same(camera,publishedCamera) && stageAspect==publishedAspect && same(cards,publishedCards) && same(settings.lights,publishedLights) && same(sources,publishedSources) && same(settings.edits,publishedEdits);
        std::string authored=publishedText;
        if(!sameInputs) {
            SdfLayerRefPtr layer=stage->GetRootLayer();
            if(settings.stageCamera) {
                layer=SdfLayer::CreateAnonymous("NatronPublic.usda");layer->TransferContent(stage->GetRootLayer());
                if(auto prim=layer->GetPrimAtPath(SdfPath("/Natron/InspectionCamera")))prim->GetNameParent()->RemoveNameChild(prim);
            }
            layer->ExportToString(&authored);
        }
        const bool changed=!next || authored!=publishedText || !same(sources,publishedSources) || !same(settings.edits,publishedEdits);
        if(changed)next=UsdSceneValue::compose(authored,sources,++revision,settings.edits);
        const QString path=directory.filePath(QStringLiteral("scene.usda"));
        if(next->saveRenderAssembly(path.toStdString(),directory.filePath(QStringLiteral("geometry.usda")).toStdString(),geometryText,!changed && !probeFiles))++geometryRevision;
        probeFiles=false;
        if(settings.stageCamera)exportInspectionCamera(path);
        if(!sameInputs){publishedText=std::move(authored);publishedSources=sources;publishedEdits=settings.edits;publishedCards=cards;publishedLights=settings.lights;publishedCamera=camera;publishedAspect=stageAspect;}
        std::atomic_store(&value,next);return path;
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
std::shared_ptr<const UsdSceneValue> UsdSceneBridge::sceneValue()const{return std::atomic_load(&_impl->value);}
std::string UsdSceneBridge::exportScene(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,const std::vector<UsdSceneSource>& sources) {
    std::lock_guard<std::mutex> lock(_impl->mutex);return _impl->exportScene(cards,camera,settings,sources).toStdString();
}
HdNvpFrame UsdSceneBridge::render(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,const HdNvpSettings& quality,const std::function<bool()>& abort,const std::vector<UsdSceneSource>& sources,const std::function<void(const HdNvpFrame&)>& progress) {
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->exportScene(cards,camera,settings,sources);const auto value=sceneValue();
    const auto targets=value->compositingTargets();std::vector<float> shadow,occlusion,reflection;
    std::map<std::array<double,4>,std::vector<std::string>> groups;
    for(const auto& target:targets)if(catchesShadows(target.value.role) && target.value.cameraVisible && target.value.shadowDensity>0){const auto& c=target.value;groups[{c.shadowDensity,c.shadowTint.x,c.shadowTint.y,c.shadowTint.z}].push_back(target.path);}
    HdNvpSettings probeQuality=quality;probeQuality.denoiser=HdNvpDenoiser::None;probeQuality.bounces=1;
    Settings probeSettings=settings;probeSettings.transparentBackground=true;
    for(const auto& group:groups){
        auto lit=renderValue(value->compositingProbe(group.second,false),camera,probeSettings,probeQuality,abort,sources,{});
        auto ref=renderValue(value->compositingProbe(group.second,true),camera,probeSettings,probeQuality,abort,sources,{});
        if(shadow.empty())shadow.resize(lit.color.size());
        for(size_t i=0;i<shadow.size();i+=4){
            auto luma=[&](const auto& p){return .2126*p[i]+.7152*p[i+1]+.0722*p[i+2];};
            const float a=shadowOpacity(luma(lit.color),luma(ref.color),group.first[0])*std::clamp(ref.color[i+3],0.f,1.f);
            shadow[i]+=float(group.first[1])*a;shadow[i+1]+=float(group.first[2])*a;shadow[i+2]+=float(group.first[3])*a;shadow[i+3]+=a;
        }
    }
    if(std::any_of(targets.begin(),targets.end(),[](const auto& t){return t.value.role==GeometryRole::Occluder || (t.value.role==GeometryRole::Surface && t.value.holdout);})){
        auto matte=renderValue(value->compositingProbe({},false),camera,probeSettings,probeQuality,abort,sources,{});occlusion.resize(matte.color.size()/4);
        for(size_t i=0;i<occlusion.size();++i)occlusion[i]=std::clamp(matte.color[i*4],0.f,1.f);
    }
    std::map<std::array<double,2>,std::vector<std::string>> reflectionGroups;
    for(const auto& target:targets)if(catchesReflections(target.value.role) && target.value.cameraVisible && target.value.reflectionStrength>0){const auto& c=target.value;reflectionGroups[{c.reflectionStrength,c.reflectionRoughness}].push_back(target.path);}
    HdNvpSettings reflectionQuality=probeQuality;reflectionQuality.bounces=std::max(2,quality.bounces);
    for(const auto& group:reflectionGroups){
        auto lit=renderValue(value->compositingProbe(group.second,false,true,group.first[1]),camera,probeSettings,reflectionQuality,abort,sources,{});
        auto ref=renderValue(value->compositingProbe(group.second,true,true,group.first[1]),camera,probeSettings,reflectionQuality,abort,sources,{});
        if(reflection.empty())reflection.resize(lit.color.size());
        // Signed radiance replacement: CG can also darken an environment
        // reflection. Common lights/environment cancel instead of being added twice.
        for(size_t i=0;i<reflection.size();i+=4)for(int c=0;c<3;++c)reflection[i+c]+=float((lit.color[i+c]-ref.color[i+c])*group.first[0]);
    }
    auto presented=[&](const HdNvpFrame& frame){if(progress){auto result=frame;result.shadow=shadow;result.occlusion=occlusion;result.reflection=reflection;progress(result);}};
    Settings beauty=settings;if(std::any_of(targets.begin(),targets.end(),[](const auto& t){return t.value.role!=GeometryRole::Surface && t.value.cameraVisible;}))beauty.transparentBackground=true;
    auto frame=renderValue(value,camera,beauty,quality,abort,sources,presented);frame.shadow=std::move(shadow);frame.occlusion=std::move(occlusion);frame.reflection=std::move(reflection);return frame;
}
HdNvpFrame UsdSceneBridge::renderValue(const std::shared_ptr<const UsdSceneValue>& value,const Camera& camera,const Settings& settings,const HdNvpSettings& quality,const std::function<bool()>& abort,const std::vector<UsdSceneSource>& sources,const std::function<void(const HdNvpFrame&)>& progress) {
    const QString host=QString::fromStdString(hostPath());
    if(abort && abort())throw std::runtime_error("hdNVP rendering cancelled");
    if(host.isEmpty() || !QFileInfo::exists(host))throw std::runtime_error("Build the hdNVP host with tools/build-hdnvp-host.ps1 or set NATRON_HDNVP_HOST.");
    const QString scene=_impl->directory.filePath(QStringLiteral("scene.usda")),output=_impl->directory.filePath(QStringLiteral("aovs"));
    if(value->saveRenderAssembly(scene.toStdString(),_impl->directory.filePath(QStringLiteral("geometry.usda")).toStdString(),_impl->geometryText,false))++_impl->geometryRevision;
    if(settings.stageCamera)_impl->exportInspectionCamera(scene);
    _impl->probeFiles=value!=sceneValue();
    const auto cancelled=std::make_shared<std::atomic<bool>>(false);
    std::mutex progressMutex;std::shared_ptr<HdNvpFrame> latest;
    auto present=[&]{std::shared_ptr<HdNvpFrame> frame;{std::lock_guard<std::mutex> lock(progressMutex);frame=std::move(latest);}if(frame && progress)progress(*frame);};
#ifdef _WIN32
    const bool sharedMemory=_impl->mapping(size_t(settings.width)*settings.height,settings.width,settings.height,value->revision());
#endif
    // Natron renders from different pool threads. QProcess and its notifiers
    // stay on one dedicated thread for the entire lifetime of this bridge.
    return _impl->invoke([&]() {
    if(cancelled->load())throw std::runtime_error("hdNVP rendering cancelled");
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
    const QJsonObject request{{QStringLiteral("scene"),scene},{QStringLiteral("output"),output},{QStringLiteral("time"),value->timeCode()},{QStringLiteral("loadPayloads"),value->loadPayloads()},{QStringLiteral("width"),settings.width},{QStringLiteral("height"),settings.height},{QStringLiteral("samples"),quality.samples},{QStringLiteral("bounces"),quality.bounces},{QStringLiteral("denoiser"),denoiser},{QStringLiteral("interactive"),quality.interactive}};
    uint64_t sourceRevision=_impl->geometryRevision;for(const auto& source:sources)sourceRevision^=source.revision+0x9e3779b97f4a7c15ULL+(sourceRevision<<6)+(sourceRevision>>2);
    auto message=request;message.insert(QStringLiteral("sourceRevision"),QString::number(sourceRevision));
    message.insert(QStringLiteral("transparentBackground"),settings.transparentBackground);
    message.insert(QStringLiteral("camera"),settings.stageCamera?QStringLiteral("/Natron/InspectionCamera"):QStringLiteral("/Natron/Camera"));
    message.insert(QStringLiteral("revision"),QString::number(value->revision()));
#ifdef _WIN32
    if(sharedMemory)message.insert(QStringLiteral("mapping"),QString::fromStdString(_impl->mappingName));
    auto header=sharedMemory?static_cast<NatronHdNvp::FrameHeader*>(_impl->mappingData):nullptr;
#endif
    _impl->worker->write(QJsonDocument(message).toJson(QJsonDocument::Compact)+'\n');QElapsedTimer timer;timer.start();QJsonObject response;
    qint64 cancellationAt=-1;
    for(;;) {
        if(cancelled->load()) {
#ifdef _WIN32
            if(header)InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->cancelled),1);
            if(cancellationAt<0)cancellationAt=timer.elapsed();
            if(timer.elapsed()-cancellationAt>10000){_impl->worker->kill();_impl->worker->waitForFinished(2000);throw std::runtime_error("hdNVP rendering cancelled");}
#else
            _impl->worker->kill();_impl->worker->waitForFinished(2000);throw std::runtime_error("hdNVP rendering cancelled");
#endif
        }
        if(timer.elapsed()>310000) {_impl->worker->kill();_impl->worker->waitForFinished(2000);throw std::runtime_error("hdNVP rendering timed out");}
        _impl->worker->waitForReadyRead(25);_impl->pending+=_impl->worker->readAllStandardOutput();
        const auto diagnostics=_impl->worker->readAllStandardError();_impl->diagnostics+=diagnostics;
        if(qEnvironmentVariableIsSet("NATRON_HDNVP_TRACE"))fprintf(stderr,"%s",diagnostics.constData());
        if(_impl->diagnostics.size()>16384)_impl->diagnostics=_impl->diagnostics.right(16384);
        int newline;while((newline=int(_impl->pending.indexOf('\n')))>=0) {const auto line=_impl->pending.left(newline);_impl->pending.remove(0,newline+1);
            if(line.startsWith("NATRON_HDNVP ")){response=QJsonDocument::fromJson(line.mid(13)).object();break;}
#ifdef _WIN32
            if(line.startsWith("NATRON_HDNVP_FRAME ")) {
                if(header->revision!=value->revision() || InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&header->ready),0,0)!=1)throw std::runtime_error("Invalid progressive frame");
                if(progress && !cancelled->load()) {
                    auto frame=std::make_shared<HdNvpFrame>();const auto count=size_t(settings.width)*settings.height;frame->color.resize(count*4);frame->depth.resize(count);frame->revision=value->revision();
                    std::memcpy(frame->color.data(),static_cast<char*>(_impl->mappingData)+NatronHdNvp::offset(0,count),count*4*sizeof(float));
                    clearTransparentRgb(frame->color);
                    std::memcpy(frame->depth.data(),static_cast<char*>(_impl->mappingData)+NatronHdNvp::offset(1,count),count*sizeof(float));for(auto& z:frame->depth)z*=float(value->metersPerUnit());
                    {std::lock_guard<std::mutex> lock(progressMutex);latest=std::move(frame);}
                }
                InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->ready),0);
            }
#endif
        }
        if(!response.isEmpty())break;
        if(_impl->worker->state()==QProcess::NotRunning)throw std::runtime_error("hdNVP host stopped (exit "+std::to_string(_impl->worker->exitCode())+"): "+_impl->diagnostics.toStdString());
    }
    if(cancelled->load() || response.value(QStringLiteral("cancelled")).toBool())throw std::runtime_error("hdNVP rendering cancelled");
    if(!response.value(QStringLiteral("ok")).toBool())throw std::runtime_error("hdNVP: "+response.value(QStringLiteral("error")).toString().toStdString());
    if(settings.transparentBackground && !response.value(QStringLiteral("plateCompositing")).toBool())throw std::runtime_error("The hdNVP host needs transparent plate compositing support. Rebuild it with tools/build-hdnvp-host.ps1.");
    HdNvpFrame frame;frame.report=QJsonDocument(response).toJson(QJsonDocument::Compact).toStdString();
    auto read=[&](const char* name,auto& values,size_t count){
#ifdef _WIN32
        if(response.value(QStringLiteral("transport")).toString()==QStringLiteral("sharedMemory")) {
            const char* names[]{"color.raw","depth.raw","primId.raw","instanceId.raw","albedo.raw","normal.raw"};size_t aov=0;while(aov<6 && std::strcmp(names[aov],name)!=0)++aov;
            if(aov==6 || header->revision!=value->revision() || header->pixels!=size_t(settings.width)*settings.height || InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&header->ready),0,0)!=2)throw std::runtime_error("Invalid completed frame mapping");
            values.resize(count);std::memcpy(values.data(),static_cast<char*>(_impl->mappingData)+NatronHdNvp::offset(aov,header->pixels),count*sizeof(values[0]));return;
        }
#endif
        QFile file(QDir(output).filePath(QString::fromLatin1(name)));if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Missing hdNVP AOV");
        const auto data=file.readAll();if(size_t(data.size())!=count*sizeof(values[0]))throw std::runtime_error("Invalid hdNVP AOV size");values.resize(count);std::memcpy(values.data(),data.constData(),data.size());};
    const size_t pixels=size_t(settings.width)*settings.height;read("color.raw",frame.color,pixels*4);read("depth.raw",frame.depth,pixels);read("primId.raw",frame.primIds,pixels);read("instanceId.raw",frame.instanceIds,pixels);read("albedo.raw",frame.albedo,pixels*3);read("normal.raw",frame.normal,pixels*3);
    clearTransparentRgb(frame.color);
    frame.revision=response.value(QStringLiteral("revision")).toString().toULongLong();
    if(frame.revision!=value->revision())throw std::runtime_error("Stale hdNVP frame revision");
    for(const auto& item:response.value(QStringLiteral("picks")).toArray()) {const auto pick=item.toObject();frame.picks.push_back({pick.value(QStringLiteral("prim")).toInt(-1),pick.value(QStringLiteral("instance")).toInt(-1),pick.value(QStringLiteral("path")).toString().toStdString()});}
    for(auto& depth:frame.depth)depth*=float(value->metersPerUnit());
    const bool zUp=std::any_of(sources.begin(),sources.end(),[](const UsdSceneSource& s){return s.mountPath.empty() && s.zUp;});
    const auto rotation=Matrix::transform({},zUp?Vec3{-90,0,0}:Vec3{});
    frame.position.resize(pixels*3);
    const Camera renderCamera=settings.stageCamera?camera:value->camera("/Natron/Camera",true);
    const double tx=renderCamera.orthographic?renderCamera.orthoWidth*.5:renderCamera.aperture/(2*renderCamera.focal),ty=tx/renderCamera.filmAspect(settings.width*settings.pixelAspect/settings.height);
    for(size_t i=0;i<pixels;++i) {
        const auto n=rotation.vector({frame.normal[i*3],frame.normal[i*3+1],frame.normal[i*3+2]});frame.normal[i*3]=float(n.x);frame.normal[i*3+1]=float(n.y);frame.normal[i*3+2]=float(n.z);
        if(frame.depth[i]>0) {
            const double nx=2*((i%settings.width)+.5)/settings.width-1+2*renderCamera.offsetX,ny=2*((i/settings.width)+.5)/settings.height-1+2*renderCamera.offsetY*tx/ty;
            const double factor=renderCamera.orthographic?1:frame.depth[i];const auto p=renderCamera.transform.point({nx*tx*factor,ny*ty*factor,-frame.depth[i]});
            frame.position[i*3]=float(p.x);frame.position[i*3+1]=float(p.y);frame.position[i*3+2]=float(p.z);
        }
    }
    return frame;
    },abort,cancelled,present);
}
#else
struct UsdSceneBridge::Impl {};
UsdSceneBridge::UsdSceneBridge():_impl(new Impl){}UsdSceneBridge::~UsdSceneBridge()=default;
bool UsdSceneBridge::available(){return false;}std::string UsdSceneBridge::hostPath(){return {};}
std::shared_ptr<const UsdSceneValue> UsdSceneBridge::sceneValue()const{return {};}
std::string UsdSceneBridge::exportScene(const std::vector<Card>&,const Camera&,const Settings&,const std::vector<UsdSceneSource>&){throw std::runtime_error("OpenUSD is unavailable");}
HdNvpFrame UsdSceneBridge::render(const std::vector<Card>&,const Camera&,const Settings&,const HdNvpSettings&,const std::function<bool()>&,const std::vector<UsdSceneSource>&,const std::function<void(const HdNvpFrame&)>&){throw std::runtime_error("OpenUSD is unavailable");}
#endif
}}}
