// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdSceneBridge.h"
#include "Gpu/UsdScene.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QTemporaryDir>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace Natron::Gpu::Cards;
PXR_NAMESPACE_USING_DIRECTIVE
void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
QJsonObject report(const HdNvpFrame& frame){return QJsonDocument::fromJson(QByteArray::fromStdString(frame.report)).object();}
void saveFrame(const HdNvpFrame& frame,const Settings& settings,const QString& output,const QString& name) {
    QFile raw(QDir(output).filePath(name+QStringLiteral(".rgba32f")));require(raw.open(QIODevice::WriteOnly),"Cannot save float render");raw.write(reinterpret_cast<const char*>(frame.color.data()),qint64(frame.color.size()*sizeof(float)));raw.close();
    QImage preview(settings.width,settings.height,QImage::Format_RGB888);
    for(int y=0;y<settings.height;++y)for(int x=0;x<settings.width;++x){auto* p=preview.scanLine(settings.height-1-y)+x*3;
        for(int c=0;c<3;++c){const auto v=std::max(0.f,frame.color[(size_t(y)*settings.width+x)*4+c]);const auto srgb=v<=.0031308f?v*12.92f:1.055f*std::pow(v,1/2.4f)-.055f;p[c]=uint8_t(std::clamp(srgb*255.f,0.f,255.f));}}
    require(preview.save(QDir(output).filePath(name+QStringLiteral(".png"))),"Cannot save preview");
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(argc>=3 && (std::string(argv[1])=="--scene" || std::string(argv[1])=="--denoiser-scene")) {
            const QString output=argc>=4?QString::fromLocal8Bit(argv[3]):QStringLiteral("build/hdnvp-scene-test");QDir().mkpath(output);
            QElapsedTimer total;total.start();const auto asset=UsdSceneAsset::load(argv[2]);
            require(!asset->cameras().empty(),"Test scene has no camera");const auto cameraPath=asset->cameras().front();auto camera=asset->camera(cameraPath,0,false);
            const auto loadMs=total.elapsed();UsdSceneSource source;source.filename=QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath().toStdString();
            source.timeCode=asset->startTime();source.metersPerUnit=asset->metersPerUnit();source.zUp=asset->zUp();source.timeCodesPerSecond=asset->timeCodesPerSecond();
            UsdSceneBridge bridge;Settings settings;settings.width=640;settings.height=360;
            auto save=[&](const HdNvpFrame& frame,const QString& name){saveFrame(frame,settings,output,name);};
            if(std::string(argv[1])=="--denoiser-scene") {
                const auto raw=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::None},{},{source});save(raw,QStringLiteral("off"));
                const auto oidn=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::Oidn},{},{source});save(oidn,QStringLiteral("oidn"));
                const auto oidnStats=report(oidn).value(QStringLiteral("stats")).toObject();
                require(oidnStats.value(QStringLiteral("Denoiser")).toString().startsWith(QStringLiteral("OIDN")) && oidnStats.value(QStringLiteral("Denoise (ms)")).toDouble()>0,"OIDN did not execute");
                require(raw.color!=oidn.color,"OIDN returned the original noisy color");
                require(raw.depth==oidn.depth && raw.primIds==oidn.primIds && raw.instanceIds==oidn.instanceIds,"OIDN altered depth/picking AOVs");
                for(size_t i=3;i<raw.color.size();i+=4)require(raw.color[i]==oidn.color[i],"OIDN altered alpha");
                const auto restored=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::None},{},{source});
                require(restored.color==raw.color,"Disabling denoising retained the filtered image");
                const auto automatic=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::Auto},{},{source});
                require(report(automatic).value(QStringLiteral("stats")).toObject().value(QStringLiteral("Denoiser")).toString().startsWith(QStringLiteral("OIDN")),"Final Auto did not select OIDN");
                require(report(raw).value(QStringLiteral("pid"))==report(automatic).value(QStringLiteral("pid")) && report(automatic).value(QStringLiteral("sceneOpens")).toInt()==1,"Batch denoiser edits recreated the worker/stage");
                const auto reference=bridge.render({},camera,settings,{128,4,HdNvpDenoiser::None},{},{source});save(reference,QStringLiteral("reference-128"));
                auto mse=[&](const HdNvpFrame& frame){double sum=0;for(size_t i=0;i<frame.color.size();++i)if(i%4!=3){const double d=frame.color[i]-reference.color[i];sum+=d*d;}return sum/(size_t(settings.width)*settings.height*3);};
                const auto dlss=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::Dlss,true},{},{source});save(dlss,QStringLiteral("dlss"));
                const auto dlssStats=report(dlss).value(QStringLiteral("stats")).toObject();
                require(dlssStats.value(QStringLiteral("Denoiser")).toString().startsWith(QStringLiteral("DLSS")) && dlssStats.value(QStringLiteral("DLSS RR Frames")).toDouble()>0,"DLSS RR did not execute on the test GPU");
                const auto preview=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::Auto,true},{},{source});
                require(report(preview).value(QStringLiteral("stats")).toObject().value(QStringLiteral("Denoiser")).toString().startsWith(QStringLiteral("DLSS")),"Preview Auto did not select DLSS RR on the test GPU");
                camera.transform.m[12]+=2;const auto moved=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::Auto,true},{},{source});save(moved,QStringLiteral("dlss-camera-moved"));
                require(moved.color!=preview.color,"DLSS camera edit returned stale history");
                require(report(dlss).value(QStringLiteral("pid"))==report(moved).value(QStringLiteral("pid")) && report(dlss).value(QStringLiteral("sceneOpens"))==report(moved).value(QStringLiteral("sceneOpens")),"DLSS camera edits recreated the worker/stage");
                QJsonObject result{{QStringLiteral("scene"),QString::fromLocal8Bit(argv[2])},{QStringLiteral("off"),report(raw)},{QStringLiteral("oidn"),report(oidn)},{QStringLiteral("dlss"),report(dlss)},{QStringLiteral("dlssMoved"),report(moved)},
                    {QStringLiteral("offMseVs128"),mse(raw)},{QStringLiteral("oidnMseVs128"),mse(oidn)},{QStringLiteral("dlssMseVs128"),mse(dlss)}};
                QFile json(QDir(output).filePath(QStringLiteral("result.json")));require(json.open(QIODevice::WriteOnly),"Cannot save denoiser report");json.write(QJsonDocument(result).toJson());
                std::cout<<"OIDN/DLSS/Auto/off/color/alpha/depth/camera denoiser checks passed\n";return 0;
            }
            auto first=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::None},{},{source});const auto firstMs=total.elapsed()-loadMs;save(first,QStringLiteral("camera-original"));
            camera.transform.m[12]+=2;total.restart();auto moved=bridge.render({},camera,settings,{8,4,HdNvpDenoiser::None},{},{source});const auto movedMs=total.elapsed();save(moved,QStringLiteral("camera-moved"));
            require(first.color!=moved.color,"Scene camera edit had no effect");
            auto a=QJsonDocument::fromJson(QByteArray::fromStdString(first.report)).object(),b=QJsonDocument::fromJson(QByteArray::fromStdString(moved.report)).object();
            require(a.value(QStringLiteral("pid"))==b.value(QStringLiteral("pid")) && b.value(QStringLiteral("sceneOpens")).toInt()==1,"User scene was reopened on camera edit");
            QJsonObject report{{QStringLiteral("scene"),QString::fromLocal8Bit(argv[2])},{QStringLiteral("camera"),QString::fromStdString(cameraPath)},{QStringLiteral("metadataLoadMs"),loadMs},{QStringLiteral("firstTotalMs"),firstMs},{QStringLiteral("cameraUpdateTotalMs"),movedMs},{QStringLiteral("first"),a},{QStringLiteral("moved"),b}};
            QFile json(QDir(output).filePath(QStringLiteral("result.json")));require(json.open(QIODevice::WriteOnly),"Cannot save scene report");json.write(QJsonDocument(report).toJson());
            std::cout<<"Composed USD scene rendered; metadata="<<loadMs<<" ms, first="<<firstMs<<" ms, camera="<<movedMs<<" ms\n";return 0;
        }
        UsdSceneBridge bridge;Card card;card.material.baseColor={.25f,.5f,2.f,1.f};
        Camera camera;camera.orthographic=true;camera.orthoWidth=4;camera.farClip=100;
        Settings settings;settings.width=64;settings.height=48;settings.samples=1;
        const auto path=bridge.exportScene({card},camera,settings);const auto stage=UsdStage::Open(path);
        require(bool(stage),"Exported USD did not open");
        const auto mesh=stage->GetPrimAtPath(SdfPath("/Natron/Geometry/Object0"));
        require(mesh.HasAPI(TfToken("NatronCompositingAPI")),"Natron API schema was not registered/applied");
        bool unlit=false;require(mesh.GetAttribute(TfToken("natron:unlit")).Get(&unlit) && unlit,"Unlit semantic was not retained");
        const auto usdCamera=UsdGeomCamera(stage->GetPrimAtPath(SdfPath("/Natron/Camera"))).GetCamera(UsdTimeCode::Default());
        require(std::abs(usdCamera.GetHorizontalAperture()*.1-camera.orthoWidth)<1e-6,"Orthographic camera units changed");
        if(argc<2 || std::string(argv[1])!="--hdnvp"){std::cout<<"USD export/schema/camera passed\n";return 0;}
        require(UsdSceneBridge::available(),"Build the hdNVP host first");
        auto first=bridge.render({card},camera,settings,{1,0,HdNvpDenoiser::None});const size_t center=(size_t(24)*64+32);
        for(int c=0;c<4;++c)require(std::abs(first.color[center*4+c]-card.material.baseColor[c])<.001,"Unlit HDR card color differed");
        require(std::abs(first.depth[center]-5)<.001,"Depth is not camera-space -Z");
        require(first.depth[0]==0 && first.color[3]==0,"Background is not transparent/zero-depth");
        auto a=QJsonDocument::fromJson(QByteArray::fromStdString(first.report)).object();
        camera.transform=Matrix::transform({1,0,5},{});auto moved=bridge.render({card},camera,settings,{1,0,HdNvpDenoiser::None});
        auto b=QJsonDocument::fromJson(QByteArray::fromStdString(moved.report)).object();
        require(a.value(QStringLiteral("pid"))==b.value(QStringLiteral("pid")) && b.value(QStringLiteral("sceneOpens")).toInt()==1,"Camera updates recreated the worker or USD stage");
        require(first.color!=moved.color,"Camera edit did not invalidate the render");
        card.material.baseColor={.5f,.25f,.1f,1.f};auto material=bridge.render({card},camera,settings,{2,0,HdNvpDenoiser::None});
        require(material.color!=moved.color,"Material edit did not invalidate the render");
        camera.transform=Matrix::transform({0,0,5},{});auto image=std::make_shared<Texture>();image->width=image->height=2;
        image->pixels={2,0,0,1, 0,.5f,0,1, 0,0,.25f,1, 1,1,1,1};card.texture=image;card.material.baseColor={1,1,1,1};
        auto textured=bridge.render({card},camera,settings,{4,0,HdNvpDenoiser::None});
        // Bilinear filtering still leaves each quadrant dominated by its source channel.
        auto at=[&](int x,int y,int c){return textured.color[(size_t(y)*64+x)*4+c];};
        require(at(22,14,0)>at(22,14,1) && at(22,14,0)>at(22,14,2),"Bottom-left image orientation changed");
        require(at(42,34,0)>.1 && at(42,34,1)>.1 && at(42,34,2)>.1,"Top-right image orientation changed");
        auto empty=bridge.render({},camera,settings,{1,0,HdNvpDenoiser::None});require(empty.depth[center]==0 && empty.color[center*4+3]==0,"Deleted USD geometry remained visible");
        std::exception_ptr threadError;
        std::thread anotherRenderThread([&]{try{empty=bridge.render({},camera,settings,{1,0,HdNvpDenoiser::None});}catch(...){threadError=std::current_exception();}});anotherRenderThread.join();
        if(threadError)std::rethrow_exception(threadError);
        require(empty.depth[center]==0,"Cross-thread render failed");
        QTemporaryDir fixture;const auto sourceFile=fixture.filePath(QStringLiteral("centimeters.usda"));
        auto sourceStage=UsdStage::CreateNew(sourceFile.toStdString());UsdGeomSetStageUpAxis(sourceStage,UsdGeomTokens->z);UsdGeomSetStageMetersPerUnit(sourceStage,.01);sourceStage->GetRootLayer()->Save();
        UsdSceneSource source;source.filename=sourceFile.toStdString();source.metersPerUnit=.01;source.zUp=true;
        Card plain;plain.material.baseColor={.25f,.5f,2.f,1.f};
        auto units=bridge.render({plain},camera,settings,{1,0,HdNvpDenoiser::None},{},{source});
        require(std::abs(units.depth[center]-5)<.001,"Composed centimeter/Z-up stage depth changed");
        for(int c=0;c<4;++c)require(std::abs(units.color[center*4+c]-plain.material.baseColor[c])<.001,"Composed centimeter/Z-up stage color changed");
        auto original=UsdGeomMesh::Define(sourceStage,SdfPath("/Original"));
        original.CreatePointsAttr().Set(VtVec3fArray{{-100,0,-100},{-100,0,100},{100,0,100},{100,0,-100}});
        original.CreateFaceVertexCountsAttr().Set(VtIntArray{4});original.CreateFaceVertexIndicesAttr().Set(VtIntArray{0,1,2,3});
        original.CreateSubdivisionSchemeAttr().Set(UsdGeomTokens->none);original.CreateDoubleSidedAttr().Set(true);sourceStage->GetRootLayer()->Save();
        source.revision=1;auto composed=bridge.render({},camera,settings,{1,0,HdNvpDenoiser::None},{},{source});
        require(std::abs(composed.depth[center]-5)<.001,"Original composed USD geometry was not loaded");
        original.AddTranslateOp().Set(GfVec3d(0,100,0));sourceStage->GetRootLayer()->Save();source.revision=2;
        auto reloaded=bridge.render({},camera,settings,{1,0,HdNvpDenoiser::None},{},{source});
        // Z-up source +Y becomes native -Z, one meter farther from the camera.
        require(std::abs(reloaded.depth[center]-6)<.001,"Referenced USD layer edits were not reloaded");
        Card pbr;pbr.material.lit=true;pbr.material.baseColor={.3f,.5f,.8f,1};pbr.material.roughness=.2;
        auto litSettings=settings;litSettings.lights.emplace_back();
        auto smooth=bridge.render({pbr},camera,litSettings,{4,2,HdNvpDenoiser::None});
        auto data=std::make_shared<Texture>();data->width=data->height=1;data->dataSampling=true;data->pixels={1,1,1,0};
        pbr.maps[1].texture=data;pbr.maps[1].useMeshUv=true;
        auto rough=bridge.render({pbr},camera,litSettings,{4,2,HdNvpDenoiser::None});
        require(std::abs(smooth.color[center*4]-rough.color[center*4])>.01,"Roughness RGB with zero alpha was lost");
        pbr.maps[2].texture=data;pbr.maps[2].useMeshUv=true;
        auto metal=bridge.render({pbr},camera,litSettings,{4,2,HdNvpDenoiser::None});
        require(std::abs(metal.color[center*4]-rough.color[center*4])>.01,"Metallic map did not affect shading");
        auto normal=std::make_shared<Texture>(*data);normal->pixels={-.7f,0,.7141428f,0};pbr.maps[0].texture=normal;pbr.maps[0].useMeshUv=true;
        auto tilted=bridge.render({pbr},camera,litSettings,{4,2,HdNvpDenoiser::None});
        require(std::abs(tilted.color[center*4]-metal.color[center*4])>.01,"Signed normal RGB with zero alpha was lost");
        auto geometry=std::make_shared<Mesh>();const std::array<Vec3,4> corners{{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}}};
        for(int face=0;face<2;++face){MeshTriangle triangle;const int indices[2][3]{{0,1,2},{0,2,3}};
            for(int v=0;v<3;++v){triangle.vertices[v].position=corners[indices[face][v]];triangle.vertices[v].normal={0,0,1};}
            triangle.color=face==0?std::array<float,4>{1,0,0,1}:std::array<float,4>{0,1,0,1};geometry->triangles.push_back(triangle);}
        Card grouped;grouped.mesh=geometry;auto faces=bridge.render({grouped},camera,settings,{4,0,HdNvpDenoiser::None});
        require(faces.color[(size_t(14)*64+42)*4]> .9 && faces.color[(size_t(34)*64+22)*4+1]>.9,"Native mesh face materials were not retained");
        std::cout<<"hdNVP HDR/color/depth/maps/camera/material/subsets/removal/thread/units/reload passed\n"<<first.report<<"\n"<<material.report<<"\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
