// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/CameraSolve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <ceres/version.h>
#include <libmv/multiview/homography.h>
#include <libmv/simple_pipeline/initialize_reconstruction.h>
#include <libmv/simple_pipeline/pipeline.h>
#include <libmv/simple_pipeline/reconstruction.h>

namespace Natron { namespace Scene {
namespace {
constexpr double degrees = 180.0 / 3.14159265358979323846;
using libmv::Mat3;
using libmv::Vec3;

[[noreturn]] void fail(CameraSolveFailure reason, const char* message)
{
    throw CameraSolveError(reason, message);
}

void update(const CameraSolveOptions& options, double progress, const std::string& message)
{
    if (options.cancelled && options.cancelled()) {
        fail(CameraSolveFailure::Cancelled, "Camera solve cancelled");
    }
    if (options.progress) {
        options.progress(progress, message);
    }
}

struct Progress : libmv::ProgressUpdateCallback {
    const CameraSolveOptions& options;
    explicit Progress(const CameraSolveOptions& options) : options(options) {}
    void invoke(double progress, const char* message) override
    {
        update(options, 0.15 + 0.7 * std::clamp(progress, 0.0, 1.0), message);
    }
};

double angle(const Vec3& a, const Vec3& b)
{
    return std::acos(std::clamp(a.normalized().dot(b.normalized()), -1.0, 1.0)) * degrees;
}

double median(std::vector<double> values)
{
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}
} // namespace

CameraReconstruction solveCamera(const CameraCalibration& calibration,
                                const std::vector<TrackObservation>& observations,
                                const CameraSolveOptions& options)
{
    if (!validLens(calibration) || calibration.width <= 0 || calibration.height <= 0 ||
        !std::isfinite(calibration.fx) || calibration.fx <= 0 ||
        !std::isfinite(calibration.fy) || calibration.fy <= 0 ||
        !std::isfinite(calibration.cx) || !std::isfinite(calibration.cy) ||
        !std::isfinite(options.minimumParallaxDegrees) || options.minimumParallaxDegrees <= 0 ||
        options.minimumParallaxDegrees >= 90 ||
        !std::isfinite(options.maximumRmsPixels) || options.maximumRmsPixels <= 0 ||
        !std::isfinite(options.homographyThresholdPixels) || options.homographyThresholdPixels <= 0) {
        fail(CameraSolveFailure::InvalidInput, "Invalid camera calibration or solve tolerances");
    }
    update(options, 0, "Validating observations");
    // libmv uses dense integer frame/track indices, including in its bundler.
    // Remap deterministically so sparse frame numbers and 64-bit point IDs
    // neither allocate enormous vectors nor lose their persistent identities.
    std::map<double, int> frames;
    std::map<int64_t, int> tracks;
    std::map<std::pair<double, int64_t>, TrackObservation> active;
    for (const auto& observation : observations) {
        if (!observation.enabled) {
            continue;
        }
        if (!std::isfinite(observation.frame) || observation.trackId < 0 ||
            !std::isfinite(observation.x) || !std::isfinite(observation.y) ||
            !std::isfinite(observation.weight) || observation.weight < 0) {
            fail(CameraSolveFailure::InvalidInput, "Invalid track observation");
        }
        if (observation.weight == 0) {
            continue;
        }
        if (!active.emplace(std::make_pair(observation.frame, observation.trackId), observation).second) {
            fail(CameraSolveFailure::InvalidInput, "Duplicate frame/track observation");
        }
        frames.emplace(observation.frame, 0);
        tracks.emplace(observation.trackId, 0);
    }
    if (frames.size() < 2 || tracks.size() < 8 ||
        frames.size() > size_t(std::numeric_limits<int>::max()) ||
        tracks.size() > size_t(std::numeric_limits<int>::max())) {
        fail(CameraSolveFailure::InvalidInput, "Camera solve needs at least two frames and eight tracks");
    }
    std::vector<double> frameIds;
    std::vector<int64_t> trackIds;
    for (auto& frame : frames) {
        frame.second = int(frameIds.size());
        frameIds.push_back(frame.first);
    }
    for (auto& track : tracks) {
        track.second = int(trackIds.size());
        trackIds.push_back(track.first);
    }
    const auto keys = options.keyframes.value_or(std::make_pair(frameIds.front(), frameIds.back()));
    if (!std::isfinite(keys.first) || !std::isfinite(keys.second) || keys.first >= keys.second ||
        !frames.count(keys.first) || !frames.count(keys.second)) {
        fail(CameraSolveFailure::InvalidInput, "Keyframes must be two distinct observed frames in time order");
    }
    libmv::Tracks calibrated;
    for (const auto& entry : active) {
        const auto& o = entry.second;
        const auto ideal=undistortPixel(calibration,o.x,o.y);
        const double x = (ideal[0] - calibration.cx) / calibration.fx;
        const double y = (ideal[1] - calibration.cy) / calibration.fy;
        if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1e6 || std::abs(y) > 1e6) {
            fail(CameraSolveFailure::InvalidInput, "Observation cannot be normalized in the calibrated pinhole domain");
        }
        calibrated.Insert(frames.at(o.frame), tracks.at(o.trackId),
                          x, y, o.weight);
    }
    const int first = frames.at(keys.first), second = frames.at(keys.second);
    const auto common = calibrated.MarkersForTracksInBothImages(first, second);
    if (common.size() < 16) {
        fail(CameraSolveFailure::InvalidInput, "Keyframes need at least eight common inlier tracks");
    }
    const auto firstMarkers = calibrated.MarkersInImage(first);
    std::vector<std::pair<Vec3, Vec3>> bearings;
    Mat3 covariance = Mat3::Zero();
    for (const auto& marker : firstMarkers) {
        const auto other = calibrated.MarkerInImageForTrack(second, marker.track);
        if (other.image < 0) {
            continue;
        }
        const Vec3 u = Vec3(marker.x, marker.y, 1).normalized();
        const Vec3 v = Vec3(other.x, other.y, 1).normalized();
        bearings.emplace_back(u, v);
        covariance += v * u.transpose();
    }
    Eigen::JacobiSVD<Mat3> rotationFit(covariance, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Mat3 sign = Mat3::Identity();
    sign(2, 2) = (rotationFit.matrixU() * rotationFit.matrixV().transpose()).determinant();
    const Mat3 rotationOnly = rotationFit.matrixU() * sign * rotationFit.matrixV().transpose();
    std::vector<double> rotationResiduals;
    for (const auto& pair : bearings) {
        rotationResiduals.push_back(angle(rotationOnly * pair.first, pair.second));
    }
    if (median(rotationResiduals) < options.minimumParallaxDegrees * 0.1) {
        fail(CameraSolveFailure::InsufficientParallax, "Keyframes are consistent with rotation only or insufficient parallax");
    }
    // The eight-point initializer is ambiguous on a plane. Do not report a
    // plausible low-RMS but arbitrary-depth result when a homography explains
    // the selected keyframes within the requested pixel tolerance.
    libmv::Mat image1(2, bearings.size()), image2(2, bearings.size());
    for (size_t i = 0; i < bearings.size(); ++i) {
        image1.col(i) = bearings[i].first.head<2>() / bearings[i].first.z();
        image2.col(i) = bearings[i].second.head<2>() / bearings[i].second.z();
    }
    Mat3 homography = Mat3::Identity();
    libmv::Homography2DFromCorrespondencesLinear(image1, image2, &homography);
    if (homography.allFinite() && std::abs(homography.determinant()) > 1e-12) {
        std::vector<double> errors;
        for (int i = 0; i < image1.cols(); ++i) {
            const Vec3 p = homography * Vec3(image1(0, i), image1(1, i), 1);
            const double error = std::abs(p.z()) > 1e-12 ?
                std::hypot((p.x() / p.z() - image2(0, i)) * calibration.fx,
                           (p.y() / p.z() - image2(1, i)) * calibration.fy) :
                std::numeric_limits<double>::infinity();
            errors.push_back(error);
        }
        if (median(errors) < options.homographyThresholdPixels) {
            fail(CameraSolveFailure::InsufficientParallax, "Keyframes are explained by a planar homography; choose tracks with more depth variation");
        }
    }
    update(options, 0.1, "Initializing calibrated two-view reconstruction");
    libmv::EuclideanReconstruction reconstruction;
    if (!libmv::EuclideanReconstructTwoFrames(common, &reconstruction)) {
        fail(CameraSolveFailure::Initialization, "libmv could not initialize the selected keyframes");
    }
    Progress progress(options);
    libmv::EuclideanCompleteReconstruction(calibrated, &reconstruction, &progress);
    update(options, 0.9, "Validating reconstruction and reprojection");
    const auto anchor = reconstruction.CameraForImage(first);
    const auto endpoint = reconstruction.CameraForImage(second);
    if (!anchor || !endpoint || !anchor->R.allFinite() || !anchor->t.allFinite() ||
        !endpoint->R.allFinite() || !endpoint->t.allFinite()) {
        fail(CameraSolveFailure::Reconstruction, "Missing or non-finite keyframe cameras");
    }
    const Vec3 anchorCenter = -anchor->R.transpose() * anchor->t;
    const Vec3 endCenter = -endpoint->R.transpose() * endpoint->t;
    const double baseline = (endCenter - anchorCenter).norm();
    if (!std::isfinite(baseline) || baseline < 1e-12) {
        fail(CameraSolveFailure::InsufficientParallax, "Solved camera baseline is degenerate");
    }
    const Mat3 basis = (Vec3(1, -1, -1)).asDiagonal();
    CameraReconstruction result;
    result.calibration = calibration;
    result.keyframe1 = keys.first;
    result.keyframe2 = keys.second;
    result.backend = "vendored libmv / Ceres " CERES_VERSION_STRING;
    for (size_t i = 0; i < frameIds.size(); ++i) {
        const auto camera = reconstruction.CameraForImage(int(i));
        if (!camera) {
            result.unregisteredFrames.push_back(frameIds[i]);
            continue;
        }
        if (!camera->R.allFinite() || !camera->t.allFinite() ||
            (camera->R * camera->R.transpose() - Mat3::Identity()).norm() > 1e-6 ||
            std::abs(camera->R.determinant() - 1) > 1e-6) {
            fail(CameraSolveFailure::Reconstruction, "Solver returned an invalid camera pose");
        }
        const Mat3 rotation = basis * anchor->R * camera->R.transpose() * basis;
        const Vec3 center = basis * (anchor->R * (-camera->R.transpose() * camera->t) + anchor->t) / baseline;
        SolvedCamera solved;
        solved.frame = frameIds[i];
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                solved.transform[column * 4 + row] = rotation(row, column);
            }
            solved.transform[12 + column] = center(column);
        }
        solved.transform[15] = 1;
        result.cameras.push_back(solved);
    }
    double errorSquared = 0;
    for (size_t i = 0; i < trackIds.size(); ++i) {
        const auto point = reconstruction.PointForTrack(int(i));
        if (!point || !point->X.allFinite()) {
            continue;
        }
        std::vector<Vec3> rays;
        std::vector<ReprojectionResidual> residuals;
        bool valid = true;
        double squared = 0, parallax = 0;
        for (const auto& marker : calibrated.MarkersForTrack(int(i))) {
            const auto camera = reconstruction.CameraForImage(marker.image);
            if (!camera) {
                continue;
            }
            const Vec3 projected = camera->R * point->X + camera->t;
            if (!projected.allFinite() || projected.z() <= 1e-10) {
                valid = false;
                break;
            }
            const auto pixel=distortPixel(calibration,calibration.cx+calibration.fx*projected.x()/projected.z(),calibration.cy+calibration.fy*projected.y()/projected.z());
            const auto& observation=active.at({frameIds[marker.image],trackIds[i]});
            const double dx = pixel[0]-observation.x;
            const double dy = pixel[1]-observation.y;
            const double error = std::hypot(dx, dy);
            if (!std::isfinite(error)) {
                valid = false;
                break;
            }
            residuals.push_back({frameIds[marker.image], trackIds[i], error});
            squared += error * error;
            const Vec3 ray = camera->R.transpose() * Vec3(marker.x, marker.y, 1).normalized();
            for (const auto& previous : rays) {
                parallax = std::max(parallax, angle(previous, ray));
            }
            rays.push_back(ray);
        }
        if (!valid || residuals.size() < 2 || parallax < options.minimumParallaxDegrees) {
            continue;
        }
        const Vec3 position = basis * (anchor->R * point->X + anchor->t) / baseline;
        result.points.push_back({trackIds[i], {position.x(), position.y(), position.z()},
                                 residuals.size(), std::sqrt(squared / residuals.size()), parallax});
        errorSquared += squared;
        for (const auto& residual : residuals) {
            result.maximumErrorPixels = std::max(result.maximumErrorPixels, residual.errorPixels);
            result.residuals.push_back(residual);
        }
    }
    if (result.points.size() < 8 || result.residuals.empty()) {
        fail(CameraSolveFailure::InsufficientParallax, "Too few reliable triangulated landmarks");
    }
    result.rmsPixels = std::sqrt(errorSquared / result.residuals.size());
    if (result.rmsPixels > options.maximumRmsPixels) {
        fail(CameraSolveFailure::Reprojection, "Reprojection RMS exceeds the requested pixel tolerance; refine the input tracks");
    }
    update(options, 1, "Camera and sparse landmarks solved");
    return result;
}

}} // namespace Natron::Scene
