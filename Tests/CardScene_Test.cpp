// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/CardScene.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
using namespace Natron::Gpu;
using namespace Natron::Gpu::Cards;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool close(double a,double b,double tolerance=1e-5){return std::abs(a-b)<=tolerance;}
std::shared_ptr<Texture> solid(std::array<float,4> color) {
    auto t=std::make_shared<Texture>();t->width=t->height=4;
    for(int i=0;i<16;++i)t->pixels.insert(t->pixels.end(),color.begin(),color.end());return t;
}
int main(int argc,char** argv) {
    try {
        Settings s;s.width=64;s.height=48;s.samples=1;Camera camera;
        Card red;red.width=red.height=10;red.texture=solid({.5f,0,0,.5f});
        Card blue=red;blue.texture=solid({0,0,1,1});blue.transform=Matrix::transform({0,0,-1},{});
        auto pixels=renderCpu({blue,red},camera,s);const size_t middle=(24*64+32)*4;
        require(close(pixels[middle],.5)&&close(pixels[middle+2],.5)&&close(pixels[middle+3],1),"Transparent front card composition or scene order is wrong.");
        Card a=red,b=red;a.transform=Matrix::transform({}, {0,40,0});b.transform=Matrix::transform({}, {0,-40,0});b.texture=solid({0,.5f,0,.5f});
        pixels=renderCpu({a,b},camera,s);
        const size_t left=(24*64+12)*4,right=(24*64+52)*4;
        require(pixels[left]!=pixels[right] && close(pixels[left+3],.75)&&close(pixels[right+3],.75),"Intersecting transparent cards did not change depth order per pixel.");
        auto gradient=std::make_shared<Texture>();gradient->width=8;gradient->height=4;
        for(int y=0;y<4;++y)for(int x=0;x<8;++x)gradient->pixels.insert(gradient->pixels.end(),{float((x+.5)/8),float((y+.5)/4),2.f,1.f});
        Card projected=red;projected.texture=gradient;projected.projected=true;projected.projector=camera;projected.projectionAspect=64./48;
        pixels=renderCpu({projected},camera,s);
        require(close(pixels[middle],32.5/64,1e-5)&&close(pixels[middle+1],24.5/48,1e-5)&&close(pixels[middle+2],2),"Fixed projection UV, row order or HDR was lost.");
        Camera moved=camera;moved.transform=Matrix::transform({.5,0,5},{});
        const auto movedPixels=renderCpu({projected},moved,s);
        require(movedPixels[middle]>pixels[middle]+.1f,"Render camera movement did not reveal a different part of the fixed projection.");
        Camera offset=camera;offset.offsetX=.08;offset.offsetY=-.06;
        const auto offsetPixels=renderCpu({projected},offset,s);
        require(close(offsetPixels[middle],32.5/64+.08,1e-5) && close(offsetPixels[middle+1],24.5/48-.06*64/48,1e-5),"Film offsets changed with the conformed image aspect.");
        Card offsetProjection=projected;offsetProjection.projector=offset;
        Card cropped=projected;auto croppedTexture=std::make_shared<Texture>();croppedTexture->width=croppedTexture->height=4;
        for(int y=0;y<4;++y)for(int x=2;x<6;++x) {
            const size_t at=(y*8+x)*4;
            croppedTexture->pixels.insert(croppedTexture->pixels.end(),gradient->pixels.begin()+at,gradient->pixels.begin()+at+4);
        }
        croppedTexture->imageUvScale={2,1};croppedTexture->imageUvOffset={-.5,0};cropped.texture=croppedTexture;
        const auto croppedPixels=renderCpu({cropped},camera,s);
        require(close(croppedPixels[middle],pixels[middle]) && croppedPixels[(24*64+12)*4+3]==0,
                "A cropped projection was stretched instead of preserving source-format UVs.");
        Card near=red,far=red;near.width=far.width=1;near.height=far.height=1;far.transform=Matrix::transform({0,0,-5},{});
        const Hit nearHit=intersect(near,moved,s,32,24),farHit=intersect(far,moved,s,32,24);
        require(nearHit.card>=0 && farHit.card>=0 && close(nearHit.depth,5)&&close(farHit.depth,10),"Camera depth is incorrect.");
        Matrix transform=Matrix::transform({2,-3,4},{20,-30,80},{2,.5,3});Vec3 point{.2,-.7,1.3};
        const auto restored=transform.inverse().point(transform.point(point));require(close(restored.x,point.x)&&close(restored.y,point.y)&&close(restored.z,point.z),"Parent transform inverse is incorrect.");
        Camera ortho=camera;ortho.orthographic=true;ortho.orthoWidth=3;
        const auto empty=renderCpu({},camera,s);require(std::all_of(empty.begin(),empty.end(),[](float v){return v==0;}),"Empty scene must be transparent.");
        bool cancelled=false;try{renderCpu({red},camera,s,[]{return true;});}catch(const std::runtime_error&){cancelled=true;}require(cancelled,"CPU cancellation failed.");
        bool singular=false;Card bad=red;bad.transform=Matrix::transform({}, {},{0,1,1});try{renderCpu({bad},camera,s);}catch(const std::invalid_argument&){singular=true;}require(singular,"Zero scale must fail clearly.");
        bool invalid=false;Camera badCamera=camera;badCamera.focal=std::numeric_limits<double>::infinity();
        try{renderCpu({red},badCamera,s);}catch(const std::invalid_argument&){invalid=true;}require(invalid,"Non-finite camera values must fail clearly.");
        Settings depthSettings=s;depthSettings.pass=RenderPass::Depth;
        const auto depth=renderCpu({blue,red},camera,depthSettings);
        require(close(depth[middle],5) && close(depth[left],5) && close(depth[middle+3],1),"Depth must be camera-space Z of the nearest visible translucent surface.");
        Card transparent=red;transparent.texture=solid({0,0,0,0});
        auto rearDepth=renderCpu({blue,transparent},camera,depthSettings);
        require(close(rearDepth[middle],6),"Transparent texels occluded the rear depth.");
        transparent=red;transparent.opacity=0;
        rearDepth=renderCpu({blue,transparent},camera,depthSettings);
        require(close(rearDepth[middle],6),"Zero-opacity cards occluded the rear depth.");
        const auto emptyDepth=renderCpu({},camera,depthSettings);
        require(std::all_of(emptyDepth.begin(),emptyDepth.end(),[](float v){return v==0;}),"Empty depth must be zero.");
        const auto croppedDepth=renderCpu({cropped},camera,depthSettings);
        require(close(croppedDepth[middle],5) && croppedDepth[left]==0,"Depth did not respect the projector's cropped data window.");
        Camera clipped=camera;clipped.nearClip=5.5;clipped.farClip=6.5;
        require(close(renderCpu({blue,red},clipped,depthSettings)[middle],6),"Depth ignored camera clipping.");
        depthSettings.width=depthSettings.height=1;depthSettings.samples=1;
        Camera transformedCamera=camera;transformedCamera.transform=Matrix::transform({2,-1,7},{15,35,-10},{1.5,.75,2});
        Card relative=red;relative.transform=transformedCamera.transform*Matrix::transform({0,0,-4},{10,20,0});
        require(close(renderCpu({relative},transformedCamera,depthSettings)[0],4),"Depth used world/ray distance instead of camera-local Z.");
        depthSettings.samples=2;Card edge=red;edge.width=1;edge.transform=Matrix::transform({.9,0,0},{});
        auto edgeDepth=renderCpu({edge},camera,depthSettings);
        require(close(edgeDepth[0],5) && close(edgeDepth[3],.5),"Antialiasing diluted raw depth at a silhouette.");
        makeDepthPreview(edgeDepth,0,10);
        require(close(edgeDepth[0],.25) && close(edgeDepth[3],.5),"Depth preview is not premultiplied by sample coverage.");
        bool invalidRange=false;try{makeDepthPreview(edgeDepth,10,10);}catch(const std::invalid_argument&){invalidRange=true;}
        require(invalidRange,"Invalid depth preview range was accepted.");
        size_t comparisons=0;float maxError=0;
        if(argc>1 && std::string(argv[1])=="--benchmark") {
            s.width=640;s.height=360;s.samples=2;
            std::vector<Card> workload(16,projected);
            for(size_t i=0;i<workload.size();++i) {
                workload[i].opacity=.12;
                workload[i].transform=Matrix::transform({0,0,-double(i)*.04},{0,double(i)-8,0});
            }
            renderCpu(workload,camera,s);
            std::vector<double> timings;
            for(int frame=0;frame<5;++frame) {
                moved.transform=Matrix::transform({frame*.05,0,5},{});
                const auto start=std::chrono::steady_clock::now();
                const auto framePixels=renderCpu(workload,moved,s);
                require(std::all_of(framePixels.begin(),framePixels.end(),[](float v){return std::isfinite(v);}),"Benchmark produced invalid pixels.");
                timings.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
            }
            std::sort(timings.begin(),timings.end());
            std::cout<<"CPU benchmark: 640x360, 16 projected cards, 2x2 samples; median_ms="<<timings[timings.size()/2]<<"\n";
        }
        if(argc>1 && std::string(argv[1])=="--gpu") {
            InstanceOptions options;options.validation=true;auto instance=VulkanInstance::create(options);
            auto device=VulkanDevice::create(instance,argc>2?std::stoi(argv[2]):-1);Renderer renderer(device);
            Card noTexture=red;noTexture.texture.reset();Card zeroWidth=red;zeroWidth.width=0;
            Camera calibrated=offset;calibrated.apertureAspect=1.71;
            Card calibratedProjection=projected;calibratedProjection.projector=calibrated;
            std::vector<std::vector<Card>> cases{{},{red},{blue,red},{a,b},{projected},{offsetProjection},{cropped},{calibratedProjection},std::vector<Card>(16,projected),{blue,transparent},{noTexture},{zeroWidth}};
            for(const auto pass:{RenderPass::Color,RenderPass::Depth})for(int samples:{1,2,4})for(const auto& cards:cases)for(const auto& view:{camera,moved,ortho,offset,calibrated}) {
                s.samples=samples;s.pass=pass;const auto cpu=renderCpu(cards,view,s),gpu=renderer.render(cards,view,s);
                require(cpu.size()==gpu.size(),"GPU output dimensions differ.");
                for(size_t i=0;i<cpu.size();++i)maxError=std::max(maxError,std::abs(cpu[i]-gpu[i]));
                require(maxError<.0005f,"GPU pixels disagree with CPU card renderer.");comparisons+=cpu.size()/4;
            }
            for(const auto& test:std::vector<std::pair<Card,Camera>>{{edge,camera},{relative,transformedCamera}}) {
                const auto cpu=renderCpu({test.first},test.second,depthSettings),gpu=renderer.render({test.first},test.second,depthSettings);
                for(size_t i=0;i<cpu.size();++i)require(close(cpu[i],gpu[i],.0005),"GPU silhouette coverage or transformed-camera depth is incorrect.");
            }
            require(instance->validationErrorCount()==0,"Vulkan validation reported errors.");
            std::cout<<"Device: "<<device->properties().deviceName<<"\n";
        }
        std::cout<<"CardScene: PASS; GPU pixels compared="<<comparisons<<"; maximum error="<<maxError<<"\n";return 0;
    }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}
}
