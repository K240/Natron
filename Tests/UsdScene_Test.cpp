// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdScene.h"
#include "Gpu/VulkanDevice.h"
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdUtils/usdzPackage.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <QCoreApplication>
using namespace Natron::Gpu;
using namespace Natron::Gpu::Cards;
PXR_NAMESPACE_USING_DIRECTIVE
void testUsdTextures(Renderer* renderer=nullptr);
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool close(double a,double b,double tolerance=1e-6){return std::abs(a-b)<tolerance;}
template<class F> void fails(F f,const char* message){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}require(rejected,message);}
void write(const std::filesystem::path& p,const char* text){std::ofstream stream(p);stream<<text;require(bool(stream),"Cannot write test fixture.");}
}
int main(int argc,char** argv) {
    QCoreApplication qt(argc,argv);
    try {
        const std::string file=std::string(NATRON_USD_FIXTURES)+"/scene.usda";
        auto asset=UsdSceneAsset::load(file);require(UsdSceneAsset::available(),"USD not compiled.");
        require(asset->startTime()==10 && asset->endTime()==58 && asset->timeCodesPerSecond()==48 && asset->frameRate()==24,"USD time metadata failed.");
        auto first=asset->evaluate(0),last=asset->evaluate(1);require(first.cards.size()==2 && first.lights.size()==2 && asset->cameras().size()==1,"References/instance proxy/light/camera traversal failed.");
        require(first.cards[0].mesh->triangles.size()==2 && first.cards[1].mesh->triangles.size()==2,"Quad triangulation failed.");
        require(close(last.cards[0].transform.point({}).z,1) && close(asset->evaluate(1,false).cards[0].transform.point({}).z,0),"USD animation timing/freeze failed.");
        require(first.cards[0].mesh->triangles[0].material.lit && close(first.cards[0].mesh->triangles[0].material.baseColor[2],1),"PreviewSurface binding failed.");
        require(first.cards[1].mesh->triangles[0].material.metallic==1,"Instance variant material failed.");
        require(close(first.cards[1].mesh->triangles[0].material.baseColor[0],1) && close(first.cards[1].mesh->triangles[0].material.baseColor[2],.2),"Instance variant base color failed.");
        auto override=UsdSceneAsset::load(file,"/World/Animated look=gold");require(override->evaluate(0).cards[0].mesh->triangles[0].material.metallic==1,"Session variant override failed.");
        require(asset->evaluate(0).cards[0].mesh->triangles[0].material.metallic==0,"Variant leaked into another stage.");
        require(asset->evaluate(0,true,"/World/Animated").cards.size()==1,"Prim subtree filtering failed.");
        const auto cam=asset->camera("Camera",0),camLast=asset->camera("/World/Camera",1);
        require(close(cam.focal,35,1e-5) && close(cam.aperture,36,1e-5) && close(camLast.focal,40,1e-5) && close(camLast.transform.point({}).x,.5),"Camera physical units/lens/transform animation failed.");
        require(close(first.lights[0].intensity,3) && close(first.lights[1].intensity,20),"USD light normalization failed.");
        const auto attributesFile=std::string(NATRON_USD_FIXTURES)+"/attributes.usda";
        auto attributes=UsdSceneAsset::load(attributesFile);auto attrs=attributes->evaluate(0);
        require(attrs.cards.size()==1 && attrs.cards[0].mesh->triangles.size()==2,"Purpose/hole filtering failed.");
        const auto& faces=attrs.cards[0].mesh->triangles;
        require(faces[0].material.baseColor[0]==1 && faces[0].material.baseColor[1]==0 && faces[1].material.baseColor[0]==0 && faces[1].material.baseColor[1]==1,"Inherited/per-face material binding failed.");
        require(close(faces[0].vertices[0].color[3],.4) && close(faces[1].vertices[0].color[3],.6),"Uniform opacity interpolation failed.");
        require(faces[0].vertices[1].uv[0]==1 && faces[0].vertices[1].uv[1]==0 && faces[0].vertices[1].normal.z==1,"Indexed UV/face-varying normals failed.");
        const auto reset=attrs.cards[0].transform.point({});require(reset.x==2 && reset.y==0,"resetXformStack ignored.");
        require(attrs.warnings.size()>=2,"Unsupported connected texture/geometry were not reported.");
        require(attributes->evaluate(1).cards.empty(),"Inherited animated visibility failed.");
        fails([&]{asset->evaluate(0,true,"/Missing");},"Invalid prim path accepted.");
        fails([&]{asset->camera("Missing",0);},"Invalid camera accepted.");
        fails([&]{UsdSceneAsset::load(file,"/World/Animated look=missing");},"Invalid variant accepted.");
        fails([&]{UsdSceneAsset::load(file,"/World/Animated missing=blue");},"Invalid variant set accepted.");
        auto dir=std::filesystem::temp_directory_path()/std::filesystem::path("natron-usd-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        write(dir/"sublayers.usda",R"USD(#usda 1.0
( subLayers = [@attributes.usda@] )
)USD");
        std::filesystem::copy_file(attributesFile,dir/"attributes.usda");
        require(UsdSceneAsset::load((dir/"sublayers.usda").string())->evaluate(0).cards.size()==1,"Sublayer composition failed.");
        write(dir/"missing-reference.usda",R"USD(#usda 1.0
def Xform "Missing" ( prepend references = @does-not-exist.usda@ ) { }
)USD");
        require(!UsdSceneAsset::load((dir/"missing-reference.usda").string())->evaluate(0).warnings.empty(),"Missing reference was not reported.");
        fails([&]{UsdSceneAsset::load((dir/"does-not-exist.usda").string());},"Missing USD file accepted.");
        // The official writer produces real binary USD and a self-contained
        // package, exercising format plugins and relative asset resolution.
        auto stage=UsdStage::Open(file);require(stage->Flatten()->Export((dir/"scene.usdc").string()),"Crate export failed.");
        auto crate=UsdSceneAsset::load((dir/"scene.usdc").string());require(crate->evaluate(1).cards.size()==2,"Binary USD import failed.");
        // Packaging rewrites asset paths in its source layers; isolate those
        // layers from the live import stages used for rendering below.
        std::filesystem::create_directories(dir/"package-source");
        std::filesystem::copy_file(file,dir/"package-source/scene.usda");
        std::filesystem::copy_file(std::string(NATRON_USD_FIXTURES)+"/asset.usda",dir/"package-source/asset.usda");
        require(UsdUtilsCreateNewUsdzPackage(SdfAssetPath((dir/"package-source/scene.usda").string()),(dir/"scene.usdz").string()),"USDZ package failed.");
        auto package=UsdSceneAsset::load((dir/"scene.usdz").string());require(package->evaluate(1).cards.size()==2,"USDZ/relative reference import failed.");
        write(dir/"units.usda",R"USD(#usda 1.0
( metersPerUnit = 0.01
  upAxis = "Z"
)
def Xform "Parent" {
 double3 xformOp:translate = (0,0,100)
 uniform token[] xformOpOrder = ["xformOp:translate"]
 def Mesh "Concave" {
  point3f[] points = [(0,0,0),(200,0,0),(200,200,0),(100,100,0),(0,200,0)]
  int[] faceVertexCounts = [5]
  int[] faceVertexIndices = [0,1,2,3,4]
  uniform token subdivisionScheme = "none"
  normal3f[] normals = [(0,0,1)] ( interpolation = "constant" )
  color3f[] primvars:displayColor = [(1,0,0)] ( interpolation = "constant" )
 }
 def Camera "Top" {
  double3 xformOp:translate = (0,0,400)
  uniform token[] xformOpOrder = ["xformOp:translate"]
  token projection = "orthographic"
  float horizontalAperture = 2000
  float2 clippingRange = (1,10000)
 }
}
)USD");
        auto units=UsdSceneAsset::load((dir/"units.usda").string());auto p=units->evaluate(0);require(p.cards[0].mesh->triangles.size()==3,"Concave triangulation failed.");
        auto position=p.cards[0].transform.point({0,0,0});require(close(position.y,1) && close(position.z,0),"Z-up centimeter conversion failed.");
        const auto top=units->camera("Top",0);require(top.orthographic && close(top.orthoWidth,2) && close(top.transform.point({}).y,5) && close(top.nearClip,.01),"Orthographic camera unit conversion failed.");
        const auto normal=p.cards[0].transform.vector({0,0,1});require(close(normal.y,.01),"USD normal orientation failed.");
        // Payload LoadNone must leave the unexpanded stage usable.
        write(dir/"payload.usda",R"USD(#usda 1.0
def Xform "Payload" ( prepend payload = @units.usda@</Parent> ) { }
)USD");
        require(UsdSceneAsset::load((dir/"payload.usda").string(),{},false)->evaluate(0).cards.empty(),"LoadNone expanded payloads.");
        require(UsdSceneAsset::load((dir/"payload.usda").string())->evaluate(0).cards.size()==1,"LoadAll failed.");
        // Reload a referenced layer, not just the root file.
        write(dir/"root.usda",R"USD(#usda 1.0
def Xform "Ref" ( prepend references = @units.usda@</Parent> ) { }
)USD");
        auto ref=UsdSceneAsset::load((dir/"root.usda").string());require(ref->evaluate(0).cards.size()==1,"Relative reference failed.");
        write(dir/"units.usda",R"USD(#usda 1.0
def Xform "Parent" { }
)USD");
        ref->reload();require(ref->evaluate(0).cards.empty(),"Referenced layer reload failed.");
        Settings settings;settings.width=80;settings.height=50;settings.samples=2;settings.lights=first.lights;
        testUsdTextures();
        const auto cpu=renderCpu(first.cards,cam,settings);require(*std::max_element(cpu.begin(),cpu.end())>0,"USD scene render is empty.");
        size_t covered=0;for(size_t i=3;i<cpu.size();i+=4)covered+=cpu[i]>0;require(covered>500,"USD scene has insufficient coverage.");
        if(argc>1 && std::string(argv[1])=="--gpu") {
            InstanceOptions options;options.validation=true;auto instance=VulkanInstance::create(options);
            auto device=VulkanDevice::create(instance,argc>2?std::stoi(argv[2]):-1);Renderer renderer(device);double maximum=0;
            testUsdTextures(&renderer);
            for(auto pass:{RenderPass::Color,RenderPass::Depth})for(double seconds:{0.,.5,1.}) {
                auto pose=asset->evaluate(seconds);settings.lights=pose.lights;settings.pass=pass;
                auto camera=asset->camera("",seconds);auto expected=renderCpu(pose.cards,camera,settings),actual=renderer.render(pose.cards,camera,settings);
                for(size_t i=0;i<expected.size();++i){maximum=std::max(maximum,double(std::abs(expected[i]-actual[i])));require(std::abs(expected[i]-actual[i])<.002,"USD Vulkan/CPU render mismatch.");}
            }
            require(instance->validationErrorCount()==0,"USD Vulkan validation errors.");std::cout<<"GPU max difference: "<<maximum<<'\n';
        }
        std::filesystem::remove_all(dir);std::cout<<"USD scene tests passed (references, variants, instances, time, units, cameras, lights, crate, USDZ, reload).\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
