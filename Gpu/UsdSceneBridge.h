// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_USDSCENEBRIDGE_H
#define NATRON_GPU_USDSCENEBRIDGE_H
#include "Gpu/CardScene.h"
namespace Natron { namespace Gpu { namespace Cards {
enum class HdNvpDenoiser {Auto,Oidn,Dlss,None};
struct HdNvpSettings {
    int samples=16,bounces=4;
    HdNvpDenoiser denoiser=HdNvpDenoiser::Auto;
    bool interactive=false;
};
struct UsdSceneSource {
    std::string filename, variants;
    double timeCode=1, metersPerUnit=1, timeCodesPerSecond=24;
    bool zUp=false, loadPayloads=true;
    uint64_t revision=0;
};
struct HdNvpFrame {
    std::vector<float> color,depth;
    std::vector<int32_t> primIds,instanceIds;
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
    HdNvpFrame render(const std::vector<Card>& cards,const Camera& camera,const Settings& settings,
                     const HdNvpSettings& quality={},const std::function<bool()>& abort={},
                     const std::vector<UsdSceneSource>& sources={});
private:
    struct Impl;std::unique_ptr<Impl> _impl;
};
}}}
#endif
