// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/Lighting.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace Natron { namespace Gpu { namespace Cards {
namespace {
constexpr double pi=3.14159265358979323846;
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 normalized(Vec3 p){const double length=std::sqrt(dot(p,p));return length>1e-20?p*(1/length):Vec3{};}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
float finiteFloat(double value){const float result=float(value);if(!std::isfinite(result))throw std::invalid_argument("Lighting exceeds Vulkan float precision; select CPU.");return result;}
}
void validateMaterial(const Material& m) {
    for(float v:m.baseColor)if(!std::isfinite(v) || v<0)throw std::invalid_argument("Material base color must be finite and nonnegative.");
    for(double v:{m.emission.x,m.emission.y,m.emission.z,m.metallic,m.roughness,m.ior})
        if(!std::isfinite(v))throw std::invalid_argument("Material parameters must be finite.");
    if(m.baseColor[3]>1 || m.emission.x<0 || m.emission.y<0 || m.emission.z<0 || m.metallic<0 || m.metallic>1 || m.roughness<0 || m.roughness>1 || m.ior<1)
        throw std::invalid_argument("Material requires nonnegative emission, metallic/roughness in [0,1] and IOR >= 1.");
}
void validateLights(const std::vector<Light>& lights) {
    for(const auto& l:lights) {
        l.transform.inverse();
        if(l.type!=LightType::Distant && l.type!=LightType::Point && l.type!=LightType::Spot && l.type!=LightType::Environment)throw std::invalid_argument("Unknown 3D light type.");
        if(l.environmentSamples!=16 && l.environmentSamples!=32 && l.environmentSamples!=64)throw std::invalid_argument("Environment samples must be 16, 32 or 64 per lobe.");
        if(l.type==LightType::Environment && l.environment) {
            const auto& t=*l.environment;const auto& pixels=t.pixelData();
            if(t.width<=0 || t.height<=0 || pixels.size()!=size_t(t.width)*t.height*4 || !t.materialSampling || !t.dataSampling)
                throw std::invalid_argument("Environment requires a straight linear radiance texture.");
            for(float v:pixels)if(!std::isfinite(v))throw std::invalid_argument("Environment pixels must be finite.");
        }
        for(double v:{l.color.x,l.color.y,l.color.z,l.intensity,l.exposure,l.coneAngle,l.coneSoftness,l.shadowBias})
            if(!std::isfinite(v))throw std::invalid_argument("Light parameters must be finite.");
        if(l.color.x<0 || l.color.y<0 || l.color.z<0 || l.intensity<0 || l.coneAngle<=0 || l.coneAngle>=90 || l.coneSoftness<0 || l.coneSoftness>1 || l.shadowBias<=0 || !std::isfinite(l.intensity*std::exp2(l.exposure)))
            throw std::invalid_argument("Invalid light color, intensity, cone or shadow bias.");
    }
}
void packMaterial(GpuMaterial& out,const Material& m) {
    std::copy(m.baseColor.begin(),m.baseColor.end(),out.base);
    out.emission[0]=finiteFloat(m.emission.x);out.emission[1]=finiteFloat(m.emission.y);out.emission[2]=finiteFloat(m.emission.z);out.emission[3]=0;
    out.surface[0]=finiteFloat(m.metallic);out.surface[1]=finiteFloat(m.roughness);out.surface[2]=finiteFloat(m.ior);out.surface[3]=m.lit?1.f:0.f;
}
void packLights(GpuLight* out,const std::vector<Light>& lights) {
    if(lights.size()>maxLights)throw std::invalid_argument("Vulkan supports up to 16 lights; select CPU for larger scenes.");
    for(size_t i=0;i<lights.size();++i) {
        const auto& l=lights[i];auto& to=out[i];const Vec3 p=l.transform.point({}),d=normalized(l.transform.vector({0,0,-1}));
        to.position[0]=finiteFloat(p.x);to.position[1]=finiteFloat(p.y);to.position[2]=finiteFloat(p.z);to.position[3]=float(l.type);
        to.direction[0]=finiteFloat(d.x);to.direction[1]=finiteFloat(d.y);to.direction[2]=finiteFloat(d.z);to.direction[3]=0;
        to.color[0]=finiteFloat(l.color.x);to.color[1]=finiteFloat(l.color.y);to.color[2]=finiteFloat(l.color.z);to.color[3]=finiteFloat(l.intensity*std::exp2(l.exposure));
        to.cone[0]=finiteFloat(std::cos(l.coneAngle*pi/180));to.cone[1]=finiteFloat(std::cos(l.coneAngle*(1-l.coneSoftness)*pi/180));to.cone[2]=l.shadows?1.f:0.f;to.cone[3]=finiteFloat(l.shadowBias);
        const auto inverse=l.type==LightType::Environment?environmentToLocal(l.transform):Matrix();
        for(int c=0;c<16;++c)to.environmentInverse[c]=finiteFloat(inverse.m[c]);
        std::fill_n(to.environment,4,0u);to.environment[1]=l.environmentSamples;
    }
}
Vec3 normalVector(const Matrix& inverse,Vec3 n) {
    const auto& m=inverse.m;return normalized({m[0]*n.x+m[1]*n.y+m[2]*n.z,m[4]*n.x+m[5]*n.y+m[6]*n.z,m[8]*n.x+m[9]*n.y+m[10]*n.z});
}
Vec3 shadeDirect(const Material& m,Vec3 base,Vec3 n,Vec3 v,Vec3 l,Vec3 radiance) {
    const double nv=std::clamp(dot(n,v),0.,1.),nl=std::clamp(dot(n,l),0.,1.);if(nv<=0 || nl<=0)return {};
    const Vec3 h=normalized(v+l);const double nh=std::clamp(dot(n,h),0.,1.),vh=std::clamp(dot(v,h),0.,1.);
    const double alpha=std::max(.04,m.roughness)*std::max(.04,m.roughness),a2=alpha*alpha;
    const double denominator=(1-nh*nh)+nh*nh*a2;
    const double distribution=a2/(pi*denominator*denominator);
    const double visibility=.5/(nl*std::sqrt(nv*nv*(1-a2)+a2)+nv*std::sqrt(nl*nl*(1-a2)+a2));
    const double f0=std::pow((m.ior-1)/(m.ior+1),2),edge=std::pow(1-vh,5);
    const double colors[]{base.x,base.y,base.z},light[]{radiance.x,radiance.y,radiance.z};double result[3];
    for(int c=0;c<3;++c) {
        const double reflectance=f0*(1-m.metallic)+std::clamp(colors[c],0.,1.)*m.metallic,f=reflectance+(1-reflectance)*edge;
        result[c]=((1-f)*(1-m.metallic)*colors[c]/pi+distribution*visibility*f)*nl*light[c];
    }
    return {result[0],result[1],result[2]};
}
Matrix environmentToLocal(const Matrix& transform) {
    // Ignore position/scale, including a stage's meter conversion. Preserve
    // mirrored orientation and remove any shear introduced by parent axes.
    const Vec3 x=normalized(transform.vector({1,0,0})),columnY=transform.vector({0,1,0});
    const Vec3 y=normalized(columnY-x*dot(x,columnY));Vec3 z=cross(x,y);
    if(dot(z,transform.vector({0,0,1}))<0)z=z*(-1);
    Matrix rotation;const Vec3 columns[]{x,y,z};
    for(int c=0;c<3;++c){rotation.m[c*4]=columns[c].x;rotation.m[c*4+1]=columns[c].y;rotation.m[c*4+2]=columns[c].z;}
    return rotation.inverse();
}
Vec3 environmentRadiance(const Light& light,const Matrix& inverse,Vec3 direction) {
    Vec3 rgb=light.color*(light.intensity*std::exp2(light.exposure));
    if(light.environment) {
        const Vec3 d=normalized(inverse.vector(direction));
        // OpenEXR/OpenUSD latlong: +Z at the center, +X at u=.25,
        // +Y at the top. Texture row zero is at the bottom.
        const auto texel=light.environment->sample(.5-std::atan2(d.x,d.z)/(2*pi),.5+std::asin(std::clamp(d.y,-1.,1.))/pi);
        rgb={rgb.x*std::max(0.f,texel[0]),rgb.y*std::max(0.f,texel[1]),rgb.z*std::max(0.f,texel[2])};
    }
    return rgb;
}
EnvironmentSample environmentSample(const Material& m,Vec3 base,Vec3 n,Vec3 v,unsigned index,unsigned count,bool specular) {
    const double nv=std::clamp(dot(n,v),0.,1.);if(nv<=0)return {};
    uint32_t bits=index;bits=(bits<<16)|(bits>>16);
    bits=((bits&0x55555555u)<<1)|((bits&0xaaaaaaaau)>>1);
    bits=((bits&0x33333333u)<<2)|((bits&0xccccccccu)>>2);
    bits=((bits&0x0f0f0f0fu)<<4)|((bits&0xf0f0f0f0u)>>4);
    bits=((bits&0x00ff00ffu)<<8)|((bits&0xff00ff00u)>>8);
    const double u=(index+.5)/count,phi=2*pi*bits*2.3283064365386963e-10;
    const double alpha=std::max(.04,m.roughness)*std::max(.04,m.roughness),a2=alpha*alpha;
    const double cosine=specular?std::sqrt((1-u)/(1+(a2-1)*u)):std::sqrt(1-u),sine=std::sqrt(std::max(0.,1-cosine*cosine));
    // An orthonormal basis without a pole singularity: a cross-product basis
    // rotates abruptly near +Z and makes finite IBL samples draw radial seams.
    const double sign=n.z>=0?1:-1,a=-1/(sign+n.z),b=n.x*n.y*a;
    const Vec3 tangent{1+sign*n.x*n.x*a,sign*b,-sign*n.x},bitangent{b,sign+n.y*n.y*a,-n.y};
    const Vec3 sampled=normalized(tangent*(sine*std::cos(phi))+bitangent*(sine*std::sin(phi))+n*cosine);
    const Vec3 l=specular?normalized(sampled*(2*dot(v,sampled))-v):sampled,h=specular?sampled:normalized(v+l);
    const double nl=std::clamp(dot(n,l),0.,1.),vh=std::clamp(dot(v,h),0.,1.),nh=std::clamp(dot(n,h),0.,1.);
    if(nl<=0 || vh<=0 || nh<=0)return {};
    const double dielectric=std::pow((m.ior-1)/(m.ior+1),2),edge=std::pow(1-vh,5);
    const double visibility=.5/(nl*std::sqrt(nv*nv*(1-a2)+a2)+nv*std::sqrt(nl*nl*(1-a2)+a2));
    const double scale=specular?4*visibility*nl*vh/nh:1;
    const double colors[]{base.x,base.y,base.z};double weight[3];
    for(int c=0;c<3;++c) {
        const double f0=dielectric*(1-m.metallic)+std::clamp(colors[c],0.,1.)*m.metallic,f=f0+(1-f0)*edge;
        // PDF cancellation avoids evaluating the sharp GGX peak at low
        // roughness and keeps CPU/Vulkan results stable in float precision.
        weight[c]=(specular?f*scale:(1-f)*(1-m.metallic)*colors[c])/count;
    }
    return {l,{weight[0],weight[1],weight[2]}};
}
std::string lightingShader() {return R"(
struct Surface {vec4 base;vec4 emission;vec4 parameters;};
struct Light {vec4 position;vec4 direction;vec4 color;vec4 cone;mat4 environmentInverse;uvec4 environment;};
vec3 safeNormalize(vec3 p) {float d=length(p);return d>1.e-20?p/d:vec3(0);}
vec3 shadeDirect(Surface m,vec3 base,vec3 n,vec3 v,vec3 l,vec3 radiance) {
    float nv=clamp(dot(n,v),0.,1.),nl=clamp(dot(n,l),0.,1.);if(nv<=0. || nl<=0.)return vec3(0);
    vec3 h=safeNormalize(v+l);float nh=clamp(dot(n,h),0.,1.),vh=clamp(dot(v,h),0.,1.);
    float alpha=max(.04,m.parameters.y)*max(.04,m.parameters.y),a2=alpha*alpha;
    float denominator=(1.-nh*nh)+nh*nh*a2;
    float distribution=a2/(3.141592653589793*denominator*denominator);
    float visibility=.5/(nl*sqrt(nv*nv*(1.-a2)+a2)+nv*sqrt(nl*nl*(1.-a2)+a2));
    float dielectric=(m.parameters.z-1.)/(m.parameters.z+1.);
    vec3 f0=mix(vec3(dielectric*dielectric),clamp(base,vec3(0),vec3(1)),m.parameters.x),f=f0+(1.-f0)*pow(1.-vh,5.);
    return ((1.-f)*(1.-m.parameters.x)*base/3.141592653589793+distribution*visibility*f)*nl*radiance;
}
vec3 environmentSample(Surface m,vec3 base,vec3 n,vec3 v,uint index,uint count,bool specular,out vec3 weight) {
    weight=vec3(0);float nv=clamp(dot(n,v),0.,1.);if(nv<=0.)return vec3(0);
    float u=(float(index)+.5)/float(count),phi=6.283185307179586*float(bitfieldReverse(index))*2.3283064365386963e-10;
    float alpha=max(.04,m.parameters.y)*max(.04,m.parameters.y),a2=alpha*alpha;
    float cosine=specular?sqrt((1.-u)/(1.+(a2-1.)*u)):sqrt(1.-u),sine=sqrt(max(0.,1.-cosine*cosine));
    float signZ=n.z>=0.?1.:-1.,a=-1./(signZ+n.z),b=n.x*n.y*a;
    vec3 tangent=vec3(1.+signZ*n.x*n.x*a,signZ*b,-signZ*n.x),bitangent=vec3(b,signZ+n.y*n.y*a,-n.y);
    vec3 sampled=safeNormalize(tangent*(sine*cos(phi))+bitangent*(sine*sin(phi))+n*cosine);
    vec3 l=specular?safeNormalize(sampled*(2.*dot(v,sampled))-v):sampled,h=specular?sampled:safeNormalize(v+l);
    float nl=clamp(dot(n,l),0.,1.),vh=clamp(dot(v,h),0.,1.),nh=clamp(dot(n,h),0.,1.);
    if(nl<=0. || vh<=0. || nh<=0.)return vec3(0);
    float dielectric=(m.parameters.z-1.)/(m.parameters.z+1.);
    vec3 f0=mix(vec3(dielectric*dielectric),clamp(base,vec3(0),vec3(1)),m.parameters.x),f=f0+(1.-f0)*pow(1.-vh,5.);
    float visibility=.5/(nl*sqrt(nv*nv*(1.-a2)+a2)+nv*sqrt(nl*nl*(1.-a2)+a2));
    weight=(specular?f*(4.*visibility*nl*vh/nh):(1.-f)*(1.-m.parameters.x)*base)/float(count);return l;
}
)";}
}}}
