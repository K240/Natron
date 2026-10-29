// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_USDSCENEBRIDGE_H
#define NATRON_GPU_USDSCENEBRIDGE_H
#include "Gpu/CardScene.h"
#include "Gpu/UsdStageValue.h"
namespace Natron { namespace Gpu { namespace Cards {
enum class HdNvpDenoiser {Auto,Oidn,Dlss,None};
struct HdNvpSettings {
    int samples=16,bounces=4;
    HdNvpDenoiser denoiser=HdNvpDenoiser::Auto;
    bool interactive=false;
};
struct HdNvpFrame {
    std::vector<float> color,depth,albedo,normal,position,shadow,occlusion,reflection;
    std::vector<int32_t> primIds,instanceIds;
    struct Pick {int32_t prim=-1,instance=-1;std::string path;};
    std::vector<Pick> picks;
    uint64_t revision=0;
    std::string pickPath(int32_t prim,int32_t instance) const;
    std::string report;
};
// The exporter and worker survive frame/camera updates. Independent USD DLLs
// reside beside the worker, never in Natron's runtime search directory.
class UsdSceneBridge {
public:
    UsdSceneBridge();~UsdSceneBridge();
    static bool available();
    static std::string hostPath();
    std::string exportScene(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,
                            const std::vector<UsdSceneSource>& sources={});
    std::shared_ptr<const UsdSceneValue> sceneValue() const;
    // The abort predicate is evaluated on the calling thread, including while
    // waiting for the host, so render-thread TLS remains available.
    HdNvpFrame render(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,
                     const HdNvpSettings& quality={},const std::function<bool()>& abort={},
                     const std::vector<UsdSceneSource>& sources={},
                     const std::function<void(const HdNvpFrame&)>& progress={});
private:
    HdNvpFrame renderValue(const std::shared_ptr<const UsdSceneValue>& value,const Camera& camera,const Settings& settings,
        const HdNvpSettings& quality,const std::function<bool()>& abort,const std::vector<UsdSceneSource>& sources,
        const std::function<void(const HdNvpFrame&)>& progress);
    struct Impl;std::unique_ptr<Impl> _impl;
};
}}}
#endif
