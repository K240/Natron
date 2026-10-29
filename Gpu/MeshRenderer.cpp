// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/CardScene.h"
#include "Gpu/Lighting.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace Natron { namespace Gpu { namespace Cards {
namespace {
constexpr size_t maxObjects=16;
struct alignas(16) GpuObject {
    float inverse[16],projector[16],size[4],projection[4],clip[4],textureUv[4];
    uint32_t geometry[4]; // root + 1, image index + 1, override material, material UV flag
    GpuMaterial surface;
    uint32_t maps[4],channels[4];
    uint32_t compositing[4];
    float shadow[4];
};
struct alignas(16) GpuSampler {float uvRows[2][4];uint32_t flags[4];};
struct alignas(16) Uniforms {float camera[16],film[4],output[4],clip[4],lightInfo[4];GpuLight lights[maxLights];GpuObject objects[maxObjects];GpuSampler samplers[maxObjects];};
struct alignas(16) Triangle {float positions[3][4],uv01[4],uv2[4],colors[3][4],material[4];uint32_t info[4];float normals[3][4];GpuMaterial surface;float mapUvs[9][4];};
struct alignas(16) BvhNode {float minimum[4],maximum[4];uint32_t info[4];}; // begin, count, left, right
static_assert(sizeof(Triangle)==400 && sizeof(BvhNode)==48 && sizeof(GpuObject)==320 && sizeof(GpuSampler)==48 && sizeof(GpuLight)==144,"Mesh std430 layout mismatch.");
float asFloat(double value) {
    const float result=float(value);
    if(!std::isfinite(result))throw std::invalid_argument("3D geometry exceeds Vulkan float precision; select CPU.");
    return result;
}
void copyMatrix(float* to,const Matrix& from){for(int i=0;i<16;++i)to[i]=asFloat(from.m[i]);}
std::string meshShader() {
    std::ostringstream out;out<<"#version 450\n";
    for(size_t i=0;i<maxObjects;++i)out<<"layout(set=0,binding="<<i<<") uniform sampler2D image"<<i<<";\n";
    out<<lightingShader()<<R"(
struct Object {mat4 inverse;mat4 projector;vec4 size;vec4 projection;vec4 clip;vec4 textureUv;uvec4 geometry;Surface surface;uvec4 maps;uvec4 channels;uvec4 compositing;vec4 shadow;};
struct Sampler {vec4 uvRows[2];uvec4 flags;};
layout(std140,set=0,binding=16) uniform Parameters {mat4 camera;vec4 film;vec4 outputInfo;vec4 clip;vec4 lightInfo;Light lights[16];Object objects[16];Sampler samplers[16];} p;
struct Triangle {vec4 positions[3];vec4 uv01;vec4 uv2;vec4 colors[3];vec4 material;uvec4 info;vec4 normals[3];Surface surface;vec4 mapUvs[9];};
struct Node {vec4 minimum;vec4 maximum;uvec4 info;};
layout(std430,set=0,binding=17) readonly buffer Triangles {Triangle triangles[];};
layout(std430,set=0,binding=18) readonly buffer Nodes {Node nodes[];};
layout(location=0) out vec4 color;
struct Hit {float depth;vec2 uv;vec4 tint;vec3 normal;Surface surface;vec2 mapUv[3];vec3 tangent;vec3 bitangent;};
Hit miss() {
    Hit hit;hit.depth=-1.;hit.uv=vec2(0);hit.tint=vec4(1);hit.normal=vec3(0,0,1);
    hit.surface=Surface(vec4(1),vec4(0),vec4(0,0,1.5,0));
    for(int j=0;j<3;++j)hit.mapUv[j]=vec2(0);
    hit.tangent=hit.bitangent=vec3(0);return hit;
}
bool address(inout float uv,uint mode) {
    if(mode==0u)return uv>=0. && uv<=1.;
    if(mode==1u)uv=clamp(uv,0.,1.);
    else if(mode==2u)uv=fract(uv);
    else {uv=mod(uv,2.);if(uv>1.)uv=2.-uv;}
    return true;
}
ivec2 texelAddress(ivec2 at,ivec2 size,uint flags) {
    if((flags&1u)!=0u && ((flags>>1)&3u)==2u)at.x=(at.x%size.x+size.x)%size.x;
    if((flags&1u)!=0u && ((flags>>3)&3u)==2u)at.y=(at.y%size.y+size.y)%size.y;
    return clamp(at,ivec2(0),size-1);
}
vec4 bilinear(sampler2D source,vec2 uv,Sampler object) {
    uint flags=object.flags.x;
    if((flags&1u)!=0u) {
        uv=vec2(dot(object.uvRows[0].xyz,vec3(uv,1)),dot(object.uvRows[1].xyz,vec3(uv,1)));
        if(!address(uv.x,(flags>>1)&3u) || !address(uv.y,(flags>>3)&3u))return vec4(0);
    }
    ivec2 size=textureSize(source,0);vec2 position=uv*vec2(size)-.5;
    ivec2 at=ivec2(floor(position));vec2 f=fract(position);
    vec4 a=texelFetch(source,texelAddress(at,size,flags),0),b=texelFetch(source,texelAddress(at+ivec2(1,0),size,flags),0);
    vec4 c=texelFetch(source,texelAddress(at+ivec2(0,1),size,flags),0),d=texelFetch(source,texelAddress(at+ivec2(1),size,flags),0);
    vec4 result=mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
    if((flags&1u)!=0u && (flags&32u)==0u){result.a=clamp(result.a,0.,1.);result.rgb*=result.a;}
    return result;
}
vec4 readImage(int i,vec2 uv) {
)";
    for(size_t i=0;i<maxObjects;++i)out<<"if(i=="<<i<<")return bilinear(image"<<i<<",uv,p.samplers[i]);\n";
    out<<R"(return vec4(0);}
bool inside(vec2 uv) {return all(greaterThanEqual(uv,vec2(0))) && all(lessThanEqual(uv,vec2(1)));}
bool boxHit(Node node,vec3 o,vec3 d,float first,float last) {
    for(int axis=0;axis<3;++axis) {
        if(d[axis]==0.) {if(o[axis]<node.minimum[axis] || o[axis]>node.maximum[axis])return false;}
        else {
            float a=(node.minimum[axis]-o[axis])/d[axis],b=(node.maximum[axis]-o[axis])/d[axis];
            first=max(first,min(a,b));last=min(last,max(a,b));if(first>last)return false;
        }
    }
    return true;
}
// Shear into ray coordinates so adjacent triangles use identical edge equations.
// 'precise' prevents FMA contraction from opening cracks on shared edges.
bool triangleHit(Triangle triangle,vec3 o,vec3 d,float first,float last,out float t,out vec3 bary) {
    vec3 ad=abs(d);int kz=ad.x>=ad.y && ad.x>=ad.z?0:ad.y>=ad.z?1:2;
    int kx=(kz+1)%3,ky=(kx+1)%3;if(d[kz]<0.){int swapAxis=kx;kx=ky;ky=swapAxis;}
    vec3 a=triangle.positions[0].xyz-o,b=triangle.positions[1].xyz-o,c=triangle.positions[2].xyz-o;
    float sx=d[kx]/d[kz],sy=d[ky]/d[kz],sz=1./d[kz];
    precise vec2 pa=vec2(a[kx]-sx*a[kz],a[ky]-sy*a[kz]);
    precise vec2 pb=vec2(b[kx]-sx*b[kz],b[ky]-sy*b[kz]);
    precise vec2 pc=vec2(c[kx]-sx*c[kz],c[ky]-sy*c[kz]);
    precise float e0=pb.x*pc.y-pb.y*pc.x,e1=pc.x*pa.y-pc.y*pa.x,e2=pa.x*pb.y-pa.y*pb.x;
    if((min(e0,min(e1,e2))<0. && max(e0,max(e1,e2))>0.))return false;
    float det=e0+e1+e2;if(det==0.)return false;
    t=(e0*a[kz]+e1*b[kz]+e2*c[kz])*sz/det;
    bary=vec3(e0,e1,e2)/det;
    return t>=first && t<=last;
}
Hit nextHit(int index,vec3 origin,vec3 direction,float first,float last) {
    Object object=p.objects[index];vec3 o=(object.inverse*vec4(origin,1)).xyz,d=(object.inverse*vec4(direction,0)).xyz;
    Hit hit=miss();
    if(object.geometry.x==0u) {
        if(any(lessThanEqual(object.size.xy,vec2(0))) || abs(d.z)<1.e-10)return hit;
        float t=-o.z/d.z;vec3 local=o+d*t;
        if(t<first || t>last || any(greaterThan(abs(local.xy),object.size.xy*.5)))return hit;
        hit.depth=t;hit.uv=local.xy/object.size.xy+.5;
        hit.normal=safeNormalize(transpose(mat3(object.inverse))*vec3(0,0,1));hit.surface=object.surface;
        for(int j=0;j<3;++j)hit.mapUv[j]=hit.uv;
        mat3 world=inverse(mat3(object.inverse));hit.tangent=world*vec3(object.size.x,0,0);hit.bitangent=world*vec3(0,object.size.y,0);
    } else {
        uint stack[64];int count=1;stack[0]=object.geometry.x-1u;
        float nearest=last;uint primitive=0xffffffffu,chosen=0xffffffffu;vec3 bary=vec3(0);
        while(count>0) {
            Node node=nodes[stack[--count]];
            if(!boxHit(node,o,d,first,nearest))continue;
            if(node.info.y==0u) {stack[count++]=node.info.z;stack[count++]=node.info.w;continue;}
            for(uint j=node.info.x;j<node.info.x+node.info.y;++j) {
                float t;vec3 b;
                if(triangleHit(triangles[j],o,d,first,nearest,t,b) && (chosen==0xffffffffu || t<nearest || triangles[j].info.x<primitive)) {
                    nearest=t;primitive=triangles[j].info.x;chosen=j;bary=b;
                }
            }
        }
        if(chosen==0xffffffffu)return hit;
        Triangle triangle=triangles[chosen];
        vec2 uv=bary.x*triangle.uv01.xy+bary.y*triangle.uv01.zw+bary.z*triangle.uv2.xy;
        vec4 tint=bary.x*triangle.colors[0]+bary.y*triangle.colors[1]+bary.z*triangle.colors[2];
        tint*=object.geometry.y!=0u?vec4(1,1,1,triangle.material.a):triangle.material;
        vec3 normal=bary.x*triangle.normals[0].xyz+bary.y*triangle.normals[1].xyz+bary.z*triangle.normals[2].xyz;
        if(dot(normal,normal)<1.e-20)normal=cross(triangle.positions[1].xyz-triangle.positions[0].xyz,triangle.positions[2].xyz-triangle.positions[0].xyz);
        hit.depth=nearest;hit.uv=(object.geometry.w&1u)!=0u?uv:fract(uv);hit.tint=tint;
        hit.normal=safeNormalize(transpose(mat3(object.inverse))*normal);hit.surface=object.geometry.z!=0u?object.surface:triangle.surface;
        for(int j=0;j<3;++j)hit.mapUv[j]=(object.channels[j]&4u)!=0u?uv:bary.x*triangle.mapUvs[j*3].xy+bary.y*triangle.mapUvs[j*3+1].xy+bary.z*triangle.mapUvs[j*3+2].xy;
        if(object.maps.x!=0u) {
            vec2 a=(object.channels.x&4u)!=0u?triangle.uv01.zw-triangle.uv01.xy:triangle.mapUvs[1].xy-triangle.mapUvs[0].xy;
            vec2 b=(object.channels.x&4u)!=0u?triangle.uv2.xy-triangle.uv01.xy:triangle.mapUvs[2].xy-triangle.mapUvs[0].xy;
            float det=a.x*b.y-a.y*b.x;
            if(abs(det)>1.e-20) {
                mat3 world=inverse(mat3(object.inverse));
                vec3 e1=world*(triangle.positions[1].xyz-triangle.positions[0].xyz),e2=world*(triangle.positions[2].xyz-triangle.positions[0].xyz);
                hit.tangent=(e1*b.y-e2*a.y)/det;hit.bitangent=(e2*a.x-e1*b.x)/det;
            }
        }
    }
    if(object.size.w>.5) {
        vec3 q=(object.projector*vec4(origin+direction*hit.depth,1)).xyz;
        if(-q.z<object.clip.x || -q.z>object.clip.y){hit.uv=vec2(-1);return hit;}
        hit.uv=object.projection.z>.5?q.xy/object.projection.xy*.5+.5:q.xy/(-q.z*object.projection.xy)*.5+.5;
        hit.uv-=object.clip.zw;
    }
    if((object.geometry.w&1u)==0u || object.size.w>.5) {
        if(!inside(hit.uv)){hit.uv=vec2(-1);return hit;}
        if(object.size.w>.5 || object.geometry.x!=0u)hit.uv=hit.uv*object.textureUv.xy+object.textureUv.zw;
    }
    return hit;
}
vec4 surfaceColor(int index,Hit hit) {
    Object object=p.objects[index];
    if(((object.geometry.w&1u)==0u || object.size.w>.5) && !inside(hit.uv))return vec4(0);
    vec4 c=object.geometry.y==0u && (object.geometry.x!=0u || hit.surface.parameters.w>.5 || object.compositing.x!=0u || object.compositing.w!=0u)?vec4(1):readImage(int(object.geometry.y)-1,hit.uv);
    c*=vec4(hit.tint.rgb*hit.tint.a,hit.tint.a);
    return c*vec4(hit.surface.base.rgb*hit.surface.base.a,hit.surface.base.a);
}
float shadowTransmission(vec3 point,vec3 normal,vec3 toward,float last,float bias,bool reference) {
    vec3 origin=point+normal*bias;float transmission=1.;last-=bias;if(last<=bias)return transmission;
    for(int i=0;i<int(p.outputInfo.z);++i) {
        if(p.objects[i].compositing.z==0u || (reference && p.objects[i].compositing.x==0u && p.objects[i].compositing.w==0u))continue;
        Hit hit=nextHit(i,origin,toward,bias,last);
        while(hit.depth>=0.) {
            transmission*=1.-clamp(surfaceColor(i,hit).a,0.,1.)*p.objects[i].size.z;
            if(transmission<=0.)return 0.;
            hit=p.objects[i].geometry.x==0u?miss():nextHit(i,origin,toward,uintBitsToFloat(floatBitsToUint(hit.depth)+1u),last);
        }
    }
    return transmission;
}
vec4 litColor(int index,Hit hit,vec3 origin,vec3 direction,vec4 c,bool reference,bool receiver) {
    if(receiver){hit.surface.base=vec4(1);hit.surface.parameters=vec4(0,1,1,1);hit.surface.emission=vec4(0);c=vec4(1);}
    if(hit.surface.parameters.w<.5 || c.a<=0.)return c;
    vec3 base=c.rgb/c.a,view=safeNormalize(-direction),normal=hit.normal;
    if(dot(normal,view)<0.)normal=-normal;
    vec3 geometricNormal=normal;Object object=p.objects[index];
    if(!receiver && object.maps.y!=0u)hit.surface.parameters.y=clamp(readImage(int(object.maps.y)-1,hit.mapUv[1])[int(object.channels.y&3u)],0.,1.);
    if(!receiver && object.maps.z!=0u)hit.surface.parameters.x=clamp(readImage(int(object.maps.z)-1,hit.mapUv[2])[int(object.channels.z&3u)],0.,1.);
    if(!receiver && object.maps.x!=0u) {
        vec3 tangent=safeNormalize(hit.tangent-normal*dot(normal,hit.tangent)),bitangent=cross(normal,tangent);
        if(dot(bitangent,hit.bitangent)<0.)bitangent=-bitangent;
        vec3 mapValue=readImage(int(object.maps.x)-1,hit.mapUv[0]).rgb;
        vec3 mapped=tangent*mapValue.x+bitangent*mapValue.y+normal*mapValue.z;
        if(dot(tangent,tangent)>0. && dot(mapped,mapped)>1.e-20)normal=safeNormalize(mapped);
    }
    vec3 point=origin+direction*hit.depth,shaded=hit.surface.emission.rgb;
    for(int i=0;i<int(p.lightInfo.x);++i) {
        Light light=p.lights[i];vec3 toward=-light.direction.xyz;float distance=1.e30,attenuation=1.;
        if(light.color.w==0.)continue;
        if(light.position.w>2.5) {
            for(int lobe=0;lobe<2;++lobe)for(uint sampleIndex=0u;sampleIndex<light.environment.y;++sampleIndex) {
                vec3 weight;vec3 l=environmentSample(hit.surface,base,normal,view,sampleIndex,light.environment.y,lobe==1,weight);
                if(all(equal(weight,vec3(0))))continue;
                vec3 radiance=light.color.rgb*light.color.w;
                if(light.environment.x!=0u) {
                    vec3 d=safeNormalize((light.environmentInverse*vec4(l,0)).xyz);
                    vec2 uv=vec2(.5-atan(d.x,d.z)/6.283185307179586,.5+asin(clamp(d.y,-1.,1.))/3.141592653589793);
                    radiance*=max(vec3(0),readImage(int(light.environment.x)-1,uv).rgb);
                }
                if(light.cone.z>.5)radiance*=shadowTransmission(point,geometricNormal,l,1.e30,light.cone.w,reference);
                shaded+=weight*radiance;
            }
            continue;
        }
        if(light.position.w>.5) {
            vec3 offset=light.position.xyz-point;float squared=dot(offset,offset);distance=sqrt(squared);toward=safeNormalize(offset);attenuation=1./max(squared,1.e-8);
            if(light.position.w>1.5) {
                float cosine=dot(light.direction.xyz,-toward);
                float t=light.cone.y>light.cone.x?clamp((cosine-light.cone.x)/(light.cone.y-light.cone.x),0.,1.):float(cosine>=light.cone.x);
                attenuation*=t*t*(3.-2.*t);
            }
        }
        if(dot(normal,toward)<=0. || attenuation<=0. || light.color.w==0.)continue;
        if(light.cone.z>.5)attenuation*=shadowTransmission(point,geometricNormal,toward,distance,light.cone.w,reference);
        shaded+=shadeDirect(hit.surface,base,normal,view,toward,light.color.rgb*light.color.w*attenuation);
    }
    return vec4(shaded*c.a,c.a);
}
void main() {
    color=vec4(0);int samples=int(p.outputInfo.w),objects=int(p.outputInfo.z),pass=int(p.clip.z);bool depthPass=pass==1;
    float nearest=0.,coverage=0.;
    for(int sy=0;sy<samples;++sy)for(int sx=0;sx<samples;++sx) {
        vec2 xy=gl_FragCoord.xy-vec2(.5)+vec2(sx+.5,sy+.5)/float(samples),ndc=2.*xy/p.outputInfo.xy-1.+vec2(p.film.w,p.clip.w);
        bool ortho=p.film.z>.5;
        vec3 origin=(p.camera*vec4(ortho?vec3(ndc*p.film.xy,0):vec3(0),1)).xyz;
        vec3 direction=(p.camera*vec4(ortho?vec3(0,0,-1):vec3(ndc*p.film.xy,-1),0)).xyz;
        Hit hits[16];for(int i=0;i<objects;++i)hits[i]=p.objects[i].compositing.y==0u?miss():nextHit(i,origin,direction,p.clip.x,p.clip.y);
        float transmission=1.;vec4 sum=vec4(0);
        while(true) {
            int chosen=-1;
            for(int i=0;i<objects;++i)if(hits[i].depth>=0. && (chosen<0 || hits[i].depth<hits[chosen].depth))chosen=i;
            if(chosen<0)break;
            Hit hit=hits[chosen];Object object=p.objects[chosen];
            vec4 c=surfaceColor(chosen,hit);
            float alpha=clamp(c.a,0.,1.)*object.size.z;
            if(depthPass && alpha>0.) {
                nearest=coverage>0.?min(nearest,hit.depth):hit.depth;coverage+=1./float(samples*samples);break;
            }
            if(!depthPass) {
                if(pass==2){
                    if(object.compositing.x==1u || object.compositing.x==4u){
                        vec3 lit=litColor(chosen,hit,origin,direction,vec4(1),false,true).rgb;
                        vec3 ref=litColor(chosen,hit,origin,direction,vec4(1),true,true).rgb;
                        float reference=dot(ref,vec3(.2126,.7152,.0722));
                        float a=reference>1.e-8?clamp((1.-max(0.,dot(lit,vec3(.2126,.7152,.0722)))/reference)*object.shadow.a,0.,1.):0.;
                        sum+=vec4(object.shadow.rgb,1)*a*alpha*transmission;
                    }
                    if(object.compositing.x!=0u || object.compositing.w!=0u)transmission*=1.-alpha;
                }else if(pass==3){
                    if(object.compositing.x==2u || object.compositing.w!=0u)sum+=vec4(alpha*transmission);
                    transmission*=1.-alpha;
                }else{
                    if(object.compositing.x==0u && object.compositing.w==0u){c=litColor(chosen,hit,origin,direction,c,false,false);sum+=vec4(c.rgb*object.size.z,alpha)*transmission;}
                    transmission*=1.-alpha;
                }
                if(transmission==0.)break;
            }
            // Collapse coplanar shared-edge hits within float roundoff, then seek
            // the next surface. There is no fixed transparent-layer budget.
            hits[chosen]=object.geometry.x==0u?miss():nextHit(chosen,origin,direction,uintBitsToFloat(floatBitsToUint(hit.depth)+1u),p.clip.y);
        }
        color+=sum/float(samples*samples);
    }
    if(depthPass)color=vec4(vec3(nearest),coverage);
})";
    return out.str();
}
}
std::vector<float> Renderer::renderMeshes(const std::vector<Card>& cards,const Camera& camera,const Settings& s) {
    if(cards.size()>maxObjects)throw std::invalid_argument("Vulkan 3D renderer supports up to 16 geometry objects; select CPU for larger scenes.");
    Uniforms u{};copyMatrix(u.camera,camera.transform);
    const double aspect=camera.filmAspect(s.width*s.pixelAspect/s.height);
    u.film[0]=asFloat(camera.orthographic?camera.orthoWidth*.5:camera.aperture/(2*camera.focal));u.film[1]=asFloat(u.film[0]/aspect);u.film[2]=camera.orthographic?1.f:0.f;u.film[3]=asFloat(2*camera.offsetX);u.clip[3]=asFloat(2*camera.offsetY*aspect);
    u.output[0]=float(s.width);u.output[1]=float(s.height);u.output[2]=float(cards.size());u.output[3]=float(s.samples);
    u.clip[0]=asFloat(camera.nearClip);u.clip[1]=asFloat(camera.farClip);u.clip[2]=float(s.pass);
    packLights(u.lights,s.lights);u.lightInfo[0]=float(s.lights.size());
    std::vector<Triangle> triangles;std::vector<BvhNode> nodes;
    std::map<const Mesh*,uint32_t> roots;
    std::vector<std::unique_ptr<VulkanImage>> textures;std::vector<FragmentInput> inputs(maxObjects);
    std::map<const Texture*,uint32_t> textureIndices;
    auto upload=[&](const std::shared_ptr<const Texture>& texture) -> uint32_t {
        if(!texture || texture->width<=0 || texture->height<=0)return 0;
        const auto found=textureIndices.find(texture.get());if(found!=textureIndices.end())return found->second;
        const size_t i=textures.size();
        if(i>=maxObjects)throw std::invalid_argument("Vulkan 3D renderer supports up to 16 distinct texture samplers; select CPU for larger scenes.");
        const auto& pixels=texture->pixelData();
        if(pixels.size()!=size_t(texture->width)*texture->height*4)throw std::invalid_argument("Invalid material texture storage.");
        auto image=std::make_unique<VulkanImage>(_device,texture->width,texture->height);
        image->upload(pixels.data(),pixels.size()*sizeof(float));
        inputs[i]={image.get(),TextureWrap::Clamp,0};textures.push_back(std::move(image));
        auto& sampler=u.samplers[i];sampler.flags[0]=texture->materialSampling?1u|(uint32_t(texture->wrapS)<<1)|(uint32_t(texture->wrapT)<<3)|(texture->dataSampling?32u:0u):0u;
        for(int row=0;row<2;++row)for(int col=0;col<3;++col)sampler.uvRows[row][col]=asFloat(texture->uvTransform[row*3+col]);
        textureIndices.emplace(texture.get(),uint32_t(i+1));return uint32_t(i+1);
    };
    for(size_t i=0;i<s.lights.size();++i)if(s.lights[i].type==LightType::Environment)
        u.lights[i].environment[0]=upload(s.lights[i].environment);
    for(size_t i=0;i<cards.size();++i) {
        const Card& card=cards[i];auto& dest=u.objects[i];copyMatrix(dest.inverse,card.transform.inverse());
        dest.compositing[0]=uint32_t(card.compositing.role);dest.compositing[1]=card.compositing.cameraVisible;dest.compositing[2]=card.compositing.shadowVisible;dest.compositing[3]=card.compositing.holdout;
        dest.shadow[0]=asFloat(card.compositing.shadowTint.x);dest.shadow[1]=asFloat(card.compositing.shadowTint.y);dest.shadow[2]=asFloat(card.compositing.shadowTint.z);dest.shadow[3]=asFloat(card.compositing.shadowDensity);
        copyMatrix(dest.projector,card.projected?card.projector.transform.inverse():Matrix());
        dest.size[0]=asFloat(card.width);dest.size[1]=asFloat(card.height);dest.size[2]=float(std::clamp(card.opacity,0.,1.));dest.size[3]=card.projected?1.f:0.f;
        const auto& projector=card.projector;
        dest.projection[0]=asFloat(projector.orthographic?projector.orthoWidth*.5:projector.aperture/(2*projector.focal));
        dest.projection[1]=asFloat(dest.projection[0]/projector.filmAspect(card.projectionAspect));dest.projection[2]=projector.orthographic?1.f:0.f;
        dest.clip[0]=asFloat(projector.nearClip);dest.clip[1]=asFloat(projector.farClip);dest.textureUv[0]=dest.textureUv[1]=1;
        dest.clip[2]=asFloat(projector.offsetX);dest.clip[3]=asFloat(projector.offsetY*projector.filmAspect(card.projectionAspect));
        dest.geometry[1]=upload(card.texture);
        for(int j=0;j<3;++j){dest.maps[j]=upload(card.maps[j].texture);dest.channels[j]=card.maps[j].channel|(card.maps[j].useMeshUv?4u:0u);}
        dest.geometry[2]=card.overrideMaterial?1u:0u;packMaterial(dest.surface,card.material);
        if(card.texture && card.texture->width>0 && card.texture->height>0) {
            if(card.texture->materialSampling) {
                dest.geometry[3]=1u;
            }
            for(int j=0;j<2;++j){dest.textureUv[j]=asFloat(card.texture->imageUvScale[j]);dest.textureUv[j+2]=asFloat(card.texture->imageUvOffset[j]);}

        }
        if(!card.mesh)continue;
        if(card.mesh->triangles.empty()){dest.size[0]=dest.size[1]=dest.size[2]=0;continue;}
        const auto found=roots.find(card.mesh.get());
        if(found!=roots.end()){dest.geometry[0]=found->second+1;continue;}
        const auto& mesh=*card.mesh;
        if(mesh.bvh.empty() || mesh.triangleOrder.size()!=mesh.triangles.size())throw std::invalid_argument("Mesh acceleration structure was not built.");
        const size_t triangleCount=triangles.size()+mesh.triangles.size(),nodeCount=nodes.size()+mesh.bvh.size();
        const auto limit=_device->properties().limits.maxStorageBufferRange;
        if(triangleCount>limit/sizeof(Triangle) || nodeCount>limit/sizeof(BvhNode))
            throw std::invalid_argument("Mesh buffers exceed the Vulkan device limit; select CPU.");
        const uint32_t root=uint32_t(nodes.size()),begin=uint32_t(triangles.size());roots[card.mesh.get()]=root;dest.geometry[0]=root+1;
        for(unsigned index:mesh.triangleOrder) {
            const auto& source=mesh.triangles[index];Triangle target{};target.info[0]=index;
            for(int v=0;v<3;++v) {
                const auto& vertex=source.vertices[v];target.positions[v][0]=asFloat(vertex.position.x);target.positions[v][1]=asFloat(vertex.position.y);target.positions[v][2]=asFloat(vertex.position.z);
                target.normals[v][0]=asFloat(vertex.normal.x);target.normals[v][1]=asFloat(vertex.normal.y);target.normals[v][2]=asFloat(vertex.normal.z);
                for(int c=0;c<4;++c)target.colors[v][c]=vertex.color[c];
                for(int j=0;j<3;++j)for(int c=0;c<2;++c)target.mapUvs[j*3+v][c]=asFloat(vertex.mapUvs[j][c]);
                for(int c=0;c<2;++c)(v<2?target.uv01[v*2+c]:target.uv2[c])=asFloat(vertex.uv[c]);
            }
            std::copy(source.color.begin(),source.color.end(),target.material);packMaterial(target.surface,source.material);triangles.push_back(target);
        }
        for(const auto& source:mesh.bvh) {
            BvhNode target{};const auto& lo=source.bounds.minimum;const auto& hi=source.bounds.maximum;
            const double minima[]{lo.x,lo.y,lo.z},maxima[]{hi.x,hi.y,hi.z};
            for(int a=0;a<3;++a){target.minimum[a]=std::nextafter(asFloat(minima[a]),-std::numeric_limits<float>::infinity());target.maximum[a]=std::nextafter(asFloat(maxima[a]),std::numeric_limits<float>::infinity());}
            target.info[0]=begin+source.begin;target.info[1]=source.count;target.info[2]=root+source.left;target.info[3]=root+source.right;nodes.push_back(target);
        }
    }
    // Storage descriptors must have a nonzero range even for an all-hidden pose.
    if(triangles.empty())triangles.resize(1);
    if(nodes.empty())nodes.resize(1);
    if(!_meshProgram) {
        FragmentLayout layout;layout.uniformBinding=16;layout.inputBindings.clear();layout.storageBindings={17,18};
        for(size_t i=0;i<maxObjects;++i)layout.inputBindings.push_back(uint32_t(i));
        _meshProgram=std::make_unique<FragmentProgram>(_device,meshShader(),uint32_t(sizeof(Uniforms)),layout);
    }
    VulkanImage output(_device,s.width,s.height);
    _meshProgram->renderMany(output,{0,0,uint32_t(s.width),uint32_t(s.height)},inputs,&u,sizeof(u),
        {{triangles.data(),triangles.size()*sizeof(Triangle)},{nodes.data(),nodes.size()*sizeof(BvhNode)}});
    const auto bytes=output.download();std::vector<float> result(bytes.size()/sizeof(float));std::memcpy(result.data(),bytes.data(),bytes.size());return result;
}
}}}
