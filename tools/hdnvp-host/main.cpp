// SPDX-License-Identifier: GPL-2.0-or-later
// A persistent, independent OpenUSD/Hydra process. No Natron or Houdini ABI crosses IPC.
#include <pxr/pxr.h>
#include <pxr/base/arch/env.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/camera.h>
#include <pxr/base/js/json.h>
#include <pxr/base/plug/registry.h>
#include <pxr/base/tf/errorMark.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/camera.h>
#include <pxr/imaging/hd/changeTracker.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>
#include <pxr/usdImaging/usdImaging/stageSceneIndex.h>
#include <array>
#include <chrono>
#include <cmath>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <windows.h>
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
std::string string(const JsObject& o,const char* k) {
    const auto i=o.find(k);if(i==o.end() || !i->second.IsString())throw std::runtime_error(std::string("Missing string: ")+k);
    return i->second.GetString();
}
double number(const JsObject& o,const char* k) {
    const auto i=o.find(k);if(i==o.end() || (!i->second.IsReal() && !i->second.IsInt()))throw std::runtime_error(std::string("Missing number: ")+k);
    const double v=i->second.IsReal()?i->second.GetReal():double(i->second.GetInt64());if(!std::isfinite(v))throw std::runtime_error("Non-finite request");return v;
}
int integer(const JsObject& o,const char* k,int minimum,int maximum) {
    const auto value=number(o,k);if(value<minimum || value>maximum || std::floor(value)!=value)throw std::runtime_error(std::string("Invalid integer: ")+k);
    return int(value);
}
class Task final : public HdTask {
public:
    Task(HdRenderPassSharedPtr p,HdRenderPassStateSharedPtr s):HdTask(SdfPath()),pass(std::move(p)),state(std::move(s)){}
    void Sync(HdSceneDelegate*,HdTaskContext*,HdDirtyBits* dirty) override {pass->Sync();*dirty=HdChangeTracker::Clean;}
    void Prepare(HdTaskContext*,HdRenderIndex* index) override {
        const auto* camera=dynamic_cast<HdCamera*>(index->GetSprim(HdPrimTypeTokens->camera,SdfPath("/Natron/Camera")));
        if(!camera)throw std::runtime_error("Hydra camera did not populate");state->SetCamera(camera);
    }
    void Execute(HdTaskContext*) override {pass->Execute(state,tags);}
    const TfTokenVector& GetRenderTags() const override {return tags;}
    bool IsConverged() const override {return pass->IsConverged();}
    HdRenderPassSharedPtr pass;HdRenderPassStateSharedPtr state;
    const TfTokenVector tags{HdRenderTagTokens->geometry,HdRenderTagTokens->render};
};
class Host {
public:
    explicit Host(const std::filesystem::path& runtime) {
        const auto root=runtime.generic_string();
        ArchSetEnv("PXR_PLUGINPATH_NAME",root+"/usd;"+root+"/resources",true);
        ArchSetEnv("HD_ENABLE_SCENE_INDEX_EMULATION","1",true);
        PlugRegistry::GetInstance().RegisterPlugins(root+"/usd/plugInfo.json");
        PlugRegistry::GetInstance().RegisterPlugins(root+"/resources/plugInfo.json");
    }
    ~Host(){clearScene();}
    JsObject render(const JsObject& request) {
        const auto started=std::chrono::steady_clock::now();TfErrorMark errors;
        const auto scene=string(request,"scene"),directory=string(request,"output");
        const auto revision=request.count("sourceRevision")?string(request,"sourceRevision"):std::string();
        const double time=number(request,"time");
        const bool payloads=!request.count("loadPayloads") || request.at("loadPayloads").GetBool();
        const auto denoiser=request.count("denoiser")?string(request,"denoiser"):std::string("auto");
        if(denoiser!="auto" && denoiser!="oidn" && denoiser!="dlss" && denoiser!="none")throw std::runtime_error("Invalid denoiser");
        const bool interactive=request.count("interactive") && request.at("interactive").GetBool();
        // hdNVP caches renderMode in its constructor. DLSS RR requires a
        // viewport delegate; its batch Auto deliberately means no denoising.
        const bool batch=!interactive && denoiser!="dlss";
        const auto rendererDenoiser=denoiser=="auto" && batch?std::string("oidn"):denoiser;
        const int w=integer(request,"width",1,16384),h=integer(request,"height",1,16384),samples=integer(request,"samples",1,1048576),bounces=integer(request,"bounces",0,64);
        if(denoiser=="dlss" && (w<32 || h<32))throw std::runtime_error("DLSS Ray Reconstruction requires at least 32 x 32 pixels. Select Auto or OIDN.");
        if(delegate && batch!=batchMode) {clearScene();delegate=HdPluginRenderDelegateUniqueHandle();}
        if(!delegate) {
            HdRenderSettingsMap settings;
            settings[TfToken("renderMode")]=VtValue(TfToken(batch?"batch":"interactive"));
            settings[TfToken("hdrtpt:denoiser")]=VtValue(rendererDenoiser);
            settings[TfToken("hdrtpt:fallbackDomeLight")]=VtValue(false);
            settings[TfToken("hdrtpt:maxSamples")]=VtValue(samples);
            settings[TfToken("hdrtpt:samplesPerFrame")]=VtValue(1);
            settings[TfToken("hdrtpt:maxBounces")]=VtValue(bounces);
            settings[TfToken("houdini:frame")]=VtValue(time);
            delegate=HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(TfToken("HdDittoRendererPlugin"),settings);
            if(!delegate)throw std::runtime_error("Cannot initialize hdNVP/Ditto renderer");
            batchMode=batch;
        }
        delegate->SetRenderSetting(TfToken("hdrtpt:denoiser"),VtValue(rendererDenoiser));
        delegate->SetRenderSetting(TfToken("hdrtpt:maxSamples"),VtValue(samples));
        delegate->SetRenderSetting(TfToken("hdrtpt:samplesPerFrame"),VtValue(1));
        delegate->SetRenderSetting(TfToken("hdrtpt:maxBounces"),VtValue(bounces));
        delegate->SetRenderSetting(TfToken("houdini:frame"),VtValue(time));
        if(scene!=scenePath || payloads!=loadPayloads) {
            clearScene();stage=UsdStage::Open(scene,payloads?UsdStage::LoadAll:UsdStage::LoadNone);loadPayloads=payloads;if(!stage)throw std::runtime_error("Cannot open scene");
            if(!UsdGeomCamera(stage->GetPrimAtPath(SdfPath("/Natron/Camera"))))throw std::runtime_error("Missing Natron camera");
            index.reset(HdRenderIndex::New(delegate.Get(),HdDriverVector()));
            UsdImagingCreateSceneIndicesInfo info;info.stage=stage;indices=UsdImagingCreateSceneIndices(info);
            index->InsertSceneIndex(indices.finalSceneIndex,SdfPath::AbsoluteRootPath());scenePath=scene;++sceneOpens;
        } else {
            // Sdf reload issues changes for fields whose values changed, retaining the stage and render index.
            stage->GetRootLayer()->Reload(true);
            if(revision!=sourceRevision) {
                for(const auto& layer:stage->GetUsedLayers())if(!layer->IsAnonymous() && layer!=stage->GetRootLayer())layer->Reload(true);
                stage->Reload();
            }
        }
        sourceRevision=revision;
        indices.stageSceneIndex->SetTime(UsdTimeCode(time));
        indices.stageSceneIndex->ApplyPendingUpdates();
        if(width!=w || height!=h || !task) {
            task.reset();destroyBuffers();HdRenderPassAovBindingVector bindings;
            const std::array<TfToken,4> names{HdAovTokens->color,HdAovTokens->depth,HdAovTokens->primId,HdAovTokens->instanceId};
            const std::array<HdFormat,4> formats{HdFormatFloat32Vec4,HdFormatFloat32,HdFormatInt32,HdFormatInt32};
            for(size_t i=0;i<buffers.size();++i) {
                buffers[i]=dynamic_cast<HdRenderBuffer*>(delegate->CreateBprim(HdPrimTypeTokens->renderBuffer,SdfPath("/__natronAov/"+names[i].GetString())));
                if(!buffers[i] || !buffers[i]->Allocate(GfVec3i(w,h,1),formats[i],false))throw std::runtime_error("Cannot allocate Hydra AOV");
                HdRenderPassAovBinding binding;binding.aovName=names[i];binding.renderBuffer=buffers[i];binding.renderBufferId=buffers[i]->GetId();bindings.push_back(binding);
            }
            auto state=std::make_shared<HdRenderPassState>();state->SetAovBindings(bindings);
            state->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0),w,h)));
            auto pass=delegate->CreateRenderPass(index.get(),HdRprimCollection(HdTokens->geometry,HdReprSelector(HdReprTokens->smoothHull),SdfPath::AbsoluteRootPath()));
            task=std::make_shared<Task>(pass,state);width=w;height=h;
        }
        HdTaskSharedPtrVector tasks{task};HdEngine engine;
        auto lastStatus=started;
        for(;;) {
            engine.Execute(index.get(),&tasks);
            if(!errors.IsClean()) {std::string text;for(auto it=errors.GetBegin();it!=errors.GetEnd();++it)text+=it->GetCommentary()+"\n";errors.Clear();throw std::runtime_error(text);}
            if(task->pass->IsConverged())break;
            if(std::chrono::steady_clock::now()-lastStatus>std::chrono::seconds(5)) {
                lastStatus=std::chrono::steady_clock::now();std::cerr<<"hdNVP progress:";
                for(const auto& [name,value]:delegate->GetRenderStats())if(name=="Samples" || name=="Renderer Stage" || name=="rendererStage" || name=="percentDone")std::cerr<<" "<<name<<"="<<value;
                std::cerr<<std::endl;
            }
            if(std::chrono::steady_clock::now()-started>std::chrono::minutes(5))throw std::runtime_error("hdNVP render timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::filesystem::create_directories(std::filesystem::u8path(directory));
        const std::array<const char*,4> files{"color.raw","depth.raw","primId.raw","instanceId.raw"};
        for(size_t i=0;i<buffers.size();++i) {
            void* data=buffers[i]->Map();if(!data)throw std::runtime_error("Cannot map completed AOV");
            const size_t count=size_t(w)*h*(i==0?4:1);
            if(i<2)for(size_t j=0;j<count;++j)if(!std::isfinite(static_cast<float*>(data)[j])) {buffers[i]->Unmap();throw std::runtime_error("Non-finite hdNVP AOV");}
            std::ofstream out(std::filesystem::u8path(directory)/files[i],std::ios::binary|std::ios::trunc);
            out.write(static_cast<const char*>(data),std::streamsize(count*4));buffers[i]->Unmap();out.close();
            if(!out)throw std::runtime_error("Cannot write AOV");
        }
        JsObject stats;for(const auto& entry:delegate->GetRenderStats()) {
            if(entry.second.IsHolding<double>())stats[entry.first]=JsValue(entry.second.UncheckedGet<double>());
            else if(entry.second.IsHolding<int64_t>())stats[entry.first]=JsValue(entry.second.UncheckedGet<int64_t>());
            else if(entry.second.IsHolding<std::string>())stats[entry.first]=JsValue(entry.second.UncheckedGet<std::string>());
        }
        // Do not silently return noisy color when a requested denoiser failed
        // to initialize/evaluate. Auto's unsupported-DLSS path chooses OIDN.
        if(denoiser!="none") {
            const auto active=stats.count("Denoiser")?stats.at("Denoiser").GetString():std::string("None");
            if(active=="None" || (active.starts_with("OIDN") && !stats.count("Denoise (ms)")))
                throw std::runtime_error("The requested denoiser did not run. DLSS requires a supported NVIDIA GPU; OIDN requires its runtime DLLs. See hdNVP diagnostics.");
        }
        return {{"ok",JsValue(true)},{"pid",JsValue(int64_t(GetCurrentProcessId()))},{"sceneOpens",JsValue(sceneOpens)},
            {"requests",JsValue(++requests)},{"denoiser",JsValue(denoiser)},{"renderMode",JsValue(std::string(batch?"batch":"interactive"))},
            {"wallMs",JsValue(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count())},{"stats",JsValue(stats)}};
    }
private:
    // Destroy tasks/buffers/index before their delegate and its GPU resources.
    void destroyBuffers(){for(auto& buffer:buffers)if(buffer){delegate->DestroyBprim(buffer);buffer=nullptr;}}
    void clearScene(){task.reset();destroyBuffers();index.reset();indices={};stage.Reset();scenePath.clear();width=height=0;}
    HdPluginRenderDelegateUniqueHandle delegate;
    UsdStageRefPtr stage;UsdImagingSceneIndices indices;std::unique_ptr<HdRenderIndex> index;
    std::array<HdRenderBuffer*,4> buffers{};std::shared_ptr<Task> task;
    std::string scenePath,sourceRevision;bool loadPayloads=true,batchMode=true;int width=0,height=0;int64_t sceneOpens=0,requests=0;
};
}
int main(int argc,char** argv) {
    if(argc!=2){std::cerr<<"Usage: NatronHdNvpHost <runtime-directory>\n";return 2;}
    Host host(std::filesystem::u8path(argv[1]));std::string line;
    while(std::getline(std::cin,line)) {
        JsObject response;
        try {JsParseError parse;const auto request=JsParseString(line,&parse);if(!request.IsObject())throw std::runtime_error("Invalid JSON request");
            if(request.GetJsObject().count("quit"))break;response=host.render(request.GetJsObject());
        } catch(const std::exception& error){response={{"ok",JsValue(false)},{"error",JsValue(std::string(error.what()))}};}
        std::cout<<"NATRON_HDNVP ";
        {JsWriter writer(std::cout,JsWriter::Style::Compact);JsWriteValue(&writer,JsValue(response));}
        std::cout<<std::endl;
    }
}
