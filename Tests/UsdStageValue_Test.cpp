// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdSceneBridge.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usd/editContext.h>
#include <iostream>
#include <stdexcept>
using namespace Natron::Gpu::Cards;
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
void require(bool v,const char* message){if(!v)throw std::runtime_error(message);}
template<class F> void fails(F f){try{f();}catch(const std::exception&){return;}throw std::runtime_error("Invalid composition accepted");}
void write(const QString& name,const QByteArray& text){QFile f(name);require(f.open(QIODevice::WriteOnly),"Cannot write test fixture");f.write(text);}
QByteArray read(const QString& name){QFile f(name);require(f.open(QIODevice::ReadOnly),"Cannot read fixture");return f.readAll();}
const std::string native=R"USD(#usda 1.0
(
    metersPerUnit = 1
    upAxis = "Y"
    timeCodesPerSecond = 24
)
def Xform "Natron" {}
)USD";
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir directory;require(directory.isValid(),"No temporary directory");
        const auto file=directory.filePath(QStringLiteral("asset.usda"));
        write(file,R"USD(#usda 1.0
(
    defaultPrim = "World"
    metersPerUnit = 1
    upAxis = "Y"
    timeCodesPerSecond = 24
)
def Xform "World" {
    double3 xformOp:translate = (1, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
    def Material "Paint" {}
    def Mesh "Mesh" (
        prepend apiSchemas = ["MaterialBindingAPI"]
        customData = { dictionary natron = { string owner = "previousProject" } }
    ) {
        uniform token subdivisionScheme = "none"
        point3f[] points = [(-1,-1,0),(1,-1,0),(1,1,0),(-1,1,0)]
        int[] faceVertexCounts = [4]
        int[] faceVertexIndices = [0,1,2,3]
        rel material:binding = </World/Paint>
        float test:sample.timeSamples = {1: 10, 2: 20}
    }
    def Xform "Off" (active = false) {}
    def Xform "Instance" (instanceable = true; prepend references = </World/Mesh>) {}
}
)USD");
        const auto original=read(file);
        UsdSceneSource a;a.filename=file.toStdString();a.ownerId="readerA";a.label="Asset A";a.mountPath="/Natron/Assets/a";a.timeCode=1;
        UsdSceneSource b=a;b.ownerId="readerB";b.label="Asset B";b.mountPath="/Natron/Assets/b";b.transform=Matrix::transform({5,0,0},{});b.timeCode=2;
        auto value=UsdSceneValue::compose(native,{a,b},42);
        auto page=value->children("/Natron/Assets");require(page.prims.size()==2,"Namespaced assets collided");
        auto mesh=value->inspect("/Natron/Assets/a/Source/Mesh");
        require(mesh.ownerId=="readerA" && mesh.material=="/Natron/Assets/a/Source/Paint","Reference binding/ownership lost");
        require(mesh.hasBounds && std::abs(mesh.minimum.x)<1e-6 && std::abs(mesh.maximum.x-2)<1e-6,"Source root transform was overwritten");
        auto moved=value->inspect("/Natron/Assets/b/Source/Mesh");require(moved.hasBounds && std::abs(moved.minimum.x-5)<1e-6,"Per-source transform failed");
        auto sample=[](const UsdPrimInfo& info){for(const auto& p:info.properties)if(p.first=="test:sample")return p.second;return std::string();};
        require(sample(mesh)=="10" && sample(moved)=="20","Independent source sampling failed");
        require(!value->inspect("/Natron/Assets/a/Source/Off").active,"Inactive prim omitted");
        auto proxy=value->children("/Natron/Assets/a/Source/Instance"); // leaf mesh prototype, no invented children
        require(value->inspect("/Natron/Assets/a/Source/Instance").instance && proxy.prims.empty(),"Instance representation failed");
        b.transform=Matrix::transform({9,0,0},{});auto other=UsdSceneValue::compose(native,{b},43);
        require(std::abs(value->inspect("/Natron/Assets/b/Source/Mesh").minimum.x-5)<1e-6,"Sibling composition changed a published stage");
        require(std::abs(other->inspect("/Natron/Assets/b/Source/Mesh").minimum.x-9)<1e-6,"Sibling opinion missing");
        require(read(file)==original,"Composition edited the imported file");
        auto shot=a;shot.mountPath.clear();auto complete=UsdSceneValue::compose(native,{shot},44);
        UsdPrimEdit delta;delta.identity="editA";delta.paths={"/World/Mesh"};delta.transform=Matrix::transform({3,2,0},{});
        auto edited=UsdSceneValue::compose(native,{shot},45,{delta});const auto shifted=edited->inspect("/World/Mesh");
        require(std::abs(shifted.minimum.x-3)<1e-6 && std::abs(shifted.minimum.y-1)<1e-6,"World-space USD transform delta failed");
        require(std::abs(complete->inspect("/World/Mesh").minimum.x)<1e-6 && read(file)==original,"Edit changed a sibling stage or the source file");
        delta.paths={"/World/Mesh","/World"};auto nested=UsdSceneValue::compose(native,{shot},45,{delta});
        require(std::abs(nested->inspect("/World/Mesh").minimum.x-3)<1e-6,"Parent/child multiselection applied the transform twice");
        delta.paths={"/World/Mesh"};
        auto sharedEdit=UsdSceneValue::compose(native,{shot},45,{delta,delta});
        require(std::abs(sharedEdit->inspect("/World/Mesh").minimum.x-3)<1e-6,"Shared upstream USD edit applied more than once");
        UsdPrimEdit hide;hide.kind=UsdPrimEdit::Kind::Visibility;hide.paths={"/World/Mesh"};hide.visible=false;
        require(UsdSceneValue::compose(native,{shot},46,{hide})->inspect("/World/Mesh").visibility=="invisible","USD visibility opinion failed");
        UsdPrimEdit bind;bind.kind=UsdPrimEdit::Kind::Material;bind.paths={"/World/Mesh"};bind.materialPath="/World/Paint";
        require(UsdSceneValue::compose(native,{shot},47,{bind})->inspect("/World/Mesh").material=="/World/Paint","USD binding opinion failed");
        auto bad=delta;bad.paths={"/Missing"};fails([&]{UsdSceneValue::compose(native,{shot},48,{bad});});
        require(complete->inspect("/World/Mesh").ownerId=="readerA","Complete-shot paths/ownership changed");
        require(complete->revision()==44 && complete->timeCode()==1,"Revision/time contract failed");
        const auto saved=directory.filePath(QStringLiteral("assembly.usda"));value->saveAssembly(saved.toStdString());auto roundtrip=UsdStage::Open(saved.toStdString());
        require(bool(roundtrip->GetPrimAtPath(SdfPath("/Natron/Assets/a/Source/Mesh"))),"Assembly round-trip lost references");
        require(read(saved).contains("references") && !read(saved).contains("point3f[] points"),"Assembly flattened source geometry");
        auto collision=a;collision.ownerId="collision";fails([&]{UsdSceneValue::compose(native,{a,collision},45);});
        auto outside=a;outside.primPath="/World/Mesh";fails([&]{UsdSceneValue::compose(native,{outside},45);});
        require(UsdSceneValue::compose(native,{a,a},45)->children("/Natron/Assets").prims.size()==1,"Shared upstream source duplicated");
        fails([&]{value->children("relative/path");});fails([&]{value->inspect("/Missing");});
        const auto payload=directory.filePath(QStringLiteral("payload.usda"));write(payload,R"USD(#usda 1.0
(defaultPrim = "Payload")
def Xform "Payload" (prepend payload = @asset.usda@</World>) {}
)USD");
        auto p=a;p.filename=payload.toStdString();p.loadPayloads=false;
        auto unloaded=UsdSceneValue::compose(native,{p},46);auto item=unloaded->inspect("/Natron/Assets/a/Source");require(item.hasPayload && !item.loaded && !item.hasChildren,"Outline loaded a payload implicitly");
        p.loadPayloads=true;auto loaded=UsdSceneValue::compose(native,{p},47);require(loaded->inspect("/Natron/Assets/a/Source/Mesh").hasBounds,"Explicit payload policy not applied");
        const auto variantFile=directory.filePath(QStringLiteral("variants.usda"));
        auto variants=UsdStage::CreateInMemory();UsdGeomSetStageMetersPerUnit(variants,.01);UsdGeomSetStageUpAxis(variants,UsdGeomTokens->z);variants->SetTimeCodesPerSecond(48);
        auto variantRoot=UsdGeomXform::Define(variants,SdfPath("/VariantRoot"));variants->SetDefaultPrim(variantRoot.GetPrim());
        auto choices=variantRoot.GetPrim().GetVariantSet("shape");
        for(const auto& choice:{"base","moved","resetChild"}) {
            choices.AddVariant(choice);choices.SetVariantSelection(choice);
            UsdEditContext edit(choices.GetVariantEditContext());
            variantRoot.AddTranslateOp().Set(GfVec3d(0,0,std::string(choice)=="base"?100:200));variantRoot.SetResetXformStack(true);
            auto cube=UsdGeomCube::Define(variants,SdfPath("/VariantRoot/Cube"));cube.CreateSizeAttr().Set(2.);
            if(std::string(choice)=="resetChild")cube.SetResetXformStack(true);
        }
        choices.SetVariantSelection("base");require(variants->GetRootLayer()->Export(variantFile.toStdString()),"Cannot save variant fixture");const auto variantOriginal=read(variantFile);
        auto variantSource=a;variantSource.filename=variantFile.toStdString();variantSource.metersPerUnit=.01;variantSource.zUp=true;variantSource.timeCodesPerSecond=48;variantSource.transform=Matrix::transform({5,0,0},{});variantSource.variants="/VariantRoot shape=moved";
        auto baseSource=variantSource;baseSource.mountPath="/Natron/Assets/base";baseSource.variants="/VariantRoot shape=base";
        auto variantValue=UsdSceneValue::compose(native,{variantSource,baseSource},48);
        const auto variantCube=variantValue->inspect("/Natron/Assets/a/Source/Cube"),baseCube=variantValue->inspect("/Natron/Assets/base/Source/Cube");
        require(variantCube.hasBounds && std::abs(variantCube.minimum.x-4.99)<1e-6 && std::abs(variantCube.minimum.y-1.99)<1e-6,"Variant/root reset or asset coordinate conversion failed");
        require(std::abs(baseCube.minimum.y-.99)<1e-6 && read(variantFile)==variantOriginal,"Independent variants modified a shared source");
        variantSource.variants="/VariantRoot shape=resetChild";fails([&]{UsdSceneValue::compose(native,{variantSource},49);});
        variantSource.variants="/VariantRoot shape=moved";
        auto unitShot=baseSource;unitShot.mountPath.clear();unitShot.transform=Matrix();
        UsdSceneBridge unitBridge;Camera unitCamera;Settings unitSettings;unitSettings.width=64;unitSettings.height=64;
        unitBridge.exportScene({},unitCamera,unitSettings,{unitShot,variantSource});
        require(std::abs(unitBridge.sceneValue()->inspect("/Natron/Assets/a/Source/Cube").minimum.y-1.99)<1e-6,"Mixed shot/asset basis applied twice");
        auto invalid=a;invalid.metersPerUnit=0;fails([&]{UsdSceneValue::compose(native,{invalid},49);});
        auto stage=UsdStage::CreateInMemory();UsdGeomXform::Define(stage,SdfPath("/Natron"));for(int i=0;i<300;++i)stage->DefinePrim(SdfPath("/Many/P"+std::to_string(i)));
        std::string text;stage->GetRootLayer()->ExportToString(&text);auto many=UsdSceneValue::compose(text,{},48);
        const auto first=many->children("/Many");const auto rest=many->children("/Many",256);require(first.prims.size()==256 && first.more && rest.prims.size()==44 && !rest.more,"Bounded child pagination failed");
        UsdSceneBridge bridge;Card card;card.ownerId="stableCard";card.label="Original";Camera camera;Settings settings;settings.width=64;settings.height=64;
        bridge.exportScene({card},camera,settings);auto old=bridge.sceneValue();card.label="Renamed";bridge.exportScene({card},camera,settings);
        require(old->inspect("/Natron/Geometry/n_stableCard").displayName=="Original" && bridge.sceneValue()->inspect("/Natron/Geometry/n_stableCard").displayName=="Renamed","Native identity/publication failed");
        auto inspectionSettings=settings;inspectionSettings.stageCamera=std::make_shared<Camera>(camera);
        bridge.exportScene({card},camera,inspectionSettings);auto publicScene=bridge.sceneValue();
        auto orbit=camera;orbit.transform.m[12]+=1;bridge.exportScene({card},orbit,inspectionSettings);
        require(bridge.sceneValue()==publicScene && publicScene->camera("/Natron/Camera").transform.m[12]==0,"Inspection movement recomposed or changed the public stage");
        for(const auto& child:publicScene->children("/Natron").prims)require(child.path!="/Natron/InspectionCamera","Inspection camera leaked into public stage");
        std::cout<<"USD composition/Outline queries passed: references, bindings, transforms, time, branch isolation, read-only sources, payloads, pagination, stable identity and assembly round-trip.\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
