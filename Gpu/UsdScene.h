// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_USDSCENE_H
#define NATRON_GPU_USDSCENE_H
#include "Gpu/CardScene.h"

namespace Natron { namespace Gpu { namespace Cards {
struct UsdScenePose {
    std::vector<Card> cards;
    std::vector<std::string> meshPaths, warnings;
    std::vector<Light> lights;
};
// OpenUSD stays behind this interface so Engine and the Qt/Python bindings do
// not depend on USD headers. Every stage is read in RH Y-up meters.
class UsdSceneAsset {
public:
    static bool available();
    static std::shared_ptr<UsdSceneAsset> load(const std::string& filename,
        const std::string& variants = {}, bool loadPayloads = true);
    ~UsdSceneAsset();
    UsdScenePose evaluate(double seconds, bool animated = true,
                         const std::string& primPath = {}) const;
    Camera camera(const std::string& path, double seconds, bool animated = true) const;
    const std::vector<std::string>& cameras() const;
    const std::vector<std::string>& meshPaths() const;
    size_t lightCount(const std::string& primPath={}) const;
    double startTime() const;
    double endTime() const;
    double timeCodesPerSecond() const;
    double frameRate() const;
    double metersPerUnit() const;
    bool zUp() const;
    uint64_t revision() const;
    void reload();
private:
    struct Impl;
    explicit UsdSceneAsset(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> _impl;
};
}}}
#endif
