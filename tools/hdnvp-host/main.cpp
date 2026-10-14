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
#include <pxr/imaging/hd/sceneDelegate.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/instancedBySchema.h>
#include <pxr/imaging/hd/instancerTopologySchema.h>
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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#include <windows.h>
#include <dbghelp.h>
#include "../../Gpu/HdNvpFrameProtocol.h"
PXR_NAMESPACE_USING_DIRECTIVE
namespace {
LONG CALLBACK traceException(EXCEPTION_POINTERS* exception) {
    const auto code=exception->ExceptionRecord->ExceptionCode;
    if(code!=EXCEPTION_BREAKPOINT && code!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
    // Optional diagnostics for the isolated worker; never consume the exception.
    const auto process=GetCurrentProcess();SymSetOptions(SYMOPT_LOAD_LINES|SYMOPT_UNDNAME|SYMOPT_DEFERRED_LOADS);
    if(!SymInitialize(process,nullptr,TRUE))return EXCEPTION_CONTINUE_SEARCH;
    auto context=*exception->ContextRecord;STACKFRAME64 frame{};
    frame.AddrPC={context.Rip,0,AddrModeFlat};frame.AddrFrame={context.Rbp,0,AddrModeFlat};frame.AddrStack={context.Rsp,0,AddrModeFlat};
    fprintf(stderr,"hdNVP exception 0x%08lx\n",code);
    for(int i=0;i<40 && frame.AddrPC.Offset;++i) {
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO)+MAX_SYM_NAME]{};
        auto* symbol=reinterpret_cast<SYMBOL_INFO*>(storage);symbol->SizeOfStruct=sizeof(SYMBOL_INFO);symbol->MaxNameLen=MAX_SYM_NAME;
        DWORD64 displacement=0;IMAGEHLP_LINE64 line{};line.SizeOfStruct=sizeof(line);DWORD column=0;
        if(SymFromAddr(process,frame.AddrPC.Offset,&displacement,symbol)) {
            fprintf(stderr,"  %s +0x%llx",symbol->Name,displacement);
            if(SymGetLineFromAddr64(process,frame.AddrPC.Offset,&column,&line))fprintf(stderr," (%s:%lu)",line.FileName,line.LineNumber);
            fputc('\n',stderr);
        }else fprintf(stderr,"  0x%llx\n",frame.AddrPC.Offset);
        if(!StackWalk64(IMAGE_FILE_MACHINE_AMD64,process,GetCurrentThread(),&frame,&context,nullptr,SymFunctionTableAccess64,SymGetModuleBase64,nullptr))break;
    }
    fflush(stderr);SymCleanup(process);return EXCEPTION_CONTINUE_SEARCH;
}
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
        const auto* camera=dynamic_cast<HdCamera*>(index->GetSprim(HdPrimTypeTokens->camera,cameraPath));
        if(!camera)throw std::runtime_error("Hydra camera did not populate");state->SetCamera(camera);
    }
    void Execute(HdTaskContext*) override {pass->Execute(state,tags);}
    const TfTokenVector& GetRenderTags() const override {return tags;}
    bool IsConverged() const override {return pass->IsConverged();}
    HdRenderPassSharedPtr pass;HdRenderPassStateSharedPtr state;
    SdfPath cameraPath{"/Natron/Camera"};
    const TfTokenVector tags{HdRenderTagTokens->geometry,HdRenderTagTokens->render};
};
// hdNVP numbers flattened instances child-first, parent-second. Follow the
// scene-index topology in that same order; prototype paths are never UI IDs.
SdfPath origin(const HdSceneIndexBaseRefPtr& scene,const SdfPath& path) {
    const auto p=HdPrimOriginSchema::GetFromParent(scene->GetPrim(path).dataSource).GetOriginPath(TfToken("scenePath"));return p.IsEmpty()?path:p;
}
SdfPath instancer(const HdContainerDataSourceHandle& data) {
    const auto paths=HdInstancedBySchema::GetFromParent(data).GetPaths();if(!paths)return {};
    const auto values=paths->GetTypedValue(0);return values.empty()?SdfPath():values.front();
}
SdfPath prototypeRoot(const UsdStageRefPtr& stage,const SdfPath& path) {
    auto prim=stage->GetPrimAtPath(path);
    if(prim && prim.IsInstanceProxy()){while(prim && !prim.IsInstance())prim=prim.GetParent();return prim?prim.GetPath():SdfPath();}
    if(prim && prim.IsInstance())return prim.GetPath();
    while(prim && !prim.IsPrototype())prim=prim.GetParent();return prim?prim.GetPath():SdfPath();
}
std::vector<SdfPath> locations(const UsdStageRefPtr& stage,const HdSceneIndexBaseRefPtr& scene,const SdfPath& id,const SdfPath& prototype,int depth=0) {
    if(depth>32)return {};const auto data=scene->GetPrim(id).dataSource;auto topology=HdInstancerTopologySchema::GetFromParent(data);
    if(!topology)return {};const auto indices=topology.ComputeInstanceIndicesForProto(prototype);const auto value=topology.GetInstanceLocations();
    if(!value)return std::vector<SdfPath>(indices.size(),origin(scene,id));
    const auto paths=value->GetTypedValue(0);const auto parent=instancer(data);
    const auto parents=parent.IsEmpty()?std::vector<SdfPath>{}:locations(stage,scene,parent,id,depth+1);
    std::vector<SdfPath> result;
    for(int index:indices) {
        if(index<0 || size_t(index)>=paths.size())return {};
        const auto path=origin(scene,paths[index]);
        if(parent.IsEmpty())result.push_back(path);
        else if(!path.IsAbsolutePath())for(const auto& p:parents)result.push_back(p.AppendPath(path));
        else {const auto root=prototypeRoot(stage,path);if(root.IsEmpty())return {};for(const auto& p:parents)result.push_back(path.ReplacePrefix(root,p));}
    }
    return result;
}
SdfPath instancePath(const UsdStageRefPtr& stage,const HdSceneIndexBaseRefPtr& scene,const SdfPath& id,int index) {
    if(index<0)return origin(scene,id);const auto parent=instancer(scene->GetPrim(id).dataSource);if(parent.IsEmpty())return {};
    const auto paths=locations(stage,scene,parent,id);if(size_t(index)>=paths.size())return {};
    const auto path=origin(scene,id);if(!path.IsAbsolutePath())return paths[index].AppendPath(path);
    const auto root=prototypeRoot(stage,path);return root.IsEmpty()?paths[index]:path.ReplacePrefix(root,paths[index]);
}
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
        struct Mapping {
            HANDLE handle=nullptr;void* data=nullptr;
            ~Mapping(){if(data)UnmapViewOfFile(data);if(handle)CloseHandle(handle);}
        } mapping;
        NatronHdNvp::FrameHeader* header=nullptr;
        if(request.count("mapping")) {
            mapping.handle=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,string(request,"mapping").c_str());
            if(!mapping.handle)throw std::runtime_error("Cannot open frame mapping");
            mapping.data=MapViewOfFile(mapping.handle,FILE_MAP_ALL_ACCESS,0,0,NatronHdNvp::bytes(size_t(w)*h));
            if(!mapping.data)throw std::runtime_error("Frame mapping is too small");
            header=static_cast<NatronHdNvp::FrameHeader*>(mapping.data);
            if(header->magic!=0x4e485646 || header->version!=1 || header->width!=uint32_t(w) || header->height!=uint32_t(h))throw std::runtime_error("Invalid frame mapping header");
        }
        auto cancelled=[&]{return header && InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&header->cancelled),0,0)!=0;};
        auto publish=[&](bool partial) {
            if(!header)return;
            if(InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&header->ready),0,0)!=0)return;
            for(size_t i=0;i<(partial?2:buffers.size());++i) {
                auto data=buffers[i]->Map();if(!data)throw std::runtime_error("Cannot map frame AOV");
                if(!partial && (i<2 || i>=4))for(size_t j=0;j<size_t(w)*h*NatronHdNvp::components[i];++j)if(!std::isfinite(static_cast<float*>(data)[j])){buffers[i]->Unmap();throw std::runtime_error("Non-finite hdNVP AOV");}
                std::memcpy(static_cast<char*>(mapping.data)+NatronHdNvp::offset(i,size_t(w)*h),data,size_t(w)*h*NatronHdNvp::components[i]*4);buffers[i]->Unmap();
            }
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&header->ready),partial?1:2);
            if(partial)std::cout<<"NATRON_HDNVP_FRAME "<<header->revision<<std::endl;
        };
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
            // Force generated layers to refresh even for updates within one
            // file-timestamp tick, and batch composition notices. Keep every
            // layer alive until recomposition and its notices have completed.
            if(revision!=sourceRevision) {
                std::vector<SdfLayerRefPtr> retained;
                for(const auto& layer:stage->GetUsedLayers())if(layer)retained.emplace_back(layer);
                SdfLayerHandleSet layers;for(const auto& layer:retained)if(!layer->IsAnonymous())layers.insert(layer);
                SdfLayer::ReloadLayers(layers,true);
            }else stage->GetRootLayer()->Reload(true);
        }
        sourceRevision=revision;
        // Match the composition's per-asset working set after open/reload.
        // The UI's Outline must inspect the same populated stage as Hydra.
        if(auto assets=stage->GetPrimAtPath(SdfPath("/Natron/Assets"))) {
            std::vector<std::pair<SdfPath,bool>> rules;
            for(const auto& asset:assets.GetAllChildren()) {
                const auto flag=asset.GetCustomDataByKey(TfToken("natron:loadPayloads"));
                if(flag.IsHolding<bool>())rules.emplace_back(asset.GetPath(),flag.UncheckedGet<bool>());
            }
            for(const auto& rule:rules)if(rule.second)stage->Load(rule.first);else stage->Unload(rule.first);
        }
        indices.stageSceneIndex->SetTime(UsdTimeCode(time));
        indices.stageSceneIndex->ApplyPendingUpdates();
        if(width!=w || height!=h || !task) {
            task.reset();destroyBuffers();HdRenderPassAovBindingVector bindings;
            const std::array<TfToken,6> names{HdAovTokens->color,HdAovTokens->depth,HdAovTokens->primId,HdAovTokens->instanceId,TfToken("albedo"),HdAovTokens->normal};
            const std::array<HdFormat,6> formats{HdFormatFloat32Vec4,HdFormatFloat32,HdFormatInt32,HdFormatInt32,HdFormatFloat32Vec3,HdFormatFloat32Vec3};
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
        task->cameraPath=SdfPath(request.count("camera")?string(request,"camera"):"/Natron/Camera");
        if(!UsdGeomCamera(stage->GetPrimAtPath(task->cameraPath)))throw std::runtime_error("Missing requested render camera");
        auto lastStatus=started;
        auto lastFrame=started;
        for(;;) {
            if(cancelled())return {{"ok",JsValue(false)},{"cancelled",JsValue(true)}};
            engine.Execute(index.get(),&tasks);
            if(!errors.IsClean()) {std::string text;for(auto it=errors.GetBegin();it!=errors.GetEnd();++it)text+=it->GetCommentary()+"\n";errors.Clear();throw std::runtime_error(text);}
            if(task->pass->IsConverged())break;
            if(header && interactive && std::chrono::steady_clock::now()-lastFrame>std::chrono::milliseconds(150)){publish(true);lastFrame=std::chrono::steady_clock::now();}
            if(std::chrono::steady_clock::now()-lastStatus>std::chrono::seconds(5)) {
                lastStatus=std::chrono::steady_clock::now();std::cerr<<"hdNVP progress:";
                for(const auto& [name,value]:delegate->GetRenderStats())if(name=="Samples" || name=="Renderer Stage" || name=="rendererStage" || name=="percentDone")std::cerr<<" "<<name<<"="<<value;
                std::cerr<<std::endl;
            }
            if(std::chrono::steady_clock::now()-started>std::chrono::minutes(5))throw std::runtime_error("hdNVP render timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(cancelled())return {{"ok",JsValue(false)},{"cancelled",JsValue(true)}};
        // A partial snapshot must be acknowledged before final publication.
        while(header && InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&header->ready),0,0)!=0) {
            if(cancelled())return {{"ok",JsValue(false)},{"cancelled",JsValue(true)}};
            if(std::chrono::steady_clock::now()-started>std::chrono::minutes(5))throw std::runtime_error("Frame acknowledgement timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        publish(false);
        std::filesystem::create_directories(std::filesystem::u8path(directory));
        const std::array<const char*,6> files{"color.raw","depth.raw","primId.raw","instanceId.raw","albedo.raw","normal.raw"};
        if(!header)for(size_t i=0;i<buffers.size();++i) {
            void* data=buffers[i]->Map();if(!data)throw std::runtime_error("Cannot map completed AOV");
            const size_t count=size_t(w)*h*(i==0?4:i>=4?3:1);
            if(i<2 || i>=4)for(size_t j=0;j<count;++j)if(!std::isfinite(static_cast<float*>(data)[j])) {buffers[i]->Unmap();throw std::runtime_error("Non-finite hdNVP AOV");}
            std::ofstream out(std::filesystem::u8path(directory)/files[i],std::ios::binary|std::ios::trunc);
            out.write(static_cast<const char*>(data),std::streamsize(count*4));buffers[i]->Unmap();out.close();
            if(!out)throw std::runtime_error("Cannot write AOV");
        }
        // Resolve IDs against the populated render index from this request.
        // Never expose prototype or delegate paths as persistent UI identity.
        JsArray picks,unresolvedPicks;std::set<std::pair<int32_t,int32_t>> pairs;
        auto* primIds=static_cast<const int32_t*>(buffers[2]->Map());auto* instanceIds=static_cast<const int32_t*>(buffers[3]->Map());
        if(!primIds || !instanceIds)throw std::runtime_error("Cannot map picking buffers");
        for(size_t p=0;p<size_t(w)*h;++p)if(primIds[p]>=0)pairs.emplace(primIds[p],instanceIds[p]);
        buffers[2]->Unmap();buffers[3]->Unmap();
        for(const auto& [prim,instance]:pairs) {
            const auto id=index->GetRprimPathFromPrimId(prim);auto delegate=index->GetSceneDelegateForRprim(id);
            SdfPath path=id;if(delegate){const auto paths=delegate->GetScenePrimPaths(id,{instance});if(!paths.empty())path=paths.front();}
            if(instance>=0) {
                const auto resolved=instancePath(stage,index->GetTerminalSceneIndex(),id,instance);
                if(!resolved.IsEmpty())path=resolved;
            }
            const auto usdPrim=stage->GetPrimAtPath(path);
            if(usdPrim && !usdPrim.IsInPrototype())picks.emplace_back(JsObject{{"prim",JsValue(prim)},{"instance",JsValue(instance)},{"path",JsValue(path.GetString())}});
            else {const auto scene=index->GetTerminalSceneIndex();const auto parent=instancer(scene->GetPrim(id).dataSource);JsArray instancePaths;if(!parent.IsEmpty())for(const auto& p:locations(stage,scene,parent,id))instancePaths.emplace_back(p.GetString());
                unresolvedPicks.emplace_back(JsObject{{"prim",JsValue(prim)},{"instance",JsValue(instance)},{"hydraPath",JsValue(id.GetString())},{"scenePath",JsValue(path.GetString())},{"origin",JsValue(origin(scene,id).GetString())},{"instancer",JsValue(parent.GetString())},{"locations",JsValue(instancePaths)}});}
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
            {"picks",JsValue(picks)},{"revision",JsValue(request.count("revision")?string(request,"revision"):std::string())},
            {"unresolvedPicks",JsValue(unresolvedPicks)},
            {"transport",JsValue(std::string(header?"sharedMemory":"files"))},
            {"wallMs",JsValue(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count())},{"stats",JsValue(stats)}};
    }
private:
    // Destroy tasks/buffers/index before their delegate and its GPU resources.
    void destroyBuffers(){for(auto& buffer:buffers)if(buffer){delegate->DestroyBprim(buffer);buffer=nullptr;}}
    void clearScene(){task.reset();destroyBuffers();index.reset();indices={};stage.Reset();scenePath.clear();width=height=0;}
    HdPluginRenderDelegateUniqueHandle delegate;
    UsdStageRefPtr stage;UsdImagingSceneIndices indices;std::unique_ptr<HdRenderIndex> index;
    std::array<HdRenderBuffer*,6> buffers{};std::shared_ptr<Task> task;
    std::string scenePath,sourceRevision;bool loadPayloads=true,batchMode=true;int width=0,height=0;int64_t sceneOpens=0,requests=0;
};
}
int main(int argc,char** argv) {
    if(GetEnvironmentVariableA("NATRON_HDNVP_TRACE",nullptr,0))AddVectoredExceptionHandler(1,traceException);
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
