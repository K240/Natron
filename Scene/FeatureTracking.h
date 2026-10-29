// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_SCENE_FEATURETRACKING_H
#define NATRON_SCENE_FEATURETRACKING_H
#include "Scene/CameraSolve.h"

namespace Natron { namespace Scene {
struct TrackingImage {
    int width = 0, height = 0;
    std::vector<float> pixels; // grayscale, top-down; pixel centers at integer indices
    std::vector<float> mask;   // optional, 0 excludes features and 1 includes them
};
struct FeatureTrackingOptions {
    int maximumFeatures = 300, minimumDistance = 12, patchRadius = 5;
    int pyramidLevels = 3, minimumTrackLength = 6, replenishInterval = 5;
    double minimumCorrelation = 0.85, maximumForwardBackwardError = 0.75;
};
struct FeatureTracks {
    int width = 0, height = 0;
    std::vector<double> frames;
    std::vector<TrackObservation> observations;
    size_t trackCount = 0;
};
using TrackingImageProvider = std::function<TrackingImage(double)>;
// Streams images; retains only two image pyramids and track observations.
// Published observations use the CameraSolve top-left pixel-edge convention.
FeatureTracks trackFeatures(const std::vector<double>& frames,
                            const TrackingImageProvider& images,
                            const FeatureTrackingOptions& options = {},
                            const CameraSolveOptions& control = {});

struct TrackedSolveOptions {
    CameraSolveOptions solve;
    bool automaticKeyframes = true;
    double inlierThresholdPixels = 1.5;
    int ransacIterations = 384;
};
// Robust keyframe initialization and residual-based refinement of automatic
// tracks. All requested image frames must register before a result is accepted.
CameraReconstruction solveTrackedCamera(const CameraCalibration& calibration,
                                       const FeatureTracks& tracks,
                                       const TrackedSolveOptions& options = {});

// Shared focal length and optional Brown k1/k2, constant over the shot.
// fy/fx is fixed. Principal-point refinement is optional and validation-gated;
// metric focal length requires a known sensor size. No lens EXIF is required.
CameraReconstruction estimateTrackedCamera(const CameraCalibration& imageDomain,
                                         const FeatureTracks& tracks,
                                         const TrackedSolveOptions& options);
}} // namespace Natron::Scene
#endif
