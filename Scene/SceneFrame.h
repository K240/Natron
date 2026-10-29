// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_SCENE_SCENEFRAME_H
#define NATRON_SCENE_SCENEFRAME_H
#include "Scene/CameraSolve.h"
namespace Natron
{
namespace Scene
{
    struct SceneFrame
    {
        // Column-major raw reconstruction -> scene coordinates. Only a positive
        // uniform similarity is allowed, so camera bases remain rigid after scale
        // is separated from their positions and clipping ranges.
        std::array<double, 16> matrix { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        bool groundAligned = false, metricScale = false;
    };
    double frameScale(const SceneFrame&);
    std::array<double, 3> framePoint(const SceneFrame&, const std::array<double, 3>&);
    SceneFrame groundFrame(const SceneFrame&, const std::vector<std::array<double, 3>>& rawPoints);
    SceneFrame originFrame(const SceneFrame&, const std::vector<std::array<double, 3>>& rawPoints);
    SceneFrame distanceFrame(
        const SceneFrame&, const std::array<double, 3>& a, const std::array<double, 3>& b, double meters);
    CameraReconstruction applySceneFrame(const CameraReconstruction&, const SceneFrame&);
}
}
#endif
