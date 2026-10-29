// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_SCENE_RECONSTRUCTIONUSD_H
#define NATRON_SCENE_RECONSTRUCTIONUSD_H
#include "Scene/CameraSolve.h"

namespace Natron { namespace Scene {

struct ReconstructionUsdOptions {
    std::string rootPath = "/Reconstruction";
    std::string solveId = "cameraSolve";
    double framesPerSecond = 24;
    double imagePixelAspect = 1;
    bool metricScale = false;
    double clipScale = 1;
};

// Authors a private USD layer containing an animated Camera and guide Points.
// No USD handles escape; callers can compose this layer or save it as an asset.
// Only complete solves are exported: interpolating across missing camera poses
// would otherwise suggest a solved camera where no pose was registered.
std::string reconstructionUsdLayer(const CameraReconstruction& reconstruction,
                                   const ReconstructionUsdOptions& options = {});

}} // namespace Natron::Scene
#endif
