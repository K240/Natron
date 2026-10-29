// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_FBXMESH_H
#define NATRON_GPU_FBXMESH_H
#include "Gpu/CardScene.h"
namespace Natron { namespace Gpu { namespace Cards {
struct FbxAnimationTake {
    std::string name;
    double begin = 0, end = 0;
};
// Retain the parsed scene; evaluated snapshots own their vertices/BVH and do
// not reference ufbx data. Static geometry preserves the unanimated file pose.
class FbxMeshAsset {
public:
    // Camera-only loading skips mesh buffers, including camera-only FBX files.
    static std::shared_ptr<FbxMeshAsset> load(const std::string& filename, bool loadGeometry = true);
    ~FbxMeshAsset();
    const std::vector<FbxAnimationTake>& takes() const;
    double frameRate() const;
    std::shared_ptr<const Mesh> staticMesh() const;
    // Seconds relative to the selected take's beginning; blank selects default.
    std::shared_ptr<const Mesh> evaluate(double seconds, const std::string& take = {}, bool applySkinTransform = false) const;
    const std::vector<std::string>& cameras() const;
    // Names are node hierarchy paths; a unique leaf name is also accepted.
    // Blank selects the first camera. Horizontal framing is preserved; the
    // rendering/source-image aspect defines vertical framing in Natron.
    Camera camera(const std::string& name = {}, bool animated = false, double seconds = 0,
                  const std::string& take = {}) const;
private:
    FbxMeshAsset();
    struct Impl;
    std::unique_ptr<Impl> _imp;
};
// Static FBX geometry, including node/geometric transforms, normalized to
// right-handed Y-up meters. Reads ASCII and binary FBX through vendored ufbx.
std::shared_ptr<const Mesh> loadFbxMesh(const std::string& filename);
}}}
#endif
