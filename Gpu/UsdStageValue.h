// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_USDSTAGEVALUE_H
#define NATRON_GPU_USDSTAGEVALUE_H
#include "Gpu/CardScene.h"
#include <cstdint>
#include <utility>

namespace Natron { namespace Gpu { namespace Cards {
struct UsdSceneSource {
    std::string filename, variants, ownerId, label;
    // Empty mountPath composes a complete shot as a sublayer. A mountPath
    // references a dependency-complete asset subtree under that namespace.
    std::string mountPath, primPath;
    Matrix transform;
    double timeCode=1, metersPerUnit=1, timeCodesPerSecond=24;
    bool zUp=false, loadPayloads=true;
    uint64_t revision=0;
};
struct UsdPrimInfo {
    std::string path,name,type,ownerId,displayName,visibility,material;
    bool active=true,loaded=true,instance=false,instanceProxy=false,hasChildren=false,hasPayload=false;
    bool hasBounds=false;
    Vec3 minimum,maximum; // world space, right-handed Y-up meters
    std::vector<std::string> layers,apis;
    std::vector<std::pair<std::string,std::string>> properties;
};
struct UsdPrimPage {std::vector<UsdPrimInfo> prims;bool more=false;};

// A published, branch-private composition. No USD handles cross this interface.
// Source layers are read-only; all authored opinions are in the private root.
// Queries return bounded, plain-data snapshots, serialized on the value's lock.
class UsdSceneValue {
public:
    static bool available();
    static std::shared_ptr<const UsdSceneValue> compose(const std::string& authoredLayer,
        const std::vector<UsdSceneSource>& sources,uint64_t revision,
        const std::vector<UsdPrimEdit>& edits={});
    ~UsdSceneValue();
    uint64_t revision() const;
    double timeCode() const;
    double metersPerUnit() const;
    bool loadPayloads() const;
    UsdPrimPage children(const std::string& path="/",size_t offset=0,size_t limit=256) const;
    UsdPrimInfo inspect(const std::string& path) const;
    Camera camera(const std::string& path) const;
    void saveAssembly(const std::string& filename) const;
    // Renderer transport separates frequently changing camera opinions from
    // geometry/source arcs; returns true only when the latter file changed.
    bool saveRenderAssembly(const std::string& filename,const std::string& geometryFile,std::string& previousGeometry,bool sameScene=false) const;
private:
    struct Impl;
    explicit UsdSceneValue(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};
}}}
#endif
