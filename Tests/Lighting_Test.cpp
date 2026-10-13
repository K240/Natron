// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/CardScene.h"
#include "Gpu/Lighting.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace Natron::Gpu;
using namespace Natron::Gpu::Cards;
namespace {
void require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
bool close(double a,double b,double tolerance=1e-5){return std::abs(a-b)<=tolerance;}
std::shared_ptr<Texture> solid(float alpha=1) {
    auto t=std::make_shared<Texture>();t->width=t->height=2;t->pixels.resize(16);
    for(size_t i=0;i<t->pixels.size();++i)t->pixels[i]=alpha;return t;
}
std::shared_ptr<Mesh> quad() {
    auto m=std::make_shared<Mesh>();
    const Vec3 points[]{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    for(const auto indices:std::vector<std::array<int,3>>{{0,1,2},{0,2,3}}) {
        MeshTriangle triangle;triangle.color={1,1,1,1};triangle.material.lit=true;
        for(int i=0;i<3;++i)triangle.vertices[i].position=points[indices[i]];
        m->triangles.push_back(triangle);
    }
    m->buildAcceleration();return m;
}
struct Case {std::vector<Card> cards;Camera camera;Settings settings;};
}
int main(int argc,char** argv) {
    try {
        constexpr double pi=3.14159265358979323846;
        Settings s;s.width=s.height=1;s.samples=1;s.lights.emplace_back();
        Camera camera;camera.orthographic=true;Card plane;plane.material.lit=true;plane.material.ior=1;
        plane.material.baseColor={.8f,.4f,.2f,1};plane.overrideMaterial=true;
        auto pixels=renderCpu({plane},camera,s);
        require(close(pixels[0],.8/pi) && close(pixels[1],.4/pi) && close(pixels[2],.2/pi) && pixels[3]==1,"Normal-incidence Lambert reference failed.");
        auto dielectric=plane;dielectric.material.ior=1.5;dielectric.material.roughness=1;
        const auto plastic=renderCpu({dielectric},camera,s);
        require(close(plastic[0],(.96*.8+.01)/pi),"GGX dielectric Fresnel/visibility reference failed.");
        dielectric.material.metallic=1;const auto metal=renderCpu({dielectric},camera,s);
        require(close(metal[0],.8/(4*pi)),"Metal retained diffuse light or lost base-color reflectance.");
        auto exposed=s;exposed.lights[0].exposure=1;auto doubled=renderCpu({plane},camera,exposed);
        require(close(doubled[0],2*pixels[0]),"Light exposure is not measured in stops.");
        auto dark=s;dark.lights.clear();plane.material.emission={2,.5,.25};auto emission=renderCpu({plane},camera,dark);
        require(close(emission[0],2) && close(emission[1],.5) && emission[3]==1,"Emission without lights failed.");
        plane.material.emission={};plane.material.baseColor[3]=.25f;plane.texture=solid(.5f);
        auto transparent=renderCpu({plane},camera,s);
        require(close(transparent[3],.125) && close(transparent[0],.1/pi),"Lighting lost straight material color or premultiplication.");
        plane.material.baseColor[3]=1;plane.texture.reset();
        auto point=s;point.lights[0].type=LightType::Point;point.lights[0].transform=Matrix::transform({0,0,2},{});
        const auto pointNear=renderCpu({plane},camera,point);point.lights[0].transform=Matrix::transform({0,0,4},{});
        const auto pointFar=renderCpu({plane},camera,point);require(close(pointNear[0],pointFar[0]*4),"Point light inverse-square attenuation failed.");
        auto spot=point;spot.lights[0].type=LightType::Spot;
        require(renderCpu({plane},camera,spot)[0]>0,"Spot cone failed to light its center.");
        spot.lights[0].transform=Matrix::transform({0,0,4},{0,60,0});require(renderCpu({plane},camera,spot)[0]==0,"Outside spot cone remained lit.");
        Card blocker;blocker.width=blocker.height=.4;blocker.texture=solid();blocker.transform=Matrix::transform({-1/std::sqrt(3.),0,1},{});
        auto shadow=s;shadow.lights[0].transform=Matrix::transform({0,0,5},{0,-30,0});
        const auto blocked=renderCpu({plane,blocker},camera,shadow);require(blocked[0]==0 && blocked[3]==1,"Offscreen geometry failed to cast a hard shadow.");
        shadow.lights[0].shadows=false;const auto clear=renderCpu({plane,blocker},camera,shadow);require(clear[0]>.1,"Disabling shadows failed.");
        shadow.lights[0].shadows=true;blocker.texture=solid(.5f);
        const auto partial=renderCpu({plane,blocker},camera,shadow);require(close(partial[0],clear[0]*.5),"Shadow ignored texture alpha.");
        blocker.texture=solid(0);require(close(renderCpu({plane,blocker},camera,shadow)[0],clear[0]),"Transparent geometry still cast a shadow.");
        Card unlit=plane;unlit.material=Material();unlit.texture=solid(.5f);
        require(renderCpu({unlit},camera,s)==renderCpu({unlit},camera,dark),"Lights changed an existing unlit image.");
        auto depth=s;depth.pass=RenderPass::Depth;require(renderCpu({plane},camera,depth)[0]==5,"Lighting changed Depth.Z.");
        bool invalid=false;auto bad=s;bad.lights[0].intensity=-1;try{renderCpu({plane},camera,bad);}catch(const std::invalid_argument&){invalid=true;}
        require(invalid,"Invalid light intensity was accepted.");invalid=false;auto badMaterial=plane;badMaterial.material.roughness=std::numeric_limits<double>::quiet_NaN();
        try{renderCpu({badMaterial},camera,s);}catch(const std::invalid_argument&){invalid=true;}require(invalid,"Nonfinite material was accepted.");
        auto environment=s;environment.lights[0].type=LightType::Environment;environment.lights[0].shadows=false;
        Card envPlane=plane;envPlane.material.ior=1.5;envPlane.material.metallic=1;envPlane.material.roughness=0;
        const auto uniform=renderCpu({envPlane},camera,environment);
        require(close(uniform[0],.8,1e-4) && close(uniform[1],.4,1e-4) && uniform[3]==1,"Uniform environment mirror reference failed.");
        environment.lights[0].exposure=1;const auto envExposed=renderCpu({envPlane},camera,environment);
        require(close(envExposed[0],2*uniform[0]),"Environment exposure did not scale radiance.");environment.lights[0].exposure=0;
        envPlane.material.metallic=0;envPlane.material.ior=1;
        const auto diffuseEnvironment=renderCpu({envPlane},camera,environment);
        require(diffuseEnvironment[0]>.799 && diffuseEnvironment[0]<.801,"Uniform environment diffuse integral failed.");
        auto sky=std::make_shared<Texture>();sky->width=16;sky->height=8;sky->materialSampling=sky->dataSampling=true;
        sky->wrapS=TextureAddress::Repeat;sky->wrapT=TextureAddress::Clamp;sky->pixels.resize(16*8*4);
        for(int y=0;y<8;++y)for(int x=0;x<16;++x) {
            const size_t at=(size_t(y)*16+x)*4;sky->pixels[at]=float((x+.5)*4/16);sky->pixels[at+1]=float(1-(x+.5)/16);sky->pixels[at+2]=float((y+.5)/8);sky->pixels[at+3]=0;
        }
        auto& dome=environment.lights[0];dome.environment=sky;
        require(close(environmentRadiance(dome,Matrix(),{0,0,1}).x,2) && close(environmentRadiance(dome,Matrix(),{1,0,0}).x,1) && close(environmentRadiance(dome,Matrix(),{-1,0,0}).x,3),"Environment latlong orientation or alpha handling failed.");
        require(close(environmentRadiance(dome,Matrix(),{0,1,0}).z,.9375) && close(environmentRadiance(dome,Matrix(),{0,-1,0}).z,.0625),"Environment poles failed.");
        envPlane.material.metallic=1;envPlane.material.ior=1.5;
        const auto mirror=renderCpu({envPlane},camera,environment);require(mirror[0]>1.5,"HDR environment radiance was clamped.");
        envPlane.material.roughness=1;const auto blurred=renderCpu({envPlane},camera,environment);require(blurred!=mirror,"Environment did not respond to roughness.");
        dome.transform=Matrix::transform({}, {0,90,0});const auto rotated=renderCpu({envPlane},camera,environment);
        require(rotated!=blurred,"Environment orientation was ignored.");
        dome.transform=Matrix::transform({100,-40,90},{0,90,0},{.01,8,2});
        require(renderCpu({envPlane},camera,environment)==rotated,"Environment translation/scale affected illumination.");
        dome.transform=Matrix();dome.shadows=true;Card envBlocker;envBlocker.width=2;envBlocker.height=4;envBlocker.texture=solid();envBlocker.transform=Matrix::transform({-1.5,0,1},{});
        const auto occluded=renderCpu({envPlane,envBlocker},camera,environment);dome.shadows=false;
        const auto unobstructed=renderCpu({envPlane,envBlocker},camera,environment);
        require(occluded[0]<unobstructed[0] && occluded[3]==unobstructed[3],"Environment occlusion or alpha failed.");
        dome.shadows=true;envBlocker.texture=solid(.5f);const auto translucent=renderCpu({envPlane,envBlocker},camera,environment);
        require(close(translucent[0],(occluded[0]+unobstructed[0])*.5),"Environment shadows ignored translucent blockers.");
        auto environmentDepth=environment;environmentDepth.pass=RenderPass::Depth;
        require(renderCpu({envPlane},camera,environmentDepth)==renderCpu({envPlane},camera,depth),"Environment changed depth.");
        require(renderCpu({unlit},camera,environment)==renderCpu({unlit},camera,dark),"Environment affected an unlit image.");
        auto mesh=quad();Card surface=plane;surface.mesh=mesh;surface.overrideMaterial=false;surface.material=Material();
        require(renderCpu({surface},camera,s)[0]>0,"Per-face material or geometric normal fallback failed.");
        // Each scene exercises the production shader, including inherited
        // nonuniform/mirrored transforms, perspective, projection and alpha.
        s.width=48;s.height=32;s.samples=2;camera.orthographic=false;
        std::vector<Case> cases;
        for(double metallic:{0.,.4,1.})for(double roughness:{0.,.2,.8,1.}) {
            Card material=plane;material.material.ior=1.5;material.material.metallic=metallic;material.material.roughness=roughness;
            material.transform=Matrix::transform({}, {17,-21,8},{2,.7,1.4});auto lights=s;lights.lights[0].transform=Matrix::transform({0,0,5},{-20,-30,0});
            cases.push_back({{material},camera,lights});
        }
        blocker.texture=solid(.5f);cases.push_back({{plane,blocker},camera,shadow});
        cases.back().settings.width=48;cases.back().settings.height=32;
        cases.push_back({{plane},camera,point});cases.push_back({{plane},camera,spot});cases.push_back({{surface},camera,s});
        surface.overrideMaterial=true;surface.material=plane.material;surface.transform=Matrix::transform({}, {0,35,0},{-1.5,.4,2});
        cases.push_back({{surface},camera,s});plane.texture=solid(.5f);plane.projected=true;plane.projector=camera;plane.projectionAspect=1.5;
        cases.push_back({{plane,blocker},camera,s});cases.push_back({{plane},camera,depth});
        auto many=s;many.lights.resize(16);cases.push_back({{surface},camera,many});
        auto smooth=quad();smooth->triangles[0].material.emission={.2,.1,.05};
        for(auto& triangle:smooth->triangles)for(auto& vertex:triangle.vertices)vertex.normal={vertex.position.x*.25,vertex.position.y*.25,1};
        smooth->buildAcceleration();surface.mesh=smooth;surface.overrideMaterial=false;cases.push_back({{surface},camera,s});
        auto hdr=plane;hdr.material.baseColor={3,.5f,2,1};hdr.material.metallic=.4;hdr.material.emission={5,2,1};cases.push_back({{hdr},camera,s});
        // Native image maps use ordinary mesh UVs; imported USD maps may use
        // independent UV sets on the same immutable mesh and BVH.
        auto mappedMesh=quad();
        for(auto& tri:mappedMesh->triangles)for(auto& v:tri.vertices)v.uv={v.position.x*.5+.5,v.position.y*.5+.5};
        auto normalMap=std::make_shared<Texture>();normalMap->width=normalMap->height=1;
        normalMap->pixels={-.4f,.2f,.8f,0};normalMap->materialSampling=normalMap->dataSampling=true;
        normalMap->wrapS=normalMap->wrapT=TextureAddress::Repeat;
        auto orm=std::make_shared<Texture>();orm->width=orm->height=2;
        orm->pixels={1,.4f,0,0, 1,.8f,1,.2f, 1,.6f,.3f,.4f, 1,.9f,.8f,.6f};
        orm->materialSampling=orm->dataSampling=true;orm->wrapS=orm->wrapT=TextureAddress::Repeat;
        Card native;native.mesh=mappedMesh;native.overrideMaterial=true;native.material.lit=true;
        native.maps={MaterialMap{normalMap,0,true},MaterialMap{orm,1,true},MaterialMap{orm,2,true}};
        auto mappedUvs=std::make_shared<Mesh>(*mappedMesh);
        for(auto& tri:mappedUvs->triangles)for(auto& v:tri.vertices)for(auto& uv:v.mapUvs)uv=v.uv;
        Card equivalent=native;equivalent.mesh=mappedUvs;for(auto& map:equivalent.maps)map.useMeshUv=false;
        require(renderCpu({native},camera,s)==renderCpu({equivalent},camera,s),"Native maps did not use the ordinary mesh UVs.");
        auto noMaps=native;noMaps.maps={};
        require(renderCpu({native},camera,s)!=renderCpu({noMaps},camera,s),"Native maps did not affect shading.");
        auto mapDepth=s;mapDepth.pass=RenderPass::Depth;
        require(renderCpu({native},camera,mapDepth)==renderCpu({noMaps},camera,mapDepth),"Native maps affected depth/alpha.");
        cases.push_back({{native},camera,s});cases.push_back({{native},camera,mapDepth});
        equivalent.mesh=mappedMesh;native.transform=Matrix::transform({-.8,0,0},{15,-20,10},{-.6,.8,1.5});
        equivalent.transform=Matrix::transform({.8,0,0},{-10,25,0},{.6,.8,1.5});
        cases.push_back({{native,equivalent},camera,s});
        environment.width=48;environment.height=32;environment.samples=1;
        for(unsigned count:{16u,32u,64u})for(double roughness:{0.,.25,.8,1.}) {
            auto lighting=environment;lighting.lights[0].environmentSamples=count;lighting.lights[0].shadows=false;
            envPlane.material.roughness=roughness;envPlane.transform=Matrix::transform({}, {17,-21,8},{2,.7,1.4});
            lighting.lights[0].transform=Matrix::transform({10,-4,9},{23,55,18},{-.01,8,2});
            cases.push_back({{envPlane},camera,lighting});
        }
        cases.push_back({{envPlane,envBlocker},camera,environment});cases.push_back({{native},camera,environment});
        auto clearAlpha=std::make_shared<Texture>(*sky);for(size_t i=3;i<clearAlpha->pixels.size();i+=4)clearAlpha->pixels[i]=1;
        auto sameRadiance=environment;sameRadiance.lights[0].environment=clearAlpha;
        require(renderCpu({envPlane},camera,environment)==renderCpu({envPlane},camera,sameRadiance),"Environment image alpha affected radiance.");
        size_t comparisons=0;float maxError=0;
        if(argc>1 && std::string(argv[1])=="--gpu") {
            InstanceOptions options;options.validation=true;auto instance=VulkanInstance::create(options);
            auto device=VulkanDevice::create(instance,argc>2?std::stoi(argv[2]):-1);Renderer renderer(device);
            for(const auto& test:cases) {
                const auto cpu=renderCpu(test.cards,test.camera,test.settings),gpu=renderer.render(test.cards,test.camera,test.settings);
                require(cpu.size()==gpu.size(),"Lighting GPU image dimensions differ.");
                for(size_t i=0;i<cpu.size();++i) {
                    require(std::isfinite(cpu[i]) && std::isfinite(gpu[i]),"Lighting produced nonfinite pixels.");
                    const float error=std::abs(cpu[i]-gpu[i]);maxError=std::max(maxError,error);
                    require(error<.001f+.002f*std::abs(cpu[i]),"CPU/Vulkan lighting differs.");
                }
                comparisons+=cpu.size()/4;
            }
            many.lights.emplace_back();bool limit=false;
            try{renderer.render({surface},camera,many);}catch(const std::invalid_argument&){limit=true;}require(limit,"GPU light limit was not enforced.");
            auto samplerBudget=environment;samplerBudget.width=samplerBudget.height=1;samplerBudget.lights.resize(16,dome);
            for(auto& l:samplerBudget.lights){l.environment=std::make_shared<Texture>(*sky);l.intensity=0;}
            Card textured=envPlane;textured.texture=samplerBudget.lights[0].environment;
            renderer.render({textured},camera,samplerBudget); // sixteen distinct samplers, one shared by geometry
            textured.texture=solid();limit=false;
            try{renderer.render({textured},camera,samplerBudget);}catch(const std::invalid_argument& e){limit=std::string(e.what()).find("16 distinct texture samplers")!=std::string::npos;}
            require(limit,"Environment images were not included in the GPU sampler budget.");
            require(instance->validationErrorCount()==0,"Vulkan lighting validation failed.");std::cout<<"Device: "<<device->properties().deviceName<<"\n";
        }
        std::cout<<"Lighting: PASS; GPU pixels compared="<<comparisons<<"; maximum error="<<maxError<<"\n";return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<"\n";return 1;}
}
