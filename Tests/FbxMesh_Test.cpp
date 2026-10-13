// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/FbxMesh.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
using namespace Natron::Gpu::Cards;
using namespace Natron::Gpu;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool close(double a,double b,double e=1e-5){return std::abs(a-b)<e;}
std::shared_ptr<Texture> solid(std::array<float,4> c){auto t=std::make_shared<Texture>();t->width=t->height=1;t->pixels.assign(c.begin(),c.end());return t;}
std::shared_ptr<Mesh> quad(double z,std::array<float,4> color) {
    auto mesh=std::make_shared<Mesh>();
    const std::array<Vec3,4> p{{{-1,-1,z},{1,-1,z},{1,1,z},{-1,1,z}}};
    const std::array<std::array<double,2>,4> uv{{{0,0},{1,0},{1,1},{0,1}}};
    for(const auto& indices:std::array<std::array<int,3>,2>{{{0,1,2},{0,2,3}}}) {
        MeshTriangle t;t.color=color;for(int i=0;i<3;++i){t.vertices[i].position=p[indices[i]];t.vertices[i].uv=uv[indices[i]];}mesh->triangles.push_back(t);
    }
    mesh->buildAcceleration();return mesh;
}
void compareSkinReference(const Mesh& mesh,const std::string& filename) {
    std::ifstream file(filename);require(bool(file),"Skinning reference OBJ missing.");
    std::vector<Vec3> positions;std::string line;
    while(std::getline(file,line))if(line.rfind("v ",0)==0) {
        std::istringstream row(line.substr(2));Vec3 p;row>>p.x>>p.y>>p.z;positions.push_back(p*.01);
    }
    require(!positions.empty(),"Skinning reference OBJ has no vertices.");
    for(const auto& triangle:mesh.triangles)for(const auto& vertex:triangle.vertices) {
        const auto match=std::find_if(positions.begin(),positions.end(),[&](Vec3 p) {
            const auto delta=p-vertex.position;return std::abs(delta.x)<1e-6 && std::abs(delta.y)<1e-6 && std::abs(delta.z)<1e-6;
        });
        if(match==positions.end()) {
            auto nearest=std::min_element(positions.begin(),positions.end(),[&](Vec3 a,Vec3 b){
                const auto da=a-vertex.position,db=b-vertex.position;return da.x*da.x+da.y*da.y+da.z*da.z<db.x*db.x+db.y*db.y+db.z*db.z;
            });
            std::ostringstream message;message<<"Skinning reference mismatch: file="<<filename<<" actual="<<vertex.position.x<<","<<vertex.position.y<<","<<vertex.position.z<<" nearest="<<nearest->x<<","<<nearest->y<<","<<nearest->z;
            throw std::runtime_error(message.str());
        }
        const double normal=vertex.normal.x*vertex.normal.x+vertex.normal.y*vertex.normal.y+vertex.normal.z*vertex.normal.z;
        require(close(normal,1),"Deformed mesh normal was not normalized.");
    }
}
}
int main(int argc,char** argv) {
    try {
        const std::string fixtures=NATRON_FBX_FIXTURES;
        const auto cameras=FbxMeshAsset::load(fixtures+"/animated-camera.fbx",false);
        require(cameras->cameras().size()==3 && cameras->takes().size()==2 && close(cameras->frameRate(),24),"Camera-only FBX inventory failed.");
        const Camera cameraStart=cameras->camera("MainCamera",true,0,"CameraMove");
        const Camera cameraEnd=cameras->camera("Rig/MainCamera",true,1,"CameraMove");
        const Camera cameraMiddle=cameras->camera({},true,.5,"CameraMove");
        const auto expectedEnd=Matrix::transform({.6,0,6},{0,12,0});
        for(int i=0;i<16;++i)require(close(cameraEnd.transform.m[i],expectedEnd.m[i]),"FBX camera axis, units or rotation animation failed.");
        require(close(cameraStart.transform.m[14],6) && close(cameraMiddle.transform.m[12],.3),"FBX camera parent motion failed.");
        require(close(cameraStart.focal,35) && close(cameraMiddle.focal,52.5) && close(cameraEnd.focal,70) && close(cameraEnd.aperture,36),"FBX lens animation/gate failed.");
        require(close(cameraStart.nearClip,.1) && close(cameraStart.farClip,1000),"FBX camera clipping units failed.");
        const Camera cameraStatic=cameras->camera("MainCamera",false,1,"Missing");
        const Camera cameraHold=cameras->camera("MainCamera",true,1,"Hold");
        require(close(cameraStatic.focal,35) && close(cameraHold.transform.m[12],0),"FBX static pose or named take failed.");
        const Camera importedOrtho=cameras->camera("OrthoCamera");
        require(importedOrtho.orthographic && close(importedOrtho.orthoWidth,4) && close(importedOrtho.transform.m[12],2),"FBX orthographic width or units failed.");
        const Camera importedFov=cameras->camera("FovCamera");
        require(close(importedFov.aperture/(2*importedFov.focal),std::tan(3.14159265358979323846/6)),"FBX horizontal FOV mode failed.");
        bool missingCamera=false,missingCameraTake=false,noCamera=false;
        try{cameras->camera("Missing");}catch(const std::invalid_argument&){missingCamera=true;}
        try{cameras->camera({},true,0,"Missing");}catch(const std::invalid_argument&){missingCameraTake=true;}
        try{FbxMeshAsset::load(fixtures+"/colored-cube.fbx",false)->camera();}catch(const std::invalid_argument&){noCamera=true;}
        require(missingCamera && missingCameraTake && noCamera,"FBX camera selection errors were not reported.");
        auto cube=loadFbxMesh(fixtures+"/colored-cube.fbx");
        require(cube->triangles.size()==12 && cube->objectCount==1,"ASCII FBX triangulation/object count failed.");
        require(close(cube->bounds.minimum.x,-1) && close(cube->bounds.maximum.x,1) && close(cube->bounds.maximum.z,1),"FBX units/axes were not converted to Y-up meters.");
        auto binary=loadFbxMesh(fixtures+"/blender-cube-binary.fbx");
        const auto binaryCameras=FbxMeshAsset::load(fixtures+"/blender-cube-binary.fbx",false);
        require(!binaryCameras->cameras().empty(),"Binary FBX cameras were not imported.");
        Card cameraCube;cameraCube.mesh=binary;Settings cameraOutput;cameraOutput.width=96;cameraOutput.height=64;
        const auto binaryCameraPixels=renderCpu({cameraCube},binaryCameras->camera(),cameraOutput);
        require(binaryCameraPixels[(32*96+48)*4+3]>.99f,"Binary Blender camera did not point at its cube.");
        const auto meshAssetCamera=FbxMeshAsset::load(fixtures+"/blender-cube-binary.fbx")->camera();
        for(int i=0;i<16;++i)require(close(meshAssetCamera.transform.m[i],binaryCameras->camera().transform.m[i]),"Geometry-loading camera conversion differs from camera-only loading.");
        require(binary->triangles.size()==12 && binary->objectCount==1,"Binary FBX mesh was not imported.");
        require(close(binary->bounds.maximum.x,1) && close(binary->bounds.minimum.x,-1),"Binary FBX hierarchy/units were lost.");
        const auto animation=FbxMeshAsset::load(fixtures+"/animated-cube.fbx");
        require(animation->takes().size()==3 && animation->takes()[0].name=="Move" && animation->takes()[1].name=="Scale","FBX animation takes were lost.");
        require(close(animation->takes()[0].begin,1) && close(animation->takes()[0].end,2) && close(animation->frameRate(),24),"FBX take range/FPS was lost.");
        require(close(animation->staticMesh()->bounds.maximum.z,1),"Static pose changed when animation was loaded.");
        const auto halfway=animation->evaluate(.5),ending=animation->evaluate(1,"Move"),scaled=animation->evaluate(.5,"Scale");
        require(close(halfway->bounds.minimum.x,-.5) && close(halfway->bounds.maximum.z,1.5),"Animated parent/child transforms or relative take start failed.");
        require(close(ending->bounds.minimum.x,0) && close(ending->bounds.maximum.z,2),"FBX animation end pose failed.");
        require(close(scaled->bounds.maximum.y,1.5) && close(scaled->bounds.maximum.z,1),"FBX named take selection failed.");
        require(close(animation->evaluate(-1)->bounds.maximum.z,1) && close(animation->evaluate(2)->bounds.maximum.z,2),"FBX constant curve extrapolation failed.");
        bool badTake=false,badTime=false;
        try{animation->evaluate(0,"missing");}catch(const std::invalid_argument&){badTake=true;}
        try{animation->evaluate(std::numeric_limits<double>::quiet_NaN());}catch(const std::invalid_argument&){badTime=true;}
        require(badTake && badTime,"Invalid FBX take or time was not reported.");
        const auto hidden=animation->evaluate(1,"Hide");
        require(hidden->triangles.empty() && !animation->evaluate(0,"Hide")->triangles.empty(),"Animated parent visibility did not hide/restore mesh geometry.");
        const auto hiddenPath=std::filesystem::temp_directory_path()/"Natron-FBX-hidden-pose.fbx";
        {
            std::ifstream in(fixtures+"/animated-cube.fbx");std::stringstream text;text<<in.rdbuf();std::string data=text.str();
            const std::string property="P: \"Visibility\", \"Visibility\", \"\", \"A\",1";
            const auto at=data.find(property);require(at!=std::string::npos,"Visibility fixture property missing.");
            data.replace(at,property.size(),"P: \"Visibility\", \"Visibility\", \"\", \"A\",0");std::ofstream out(hiddenPath);out<<data;
        }
        const auto initiallyHidden=FbxMeshAsset::load(hiddenPath.u8string());std::filesystem::remove(hiddenPath);
        require(initiallyHidden->staticMesh()->triangles.empty() && !initiallyHidden->evaluate(0,"Hide")->triangles.empty(),"Hidden file pose prevented loading a visible animated pose.");
        const auto skin=FbxMeshAsset::load(fixtures+"/animated-skin.fbx");
        const auto skinStart=skin->evaluate(0,"",true),skinEnd=skin->evaluate(8./24,"",true);
        require(close(skinStart->triangles.front().color[3],1),"Maya black TransparentColor with factor=1 became invisible.");
        compareSkinReference(*skinStart,fixtures+"/animated-skin-frame0.obj");
        compareSkinReference(*skinEnd,fixtures+"/animated-skin-frame8.obj");
        require(!close(skinStart->bounds.maximum.x,skinEnd->bounds.maximum.x),"Animated skin pose did not change.");
        require(close(skin->evaluate(0)->bounds.maximum.z,.005),"Bone-space skinning applied mesh transforms twice.");
        // Move the animation track to the root joint to check skeletal motion
        // independently of the optional Maya mesh-transform convention.
        const auto bonePath=std::filesystem::temp_directory_path()/"Natron-FBX-bone-animation.fbx";
        {std::ifstream in(fixtures+"/animated-skin.fbx");std::stringstream text;text<<in.rdbuf();std::string data=text.str();
            const std::string connection="C: \"OP\",2415043572384,2414390291472, \"Lcl Translation\"";
            const auto at=data.find(connection);require(at!=std::string::npos,"Skin fixture translation connection missing.");
            data.replace(at,connection.size(),"C: \"OP\",2415043572384,2414390319312, \"Lcl Translation\"");std::ofstream out(bonePath);out<<data;
        }
        const auto bones=FbxMeshAsset::load(bonePath.u8string());std::filesystem::remove(bonePath);
        const auto bonesStart=bones->evaluate(0),bonesEnd=bones->evaluate(8./24);
        compareSkinReference(*bonesStart,fixtures+"/animated-skin-frame0.obj");
        for(size_t t=0;t<bonesStart->triangles.size();++t)for(int v=0;v<3;++v) {
            const auto a=bonesStart->triangles[t].vertices[v].position,b=bonesEnd->triangles[t].vertices[v].position;
            require(close(a.x,b.x) && close(a.y,b.y) && close(a.z-b.z,.02),"Animated bones were not applied to weighted vertices.");
        }
        const auto blend=FbxMeshAsset::load(fixtures+"/animated-blend-shape.fbx");
        const auto blendStart=blend->evaluate(0),blendLater=blend->evaluate(1-1./24);
        require(close(blendStart->triangles.front().color[3],1),"Blend-shape FBX material opacity was lost.");
        // TopH offsets are +/- .3169564 cm; first key is at frame 1 (24 FPS).
        size_t topVertices=0;
        for(const auto& triangle:blendStart->triangles)for(const auto& v:triangle.vertices)if(v.position.y>0) {
            require(close(std::abs(v.position.x),(.5-.3169564)*.01,1e-8),"Blend-shape vertex offsets were lost or transformed twice.");++topVertices;
        }
        require(topVertices>0,"Blend-shape fixture top vertices were not found.");
        bool changed=false;
        for(size_t t=0;t<blendStart->triangles.size();++t)for(int v=0;v<3;++v) {
            const auto a=blendStart->triangles[t].vertices[v].position,b=blendLater->triangles[t].vertices[v].position;
            changed |= std::abs(a.x-b.x)+std::abs(a.y-b.y)+std::abs(a.z-b.z)>1e-5;
        }
        require(changed,"Animated blend shapes were not evaluated.");
        bool missing=false,corrupt=false;
        try{loadFbxMesh(fixtures+"/missing.fbx");}catch(const std::runtime_error&){missing=true;}
        require(missing,"Missing FBX must fail clearly.");
        const auto temporary=std::filesystem::temp_directory_path()/std::filesystem::u8path("Natron-FBX-テスト.fbx");
        {std::ifstream source(fixtures+"/colored-cube.fbx");std::stringstream text;text<<source.rdbuf();std::string content=text.str();
            const std::string original="\"A\",0,0,0";const auto position=content.find(original);require(position!=std::string::npos,"Fixture transform missing.");
            content.replace(position,original.size(),"\"A\",100,200,300\n            P: \"GeometricTranslation\", \"Vector3D\", \"Vector\", \"\",25,0,0");
            std::ofstream out(temporary,std::ios::binary);out<<content;
        }
        auto transformed=loadFbxMesh(temporary.u8string());
        require(close(transformed->bounds.minimum.x,.25) && close(transformed->bounds.minimum.y,1) && close(transformed->bounds.minimum.z,2),"Node/geometric transforms or Unicode filename failed.");
        {std::ofstream out(temporary,std::ios::binary);out<<"not an FBX";}
        try{loadFbxMesh(temporary.u8string());}catch(const std::runtime_error&){corrupt=true;}
        std::filesystem::remove(temporary);require(corrupt,"Corrupt FBX must fail clearly.");
        Camera camera;Settings settings;settings.width=settings.height=1;settings.samples=1;
        Card geometry;geometry.mesh=cube;
        auto pixels=renderCpu({geometry},camera,settings);
        require(close(pixels[0],.8) && close(pixels[1],.2) && close(pixels[3],1),"FBX face material base color was lost.");
        settings.pass=RenderPass::Depth;require(close(renderCpu({geometry},camera,settings)[0],4),"Mesh nearest-surface depth failed.");
        geometry.mesh=quad(0,{1,0,0,.5});settings.pass=RenderPass::Color;
        pixels=renderCpu({geometry},camera,settings);
        require(close(pixels[0],.5) && close(pixels[3],.5),"Shared triangle edge was composited twice or material alpha lost.");
        Card rear;rear.texture=solid({0,0,1,1});rear.width=rear.height=10;rear.transform=Matrix::transform({0,0,-2},{});
        pixels=renderCpu({rear,geometry},camera,settings);
        require(close(pixels[0],.5) && close(pixels[2],.5) && close(pixels[3],1),"Mixed Card/mesh transparent depth order failed.");
        auto texture=std::make_shared<Texture>();texture->width=texture->height=2;
        texture->pixels={0,0,0,1, 1,0,0,1, 0,1,0,1, 1,1,0,1};geometry.texture=texture;
        pixels=renderCpu({geometry},camera,settings);
        require(close(pixels[0],.25) && close(pixels[1],.25),"Barycentric UV/image material override failed.");
        geometry.projected=true;geometry.projector=camera;geometry.projectionAspect=1;
        require(close(renderCpu({geometry},camera,settings)[0],.25),"Project3D did not project onto a mesh.");
        geometry.texture=solid({0,0,0,0});settings.pass=RenderPass::Depth;
        require(close(renderCpu({rear,geometry},camera,settings)[0],7),"Transparent mesh texels occluded the rear depth.");
        Camera moved=camera;moved.transform=Matrix::transform({2,-1,7},{10,25,0},{2,.5,1.5});
        geometry.texture.reset();geometry.projected=false;geometry.transform=moved.transform*Matrix::transform({0,0,-3},{});
        require(close(renderCpu({geometry},moved,settings)[0],3),"Mesh depth is not camera-local under parent/nonuniform transforms.");
        geometry.transform=Matrix();geometry.mesh=cube;camera.nearClip=4.5;
        require(close(renderCpu({geometry},camera,settings)[0],6),"Mesh camera clipping ignored the rear face.");
        std::vector<MeshHit> hits;cube->intersect({0,0,5},{0,0,-1},.001,100,hits);
        require(hits.size()>=2 && std::any_of(hits.begin(),hits.end(),[](const MeshHit& h){return close(h.depth,4);}),"BVH traversal missed the cube.");
        bool aborted=false;try{renderCpu({geometry},camera,settings,[]{return true;});}catch(const std::runtime_error&){aborted=true;}require(aborted,"Mesh render did not honor cancellation.");
        std::cout<<"FBX meshes/cameras: PASS (ASCII/binary, transforms/units, materials, UVs, projection, transparency, clipping, depth, BVH, animation/takes, skinning, blend shapes, visibility, camera axes/lens/orthographic/FOV, errors).\n";
        if(argc>1 && std::string(argv[1])=="--gpu") {
            InstanceOptions options;options.validation=true;auto instance=VulkanInstance::create(options);
            auto device=VulkanDevice::create(instance,argc>2?std::stoi(argv[2]):-1);
            size_t comparisons=0;float maxError=0;
            {
                Renderer renderer(device);Camera view;view.transform=Matrix::transform({0,0,6},{});
                Camera orbit=view;orbit.transform=Matrix::transform({1.2,.3,6},{-3,11,0});
                Camera ortho=view;ortho.orthographic=true;ortho.orthoWidth=4.3;
                Card imported;imported.mesh=cube;Card binaryCard=imported;binaryCard.mesh=binary;
                Card tinted=imported;tinted.opacity=.35;tinted.transform=Matrix::transform({-.3,.1,0},{13,24,7},{.8,1.1,.7});
                Card uv=tinted;uv.texture=texture;
                Card projected=uv;projected.projected=true;projected.projector=view;projected.projectionAspect=1.3;
                Card cropped=projected;auto cropTexture=std::make_shared<Texture>(*texture);cropTexture->imageUvScale={1.8,1.4};cropTexture->imageUvOffset={-.4,-.2};cropped.texture=cropTexture;
                Card invisible=uv;invisible.texture=solid({0,0,0,0});
                Card vertexColor;auto colored=quad(0,{.2f,.3f,.7f,.6f});
                for(auto& triangle:colored->triangles)for(int v=0;v<3;++v)triangle.vertices[v].color={float(v+1),.5f,.3f,.75f};
                colored->buildAcceleration();vertexColor.mesh=colored;
                Card hdr=vertexColor;hdr.texture=solid({2.f,-.2f,.3f,.7f});
                Card shared;shared.mesh=quad(0,{1,0,0,.5});
                std::vector<Card> instances(16,tinted);
                for(size_t i=0;i<instances.size();++i)instances[i].transform=Matrix::transform({double(i%4)*.5-.75,double(i/4)*.5-.75,-double(i)*.07},{8,double(i)*9,3},{.3,.3,.3});
                std::vector<std::vector<Card>> cases{{imported},{binaryCard},{tinted,rear},{uv,rear},{projected,rear},{cropped,rear},{invisible,rear},{vertexColor},{hdr},{shared},{rear,shared},instances};
                for(const auto& mesh:{halfway,ending,scaled,hidden,skinStart,skinEnd,bonesStart,bonesEnd,blendStart,blendLater}) {
                    Card animated;animated.mesh=mesh;
                    if(!mesh->triangles.empty()) {
                        const auto lo=mesh->bounds.minimum,hi=mesh->bounds.maximum,center=(lo+hi)*.5;
                        const double size=2/std::max({hi.x-lo.x,hi.y-lo.y,hi.z-lo.z});
                        animated.transform=Matrix::transform(center*(-size),{8,17,3},{size,size,size});
                    }
                    cases.push_back({animated});
                }
                Settings output;output.width=64;output.height=48;output.pixelAspect=1.1;
                auto compare=[&](const std::vector<Card>& scene,const Camera& c,const Settings& s) {
                    const auto cpu=renderCpu(scene,c,s),gpu=renderer.render(scene,c,s);require(cpu.size()==gpu.size(),"GPU mesh dimensions differ.");
                    for(size_t i=0;i<cpu.size();++i) {
                        require(std::isfinite(gpu[i]),"GPU mesh produced non-finite pixels.");
                        const float error=std::abs(cpu[i]-gpu[i]);maxError=std::max(maxError,error);
                        if(error>.0005f) {
                            std::ostringstream message;message<<"GPU mesh disagrees with CPU: pixel="<<i/4<<" channel="<<i%4<<" cpu="<<cpu[i]<<" gpu="<<gpu[i]<<" pass="<<int(s.pass)<<" samples="<<s.samples<<" objects="<<scene.size()<<" texture="<<bool(scene.front().texture)<<" projected="<<scene.front().projected<<" triangles="<<(scene.front().mesh?scene.front().mesh->triangles.size():0)<<" cameraX="<<c.transform.m[12]<<" ortho="<<c.orthographic;
                            Settings expanded=s;expanded.width*=s.samples;expanded.height*=s.samples;expanded.samples=1;
                            const auto ecpu=renderCpu(scene,c,expanded),egpu=renderer.render(scene,c,expanded);
                            for(int yy=0;yy<s.samples;++yy)for(int xx=0;xx<s.samples;++xx) {
                                const size_t at=(size_t((i/4/s.width)*s.samples+yy)*expanded.width+(i/4%s.width)*s.samples+xx)*4;
                                message<<"\n sample "<<xx<<","<<yy<<": ";for(int j=0;j<4;++j)message<<ecpu[at+j]<<"/"<<egpu[at+j]<<" ";
                            }
                            throw std::runtime_error(message.str());
                        }
                    }
                    comparisons+=cpu.size()/4;
                };
                for(auto pass:{RenderPass::Color,RenderPass::Depth})for(int samples:{1,2,4})for(const auto& scene:cases)for(const auto& c:{view,orbit,ortho}) {
                    output.pass=pass;output.samples=samples;compare(scene,c,output);
                }
                // Shared-edge alpha, transformed-camera Z and clipped back faces.
                Settings one;one.samples=1;
                for(const auto& camera:{cameraStart,cameraMiddle,cameraEnd,importedOrtho,importedFov,binaryCameras->camera()})
                    for(auto pass:{RenderPass::Color,RenderPass::Depth}){one.pass=pass;compare({imported},camera,one);}
                for(auto pass:{RenderPass::Color,RenderPass::Depth}) {
                    one.pass=pass;compare({shared},view,one);Camera clipped=view;clipped.nearClip=5.5;compare({imported},clipped,one);
                    compare({geometry},moved,one);
                }
                // More than 16 transparent surfaces in a single mesh: no layer truncation.
                auto layers=std::make_shared<Mesh>();
                for(int i=0;i<40;++i) {auto layer=quad(-i*.02,{.2f,.5f,.8f,.02f});layers->triangles.insert(layers->triangles.end(),layer->triangles.begin(),layer->triangles.end());}
                layers->buildAcceleration();Card layered;layered.mesh=layers;
                one.pass=RenderPass::Color;compare({layered,rear},view,one);
                // Reusing the program after increasing and shrinking storage buffers.
                compare({imported},view,output);compare({layered},view,one);compare({imported},view,one);
                bool excessive=false;try{renderer.render(std::vector<Card>(17,imported),view,one);}catch(const std::invalid_argument&){excessive=true;}
                require(excessive,"GPU object limit was not reported.");
            }
            require(instance->validationErrorCount()==0,"Vulkan validation reported mesh errors.");
            std::cout<<"Device: "<<device->properties().deviceName<<"; GPU mesh pixels compared="<<comparisons<<"; maximum error="<<maxError<<"; validation errors=0\n";
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<"FBX meshes: FAIL: "<<e.what()<<'\n';return 1;}
}
