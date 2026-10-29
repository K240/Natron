// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdScene.h"
#include <QImage>
#include <QColor>
#include <pxr/usd/usdUtils/usdzPackage.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
using namespace Natron::Gpu::Cards;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
bool close(float a,float b){return std::abs(a-b)<1e-5f;}
void write(const std::filesystem::path& path,const std::string& text){std::ofstream f(path);f<<text;require(bool(f),"Cannot write texture test stage.");}
std::string stage(const std::string& file="rgba.png",const std::string& space="raw",bool alpha=true) {
    std::ostringstream s;s<<R"USD(#usda 1.0
( metersPerUnit = 1
  startTimeCode = 1
  endTimeCode = 25
  timeCodesPerSecond = 24
)
def Material "Material" {
 asset inputs:file = @)USD"<<file<<R"USD(@
 string inputs:uvSet = "uvSet"
 token outputs:surface.connect = </Material/Surface.outputs:surface>
 def Shader "Surface" {
  uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor.connect = </Material/Image.outputs:rgb>
)USD";
    if(alpha)s<<"  float inputs:opacity.connect = </Material/Image.outputs:a>\n";
    s<<R"USD(  float inputs:roughness = 0.7
  token outputs:surface
 }
 def Shader "Image" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file.connect = </Material.inputs:file>
  token inputs:sourceColorSpace = ")USD"<<space<<R"USD("
  token inputs:wrapS = "repeat"
  token inputs:wrapT = "clamp"
  float4 inputs:fallback = (0.25,0.5,0.75,0.5)
  float2 inputs:st.connect = </Material/Transform.outputs:result>
  float3 outputs:rgb
  float outputs:a
 }
 def Shader "Transform" {
  uniform token info:id = "UsdTransform2d"
  float2 inputs:in.connect = </Material/UV.outputs:result>
  float2 inputs:scale = (1,1)
  float2 inputs:translation.timeSamples = {1:(0,0),25:(0.5,0.25)}
  float2 outputs:result
 }
 def Shader "UV" {
  uniform token info:id = "UsdPrimvarReader_float2"
  string inputs:varname.connect = </Material.inputs:uvSet>
  float2 outputs:result
 }
}
def Material "Plain" {
 token outputs:surface.connect = </Plain/Surface.outputs:surface>
 def Shader "Surface" {
  uniform token info:id = "UsdPreviewSurface"
  color3f inputs:diffuseColor = (0.1,0.8,0.2)
  token outputs:surface
 }
}
def Mesh "Panel" ( prepend apiSchemas = ["MaterialBindingAPI"] ) {
 point3f[] points = [(-1.6,-0.8,0),(0,-0.8,0),(0,0.8,0),(-1.6,0.8,0),(1.6,-0.8,0),(1.6,0.8,0)]
 int[] faceVertexCounts = [4,4]
 int[] faceVertexIndices = [0,1,2,3,1,4,5,2]
 uniform token subdivisionScheme = "none"
 normal3f[] normals = [(0,0,1)] ( interpolation = "constant" )
 texCoord2f[] primvars:st = [(0.9,0.9)] ( interpolation = "constant" )
 texCoord2f[] primvars:uvSet = [(-0.5,-0.5),(1.5,-0.5),(1.5,1.5),(-0.5,1.5)] ( interpolation = "faceVarying" )
 int[] primvars:uvSet:indices = [0,1,2,3,0,1,2,3]
 rel material:binding = </Material>
 def GeomSubset "Second" ( prepend apiSchemas = ["MaterialBindingAPI"] ) {
  uniform token elementType = "face"
  uniform token familyName = "materialBind"
  int[] indices = [1]
  rel material:binding = </Plain>
 }
}
def DistantLight "Key" {
 float inputs:angle = 0
 float inputs:intensity = 1.5
}
)USD";return s.str();
}
void compare(Renderer* renderer,const std::vector<Card>& cards,const Camera& camera,Settings settings) {
    if(!renderer)return;
    for(auto pass:{RenderPass::Color,RenderPass::Depth}) {
        settings.pass=pass;const auto cpu=renderCpu(cards,camera,settings),gpu=renderer->render(cards,camera,settings);double maximum=0;
        require(cpu.size()==gpu.size(),"Texture render output size differs.");
        for(size_t i=0;i<cpu.size();++i){maximum=std::max(maximum,double(std::abs(cpu[i]-gpu[i])));require(std::isfinite(gpu[i]) && std::abs(cpu[i]-gpu[i])<.002f,"USD texture CPU/Vulkan mismatch.");}
        std::cout<<"Texture "<<(pass==RenderPass::Color?"color":"depth")<<" max difference: "<<maximum<<'\n';
    }
}
void testEnvironment(const std::filesystem::path& dir,Renderer* renderer) {
    QImage image(16,8,QImage::Format_RGBA8888);
    for(int y=0;y<8;++y)for(int x=0;x<16;++x)image.setPixelColor(x,y,QColor(16*x,255-16*x,32*y,0));
    require(image.save(QString::fromStdString((dir/"dome.png").string())),"Cannot write dome image.");
    const std::string dome=R"USD(#usda 1.0
( metersPerUnit = 0.01
  upAxis = "Z"
  startTimeCode = 1
  endTimeCode = 25
  timeCodesPerSecond = 24
)
def DomeLight "Dome" ( prepend apiSchemas = ["ShadowAPI"] ) {
 asset inputs:texture:file = @dome.png@ ( colorSpace = "raw" )
 token inputs:texture:format = "latlong"
 color3f inputs:color = (0.5,0.75,1)
 float inputs:intensity = 2
 float inputs:exposure.timeSamples = { 1: 0, 25: 1 }
 float xformOp:rotateY.timeSamples = { 1: 0, 25: 90 }
 uniform token[] xformOpOrder = ["xformOp:rotateY"]
 bool inputs:shadow:enable = false
 bool inputs:normalize = true
}
)USD";
    write(dir/"dome.usda",dome);auto asset=UsdSceneAsset::load((dir/"dome.usda").string());
    const auto first=asset->evaluate(0),last=asset->evaluate(1);
    require(first.warnings.empty() && first.lights.size()==1 && first.lights[0].type==LightType::Environment,"USD dome traversal failed.");
    const auto& light=first.lights[0];require(light.environment && light.intensity==2 && !light.shadows,"USD dome parameters failed.");
    require(light.environment==last.lights[0].environment && last.lights[0].exposure==1,"USD dome animation did not reuse image pixels.");
    const auto pole=light.transform.vector({0,1,0});require(std::abs(pole.z+.01)<1e-6,"Z-up dome local Y pole lost its stage transform.");
    const auto rgba=light.environment->sample(.25,.75);require(close(rgba[0],56/255.f) && rgba[3]==1,"Dome raw color or alpha handling failed.");
    Card card;card.material.lit=true;card.material.metallic=1;card.material.roughness=.2;Camera camera;
    Settings settings;settings.width=32;settings.height=24;settings.samples=1;settings.lights=first.lights;
    const auto before=renderCpu({card},camera,settings);settings.lights=last.lights;
    require(renderCpu({card},camera,settings)!=before,"Dome transform/exposure animation did not change shading.");compare(renderer,{card},camera,settings);
    settings.lights=first.lights;compare(renderer,{card},camera,settings);
    auto automatic=dome;automatic.erase(automatic.find(" ( colorSpace = \"raw\" )"),std::string(" ( colorSpace = \"raw\" )").size());
    write(dir/"dome-auto.usda",automatic);const auto autoImage=UsdSceneAsset::load((dir/"dome-auto.usda").string())->evaluate(0).lights[0].environment;
    require(autoImage->sample(.25,.75)[0]<rgba[0]*.4f,"Dome auto sRGB decoding failed.");
    auto uniform=dome;const auto file=uniform.find("@dome.png@");uniform.replace(file,10,"@@");write(dir/"dome-uniform.usda",uniform);
    require(!UsdSceneAsset::load((dir/"dome-uniform.usda").string())->evaluate(0).lights[0].environment,"Empty dome texture did not use uniform color.");
    auto missing=dome;missing.replace(missing.find("@dome.png@"),10,"@missing-dome.png@");write(dir/"dome-missing.usda",missing);
    auto fallback=UsdSceneAsset::load((dir/"dome-missing.usda").string())->evaluate(0);
    require(!fallback.warnings.empty() && fallback.lights[0].environment->sample(.5,.5)[0]==0,"Missing dome texture did not report/use black radiance.");
    auto unsupported=dome;unsupported.replace(unsupported.find("\"latlong\""),9,"\"mirroredBall\"");write(dir/"dome-unsupported.usda",unsupported);
    auto invalid=UsdSceneAsset::load((dir/"dome-unsupported.usda").string())->evaluate(0);
    require(!invalid.warnings.empty() && invalid.lights[0].intensity==0,"Unsupported dome parameterization was silently used.");
    require(UsdUtilsCreateNewUsdzPackage(SdfAssetPath((dir/"dome.usda").string()),(dir/"dome.usdz").string()),"Dome USDZ packaging failed.");
    const auto packaged=UsdSceneAsset::load((dir/"dome.usdz").string())->evaluate(0);
    require(packaged.warnings.empty() && packaged.lights[0].environment->pixelData()==light.environment->pixelData(),"Embedded dome texture did not resolve.");
    image.fill(QColor(0,255,0,0));require(image.save(QString::fromStdString((dir/"dome.png").string())),"Cannot update dome image.");
    asset->reload();const auto reloaded=asset->evaluate(0).lights[0].environment->sample(.25,.75);
    require(reloaded[0]==0 && reloaded[1]==1,"Dome reload retained old pixels.");
}
void testDataMaps(const std::filesystem::path& dir,Renderer* renderer) {
    QImage normal(1,1,QImage::Format_RGBA8888);normal.fill(QColor(51,128,230,0));
    require(normal.save(QString::fromStdString((dir/"normal.png").string())),"Cannot write normal map.");
    QImage orm(2,2,QImage::Format_RGBA8888);
    orm.setPixelColor(0,0,QColor(30,90,160,40));orm.setPixelColor(1,0,QColor(50,210,220,60));
    orm.setPixelColor(0,1,QColor(60,110,120,80));orm.setPixelColor(1,1,QColor(80,180,200,100));
    require(orm.save(QString::fromStdString((dir/"orm.png").string())),"Cannot write scalar map.");
    auto text=stage("rgba.png","raw",false);
    const auto rough=text.find("  float inputs:roughness = 0.7");
    text.replace(rough,std::string("  float inputs:roughness = 0.7").size(),R"USD(  normal3f inputs:normal.connect = </Material/Normal.outputs:rgb>
  float inputs:roughness.connect = </Material/ORM.outputs:g>
  float inputs:metallic.connect = </Material/ORM.outputs:b>)USD");
    const auto nodes=text.find(" def Shader \"Transform\"");
    text.insert(nodes,R"USD( def Shader "Normal" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @normal.png@
  token inputs:sourceColorSpace = "raw"
  token inputs:wrapS = "repeat"
  token inputs:wrapT = "mirror"
  float4 inputs:scale = (2,2,2,1)
  float4 inputs:bias = (-1,-1,-1,0)
  float2 inputs:st.connect = </Material/Transform.outputs:result>
  float3 outputs:rgb
 }
 def Shader "ORM" {
  uniform token info:id = "UsdUVTexture"
  asset inputs:file = @orm.png@
  token inputs:sourceColorSpace = "raw"
  token inputs:wrapS = "repeat"
  token inputs:wrapT = "clamp"
  float2 inputs:st.connect = </Material/DataUV.outputs:result>
  float outputs:r
  float outputs:g
  float outputs:b
  float outputs:a
 }
 def Shader "DataUV" {
  uniform token info:id = "UsdPrimvarReader_float2"
  string inputs:varname = "st"
  float2 outputs:result
 }
)USD");
    write(dir/"maps.usda",text);auto asset=UsdSceneAsset::load((dir/"maps.usda").string());auto pose=asset->evaluate(0);
    require(pose.warnings.empty() && pose.cards.size()==2,"USD PBR maps produce import diagnostics.");
    const auto& card=pose.cards[0];const auto& maps=card.maps;
    require(maps[0].texture && maps[1].texture && maps[2].texture,"Missing PBR maps.");
    require(maps[1].texture==maps[2].texture && maps[1].channel==1 && maps[2].channel==2,"Packed ORM sampler/channel sharing failed.");
    auto n=maps[0].texture->sample(.25,.75);
    require(close(n[0],-.6f) && close(n[2],230/255.f*2-1) && n[3]==0,"Normal map lost signed/raw values or was premultiplied.");
    auto scalar=maps[1].texture->sample(.25,.75);
    require(close(scalar[1],90/255.f) && close(scalar[2],160/255.f) && close(scalar[3],40/255.f),"Scalar map channels were color converted/premultiplied.");
    const auto& vertex=card.mesh->triangles[0].vertices[0];
    require(vertex.mapUvs[0][0]==-.5 && std::abs(vertex.mapUvs[1][0]-.9)<1e-6,"Independent material UV sets ignored.");
    auto animated=asset->evaluate(1);
    require(close(float(animated.cards[0].maps[0].texture->uvTransform[2]),.5) && maps[0].texture->sharedPixels==animated.cards[0].maps[0].texture->sharedPixels,"Animated data sampler copied/lost pixels.");
    Settings settings;settings.width=80;settings.height=50;settings.samples=2;settings.lights=pose.lights;
    settings.lights[0].transform=Matrix::transform({}, {15,-30,0});Camera camera;
    const auto baseline=renderCpu(pose.cards,camera,settings);
    for(int j=0;j<3;++j) {
        auto without=pose.cards;without[0].maps[j]={};
        const auto plain=renderCpu(without,camera,settings);double difference=0;
        for(size_t i=0;i<plain.size();++i)difference=std::max(difference,double(std::abs(plain[i]-baseline[i])));
        require(difference>.001,"PBR map did not influence lighting.");
    }
    // An analytic constant normal must match normals authored directly on the
    // mesh. This also checks handedness, independently of CPU/GPU agreement.
    auto authored=pose.cards;auto mesh=std::make_shared<Mesh>(*authored[0].mesh);
    const double len=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
    for(auto& tri:mesh->triangles)for(auto& v:tri.vertices)v.normal={n[0]/len,n[1]/len,n[2]/len};
    authored[0].mesh=mesh;authored[0].maps[0]={};const auto expected=renderCpu(authored,camera,settings);
    for(size_t i=0;i<expected.size();++i)require(std::abs(expected[i]-baseline[i])<1e-5,"Tangent-space normal orientation incorrect.");
    compare(renderer,pose.cards,camera,settings);compare(renderer,animated.cards,camera,settings);
    for(auto scale:{Vec3{1.4,.7,1.2},Vec3{-1.2,.8,1}}) {
        auto transformed=pose.cards;transformed[0].transform=Matrix::transform({}, {12,20,25},scale);
        compare(renderer,transformed,camera,settings);
    }
    auto degenerate=pose.cards;mesh=std::make_shared<Mesh>(*card.mesh);
    for(auto& tri:mesh->triangles)for(auto& v:tri.vertices)v.mapUvs[0]={.5,.5};
    degenerate[0].mesh=mesh;auto flat=degenerate;flat[0].maps[0]={};
    require(renderCpu(degenerate,camera,settings)==renderCpu(flat,camera,settings),"Degenerate UVs did not retain geometric shading normal.");
    compare(renderer,degenerate,camera,settings);
    settings.pass=RenderPass::Depth;flat=pose.cards;flat[0].maps={};
    require(renderCpu(flat,camera,settings)==renderCpu(pose.cards,camera,settings),"PBR data maps changed alpha/depth coverage.");settings.pass=RenderPass::Color;
    // Alpha is a selectable scalar, independent of surface opacity.
    // Mirror only the normal map's UVs: the tangent X direction must flip.
    auto mirrored=pose.cards;mesh=std::make_shared<Mesh>(*card.mesh);
    for(auto& tri:mesh->triangles)for(auto& v:tri.vertices)v.mapUvs[0][0]=-v.mapUvs[0][0];
    mirrored[0].mesh=mesh;authored=mirrored;mesh=std::make_shared<Mesh>(*mesh);
    for(auto& tri:mesh->triangles)for(auto& v:tri.vertices)v.normal={-n[0]/len,n[1]/len,n[2]/len};
    authored[0].mesh=mesh;authored[0].maps[0]={};
    auto mirroredImage=renderCpu(mirrored,camera,settings),authoredImage=renderCpu(authored,camera,settings);
    for(size_t i=0;i<mirroredImage.size();++i)require(std::abs(mirroredImage[i]-authoredImage[i])<1e-5,"Mirrored UV tangent handedness incorrect.");
    compare(renderer,mirrored,camera,settings);
    // Spatially varying normals must follow the animated sampler without
    // copying pixels. Use a frozen camera/light and remove base-color animation.
    QImage varying(2,2,QImage::Format_RGBA8888);
    varying.setPixelColor(0,0,QColor(51,128,230,0));varying.setPixelColor(1,0,QColor(204,128,230,0));
    varying.setPixelColor(0,1,QColor(128,51,230,0));varying.setPixelColor(1,1,QColor(128,204,230,0));
    require(varying.save(QString::fromStdString((dir/"varying-normal.png").string())),"Cannot write varying normal map.");
    auto varyingText=text;const auto normalFile=varyingText.find("@normal.png@");varyingText.replace(normalFile,12,"@varying-normal.png@");
    write(dir/"varying-map.usda",varyingText);auto varyingAsset=UsdSceneAsset::load((dir/"varying-map.usda").string());
    auto varyingFirst=varyingAsset->evaluate(0),varyingLast=varyingAsset->evaluate(1);
    varyingFirst.cards[0].texture.reset();varyingLast.cards[0].texture.reset();
    require(renderCpu(varyingFirst.cards,camera,settings)!=renderCpu(varyingLast.cards,camera,settings),"Animated normal-map UVs did not change lighting.");
    compare(renderer,varyingFirst.cards,camera,settings);compare(renderer,varyingLast.cards,camera,settings);
    Card plainCard;plainCard.width=2;plainCard.height=1.5;plainCard.material.lit=true;plainCard.maps=maps;
    compare(renderer,{plainCard},camera,settings);
    auto alpha=text;const auto channel=alpha.find("roughness.connect = </Material/ORM.outputs:g>");
    alpha.replace(channel,std::string("roughness.connect = </Material/ORM.outputs:g>").size(),"roughness.connect = </Material/ORM.outputs:a>");
    write(dir/"alpha-map.usda",alpha);auto alphaPose=UsdSceneAsset::load((dir/"alpha-map.usda").string())->evaluate(0);
    require(alphaPose.cards[0].maps[1].channel==3,"Scalar alpha channel selection failed.");compare(renderer,alphaPose.cards,camera,settings);
    auto missing=text;const auto file=missing.find("@normal.png@");missing.replace(file,12,"@missing-normal.png@");
    write(dir/"missing-map.usda",missing);auto fallback=UsdSceneAsset::load((dir/"missing-map.usda").string())->evaluate(0);
    require(!fallback.warnings.empty() && fallback.cards[0].maps[0].texture,"Data map fallback missing.");compare(renderer,fallback.cards,camera,settings);
    normal.fill(QColor(128,128,255,0));require(normal.save(QString::fromStdString((dir/"normal.png").string())),"Cannot update normal map.");
    asset->reload();require(asset->evaluate(0).cards[0].maps[0].texture->sample(.5,.5)[2]==1,"Reload retained old normal map pixels.");
    require(UsdUtilsCreateNewUsdzPackage(SdfAssetPath((dir/"maps.usda").string()),(dir/"maps.usdz").string()),"PBR map USDZ packaging failed.");
    auto packaged=UsdSceneAsset::load((dir/"maps.usdz").string())->evaluate(0);require(packaged.warnings.empty() && packaged.cards[0].maps[2].texture,"Embedded PBR maps failed.");compare(renderer,packaged.cards,camera,settings);
    if(renderer) {
        std::vector<Card> many(5,card);
        for(auto& c:many){c.texture=std::make_shared<Texture>(*card.texture);for(auto& m:c.maps)m.texture=std::make_shared<Texture>(*m.texture);}
        bool rejected=false;try{renderer->render(many,camera,settings);}catch(const std::invalid_argument& e){rejected=std::string(e.what()).find("16 distinct texture samplers")!=std::string::npos;}
        require(rejected,"Vulkan texture sampler budget was not checked.");
    }
}

}
void testUsdTextures(Renderer* renderer) {
    const auto dir=std::filesystem::temp_directory_path()/std::filesystem::path("natron-usd-textures-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    QImage png(2,2,QImage::Format_RGBA8888);
    png.setPixelColor(0,0,QColor(204,102,51,128));png.setPixelColor(1,0,QColor(51,153,255,255));
    png.setPixelColor(0,1,QColor(255,64,128,0));png.setPixelColor(1,1,QColor(128,128,128,64));
    require(png.save(QString::fromStdString((dir/"rgba.png").string())),"Cannot write PNG fixture.");
    write(dir/"stage.usda",stage());auto asset=UsdSceneAsset::load((dir/"stage.usda").string());auto first=asset->evaluate(0),last=asset->evaluate(1);
    require(first.cards.size()==2 && first.meshPaths.size()==1 && first.warnings.empty(),"Textured material subset/interface import failed.");
    require(first.cards[0].texture && !first.cards[1].texture && first.cards[0].mesh->triangles.size()==2,"Material image groups failed.");
    auto tex=first.cards[0].texture;
    require(tex==asset->evaluate(0).cards[0].texture,"USD image cache did not reuse immutable pixels.");
    require(first.cards[0].mesh->triangles[0].vertices[0].uv[0]==-.5,"Named/indexed UV primvar ignored.");
    const auto topLeft=tex->sample(.25,.75);
    if(!close(topLeft[0],.8f*128/255) || !close(topLeft[3],128/255.f))std::cerr<<"PNG sample: "<<topLeft[0]<<", "<<topLeft[1]<<", "<<topLeft[2]<<", "<<topLeft[3]<<" size "<<tex->width<<'x'<<tex->height<<'\n';
    require(close(topLeft[0],.8f*128/255) && close(topLeft[3],128/255.f),"Straight-alpha PNG/orientation incorrect.");
    require(close(tex->sample(.25,.25)[3],0) && close(tex->sample(1.25,.75)[0],topLeft[0]),"Alpha/repeat addressing failed.");
    require(close(tex->sample(.75,2)[0],.2f),"Clamp addressing failed.");
    const auto seam=tex->sample(0,.75);require(close(seam[3],(128/255.f+1)/2),"Repeat seam failed to filter across texture edges.");
    require(close(float(last.cards[0].texture->uvTransform[2]),.5) && close(float(last.cards[0].texture->uvTransform[5]),.25),"Animated UV transform not evaluated.");
    require(tex->sharedPixels==last.cards[0].texture->sharedPixels,"UV animation copied/redecoded image pixels.");
    require(asset->evaluate(1,false).cards[0].texture==tex,"Frozen USD texture changed.");
    Settings settings;settings.width=80;settings.height=50;settings.samples=2;settings.lights=first.lights;Camera camera;
    auto rendered=renderCpu(first.cards,camera,settings);require(rendered!=renderCpu(last.cards,camera,settings),"UV animation did not affect output.");
    compare(renderer,first.cards,camera,settings);compare(renderer,last.cards,camera,settings);
    auto layered=first.cards;Card background;background.width=4;background.height=2;background.transform=Matrix::transform({0,0,-1},{});background.material.lit=true;
    layered.push_back(background);compare(renderer,layered,camera,settings);
    for(auto mode:{TextureAddress::Black,TextureAddress::Clamp,TextureAddress::Repeat,TextureAddress::Mirror}) {
        auto texture=std::make_shared<Texture>(*tex);texture->wrapS=texture->wrapT=mode;texture->uvTransform={.8,.4,-.6,-.4,.8,.2};
        auto cards=first.cards;cards[0].texture=texture;compare(renderer,cards,camera,settings);
    }
    auto mirror=std::make_shared<Texture>(*tex);mirror->wrapS=TextureAddress::Mirror;
    require(close(mirror->sample(1.25,.75)[0],tex->sample(.75,.75)[0]) && close(mirror->sample(-.25,.75)[0],topLeft[0]),"Mirror addressing failed.");
    mirror->wrapS=TextureAddress::Black;require(mirror->sample(-.01,.75)[3]==0,"Black addressing failed.");
    write(dir/"auto.usda",stage("rgba.png","auto",false));auto automatic=UsdSceneAsset::load((dir/"auto.usda").string())->evaluate(0);
    auto rgb=automatic.cards[0].texture->sample(.25,.75);const float expected=std::pow((.8f+.055f)/1.055f,2.4f);
    require(close(rgb[0],expected) && rgb[3]==1,"Auto sRGB/independent opacity failed.");
    write(dir/"srgb.usda",stage("rgba.png","sRGB",false));require(close(UsdSceneAsset::load((dir/"srgb.usda").string())->evaluate(0).cards[0].texture->sample(.25,.75)[0],expected),"Explicit sRGB failed.");
    require(png.save(QString::fromStdString((dir/"rgb.jpg").string()),"JPG",100),"Cannot write JPEG fixture.");
    const QImage jpeg(QString::fromStdString((dir/"rgb.jpg").string()));
    write(dir/"jpeg.usda",stage("rgb.jpg","raw",false));
    const auto jpg=UsdSceneAsset::load((dir/"jpeg.usda").string())->evaluate(0);
    require(jpg.warnings.empty() && close(jpg.cards[0].texture->sample(.25,.75)[0],float(jpeg.pixelColor(0,0).redF())),"JPEG decoding/orientation failed.");
    auto adjusted=stage();const auto transform=adjusted.find("  float2 inputs:st.connect");
    adjusted.insert(transform,"  float4 inputs:scale = (0.5,1,1,0.5)\n  float4 inputs:bias = (0.1,0,0,0.1)\n");
    write(dir/"adjusted.usda",adjusted);const auto adjust=UsdSceneAsset::load((dir/"adjusted.usda").string())->evaluate(0).cards[0].texture->sample(.25,.75);
    require(close(adjust[3],128/255.f*.5f+.1f) && close(adjust[0],.5f*adjust[3]),"Texture scale/bias applied in the wrong order.");
    QImage gray(1,1,QImage::Format_Grayscale8);gray.fill(128);require(gray.save(QString::fromStdString((dir/"gray.png").string())),"Cannot write grayscale fixture.");
    write(dir/"gray.usda",stage("gray.png","auto",false));require(close(UsdSceneAsset::load((dir/"gray.usda").string())->evaluate(0).cards[0].texture->sample(.25,.75)[0],128/255.f),"Single-channel auto color space failed.");
    QImage wide(1,1,QImage::Format_RGBA64);wide.fill(QColor::fromRgbF(.5,.25,.75,.5));require(wide.save(QString::fromStdString((dir/"wide.png").string())),"Cannot write 16-bit fixture.");
    write(dir/"wide.usda",stage("wide.png","auto",false));require(close(UsdSceneAsset::load((dir/"wide.usda").string())->evaluate(0).cards[0].texture->sample(.25,.75)[0],float(wide.pixelColor(0,0).redF())),"16-bit PNG precision/raw auto failed.");
    // The package must resolve and decode the embedded image, including the
    // file value authored on a material interface instead of the texture node.
    require(UsdUtilsCreateNewUsdzPackage(SdfAssetPath((dir/"stage.usda").string()),(dir/"scene.usdz").string()),"Textured USDZ package failed.");
    auto packaged=UsdSceneAsset::load((dir/"scene.usdz").string())->evaluate(0);
    require(packaged.warnings.empty() && close(packaged.cards[0].texture->sample(.25,.75)[0],topLeft[0]),"Embedded USDZ image failed.");compare(renderer,packaged.cards,camera,settings);
    write(dir/"missing.usda",stage("missing.png"));auto missing=UsdSceneAsset::load((dir/"missing.usda").string())->evaluate(0);
    require(!missing.warnings.empty() && close(missing.cards[0].texture->sample(-2,3)[0],.125f),"Missing image fallback/diagnostic failed.");
    testDataMaps(dir,renderer);
    testEnvironment(dir,renderer);
    // A same-second external image edit becomes visible on explicit reload.
    png.fill(QColor(0,255,0,255));require(png.save(QString::fromStdString((dir/"rgba.png").string())),"Cannot update PNG fixture.");
    asset->reload();const auto reloaded=asset->evaluate(0).cards[0].texture->sample(.25,.75);
    require(reloaded[0]==0 && reloaded[1]==1 && reloaded[3]==1,"External image reload retained cached pixels.");
    std::filesystem::remove_all(dir);
}
