// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_SCENE_CAMERASOLVE_H
#define NATRON_SCENE_CAMERASOLVE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Natron { namespace Scene {

// Full-resolution coordinates, measured from the top-left image
// edge. The center of the first pixel is (0.5, 0.5). fx/fy/cx/cy use this domain.
struct CameraCalibration {
    int width = 0, height = 0;
    double fx = 0, fy = 0, cx = 0, cy = 0;
    // Brown radial model in normalized camera coordinates: 1+k1*r^2+k2*r^4.
    double k1 = 0, k2 = 0;
};

std::array<double,2> distortPixel(const CameraCalibration&, double x, double y);
std::array<double,2> undistortPixel(const CameraCalibration&, double x, double y);
bool validLens(const CameraCalibration&);

struct TrackObservation {
    double frame = 0;
    int64_t trackId = 0;
    double x = 0, y = 0, weight = 1;
    bool enabled = true;
};

struct SolvedCamera {
    double frame = 0;
    // Column-major camera-to-world affine matrix. Right-handed Y-up;
    // camera +X right, +Y up, -Z forward. Scale is arbitrary solve units.
    std::array<double, 16> transform{};
};

struct SolvedPoint {
    int64_t trackId = 0;
    std::array<double, 3> position{};
    size_t observations = 0;
    double rmsPixels = 0, parallaxDegrees = 0;
};

struct ReprojectionResidual {
    double frame = 0;
    int64_t trackId = 0;
    double errorPixels = 0;
};

struct CameraReconstruction {
    CameraCalibration calibration;
    std::vector<SolvedCamera> cameras;
    std::vector<SolvedPoint> points;
    std::vector<ReprojectionResidual> residuals;
    std::vector<double> unregisteredFrames;
    double rmsPixels = 0, maximumErrorPixels = 0;
    double keyframe1 = 0, keyframe2 = 0;
    std::string backend;
    bool estimatedIntrinsics = false;
    double calibrationCoverage = 0;
    bool calibrationAmbiguous = false;
    double focalMinimum = 0, focalMaximum = 0;
    bool estimatedPrincipalPoint = false;
    double principalPointValidationGain = 0;
    // First keyframe at identity, keyframe camera distance normalized to one.
    // No known-distance/survey constraint has established physical scale.
};

enum class CameraSolveFailure {
    InvalidInput, InsufficientParallax, Initialization, Reconstruction,
    Reprojection, Cancelled
};

class CameraSolveError : public std::runtime_error {
public:
    CameraSolveError(CameraSolveFailure reason, const std::string& message)
        : std::runtime_error(message), reason(reason) {}
    CameraSolveFailure reason;
};

struct CameraSolveOptions {
    enum class Intrinsics { Fixed, Focal, FocalAndRadial };
    Intrinsics intrinsics = Intrinsics::Fixed;
    bool refinePrincipalPoint = false;
    // Default: first/last observed frames. Automatic keyframe search and
    // robust outlier estimation are subsequent slices, not implicit here.
    std::optional<std::pair<double, double>> keyframes;
    double minimumParallaxDegrees = 0.5;
    double maximumRmsPixels = 1;
    double homographyThresholdPixels = 1;
    std::function<bool()> cancelled;
    // Called on the solving thread between libmv operations. Cancellation is
    // checked there too; it does not interrupt an individual Ceres minimizer.
    std::function<void(double, const std::string&)> progress;
};

// Known intrinsics, one static non-planar scene, one global-shutter camera,
// inlier tracks. Planar/pure-rotation keyframe pairs are rejected conservatively.
// Returns a value without Qt, Eigen, USD, renderer or live graph handles.
CameraReconstruction solveCamera(const CameraCalibration& calibration,
                                const std::vector<TrackObservation>& observations,
                                const CameraSolveOptions& options = {});

}} // namespace Natron::Scene
#endif
