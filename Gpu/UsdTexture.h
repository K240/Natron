// SPDX-License-Identifier: GPL-2.0-or-later
// Private OpenUSD material-texture adapter; excluded from USD-disabled builds.
#ifndef NATRON_GPU_USDTEXTURE_H
#define NATRON_GPU_USDTEXTURE_H
#include "Gpu/CardScene.h"
#include <map>
#include <pxr/base/gf/vec2f.h>
#include <pxr/usd/usdShade/shader.h>
namespace Natron { namespace Gpu { namespace Cards {
struct UsdTextureBinding {
    std::shared_ptr<const Texture> texture;
    pxr::TfToken uvName;
    pxr::GfVec2f uvFallback{0};
    bool opacityConnected = false;
    unsigned channel = 0;
};
struct UsdTextureCache {
    struct Entry {UsdTextureBinding binding;std::vector<std::string> warnings;};
    std::map<std::string,Entry> entries, images;
};
UsdTextureBinding usdDiffuseTexture(const pxr::UsdShadeShader& surface,pxr::UsdTimeCode time,
    UsdTextureCache& cache,std::vector<std::string>& warnings);
UsdTextureBinding usdDataTexture(const pxr::UsdShadeShader& surface,const char* name,pxr::UsdTimeCode time,
    UsdTextureCache& cache,std::vector<std::string>& warnings);
std::shared_ptr<const Texture> usdEnvironmentTexture(const pxr::UsdPrim& light,pxr::UsdTimeCode time,
    UsdTextureCache& cache,std::vector<std::string>& warnings);
}}}
#endif
