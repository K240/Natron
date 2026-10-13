// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdTexture.h"
#include <QColorSpace>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <pxr/base/gf/vec4f.h>
#include <pxr/usd/ar/asset.h>
#include <pxr/usd/ar/resolver.h>
#include <pxr/usd/sdf/assetPath.h>

namespace Natron { namespace Gpu { namespace Cards {
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void warn(std::vector<std::string>& out,const std::string& message) {
    if(std::find(out.begin(),out.end(),message)==out.end())out.push_back(message);
}
template<class T> T input(const UsdShadeShader& shader,const char* name,UsdTimeCode time,T fallback) {
    const auto i=shader.GetInput(TfToken(name));
    if(i){const auto attrs=i.GetValueProducingAttributes();T result;if(attrs.size()==1 && attrs[0].Get(&result,time))return result;}
    return fallback;
}
UsdAttribute output(const UsdShadeInput& i) {
    if(!i || !i.HasConnectedSource())return {};
    const auto attrs=i.GetValueProducingAttributes(true);return attrs.size()==1?attrs[0]:UsdAttribute();
}
bool shaderIs(const UsdShadeShader& shader,const char* id,UsdTimeCode time) {
    TfToken token;return shader && shader.GetIdAttr().Get(&token,time) && token==TfToken(id);
}
struct Coordinates {TfToken name;GfVec2f fallback{0};std::array<double,6> matrix{1,0,0,0,1,0};};
// Follow material interfaces and the standard primvar/affine coordinate nodes.
Coordinates coordinates(const UsdShadeShader& shader,const char* inputName,UsdTimeCode time,std::set<SdfPath>& visited) {
    const auto attr=output(shader.GetInput(TfToken(inputName)));
    if(!attr)return {{},input(shader,inputName,time,GfVec2f(0))};
    const UsdShadeShader source(attr.GetPrim());
    if(!visited.insert(attr.GetPath()).second || visited.size()>16)throw std::runtime_error("cyclic/too deep texture coordinate graph");
    if(shaderIs(source,"UsdPrimvarReader_float2",time) && attr.GetBaseName()==TfToken("result")) {
        auto name=input(source,"varname",time,std::string());
        if(name.empty())name=input(source,"varname",time,TfToken()).GetString();
        return {TfToken(name),input(source,"fallback",time,GfVec2f(0))};
    }
    if(!shaderIs(source,"UsdTransform2d",time) || attr.GetBaseName()!=TfToken("result"))throw std::runtime_error("unsupported texture coordinate node "+attr.GetPath().GetString());
    auto c=coordinates(source,"in",time,visited);
    const auto scale=input(source,"scale",time,GfVec2f(1)),translate=input(source,"translation",time,GfVec2f(0));
    const double angle=input(source,"rotation",time,0.f)*3.14159265358979323846/180;
    const double a=std::cos(angle)*scale[0],b=-std::sin(angle)*scale[1],d=std::sin(angle)*scale[0],e=std::cos(angle)*scale[1];
    const auto m=c.matrix;
    c.matrix={a*m[0]+b*m[3],a*m[1]+b*m[4],a*m[2]+b*m[5]+translate[0],d*m[0]+e*m[3],d*m[1]+e*m[4],d*m[2]+e*m[5]+translate[1]};
    for(double v:c.matrix)if(!std::isfinite(v))throw std::runtime_error("nonfinite UV transform");
    return c;
}
TextureAddress wrap(const std::string& token) {
    if(token=="black" || token.empty())return TextureAddress::Black;
    if(token=="clamp")return TextureAddress::Clamp;
    if(token=="repeat")return TextureAddress::Repeat;
    if(token=="mirror")return TextureAddress::Mirror;
    throw std::runtime_error("unsupported texture wrap mode "+token);
}
float linear(float v){return v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f);}
}
static UsdTextureBinding surfaceTexture(const UsdShadeShader& surface,const char* name,bool data,UsdTimeCode time,
    UsdTextureCache& cache,std::vector<std::string>& warnings) {
    const auto color=output(surface.GetInput(TfToken(name)));
    if(!color)return {};
    const UsdShadeShader shader(color.GetPrim());
    const auto selected=color.GetBaseName().GetString();
    const bool vector=std::string(name)=="diffuseColor" || std::string(name)=="normal";
    const auto channel=std::string("rgba").find(selected);
    if(!shaderIs(shader,"UsdUVTexture",time) || (vector?selected!="rgb":selected.size()!=1 || channel==std::string::npos))return {};
    try {
        for(const char* name:{"file","sourceColorSpace","scale","bias","fallback","wrapS","wrapT"})
            if(output(shader.GetInput(TfToken(name))))throw std::runtime_error(std::string("unsupported connected texture input ")+name);
        std::set<SdfPath> visited;const auto coords=coordinates(shader,"st",time,visited);
        const auto file=input(shader,"file",time,SdfAssetPath());
        const auto space=input(shader,"sourceColorSpace",time,TfToken("auto"));
        const auto scale=input(shader,"scale",time,GfVec4f(1)),bias=input(shader,"bias",time,GfVec4f(0)),fallback=input(shader,"fallback",time,GfVec4f(0,0,0,1));
        const auto wrapS=input(shader,"wrapS",time,TfToken("useMetadata")),wrapT=input(shader,"wrapT",time,TfToken("useMetadata"));
        for(const auto& values:{scale,bias,fallback})for(int c=0;c<4;++c)if(!std::isfinite(values[c]))throw std::runtime_error("nonfinite texture scale/bias/fallback");
        // RGB connections do not implicitly connect the file's alpha. Opacity
        // may use a/r/g/b from the very same reader (including scale/bias).
        const auto opacity=output(surface.GetInput(TfToken("opacity")));int alpha=-1;
        if(!data && opacity && opacity.GetPrim()==shader.GetPrim()) {
            const auto channel=opacity.GetBaseName().GetString();const auto at=std::string("rgba").find(channel);
            if(channel.size()==1 && at!=std::string::npos)alpha=int(at);
        }
        std::ostringstream imageKey;imageKey.precision(17);imageKey<<shader.GetPath()<<'|'<<file.GetAssetPath()<<'|'<<file.GetResolvedPath()<<'|'<<space<<'|'<<wrapS<<'|'<<wrapT<<'|'<<alpha<<'|'<<data;
        for(auto values:{scale,bias,fallback})imageKey<<'|'<<values;
        std::ostringstream key;key.precision(17);key<<imageKey.str()<<'|'<<selected<<'|'<<coords.name<<'|'<<coords.fallback;for(double v:coords.matrix)key<<'|'<<v;
        const auto found=cache.entries.find(key.str());
        if(found!=cache.entries.end()){for(const auto& w:found->second.warnings)warn(warnings,w);return found->second.binding;}
        const auto cooked=cache.images.find(imageKey.str());
        if(cooked!=cache.images.end()) {
            auto entry=cooked->second;auto texture=entry.binding.texture;
            if(texture->uvTransform!=coords.matrix){auto changed=std::make_shared<Texture>(*texture);changed->uvTransform=coords.matrix;texture=changed;}
            entry.binding={texture,coords.name,coords.fallback,alpha>=0,vector?0u:unsigned(channel)};
            if(cache.entries.size()>=64)cache.entries.clear();cache.entries.emplace(key.str(),entry);
            for(const auto& w:entry.warnings)warn(warnings,w);return entry.binding;
        }
        UsdTextureCache::Entry entry;auto texture=std::make_shared<Texture>();texture->materialSampling=true;texture->dataSampling=data;texture->uvTransform=coords.matrix;
        QImage image;int pngBits=0,pngChannels=0;
        if(file.GetAssetPath().find("<UDIM>")!=std::string::npos)warn(entry.warnings,shader.GetPath().GetString()+": UDIM textures are not evaluated; using texture fallback.");
        else if(!file.GetResolvedPath().empty()) {
            const auto asset=ArGetResolver().OpenAsset(ArResolvedPath(file.GetResolvedPath()));
            if(asset && asset->GetSize()<=size_t(std::numeric_limits<int>::max())) {
                const auto buffer=asset->GetBuffer();if(buffer) {
                    const auto data=reinterpret_cast<const uchar*>(buffer.get());
                    image=QImage::fromData(data,int(asset->GetSize()));
                    // Qt expands grayscale+alpha into RGBA; retain the source
                    // channel count for the PreviewSurface auto-color rule.
                    if(asset->GetSize()>26 && data[0]==137 && data[1]=='P' && data[2]=='N' && data[3]=='G') {
                        pngBits=data[24];const int type=data[25];pngChannels=type==0?1:type==4?2:type==6?4:3;
                    }
                }
            }
        }
        bool srgb=false;
        if(!image.isNull()) {
            if(space==TfToken("sRGB"))srgb=true;
            else if(space==TfToken("auto")) {
                const auto colorSpace=image.colorSpace();
                const bool gray=image.format()==QImage::Format_Grayscale8 || image.format()==QImage::Format_Grayscale16 || image.format()==QImage::Format_Indexed8;
                srgb=colorSpace.isValid()?colorSpace.transferFunction()==QColorSpace::TransferFunction::SRgb:
                    pngBits?(pngBits==8 && pngChannels>=3):(image.depth()<=32 && !gray);
                if(colorSpace.isValid() && colorSpace.transferFunction()!=QColorSpace::TransferFunction::SRgb && colorSpace.transferFunction()!=QColorSpace::TransferFunction::Linear)
                    warn(entry.warnings,shader.GetPath().GetString()+": non-sRGB texture profile is read as raw; set sourceColorSpace explicitly.");
            }else if(space!=TfToken("raw"))throw std::runtime_error("unsupported sourceColorSpace "+space.GetString());
            auto mode=[&](TfToken token,const char* name){return wrap(token==TfToken("useMetadata")?image.text(QString::fromLatin1(name)).toStdString():token.GetString());};
            texture->wrapS=mode(wrapS,"wrapS");texture->wrapT=mode(wrapT,"wrapT");
            texture->width=image.width();texture->height=image.height();
            const bool byteImage=image.depth()<=32;
            image=image.convertToFormat(byteImage?QImage::Format_RGBA8888:QImage::Format_RGBA32FPx4);
            if(image.isNull())throw std::runtime_error("texture pixel conversion failed");
            texture->pixels.resize(size_t(texture->width)*texture->height*4);
            for(int y=0;y<texture->height;++y) {
                const uchar* row=image.constScanLine(texture->height-1-y);const float* source=reinterpret_cast<const float*>(row);
                for(int x=0;x<texture->width;++x) {
                    float rgba[4];for(int c=0;c<4;++c){const float v=byteImage?row[x*4+c]/255.f:source[x*4+c];rgba[c]=(c<3 && srgb?linear(v):v)*scale[c]+bias[c];}
                    float* dest=texture->pixels.data()+(size_t(y)*texture->width+x)*4;
                    for(int c=0;c<4;++c)dest[c]=data?rgba[c]:c<3?std::max(0.f,rgba[c]):alpha<0?1.f:std::clamp(rgba[alpha],0.f,1.f);
                }
            }
        }else {
            warn(entry.warnings,shader.GetPath().GetString()+": cannot decode texture "+file.GetAssetPath()+" (PNG/JPEG and installed Qt image formats supported); using texture fallback.");
            texture->width=texture->height=1;texture->wrapS=texture->wrapT=TextureAddress::Clamp;
            GfVec4f rgba;for(int c=0;c<4;++c)rgba[c]=fallback[c]*scale[c]+bias[c];
            texture->pixels.resize(4);for(int c=0;c<4;++c)texture->pixels[c]=data?rgba[c]:c<3?std::max(0.f,rgba[c]):alpha<0?1.f:std::clamp(rgba[alpha],0.f,1.f);
        }
        texture->sharedPixels=std::make_shared<const std::vector<float>>(std::move(texture->pixels));
        entry.binding={texture,coords.name,coords.fallback,alpha>=0,vector?0u:unsigned(channel)};
        if(cache.images.size()>=16)cache.images.clear();cache.images.emplace(imageKey.str(),entry);
        // Animated file/UV opinions must not grow the cache without a bound.
        if(cache.entries.size()>=64)cache.entries.clear();cache.entries.emplace(key.str(),entry);
        for(const auto& w:entry.warnings)warn(warnings,w);return entry.binding;
    }catch(const std::exception& e){warn(warnings,shader.GetPath().GetString()+": "+e.what()+"; using authored surface values.");return {};}
}
UsdTextureBinding usdDiffuseTexture(const UsdShadeShader& surface,UsdTimeCode time,UsdTextureCache& cache,std::vector<std::string>& warnings) {
    return surfaceTexture(surface,"diffuseColor",false,time,cache,warnings);
}
UsdTextureBinding usdDataTexture(const UsdShadeShader& surface,const char* name,UsdTimeCode time,UsdTextureCache& cache,std::vector<std::string>& warnings) {
    return surfaceTexture(surface,name,true,time,cache,warnings);
}
std::shared_ptr<const Texture> usdEnvironmentTexture(const UsdPrim& light,UsdTimeCode time,UsdTextureCache& cache,std::vector<std::string>& warnings) {
    const auto attribute=light.GetAttribute(TfToken("inputs:texture:file"));SdfAssetPath file;
    attribute.Get(&file,time);if(file.GetAssetPath().empty() && !attribute.HasAuthoredConnections())return {};
    const auto space=attribute.GetColorSpace();
    const std::string key="environment|"+file.GetAssetPath()+"|"+file.GetResolvedPath()+"|"+space.GetString();
    const auto found=cache.images.find(key);if(found!=cache.images.end()) {
        for(const auto& w:found->second.warnings)warn(warnings,w);return found->second.binding.texture;
    }
    UsdTextureCache::Entry entry;auto texture=std::make_shared<Texture>();
    texture->materialSampling=texture->dataSampling=true;texture->wrapS=TextureAddress::Repeat;texture->wrapT=TextureAddress::Clamp;
    try {
        if(attribute.HasAuthoredConnections())throw std::runtime_error("connected dome texture assets are not evaluated");
        if(file.GetAssetPath().find("<UDIM>")!=std::string::npos)throw std::runtime_error("UDIM dome textures are not evaluated");
        QImage image;const auto asset=ArGetResolver().OpenAsset(ArResolvedPath(file.GetResolvedPath()));
        int pngBits=0,pngChannels=0;
        if(asset && asset->GetSize()<=size_t(std::numeric_limits<int>::max()))if(const auto buffer=asset->GetBuffer()) {
            const auto data=reinterpret_cast<const uchar*>(buffer.get());image=QImage::fromData(data,int(asset->GetSize()));
            if(asset->GetSize()>26 && data[0]==137 && data[1]=='P' && data[2]=='N' && data[3]=='G') {
                pngBits=data[24];const int type=data[25];pngChannels=type==0?1:type==4?2:type==6?4:3;
            }
        }
        if(image.isNull())throw std::runtime_error("cannot decode dome texture "+file.GetAssetPath()+" (PNG/JPEG and installed Qt image formats supported)");
        const bool gray=image.format()==QImage::Format_Grayscale8 || image.format()==QImage::Format_Grayscale16 || image.format()==QImage::Format_Indexed8;
        bool srgb=space==TfToken("sRGB");
        if(space.IsEmpty() || space==TfToken("auto")) {
            const auto profile=image.colorSpace();
            srgb=profile.isValid()?profile.transferFunction()==QColorSpace::TransferFunction::SRgb:
                pngBits?(pngBits==8 && pngChannels>=3):(image.depth()<=32 && !gray);
            if(profile.isValid() && profile.transferFunction()!=QColorSpace::TransferFunction::SRgb && profile.transferFunction()!=QColorSpace::TransferFunction::Linear)
                warn(entry.warnings,light.GetPath().GetString()+": non-sRGB dome profile is read as raw; set texture asset colorSpace explicitly.");
        }else if(!srgb && space!=TfToken("raw") && space!=TfToken("linear"))throw std::runtime_error("unsupported dome texture colorSpace "+space.GetString());
        texture->width=image.width();texture->height=image.height();const bool bytes=image.depth()<=32;
        image=image.convertToFormat(bytes?QImage::Format_RGBA8888:QImage::Format_RGBA32FPx4);
        if(image.isNull())throw std::runtime_error("dome pixel conversion failed");
        texture->pixels.resize(size_t(texture->width)*texture->height*4);
        for(int y=0;y<texture->height;++y) {
            const auto row=image.constScanLine(texture->height-1-y);const auto values=reinterpret_cast<const float*>(row);
            for(int x=0;x<texture->width;++x) {
                float* dest=texture->pixels.data()+(size_t(y)*texture->width+x)*4;
                for(int c=0;c<3;++c){const float v=bytes?row[x*4+c]/255.f:values[x*4+c];dest[c]=std::max(0.f,srgb?linear(v):v);}
                dest[3]=1;
            }
        }
    }catch(const std::exception& e) {
        warn(entry.warnings,light.GetPath().GetString()+": "+e.what()+"; using a black environment.");
        texture->width=texture->height=1;texture->pixels={0,0,0,1};
    }
    texture->sharedPixels=std::make_shared<const std::vector<float>>(std::move(texture->pixels));entry.binding.texture=texture;
    if(cache.images.size()>=16)cache.images.clear();cache.images.emplace(key,entry);
    for(const auto& w:entry.warnings)warn(warnings,w);return texture;
}
}}}
