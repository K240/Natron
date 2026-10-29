// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/UsdStageValue.h"
#include "Gpu/Compositing.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#ifdef NATRON_HAS_USD
#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/references.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdShade/shader.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
PXR_NAMESPACE_USING_DIRECTIVE
#endif
namespace Natron { namespace Gpu { namespace Cards {
#ifdef NATRON_HAS_USD
namespace {
SdfPath primPath(const std::string& text) {
    if(!SdfPath::IsValidPathString(text))throw std::runtime_error("Invalid USD prim path: "+text);
    SdfPath p(text);if(!p.IsAbsolutePath() || (!p.IsPrimPath() && p!=SdfPath::AbsoluteRootPath()))
        throw std::runtime_error("Expected an absolute USD prim path: "+text);
    return p;
}
GfMatrix4d matrix(const Matrix& m) {GfMatrix4d r;for(int i=0;i<4;++i)for(int j=0;j<4;++j)r[i][j]=m.m[i*4+j];return r;}
Matrix matrix(const GfMatrix4d& m) {Matrix r;for(int i=0;i<4;++i)for(int j=0;j<4;++j)r.m[i*4+j]=m[i][j];return r;}
Compositing compositing(const UsdPrim& prim,double time) {
    auto integer=[&](const char* name,int fallback){int v=fallback;auto p=UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken(name));if(p)p.Get(&v,UsdTimeCode(time));return v;};
    Compositing r;r.role=GeometryRole(integer("natron:compositingRole",0));
    const int mask=integer("natron:rayMask",7);r.cameraVisible=mask&1;r.reflectionVisible=mask&2;r.shadowVisible=mask&4;r.holdout=integer("natron:holdout",0)!=0;
    float density=1;auto d=UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken("natron:shadowDensity"));if(d)d.Get(&density,UsdTimeCode(time));r.shadowDensity=density;
    GfVec3f tint(0);auto t=UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken("natron:shadowTint"));if(t)t.Get(&tint,UsdTimeCode(time));r.shadowTint={tint[0],tint[1],tint[2]};
    auto scalar=[&](const char* name,float fallback){auto p=UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(TfToken(name));if(p)p.Get(&fallback,UsdTimeCode(time));return fallback;};
    r.reflectionStrength=scalar("natron:reflectionStrength",.25f);r.reflectionRoughness=scalar("natron:reflectionRoughness",.2f);validateCompositing(r);return r;
}
std::string summarize(const VtValue& value) {
    if(value.IsArrayValued())return std::to_string(value.GetArraySize())+" elements ("+value.GetTypeName()+")";
    const auto s=TfStringify(value);return s.size()>512?s.substr(0,512)+"...":s;
}
void selectVariants(const UsdStageRefPtr& stage,const std::string& selections,const SdfPath& sourceRoot={},const SdfPath& mount={}) {
    std::istringstream lines(selections);std::string line;
    while(std::getline(lines,line)) {
        auto comment=line.find('#');if(comment!=std::string::npos)line.resize(comment);
        std::istringstream words(line);std::string path,choice,extra;if(!(words>>path))continue;
        if(!(words>>choice) || words>>extra)throw std::runtime_error("Invalid USD variant selection");
        auto target=primPath(path);
        if(!mount.IsEmpty()) {
            if(!target.HasPrefix(sourceRoot))throw std::runtime_error("Variant selection lies outside the referenced asset");
            target=target.ReplacePrefix(sourceRoot,mount.AppendChild(TfToken("Source")));
        }
        auto prim=stage->GetPrimAtPath(target);auto equal=choice.find('=');if(!prim || equal==std::string::npos)throw std::runtime_error("USD variant prim or selection is invalid: "+path);
        auto set=prim.GetVariantSet(choice.substr(0,equal));const auto names=set.GetVariantNames();const auto value=choice.substr(equal+1);
        if(std::find(names.begin(),names.end(),value)==names.end() || !set.SetVariantSelection(value))throw std::runtime_error("Unknown USD variant selection: "+choice);
    }
}
}
struct UsdSceneValue::Impl {
    mutable std::mutex mutex;
    UsdStageRefPtr stage;
    uint64_t revision=0;
    double time=1,meters=1;
    bool payloads=true;
    Matrix basis;
    std::vector<std::pair<SdfPath,std::string>> owners;
    std::vector<std::pair<UsdStageRefPtr,std::string>> shotOwners;
    UsdPrimInfo info(const UsdPrim& prim,bool details) const {
        UsdPrimInfo r;r.path=prim.GetPath().GetString();r.name=prim.GetName().GetString();r.type=prim.GetTypeName().GetString();
        r.displayName=prim.GetDisplayName();r.active=prim.IsActive();r.loaded=prim.IsLoaded();r.instance=prim.IsInstance();
        r.instanceProxy=prim.IsInstanceProxy();r.hasPayload=prim.HasAuthoredPayloads();
        const auto predicate=UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate);
        r.hasChildren=!prim.GetFilteredChildren(predicate).empty();
        // Imported ownership belongs to this branch's reader, even when an
        // asset contains Natron identities from an earlier project.
        for(const auto& source:shotOwners)if(source.first->GetPrimAtPath(prim.GetPath())){r.ownerId=source.second;break;}
        if(r.ownerId.empty())for(const auto& owner:owners)if(prim.GetPath().HasPrefix(owner.first)){r.ownerId=owner.second;break;}
        if(r.ownerId.empty())for(auto ancestor=prim;ancestor && !ancestor.IsPseudoRoot();ancestor=ancestor.GetParent()) {
            auto owner=ancestor.GetCustomDataByKey(TfToken("natron:owner"));
            if(owner.IsHolding<std::string>()){r.ownerId=owner.UncheckedGet<std::string>();break;}
        }
        if(UsdGeomImageable imageable{prim})r.visibility=imageable.ComputeVisibility(UsdTimeCode(time)).GetString();
        if(!details)return r;
        if(prim.IsActive() && prim.IsLoaded() && UsdGeomXformable(prim)) {
            UsdGeomXformCache cache{UsdTimeCode(time)};
            r.transform=basis*matrix(cache.GetLocalToWorldTransform(prim));r.hasTransform=true;
        }
        for(const auto& spec:prim.GetPrimStack()) {
            const auto& id=spec->GetLayer()->GetIdentifier();
            if(std::find(r.layers.begin(),r.layers.end(),id)==r.layers.end())r.layers.push_back(id);
        }
        for(const auto& api:prim.GetAppliedSchemas())r.apis.push_back(api.GetString());
        if(prim.IsActive() && prim.IsLoaded() && UsdGeomImageable(prim)) {
            UsdGeomBBoxCache cache(UsdTimeCode(time),{UsdGeomTokens->default_,UsdGeomTokens->render,UsdGeomTokens->proxy,UsdGeomTokens->guide},true);
            const auto box=cache.ComputeWorldBound(prim);const auto range=box.ComputeAlignedRange();
            if(!range.IsEmpty()) {
                bool first=true;
                for(int i=0;i<8;++i){auto p=basis.point({i&1?range.GetMax()[0]:range.GetMin()[0],i&2?range.GetMax()[1]:range.GetMin()[1],i&4?range.GetMax()[2]:range.GetMin()[2]});
                    if(first){r.minimum=r.maximum=p;first=false;}else {r.minimum={std::min(r.minimum.x,p.x),std::min(r.minimum.y,p.y),std::min(r.minimum.z,p.z)};r.maximum={std::max(r.maximum.x,p.x),std::max(r.maximum.y,p.y),std::max(r.maximum.z,p.z)};}}
                r.hasBounds=true;
            }
        }
        if(const auto material=UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial())r.material=material.GetPath().GetString();
        for(const auto& set:prim.GetVariantSets().GetNames())r.properties.emplace_back("variant:"+set,prim.GetVariantSet(set).GetVariantSelection());
        for(const auto& name:{"references","payload","instanceable","kind"}) {
            VtValue v;if(prim.GetMetadata(TfToken(name),&v))r.properties.emplace_back(name,summarize(v));
        }
        for(const auto& attr:prim.GetAttributes()) {
            VtValue v;if(attr.Get(&v,UsdTimeCode(time)))r.properties.emplace_back(attr.GetName().GetString(),summarize(v));
        }
        for(const auto& rel:prim.GetRelationships()) {
            SdfPathVector paths;rel.GetTargets(&paths);std::string value;
            for(const auto& path:paths){if(value.size()>512){value+="...";break;}if(!value.empty())value+=", ";value+=path.GetString();}
            r.properties.emplace_back(rel.GetName().GetString(),value);
        }
        return r;
    }
};
bool UsdSceneValue::available(){return true;}
std::shared_ptr<const UsdSceneValue> UsdSceneValue::compose(const std::string& authored,const std::vector<UsdSceneSource>& sources,uint64_t revision,const std::vector<UsdPrimEdit>& edits) {
    auto impl=std::make_unique<Impl>();impl->revision=revision;
    const auto root=SdfLayer::CreateAnonymous("NatronComposition.usda");
    if(!root->ImportFromString(authored))throw std::runtime_error("Cannot compose Natron's private USD layer");
    impl->stage=UsdStage::Open(root,UsdStage::LoadNone);auto stage=impl->stage;
    std::vector<std::string> layers;std::map<std::string,UsdStageRefPtr> originals;
    bool firstShot=true;
    for(const auto& source:sources) {
        if(!std::isfinite(source.timeCode) || !std::isfinite(source.metersPerUnit) || source.metersPerUnit<=0 || !std::isfinite(source.timeCodesPerSecond) || source.timeCodesPerSecond<=0)throw std::runtime_error("Invalid USD source time or units");
        auto original=UsdStage::Open(source.filename,UsdStage::LoadNone);if(!original)throw std::runtime_error("Cannot open USD source: "+source.filename);
        originals[source.filename]=original;
        if(source.mountPath.empty()) {
            if(source.transform.m!=Matrix().m)throw std::runtime_error("A transformed shot needs an explicit asset namespace");
            if(original->GetPrimAtPath(SdfPath("/Natron")))throw std::runtime_error("The shot source uses the reserved /Natron namespace");
            if(firstShot){impl->time=source.timeCode;impl->payloads=source.loadPayloads;firstShot=false;}
            else if(source.timeCode!=impl->time || source.loadPayloads!=impl->payloads)
                throw std::runtime_error("Shot sublayers need matching time and payload policy; use Asset reference for independent sources");
            if(UsdGeomGetStageMetersPerUnit(stage)!=source.metersPerUnit || UsdGeomGetStageUpAxis(stage)!=(source.zUp?UsdGeomTokens->z:UsdGeomTokens->y) || stage->GetTimeCodesPerSecond()!=source.timeCodesPerSecond)
                throw std::runtime_error("Shot sublayers need matching units, up-axis and time rate; use Asset reference for independent sources");
            if(std::find(layers.begin(),layers.end(),source.filename)==layers.end())layers.push_back(source.filename);
            impl->shotOwners.emplace_back(original,source.ownerId);
            for(const auto& prim:original->GetPseudoRoot().GetAllChildren())impl->owners.emplace_back(prim.GetPath(),source.ownerId);
        }
    }
    if(firstShot && !sources.empty())impl->time=sources.front().timeCode;
    root->SetSubLayerPaths(layers);
    std::map<std::string,const UsdSceneSource*> mounted;
    for(auto it=sources.rbegin();it!=sources.rend();++it) {
        const auto& source=*it;
        auto original=originals.at(source.filename);SdfPath sourceRoot,mount;
        if(!source.mountPath.empty()) {
            const auto previous=mounted.find(source.mountPath);
            if(previous!=mounted.end()) {
                const auto& other=*previous->second;
                if(other.filename==source.filename && other.primPath==source.primPath && other.variants==source.variants && other.ownerId==source.ownerId && other.timeCode==source.timeCode && other.loadPayloads==source.loadPayloads && other.transform.m==source.transform.m)continue;
                throw std::runtime_error("Conflicting USD asset branches share a namespace: "+source.mountPath);
            }mounted[source.mountPath]=&source;
            mount=primPath(source.mountPath);if(mount==SdfPath::AbsoluteRootPath() || stage->GetPrimAtPath(mount))throw std::runtime_error("USD asset mount path already exists: "+source.mountPath);
            if(mount==SdfPath("/Natron/Assets") || !mount.HasPrefix(SdfPath("/Natron/Assets")))throw std::runtime_error("Asset mounts must be beneath /Natron/Assets");
            // Validate the selected variant and requested payload working set in
            // a private session, without authoring into the imported root layer.
            const auto session=SdfLayer::CreateAnonymous("NatronAssetValidation.usda");
            original=UsdStage::Open(original->GetRootLayer(),session,UsdStage::LoadNone);original->SetEditTarget(session);
            selectVariants(original,source.variants);
            auto prim=source.primPath.empty()?original->GetDefaultPrim():original->GetPrimAtPath(primPath(source.primPath));
            if(!prim || prim.IsPseudoRoot())throw std::runtime_error("Asset reference needs a valid Prim subtree or source defaultPrim");
            if(source.loadPayloads)original->Load(prim.GetPath());
            if(!original->GetCompositionErrors().empty())throw std::runtime_error("USD asset has unresolved dependencies");
            for(const auto& descendant:UsdPrimRange(prim,UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate))) {
                if(descendant!=prim && UsdGeomXformable(descendant) && UsdGeomXformable(descendant).GetResetXformStack())throw std::runtime_error("Nested resetXformStack needs the asset assembly adapter; use Shot layers to preserve the source coordinate frame");
                auto check=[&](const SdfPathVector& paths){for(const auto& path:paths)if(!path.HasPrefix(prim.GetPath()))throw std::runtime_error("Asset subtree has a dependency outside "+prim.GetPath().GetString()+": "+path.GetString()+". Choose a common root or Shot layers.");};
                for(const auto& rel:descendant.GetRelationships()){SdfPathVector paths;rel.GetTargets(&paths);check(paths);}
                for(const auto& attr:descendant.GetAttributes()){SdfPathVector paths;attr.GetConnections(&paths);check(paths);}
            }
            sourceRoot=prim.GetPath();auto asset=UsdGeomXform::Define(stage,mount);
            impl->owners.emplace_back(mount,source.ownerId);
            const auto referencePath=mount.AppendChild(TfToken("Source"));auto reference=stage->OverridePrim(referencePath);
            // USD adjusts layer rates across references; align each requested
            // source sample to the composition time explicitly.
            const double rate=stage->GetTimeCodesPerSecond()/original->GetTimeCodesPerSecond();
            reference.GetReferences().AddReference(source.filename,sourceRoot,SdfLayerOffset(impl->time-source.timeCode*rate));
            asset.GetPrim().SetCustomDataByKey(TfToken("natron:owner"),VtValue(source.ownerId));asset.GetPrim().SetDisplayName(source.label);
            asset.GetPrim().SetCustomDataByKey(TfToken("natron:loadPayloads"),VtValue(source.loadPayloads));
            const auto sourceBasis=Matrix::transform({},source.zUp?Vec3{-90,0,0}:Vec3{},{source.metersPerUnit,source.metersPerUnit,source.metersPerUnit});
            asset.AddTransformOp().Set(matrix(source.transform*sourceBasis));
        }
        selectVariants(stage,source.variants,sourceRoot,mount);
        if(!mount.IsEmpty()) {
            if(source.loadPayloads)stage->Load(mount);else stage->Unload(mount);
            // The referenced root's reset must not bypass its assembly wrapper.
            if(auto reference=UsdGeomXformable(stage->GetPrimAtPath(mount.AppendChild(TfToken("Source")))))reference.SetResetXformStack(false);
        }
    }
    if(impl->payloads)for(const auto& source:sources)if(source.mountPath.empty())for(const auto& prim:originals.at(source.filename)->GetPseudoRoot().GetAllChildren())stage->Load(prim.GetPath());
    if(!stage->GetCompositionErrors().empty())throw std::runtime_error("USD source composition has unresolved dependencies");
    impl->meters=UsdGeomGetStageMetersPerUnit(stage);impl->basis=Matrix::transform({},UsdGeomGetStageUpAxis(stage)==UsdGeomTokens->z?Vec3{-90,0,0}:Vec3{},{impl->meters,impl->meters,impl->meters});
    // Apply graph opinions only after source arcs have composed. The source
    // layers remain read-only, including instance prototypes.
    std::vector<const UsdPrimEdit*> applied;
    for(const auto& edit:edits) {
        // A shared upstream edit can reach Scene through several inputs.
        // Apply identical opinions once, while retaining distinct instances.
        if(!edit.identity.empty() && std::any_of(applied.begin(),applied.end(),[&](const UsdPrimEdit* p){return p->identity==edit.identity && p->kind==edit.kind && p->paths==edit.paths && p->transform.m==edit.transform.m && p->visible==edit.visible && p->materialPath==edit.materialPath && p->cameraVisible==edit.cameraVisible && p->shadowVisible==edit.shadowVisible && p->reflectionVisible==edit.reflectionVisible && p->holdout==edit.holdout && p->role==edit.role && p->shadowDensity==edit.shadowDensity && p->shadowTint.x==edit.shadowTint.x && p->shadowTint.y==edit.shadowTint.y && p->shadowTint.z==edit.shadowTint.z && p->reflectionStrength==edit.reflectionStrength && p->reflectionRoughness==edit.reflectionRoughness;}))continue;
        applied.push_back(&edit);
        std::vector<UsdPrim> targets;
        for(const auto& text:edit.paths) {
            const auto path=primPath(text);auto prim=stage->GetPrimAtPath(path);
            if(!prim || prim.IsPseudoRoot() || prim.IsInPrototype() || path==SdfPath("/Natron/InspectionCamera"))throw std::runtime_error("USD edit target is missing, internal or is a prototype: "+text);
            if(prim.IsInstanceProxy()) {
                auto instance=prim.GetParent();while(instance && !instance.IsInstance())instance=instance.GetParent();
                if(!instance || !instance.SetInstanceable(false))throw std::runtime_error("Cannot author this USD instance override: "+text);
                prim=stage->GetPrimAtPath(path);
            }
            targets.push_back(prim);
        }
        if(edit.kind==UsdPrimEdit::Kind::Compositing){
            // A downstream role applies to the selected subtree even if a
            // source Mesh authors its own inherited-primvar defaults.
            std::vector<SdfPath> instances;
            for(const auto& root:targets)for(const auto& p:UsdPrimRange(root,UsdTraverseInstanceProxies()))if(p.IsInstance())instances.push_back(p.GetPath());
            for(const auto& path:instances)if(auto prim=stage->GetPrimAtPath(path))prim.SetInstanceable(false);
            std::vector<UsdPrim> expanded=targets;
            for(const auto& root:targets)for(const auto& p:UsdPrimRange(stage->GetPrimAtPath(root.GetPath())))if(UsdGeomMesh(p))expanded.push_back(p);
            std::sort(expanded.begin(),expanded.end(),[](const auto& a,const auto& b){return a.GetPath()<b.GetPath();});
            expanded.erase(std::unique(expanded.begin(),expanded.end()),expanded.end());targets=std::move(expanded);
        }
        if(edit.kind==UsdPrimEdit::Kind::Transform) {
            for(double v:edit.transform.m)if(!std::isfinite(v))throw std::runtime_error("USD transform delta must be finite");
            edit.transform.inverse();
            // Parents must be authored first, so a selected descendant receives
            // the world delta once even when the selection order is arbitrary.
            std::sort(targets.begin(),targets.end(),[](const UsdPrim& a,const UsdPrim& b){return a.GetPath()<b.GetPath();});
            targets.erase(std::unique(targets.begin(),targets.end()),targets.end());
            UsdGeomXformCache cache{UsdTimeCode(impl->time)};std::vector<Matrix> worlds;
            for(const auto& prim:targets) {
                if(!UsdGeomXformable(prim))throw std::runtime_error("USD transform target is not Xformable: "+prim.GetPath().GetString());
                worlds.push_back(matrix(cache.GetLocalToWorldTransform(prim)));
            }
            for(size_t i=0;i<targets.size();++i) {
                auto prim=targets[i];UsdGeomXformCache updated{UsdTimeCode(impl->time)};
                auto xform=UsdGeomXformable(prim);const bool reset=xform.GetResetXformStack();
                const auto parent=reset?Matrix():matrix(updated.GetLocalToWorldTransform(prim.GetParent()));
                const auto local=parent.inverse()*impl->basis.inverse()*edit.transform*impl->basis*worlds[i];
                const TfToken name("xformOp:transform:natron_"+TfMakeValidIdentifier(edit.identity));
                prim.CreateAttribute(name,SdfValueTypeNames->Matrix4d).Set(matrix(local));
                VtTokenArray order;if(reset)order.push_back(TfToken("!resetXformStack!"));order.push_back(name);
                xform.CreateXformOpOrderAttr().Set(order);
            }
        } else for(const auto& prim:targets) {
            if(edit.kind==UsdPrimEdit::Kind::Visibility) {
                auto imageable=UsdGeomImageable(prim);if(!imageable)throw std::runtime_error("USD visibility target is not Imageable");
                imageable.CreateVisibilityAttr().Set(edit.visible?UsdGeomTokens->inherited:UsdGeomTokens->invisible);
            } else if(edit.kind==UsdPrimEdit::Kind::Material) {
                const auto material=UsdShadeMaterial(stage->GetPrimAtPath(primPath(edit.materialPath)));
                if(!material || !UsdShadeMaterialBindingAPI::Apply(prim).Bind(material,UsdShadeTokens->strongerThanDescendants))throw std::runtime_error("Cannot bind USD material: "+edit.materialPath);
            } else {
                Compositing comp;comp.role=edit.role;comp.shadowDensity=edit.shadowDensity;comp.shadowTint=edit.shadowTint;comp.reflectionStrength=edit.reflectionStrength;comp.reflectionRoughness=edit.reflectionRoughness;validateCompositing(comp);
                prim.AddAppliedSchema(TfToken("NatronCompositingAPI"));
                prim.CreateAttribute(TfToken("natron:compositingRole"),SdfValueTypeNames->Int).Set(int(edit.role));
                prim.CreateAttribute(TfToken("natron:shadowDensity"),SdfValueTypeNames->Float).Set(float(edit.shadowDensity));
                prim.CreateAttribute(TfToken("natron:shadowTint"),SdfValueTypeNames->Color3f).Set(GfVec3f(edit.shadowTint.x,edit.shadowTint.y,edit.shadowTint.z));
                prim.CreateAttribute(TfToken("natron:reflectionStrength"),SdfValueTypeNames->Float).Set(float(edit.reflectionStrength));
                prim.CreateAttribute(TfToken("natron:reflectionRoughness"),SdfValueTypeNames->Float).Set(float(edit.reflectionRoughness));
                for(const auto& flag:std::vector<std::pair<const char*,bool>>{{"natron:cameraVisible",edit.cameraVisible},{"natron:shadowVisible",edit.shadowVisible},{"natron:reflectionVisible",edit.reflectionVisible},{"natron:holdout",edit.holdout}})
                    prim.CreateAttribute(TfToken(flag.first),SdfValueTypeNames->Bool).Set(flag.second);
                const int mask=(edit.cameraVisible?1:0)|(edit.reflectionVisible?2:0)|(edit.shadowVisible?4:0);
                UsdGeomPrimvarsAPI pv(prim);pv.CreatePrimvar(TfToken("natron:rayMask"),SdfValueTypeNames->Int,UsdGeomTokens->constant).Set(mask);
                pv.CreatePrimvar(TfToken("natron:holdout"),SdfValueTypeNames->Int,UsdGeomTokens->constant).Set(edit.holdout || edit.role!=GeometryRole::Surface?1:0);
                pv.CreatePrimvar(TfToken("natron:compositingRole"),SdfValueTypeNames->Int,UsdGeomTokens->constant).Set(int(edit.role));
                pv.CreatePrimvar(TfToken("natron:shadowDensity"),SdfValueTypeNames->Float,UsdGeomTokens->constant).Set(float(edit.shadowDensity));
                pv.CreatePrimvar(TfToken("natron:shadowTint"),SdfValueTypeNames->Color3f,UsdGeomTokens->constant).Set(GfVec3f(edit.shadowTint.x,edit.shadowTint.y,edit.shadowTint.z));
                pv.CreatePrimvar(TfToken("natron:reflectionStrength"),SdfValueTypeNames->Float,UsdGeomTokens->constant).Set(float(edit.reflectionStrength));
                pv.CreatePrimvar(TfToken("natron:reflectionRoughness"),SdfValueTypeNames->Float,UsdGeomTokens->constant).Set(float(edit.reflectionRoughness));
            }
        }
    }
    return std::shared_ptr<const UsdSceneValue>(new UsdSceneValue(std::move(impl)));
}
UsdSceneValue::UsdSceneValue(std::unique_ptr<Impl> impl):_impl(std::move(impl)){}
UsdSceneValue::~UsdSceneValue()=default;
uint64_t UsdSceneValue::revision()const{return _impl->revision;}
double UsdSceneValue::timeCode()const{return _impl->time;}
double UsdSceneValue::metersPerUnit()const{return _impl->meters;}
bool UsdSceneValue::loadPayloads()const{return _impl->payloads;}
std::vector<UsdCompositingTarget> UsdSceneValue::compositingTargets()const {
    std::lock_guard<std::mutex> lock(_impl->mutex);std::vector<UsdCompositingTarget> result;
    for(const auto& prim:UsdPrimRange(_impl->stage->GetPseudoRoot(),UsdTraverseInstanceProxies()))if(UsdGeomMesh(prim)){
        auto imageable=UsdGeomImageable(prim);const auto purpose=imageable.ComputePurpose();
        if(imageable.ComputeVisibility(UsdTimeCode(_impl->time))==UsdGeomTokens->invisible || purpose==UsdGeomTokens->guide || purpose==UsdGeomTokens->proxy)continue;
        result.push_back({prim.GetPath().GetString(),compositing(prim,_impl->time)});
    }return result;
}
std::shared_ptr<const UsdSceneValue> UsdSceneValue::compositingProbe(const std::vector<std::string>& receivers,bool reference,bool reflection,double roughness)const {
    const auto targets=compositingTargets();std::lock_guard<std::mutex> lock(_impl->mutex);
    auto impl=std::make_unique<Impl>();impl->time=_impl->time;impl->meters=_impl->meters;impl->basis=_impl->basis;impl->payloads=_impl->payloads;impl->revision=_impl->revision;
    const auto root=SdfLayer::CreateAnonymous("NatronCompositingProbe.usda");root->TransferContent(_impl->stage->GetRootLayer());
    impl->stage=UsdStage::Open(root,UsdStage::LoadNone);auto stage=impl->stage;stage->SetLoadRules(_impl->stage->GetLoadRules());
    // Private per-instance primary/secondary overrides never modify source
    // prototypes. Preserve the published instance structure in the UI stage.
    std::vector<SdfPath> instances;for(const auto& prim:UsdPrimRange(stage->GetPseudoRoot(),UsdTraverseInstanceProxies()))if(prim.IsInstance())instances.push_back(prim.GetPath());
    for(const auto& path:instances)stage->GetPrimAtPath(path).SetInstanceable(false);
    const auto material=UsdShadeMaterial::Define(stage,SdfPath("/Natron/Materials/CompositingProbe"));
    auto shader=UsdShadeShader::Define(stage,SdfPath("/Natron/Materials/CompositingProbe/Surface"));shader.CreateIdAttr().Set(TfToken("UsdPreviewSurface"));
    const bool matte=receivers.empty();
    shader.CreateInput(TfToken("diffuseColor"),SdfValueTypeNames->Color3f).Set(matte?GfVec3f(0):GfVec3f(1));
    shader.CreateInput(TfToken("emissiveColor"),SdfValueTypeNames->Color3f).Set(matte?GfVec3f(1):GfVec3f(0));
    shader.CreateInput(TfToken("useSpecularWorkflow"),SdfValueTypeNames->Int).Set(1);shader.CreateInput(TfToken("specularColor"),SdfValueTypeNames->Color3f).Set(GfVec3f(0));
    shader.CreateInput(TfToken("roughness"),SdfValueTypeNames->Float).Set(1.f);shader.CreateInput(TfToken("ior"),SdfValueTypeNames->Float).Set(1.f);
    if(reflection){
        shader.CreateInput(TfToken("diffuseColor"),SdfValueTypeNames->Color3f).Set(GfVec3f(1));
        shader.CreateInput(TfToken("useSpecularWorkflow"),SdfValueTypeNames->Int).Set(0);
        shader.CreateInput(TfToken("metallic"),SdfValueTypeNames->Float).Set(1.f);
        shader.CreateInput(TfToken("roughness"),SdfValueTypeNames->Float).Set(float(roughness));
    }
    shader.CreateOutput(TfToken("surface"),SdfValueTypeNames->Token);material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(),TfToken("surface"));
    for(const auto& target:targets){
        auto prim=stage->GetPrimAtPath(SdfPath(target.path));if(!prim)continue;const auto& c=target.value;
        int mask=(c.cameraVisible?1:0)|(c.reflectionVisible?2:0)|(c.shadowVisible?4:0);bool holdout=c.holdout || c.role!=GeometryRole::Surface;
        const bool proxy=c.role==GeometryRole::Occluder || (c.role==GeometryRole::Surface && c.holdout);
        const bool receiver=std::find(receivers.begin(),receivers.end(),target.path)!=receivers.end();
        if(matte){holdout=!proxy;mask&=1;}
        else if(receiver){holdout=false;}
        else if(c.role==GeometryRole::Surface && !c.holdout){mask&=~1;if(reference)mask=reflection?(mask&~2):0;holdout=false;}
        else holdout=true;
        UsdGeomPrimvarsAPI pv(prim);pv.CreatePrimvar(TfToken("natron:rayMask"),SdfValueTypeNames->Int,UsdGeomTokens->constant).Set(mask);
        pv.CreatePrimvar(TfToken("natron:holdout"),SdfValueTypeNames->Int,UsdGeomTokens->constant).Set(holdout?1:0);
        if((matte && proxy) || receiver)UsdShadeMaterialBindingAPI::Apply(prim).Bind(material,UsdShadeTokens->strongerThanDescendants);
    }
    return std::shared_ptr<const UsdSceneValue>(new UsdSceneValue(std::move(impl)));
}
UsdPrimPage UsdSceneValue::children(const std::string& path,size_t offset,size_t limit)const {
    std::lock_guard<std::mutex> lock(_impl->mutex);auto prim=_impl->stage->GetPrimAtPath(primPath(path));if(!prim)throw std::runtime_error("USD prim no longer exists: "+path);
    UsdPrimPage result;limit=std::clamp<size_t>(limit,1,256);size_t i=0;
    for(const auto& child:prim.GetFilteredChildren(UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate))) {
        if(child.GetPath()==SdfPath("/Natron/InspectionCamera"))continue;
        if(i++<offset)continue;if(result.prims.size()==limit){result.more=true;break;}result.prims.push_back(_impl->info(child,false));
    }return result;
}
UsdPrimInfo UsdSceneValue::inspect(const std::string& path)const {
    std::lock_guard<std::mutex> lock(_impl->mutex);auto prim=_impl->stage->GetPrimAtPath(primPath(path));if(!prim)throw std::runtime_error("USD prim no longer exists: "+path);return _impl->info(prim,true);
}
Camera UsdSceneValue::camera(const std::string& path,bool retainFilmGate)const {
    std::lock_guard<std::mutex> lock(_impl->mutex);auto c=UsdGeomCamera(_impl->stage->GetPrimAtPath(primPath(path)));
    if(!c)throw std::runtime_error("USD Prim is not a camera: "+path);
    const auto time=UsdTimeCode(_impl->time);UsdGeomXformCache cache(time);Camera result;
    result.transform=_impl->basis*matrix(cache.GetLocalToWorldTransform(c.GetPrim()));
    for(int col=0;col<3;++col)for(int row=0;row<3;++row)result.transform.m[col*4+row]/=_impl->meters;
    auto value=[&](const UsdAttribute& a,float fallback){float v=fallback;a.Get(&v,time);return double(v);};
    const auto horizontal=value(c.GetHorizontalApertureAttr(),20.955f),vertical=value(c.GetVerticalApertureAttr(),15.2908f);
    if(!std::isfinite(horizontal) || horizontal<=0 || !std::isfinite(vertical) || vertical<=0)throw std::runtime_error("USD camera aperture must be finite and positive");
    result.focal=value(c.GetFocalLengthAttr(),50)*_impl->meters*100;result.aperture=horizontal*_impl->meters*100;
    if(retainFilmGate)result.apertureAspect=horizontal/vertical;
    result.offsetX=value(c.GetHorizontalApertureOffsetAttr(),0)/horizontal;result.offsetY=value(c.GetVerticalApertureOffsetAttr(),0)/horizontal;
    TfToken projection;c.GetProjectionAttr().Get(&projection,time);result.orthographic=projection==UsdGeomTokens->orthographic;
    result.orthoWidth=horizontal*_impl->meters*.1;GfVec2f clip(1,1000000);c.GetClippingRangeAttr().Get(&clip,time);
    result.nearClip=clip[0]*_impl->meters;result.farClip=clip[1]*_impl->meters;result.label=c.GetPrim().GetDisplayName();return result;
}
void UsdSceneValue::saveAssembly(const std::string& filename)const {std::lock_guard<std::mutex> lock(_impl->mutex);if(!_impl->stage->GetRootLayer()->Export(filename))throw std::runtime_error("Cannot export USD assembly");}
bool UsdSceneValue::saveRenderAssembly(const std::string& filename,const std::string& geometryFile,std::string& previousGeometry,bool sameScene)const {
    std::lock_guard<std::mutex> lock(_impl->mutex);const auto source=_impl->stage->GetRootLayer();
    const auto base=sameScene?SdfLayerRefPtr():SdfLayer::CreateAnonymous("NatronGeometry.usda");if(base)base->TransferContent(source);
    const auto camera=SdfLayer::CreateAnonymous("NatronCamera.usda");
    SdfCreatePrimInLayer(camera,SdfPath("/Natron"));
    const auto root=SdfPath::AbsoluteRootPath();
    for(const auto& key:source->ListFields(root))if(key!=SdfChildrenKeys->PrimChildren && key!=SdfFieldKeys->SubLayers && key!=SdfFieldKeys->SubLayerOffsets)camera->SetField(root,key,source->GetField(root,key));
    for(const auto& path:{SdfPath("/Natron/Camera"),SdfPath("/Natron/InspectionCamera")})if(source->GetPrimAtPath(path)) {
        if(!SdfCopySpec(source,path,camera,path))throw std::runtime_error("Cannot export camera opinions");
        if(base)if(auto prim=base->GetPrimAtPath(path))prim->GetNameParent()->RemoveNameChild(prim);
    }
    std::string geometry;if(base)base->ExportToString(&geometry);const bool changed=base && geometry!=previousGeometry;
    if(changed){if(!base->Export(geometryFile))throw std::runtime_error("Cannot export renderer geometry opinions");previousGeometry=std::move(geometry);}
    camera->SetSubLayerPaths({geometryFile});if(!camera->Export(filename))throw std::runtime_error("Cannot export renderer camera opinions");return changed;
}
#else
struct UsdSceneValue::Impl {};
UsdSceneValue::UsdSceneValue(std::unique_ptr<Impl> impl):_impl(std::move(impl)){}UsdSceneValue::~UsdSceneValue()=default;
bool UsdSceneValue::available(){return false;}
std::vector<UsdCompositingTarget> UsdSceneValue::compositingTargets()const{throw std::runtime_error("OpenUSD is unavailable");}
std::shared_ptr<const UsdSceneValue> UsdSceneValue::compositingProbe(const std::vector<std::string>&,bool,bool,double)const{throw std::runtime_error("OpenUSD is unavailable");}
std::shared_ptr<const UsdSceneValue> UsdSceneValue::compose(const std::string&,const std::vector<UsdSceneSource>&,uint64_t,const std::vector<UsdPrimEdit>&){throw std::runtime_error("OpenUSD is unavailable");}
uint64_t UsdSceneValue::revision()const{return 0;}double UsdSceneValue::timeCode()const{return 1;}double UsdSceneValue::metersPerUnit()const{return 1;}bool UsdSceneValue::loadPayloads()const{return false;}
UsdPrimPage UsdSceneValue::children(const std::string&,size_t,size_t)const{throw std::runtime_error("OpenUSD is unavailable");}
UsdPrimInfo UsdSceneValue::inspect(const std::string&)const{throw std::runtime_error("OpenUSD is unavailable");}
Camera UsdSceneValue::camera(const std::string&,bool)const{throw std::runtime_error("OpenUSD is unavailable");}
void UsdSceneValue::saveAssembly(const std::string&)const{throw std::runtime_error("OpenUSD is unavailable");}
bool UsdSceneValue::saveRenderAssembly(const std::string&,const std::string&,std::string&,bool)const{throw std::runtime_error("OpenUSD is unavailable");}
#endif
}}}
