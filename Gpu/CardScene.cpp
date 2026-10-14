// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/CardScene.h"
#include "Gpu/Lighting.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sstream>

namespace Natron { namespace Gpu { namespace Cards {
Matrix Matrix::operator*(const Matrix& b) const {
    Matrix r; r.m.fill(0);
    for(int c=0;c<4;++c) for(int y=0;y<4;++y) for(int k=0;k<4;++k)
        r.m[c*4+y] += m[k*4+y]*b.m[c*4+k];
    return r;
}
Vec3 Matrix::point(Vec3 p) const { return vector(p)+Vec3{m[12],m[13],m[14]}; }
Vec3 Matrix::vector(Vec3 p) const {
    return {m[0]*p.x+m[4]*p.y+m[8]*p.z, m[1]*p.x+m[5]*p.y+m[9]*p.z, m[2]*p.x+m[6]*p.y+m[10]*p.z};
}
Matrix Matrix::transform(Vec3 t, Vec3 r, Vec3 s) {
    const double d=3.14159265358979323846/180.;
    Matrix x,y,z,scale;
    x.m[5]=x.m[10]=std::cos(r.x*d); x.m[6]=std::sin(r.x*d); x.m[9]=-x.m[6];
    y.m[0]=y.m[10]=std::cos(r.y*d); y.m[8]=std::sin(r.y*d); y.m[2]=-y.m[8];
    z.m[0]=z.m[5]=std::cos(r.z*d); z.m[1]=std::sin(r.z*d); z.m[4]=-z.m[1];
    scale.m[0]=s.x; scale.m[5]=s.y; scale.m[10]=s.z;
    Matrix out=z*y*x*scale; out.m[12]=t.x; out.m[13]=t.y; out.m[14]=t.z;
    return out;
}
Matrix Matrix::inverse() const {
    if(!std::all_of(m.begin(),m.end(),[](double v){return std::isfinite(v);}))
        throw std::invalid_argument("3D transforms must contain finite values.");
    const double a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],i=m[10];
    const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);
    if (!std::isfinite(det) || std::abs(det)<1e-12) throw std::invalid_argument("Singular 3D transform (zero scale).");
    Matrix r;
    r.m[0]=(e*i-f*h)/det; r.m[4]=(c*h-b*i)/det; r.m[8]=(b*f-c*e)/det;
    r.m[1]=(f*g-d*i)/det; r.m[5]=(a*i-c*g)/det; r.m[9]=(c*d-a*f)/det;
    r.m[2]=(d*h-e*g)/det; r.m[6]=(b*g-a*h)/det; r.m[10]=(a*e-b*d)/det;
    Vec3 t=r.vector({-m[12],-m[13],-m[14]}); r.m[12]=t.x;r.m[13]=t.y;r.m[14]=t.z;
    return r;
}
std::array<float,4> Texture::sample(double u,double v) const {
    std::array<float,4> r{};
    const auto& data=pixelData();
    if(width<=0 || height<=0 || data.size()!=size_t(width)*height*4 || !std::isfinite(u) || !std::isfinite(v)) return r;
    if(materialSampling) {
        const double s=u,t=v;u=uvTransform[0]*s+uvTransform[1]*t+uvTransform[2];v=uvTransform[3]*s+uvTransform[4]*t+uvTransform[5];
        auto address=[](double& coordinate,TextureAddress mode) {
            if(!std::isfinite(coordinate))return false;
            if(mode==TextureAddress::Black)return coordinate>=0 && coordinate<=1;
            if(mode==TextureAddress::Clamp)coordinate=std::clamp(coordinate,0.,1.);
            else if(mode==TextureAddress::Repeat)coordinate-=std::floor(coordinate);
            else {coordinate-=2*std::floor(coordinate/2);if(coordinate>1)coordinate=2-coordinate;}
            return true;
        };
        if(!address(u,wrapS) || !address(v,wrapT))return r;
    }else if(u<0 || u>1 || v<0 || v>1)return r;
    const double x=u*width-.5,y=v*height-.5;
    const int ix=int(std::floor(x)),iy=int(std::floor(y));
    const double fx=x-ix,fy=y-iy;
    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) {
        auto texel=[&](int at,int size,TextureAddress mode){return materialSampling && mode==TextureAddress::Repeat?(at%size+size)%size:std::clamp(at,0,size-1);};
        const size_t offset=(size_t(texel(iy+dy,height,wrapT))*width+texel(ix+dx,width,wrapS))*4;
        const float w=float((dx?fx:1-fx)*(dy?fy:1-fy));
        for(int c=0;c<4;++c) r[c]+=data[offset+c]*w;
    }
    if(materialSampling && !dataSampling){r[3]=std::clamp(r[3],0.f,1.f);for(int c=0;c<3;++c)r[c]*=r[3];}
    return r;
}
static void validateCamera(const Camera& c) {
    for(double v:{c.focal,c.aperture,c.nearClip,c.farClip,c.orthoWidth,c.offsetX,c.offsetY})
        if(!std::isfinite(v))throw std::invalid_argument("3D camera parameters must be finite.");
    if(!(c.focal>0) || !(c.aperture>0) || !(c.nearClip>0) || !(c.farClip>c.nearClip) || !(c.orthoWidth>0))
        throw std::invalid_argument("Invalid 3D camera parameters.");
    c.transform.inverse();
}
static void validate(const Camera& c,const Settings& s,const std::vector<Card>& cards) {
    if(s.width<=0 || s.height<=0 || s.pixelAspect<=0 || !std::isfinite(s.pixelAspect) ||
       (s.samples!=1 && s.samples!=2 && s.samples!=4))throw std::invalid_argument("Invalid 3D output format.");
    validateCamera(c);
    validateLights(s.lights);
    for(const auto& card:cards) {
        validateMaterial(card.material);
        for(const auto& map:card.maps)if(map.channel>3)throw std::invalid_argument("Material map channel must be RGBA.");
        card.transform.inverse();
        if(!std::isfinite(card.width) || !std::isfinite(card.height) || !std::isfinite(card.opacity))
            throw std::invalid_argument("3D card parameters must be finite.");
        if(card.projected) {
            validateCamera(card.projector);
            if(!(card.projectionAspect>0) || !std::isfinite(card.projectionAspect))
                throw std::invalid_argument("Invalid camera projection aspect ratio.");
        }
    }
}
namespace {
double dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 normalized(Vec3 p){const double length=std::sqrt(dot(p,p));return length>1e-20?p*(1/length):Vec3{};}
Vec3 cross(Vec3 a,Vec3 b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
struct Ray {Vec3 origin,direction;};
struct PreparedCard {
    const Card* card;
    Matrix inverse, projectorInverse;
    float opacity;
    explicit PreparedCard(const Card& c):card(&c),inverse(c.transform.inverse()),
        projectorInverse(c.projected?c.projector.transform.inverse():Matrix()),opacity(float(std::clamp(c.opacity,0.,1.))) {}
};
Ray cameraRay(const Camera& camera,const Settings& s,double x,double y) {
    const double aspect=s.width*s.pixelAspect/s.height, tx=camera.aperture/(2*camera.focal),ty=tx/aspect;
    const double nx=2*x/s.width-1+2*camera.offsetX,ny=2*y/s.height-1+2*camera.offsetY*aspect;
    return {camera.transform.point(camera.orthographic?Vec3{nx*camera.orthoWidth*.5,ny*camera.orthoWidth*.5/aspect,0}:Vec3{}),
            camera.transform.vector(camera.orthographic?Vec3{0,0,-1}:Vec3{nx*tx,ny*ty,-1})};
}
Hit intersectPrepared(const PreparedCard& prepared,const Camera& camera,const Ray& ray) {
    const auto& card=*prepared.card;
    const Vec3 lo=prepared.inverse.point(ray.origin),ld=prepared.inverse.vector(ray.direction);
    if(std::abs(ld.z)<1e-12) return {};
    const double depth=-lo.z/ld.z;
    if(depth<camera.nearClip || depth>camera.farClip || card.width<=0 || card.height<=0) return {};
    const Vec3 p=lo+ld*depth;
    if(std::abs(p.x)>card.width*.5 || std::abs(p.y)>card.height*.5) return {};
    double u=p.x/card.width+.5,v=p.y/card.height+.5;
    if(card.projected) {
        const Vec3 q=prepared.projectorInverse.point(ray.origin+ray.direction*depth);
        if(-q.z<card.projector.nearClip || -q.z>card.projector.farClip) return {};
        const double px=card.projector.aperture/(2*card.projector.focal),py=px/card.projectionAspect;
        if(card.projector.orthographic) {u=q.x/card.projector.orthoWidth+.5;v=q.y/(card.projector.orthoWidth/card.projectionAspect)+.5;}
        else {u=q.x/(-q.z*px)*.5+.5;v=q.y/(-q.z*py)*.5+.5;}
        u-=card.projector.offsetX;v-=card.projector.offsetY*card.projectionAspect;
    }
    if(u<0 || u>1 || v<0 || v>1) return {};
    if(card.projected && card.texture) {
        u=u*card.texture->imageUvScale[0]+card.texture->imageUvOffset[0];
        v=v*card.texture->imageUvScale[1]+card.texture->imageUvOffset[1];
        if(u<0 || u>1 || v<0 || v>1) return {};
    }
    Hit hit;hit.card=0;hit.depth=depth;hit.u=u;hit.v=v;
    hit.normal=normalVector(prepared.inverse,{0,0,1});hit.material=card.material;
    hit.tangent=card.transform.vector({card.width,0,0});hit.bitangent=card.transform.vector({0,card.height,0});
    for(auto& uv:hit.mapUvs)uv={p.x/card.width+.5,p.y/card.height+.5};return hit;
}
void meshHits(const PreparedCard& prepared,const Camera& camera,const Ray& ray,int cardIndex,
              std::vector<Hit>& hits,std::vector<MeshHit>& scratch) {
    const auto& card=*prepared.card;scratch.clear();
    card.mesh->intersect(prepared.inverse.point(ray.origin),prepared.inverse.vector(ray.direction),camera.nearClip,camera.farClip,scratch);
    std::sort(scratch.begin(),scratch.end(),[](const MeshHit& a,const MeshHit& b){return a.depth<b.depth || (a.depth==b.depth && a.triangle<b.triangle);});
    double lastDepth=-1;
    for(const auto& meshHit:scratch) {
        // A ray on a shared edge must composite that surface only once.
        if(lastDepth>=0 && std::abs(meshHit.depth-lastDepth)<=1e-10*std::max(1.,std::abs(meshHit.depth)))continue;
        lastDepth=meshHit.depth;
        const auto& triangle=card.mesh->triangles[meshHit.triangle];const std::array<double,3> b{1-meshHit.b1-meshHit.b2,meshHit.b1,meshHit.b2};
        Hit hit;hit.card=cardIndex;hit.depth=meshHit.depth;hit.primitive=meshHit.triangle;hit.tint.fill(0);
        hit.material=card.overrideMaterial?card.material:triangle.material;
        Vec3 normal;
        for(int i=0;i<3;++i) {
            normal=normal+triangle.vertices[i].normal*b[i];
            hit.u+=b[i]*triangle.vertices[i].uv[0];hit.v+=b[i]*triangle.vertices[i].uv[1];
            for(int j=0;j<3;++j)for(int c=0;c<2;++c)hit.mapUvs[j][c]+=b[i]*(card.maps[j].useMeshUv?triangle.vertices[i].uv[c]:triangle.vertices[i].mapUvs[j][c]);
            for(int c=0;c<4;++c)hit.tint[c]+=float(b[i])*triangle.vertices[i].color[c];
        }
        if(dot(normal,normal)<1e-20)normal=cross(triangle.vertices[1].position-triangle.vertices[0].position,triangle.vertices[2].position-triangle.vertices[0].position);
        hit.normal=normalVector(prepared.inverse,normal);
        if(card.maps[0].texture) {
            const auto& a=triangle.vertices[0];const auto& b=triangle.vertices[1];const auto& c=triangle.vertices[2];
            const auto ua=card.maps[0].useMeshUv?a.uv:a.mapUvs[0],ub=card.maps[0].useMeshUv?b.uv:b.mapUvs[0],uc=card.maps[0].useMeshUv?c.uv:c.mapUvs[0];
            const double u1=ub[0]-ua[0],v1=ub[1]-ua[1],u2=uc[0]-ua[0],v2=uc[1]-ua[1];
            const double det=u1*v2-v1*u2;
            if(std::abs(det)>1e-20) {
                const auto e1=card.transform.vector(b.position-a.position),e2=card.transform.vector(c.position-a.position);
                hit.tangent=(e1*v2-e2*v1)*(1/det);hit.bitangent=(e2*u1-e1*u2)*(1/det);
            }
        }
        for(int c=0;c<4;++c)if(c==3 || !card.texture)hit.tint[c]*=triangle.color[c];
        if(card.projected) {
            const Vec3 q=prepared.projectorInverse.point(ray.origin+ray.direction*hit.depth);
            if(-q.z<card.projector.nearClip || -q.z>card.projector.farClip)continue;
            const double px=card.projector.aperture/(2*card.projector.focal),py=px/card.projectionAspect;
            if(card.projector.orthographic){hit.u=q.x/card.projector.orthoWidth+.5;hit.v=q.y/(card.projector.orthoWidth/card.projectionAspect)+.5;}
            else {hit.u=q.x/(-q.z*px)*.5+.5;hit.v=q.y/(-q.z*py)*.5+.5;}
            hit.u-=card.projector.offsetX;hit.v-=card.projector.offsetY*card.projectionAspect;
            if(hit.u<0 || hit.u>1 || hit.v<0 || hit.v>1)continue;
        }else if(!card.texture || !card.texture->materialSampling){hit.u-=std::floor(hit.u);hit.v-=std::floor(hit.v);}
        if(card.texture && !card.texture->materialSampling) {
            hit.u=hit.u*card.texture->imageUvScale[0]+card.texture->imageUvOffset[0];
            hit.v=hit.v*card.texture->imageUvScale[1]+card.texture->imageUvOffset[1];
            if(hit.u<0 || hit.u>1 || hit.v<0 || hit.v>1)continue;
        }
        hits.push_back(hit);
    }
}
std::array<float,4> surfaceColor(const Card& card,const Hit& hit) {
    auto color=card.texture?card.texture->sample(hit.u,hit.v):std::array<float,4>{1,1,1,1};
    const auto& base=hit.material.baseColor;
    for(int c=0;c<3;++c)color[c]*=hit.tint[c]*hit.tint[3]*base[c]*base[3];
    color[3]*=hit.tint[3]*base[3];return color;
}
double shadowTransmission(const std::vector<PreparedCard>& cards,Vec3 point,Vec3 normal,Vec3 towardLight,double distance,double bias) {
    const Ray ray{point+normal*bias,towardLight};Camera range;range.nearClip=bias;range.farClip=distance-bias;
    if(range.farClip<=range.nearClip)return 1;
    double transmission=1;std::vector<Hit> hits;std::vector<MeshHit> scratch;
    for(size_t i=0;i<cards.size();++i) {
        const auto& card=*cards[i].card;hits.clear();
        if(card.mesh)meshHits(cards[i],range,ray,int(i),hits,scratch);
        else if(card.texture || card.material.lit) {
            auto hit=intersectPrepared(cards[i],range,ray);if(hit.card>=0)hits.push_back(hit);
        }
        for(const auto& hit:hits) {
            const double alpha=std::clamp(surfaceColor(card,hit)[3],0.f,1.f)*cards[i].opacity;
            transmission*=1-alpha;if(transmission<=0)return 0;
        }
    }
    return transmission;
}
std::array<float,4> litColor(const std::vector<PreparedCard>& cards,const Hit& hit,const Ray& ray,const Settings& settings,
                           const std::vector<Matrix>& environmentInverses,std::array<float,4> color) {
    if(!hit.material.lit || color[3]<=0)return color;
    const Vec3 base{color[0]/color[3],color[1]/color[3],color[2]/color[3]},view=normalized(ray.direction*(-1));
    Vec3 normal=hit.normal;if(dot(normal,view)<0)normal=normal*(-1);
    const Vec3 geometricNormal=normal;
    Material material=hit.material;const auto& maps=cards[hit.card].card->maps;
    if(maps[1].texture)material.roughness=std::clamp(double(maps[1].texture->sample(hit.mapUvs[1][0],hit.mapUvs[1][1])[maps[1].channel]),0.,1.);
    if(maps[2].texture)material.metallic=std::clamp(double(maps[2].texture->sample(hit.mapUvs[2][0],hit.mapUvs[2][1])[maps[2].channel]),0.,1.);
    if(maps[0].texture) {
        const Vec3 tangent=normalized(hit.tangent-normal*dot(normal,hit.tangent));
        const Vec3 crossBitangent=cross(normal,tangent);
        const Vec3 bitangent=crossBitangent*(dot(crossBitangent,hit.bitangent)<0?-1:1);
        const auto sample=maps[0].texture->sample(hit.mapUvs[0][0],hit.mapUvs[0][1]);
        const Vec3 mapped=tangent*sample[0]+bitangent*sample[1]+normal*sample[2];
        if(dot(tangent,tangent)>0 && dot(mapped,mapped)>1e-20)normal=normalized(mapped);
    }
    const Vec3 point=ray.origin+ray.direction*hit.depth;Vec3 shaded=material.emission;
    for(size_t lightIndex=0;lightIndex<settings.lights.size();++lightIndex) {
        const auto& light=settings.lights[lightIndex];
        if(light.intensity==0)continue;
        if(light.type==LightType::Environment) {
            for(bool specular:{false,true})for(unsigned i=0;i<light.environmentSamples;++i) {
                const auto sample=environmentSample(material,base,normal,view,i,light.environmentSamples,specular);
                if(dot(sample.weight,sample.weight)==0)continue;
                const double transmission=light.shadows?shadowTransmission(cards,point,geometricNormal,sample.direction,1e30,light.shadowBias):1;
                const Vec3 radiance=environmentRadiance(light,environmentInverses[lightIndex],sample.direction)*transmission;
                shaded=shaded+Vec3{sample.weight.x*radiance.x,sample.weight.y*radiance.y,sample.weight.z*radiance.z};
            }
            continue;
        }
        const Vec3 forward=normalized(light.transform.vector({0,0,-1}));
        Vec3 toward=forward*(-1);double distance=1e30,attenuation=1;
        if(light.type!=LightType::Distant) {
            const Vec3 offset=light.transform.point({})-point;const double squared=dot(offset,offset);
            distance=std::sqrt(squared);toward=normalized(offset);attenuation=1/std::max(squared,1e-8);
            if(light.type==LightType::Spot) {
                const double outer=std::cos(light.coneAngle*3.14159265358979323846/180),inner=std::cos(light.coneAngle*(1-light.coneSoftness)*3.14159265358979323846/180);
                const double cosine=dot(forward,toward*(-1));
                const double t=inner>outer?std::clamp((cosine-outer)/(inner-outer),0.,1.):double(cosine>=outer);
                attenuation*=t*t*(3-2*t);
            }
        }
        if(dot(normal,toward)<=0 || attenuation<=0 || light.intensity==0)continue;
        if(light.shadows)attenuation*=shadowTransmission(cards,point,geometricNormal,toward,distance,light.shadowBias);
        const Vec3 radiance=light.color*(light.intensity*std::exp2(light.exposure)*attenuation);
        shaded=shaded+shadeDirect(material,base,normal,view,toward,radiance);
    }
    color[0]=float(shaded.x*color[3]);color[1]=float(shaded.y*color[3]);color[2]=float(shaded.z*color[3]);return color;
}
}
Hit intersect(const Card& card,const Camera& camera,const Settings& s,double x,double y) {
    const PreparedCard prepared(card);const Ray ray=cameraRay(camera,s,x,y);
    if(!card.mesh)return intersectPrepared(prepared,camera,ray);
    std::vector<Hit> hits;std::vector<MeshHit> scratch;meshHits(prepared,camera,ray,0,hits,scratch);
    return hits.empty()?Hit():hits.front();
}
std::vector<float> renderCpu(const std::vector<Card>& cards,const Camera& camera,const Settings& s,const std::function<bool()>& abort) {
    validate(camera,s,cards);
    std::vector<PreparedCard> prepared;prepared.reserve(cards.size());
    for(const auto& card:cards)prepared.emplace_back(card);
    std::vector<Matrix> environmentInverses;environmentInverses.reserve(s.lights.size());
    for(const auto& light:s.lights)environmentInverses.push_back(light.type==LightType::Environment?environmentToLocal(light.transform):Matrix());
    std::vector<float> result(size_t(s.width)*s.height*4);
    std::vector<Hit> hits;hits.reserve(cards.size());
    std::vector<MeshHit> meshScratch;
    for(int y=0;y<s.height;++y) {
        if(abort && abort()) throw std::runtime_error("3D render aborted.");
        for(int x=0;x<s.width;++x) for(int sy=0;sy<s.samples;++sy) for(int sx=0;sx<s.samples;++sx) {
            hits.clear();
            const Ray ray=cameraRay(camera,s,x+(sx+.5)/s.samples,y+(sy+.5)/s.samples);
            for(size_t i=0;i<cards.size();++i) {
                if(cards[i].mesh){meshHits(prepared[i],camera,ray,int(i),hits,meshScratch);continue;}
                if(!cards[i].texture && !cards[i].material.lit)continue;
                Hit hit=intersectPrepared(prepared[i],camera,ray);
                if(hit.card>=0) {hit.card=int(i);hits.push_back(hit);}
            }
            // Input index preserves the previous stable order for equal depths,
            // without allocating a stable-sort scratch buffer for every sample.
            std::sort(hits.begin(),hits.end(),[](const Hit&a,const Hit&b){return a.depth<b.depth || (a.depth==b.depth && (a.card<b.card || (a.card==b.card && a.primitive<b.primitive)));});
            float transmission=1;
            for(const Hit&hit:hits) {
                const auto& card=cards[hit.card];auto color=surfaceColor(card,hit);
                const float opacity=prepared[hit.card].opacity,alpha=std::clamp(color[3],0.f,1.f)*opacity;
                const size_t at=(size_t(y)*s.width+x)*4;
                if(s.pass==RenderPass::Depth) {
                    if(alpha<=0)continue;
                    const float depth=float(hit.depth);
                    if(result[at+3]==0 || depth<result[at])for(int c=0;c<3;++c)result[at+c]=depth;
                    result[at+3]+=1.f/(s.samples*s.samples);break;
                }
                color=litColor(prepared,hit,ray,s,environmentInverses,color);
                const float weight=transmission/(s.samples*s.samples);
                for(int c=0;c<3;++c) result[at+c]+=color[c]*opacity*weight;
                result[at+3]+=alpha*weight;transmission*=1-alpha;
            }
        }
    }
    return result;
}
void makeDepthPreview(std::vector<float>& pixels,double nearDepth,double farDepth) {
    if(pixels.size()%4)throw std::invalid_argument("Depth preview requires RGBA pixel storage.");
    if(!std::isfinite(nearDepth) || !std::isfinite(farDepth) || nearDepth<0 || farDepth<=nearDepth)
        throw std::invalid_argument("Depth preview requires a finite range with 0 <= near < far.");
    for(size_t i=0;i<pixels.size();i+=4) {
        const float gray=float(std::clamp((farDepth-pixels[i])/(farDepth-nearDepth),0.,1.))*pixels[i+3];
        for(int c=0;c<3;++c)pixels[i+c]=gray;
    }
}
namespace {
constexpr int maxCards=16;
struct alignas(16) GpuCard {float inverse[16],projector[16],size[4],projection[4],clip[4],textureUv[4];};
struct alignas(16) Uniforms {float camera[16],film[4],output[4],clip[4];GpuCard cards[maxCards];};
void copyMatrix(float* to,const Matrix& from){for(int i=0;i<16;++i)to[i]=float(from.m[i]);}
std::string shader() {
    std::ostringstream out;out<<"#version 450\n";
    for(int i=0;i<maxCards;++i)out<<"layout(set=0,binding="<<i<<") uniform sampler2D image"<<i<<";\n";
    out<<R"(
struct Card {mat4 inverse;mat4 projector;vec4 size;vec4 projection;vec4 clip;vec4 textureUv;};
layout(std140,set=0,binding=16) uniform Parameters {mat4 camera;vec4 film;vec4 outputInfo;vec4 clip;Card cards[16];} p;
layout(location=0) out vec4 color;
vec4 readImage(int i,vec2 uv) {
)";
    for(int i=0;i<maxCards;++i)out<<"if(i=="<<i<<")return texture(image"<<i<<",uv);\n";
    out<<R"(return vec4(0);}
void main() {
    color=vec4(0);int samples=int(p.outputInfo.w);bool depthPass=p.clip.z>.5;
    float nearest=0.,coverage=0.;
    for(int sy=0;sy<samples;++sy)for(int sx=0;sx<samples;++sx) {
        vec2 xy=gl_FragCoord.xy-vec2(.5)+vec2(sx+.5,sy+.5)/float(samples);
        vec2 ndc=2.*xy/p.outputInfo.xy-1.+vec2(p.film.w,p.clip.w);
        bool ortho=p.film.z>.5;
        vec3 origin=(p.camera*vec4(ortho?vec3(ndc*p.film.xy,0):vec3(0),1)).xyz;
        vec3 direction=(p.camera*vec4(ortho?vec3(0,0,-1):vec3(ndc*p.film.xy,-1),0)).xyz;
        float depths[16];vec2 uvs[16];int indices[16];int count=0;
        for(int i=0;i<int(p.outputInfo.z);++i) {
            Card card=p.cards[i];if(any(lessThanEqual(card.size.xy,vec2(0))))continue;
            vec3 o=(card.inverse*vec4(origin,1)).xyz,d=(card.inverse*vec4(direction,0)).xyz;
            if(abs(d.z)<1.e-10)continue;
            float t=-o.z/d.z;vec3 local=o+d*t;
            if(t<p.clip.x || t>p.clip.y || any(greaterThan(abs(local.xy),card.size.xy*.5)))continue;
            vec2 uv=local.xy/card.size.xy+.5;
            if(card.size.w>.5) {
                vec3 q=(card.projector*vec4(origin+direction*t,1)).xyz;
                if(-q.z<card.clip.x || -q.z>card.clip.y)continue;
                uv=card.projection.z>.5 ? q.xy/card.projection.xy*.5+.5 : q.xy/(-q.z*card.projection.xy)*.5+.5;
                uv-=card.clip.zw;
            }
            if(any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1))))continue;
            if(card.size.w>.5)uv=uv*card.textureUv.xy+card.textureUv.zw;
            if(any(lessThan(uv,vec2(0))) || any(greaterThan(uv,vec2(1))))continue;
            int j=count;
            while(j>0 && depths[j-1]>t){depths[j]=depths[j-1];uvs[j]=uvs[j-1];indices[j]=indices[j-1];--j;}
            depths[j]=t;uvs[j]=uv;indices[j]=i;++count;
        }
        float transmission=1.;vec4 sum=vec4(0);
        for(int j=0;j<count;++j) {
            int i=indices[j];vec4 c=readImage(i,uvs[j]);float a=clamp(c.a,0.,1.)*p.cards[i].size.z;
            if(depthPass) {
                if(a<=0.)continue;
                nearest=coverage>0.?min(nearest,depths[j]):depths[j];
                coverage+=1./float(samples*samples);break;
            }
            sum+=vec4(c.rgb*p.cards[i].size.z,a)*transmission;transmission*=1.-a;
        }
        color+=sum/float(samples*samples);
    }
    if(depthPass)color=vec4(vec3(nearest),coverage);
})";
    return out.str();
}
}
Renderer::Renderer(std::shared_ptr<VulkanDevice> device):_device(std::move(device)) {
    FragmentLayout layout;layout.uniformBinding=16;layout.inputBindings.clear();
    for(int i=0;i<maxCards;++i)layout.inputBindings.push_back(i);
    _program=std::make_unique<FragmentProgram>(_device,shader(),uint32_t(sizeof(Uniforms)),layout);
}
std::vector<float> Renderer::render(const std::vector<Card>& cards,const Camera& c,const Settings& s) {
    validate(c,s,cards);
    if(std::any_of(cards.begin(),cards.end(),[](const Card& card){return bool(card.mesh) || card.material.lit;}))
        return renderMeshes(cards,c,s);
    if(cards.size()>maxCards)throw std::invalid_argument("Vulkan card renderer supports up to 16 cards; select CPU for larger scenes.");
    Uniforms u{};copyMatrix(u.camera,c.transform);
    const double aspect=s.width*s.pixelAspect/s.height;
    u.film[0]=float(c.orthographic?c.orthoWidth*.5:c.aperture/(2*c.focal));u.film[1]=float(u.film[0]/aspect);u.film[2]=c.orthographic?1.f:0.f;u.film[3]=float(2*c.offsetX);u.clip[3]=float(2*c.offsetY*aspect);
    u.output[0]=float(s.width);u.output[1]=float(s.height);u.output[2]=float(cards.size());u.output[3]=float(s.samples);
    u.clip[0]=float(c.nearClip);u.clip[1]=float(c.farClip);u.clip[2]=s.pass==RenderPass::Depth?1.f:0.f;
    std::vector<std::unique_ptr<VulkanImage>> textures;
    std::vector<FragmentInput> inputs(maxCards);
    for(size_t i=0;i<cards.size();++i) {
        const Card& card=cards[i];auto& dest=u.cards[i];copyMatrix(dest.inverse,card.transform.inverse());
        copyMatrix(dest.projector,card.projector.transform.inverse());
        dest.size[0]=float(card.width);dest.size[1]=float(card.height);dest.size[2]=float(std::clamp(card.opacity,0.,1.));dest.size[3]=card.projected?1.f:0.f;
        const auto& camera=card.projector;
        dest.projection[0]=float(camera.orthographic?camera.orthoWidth*.5:camera.aperture/(2*camera.focal));
        dest.projection[1]=float(dest.projection[0]/card.projectionAspect);dest.projection[2]=camera.orthographic?1.f:0.f;
        dest.clip[0]=float(camera.nearClip);dest.clip[1]=float(camera.farClip);
        dest.clip[2]=float(camera.offsetX);dest.clip[3]=float(camera.offsetY*card.projectionAspect);
        dest.textureUv[0]=dest.textureUv[1]=1;
        if(card.texture && card.texture->width>0 && card.texture->height>0) {
            for(int j=0;j<2;++j) {
                dest.textureUv[j]=float(card.texture->imageUvScale[j]);
                dest.textureUv[j+2]=float(card.texture->imageUvOffset[j]);
            }
            auto image=std::make_unique<VulkanImage>(_device,card.texture->width,card.texture->height);
            const auto& pixels=card.texture->pixelData();image->upload(pixels.data(),pixels.size()*sizeof(float));
            inputs[i]={image.get(),TextureWrap::Clamp,1};textures.push_back(std::move(image));
        }
    }
    VulkanImage output(_device,s.width,s.height);
    _program->renderMany(output,{0,0,uint32_t(s.width),uint32_t(s.height)},inputs,&u,sizeof(u));
    const auto bytes=output.download();std::vector<float> result(bytes.size()/sizeof(float));
    std::memcpy(result.data(),bytes.data(),bytes.size());return result;
}
}}}
