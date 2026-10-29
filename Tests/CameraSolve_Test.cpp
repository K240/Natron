// SPDX-License-Identifier: GPL-2.0-or-later
#include "Tests/CameraSolveFixture.h"
#include <algorithm>
#include <iostream>
#include <limits>

using namespace CameraSolveFixture;
int main()
{
    try {
        const auto fixture = make();
        auto observations = fixture.observations;
        auto disabled = observations.front();
        disabled.enabled = false;
        disabled.x = std::numeric_limits<double>::quiet_NaN();
        observations.push_back(disabled);
        std::reverse(observations.begin(), observations.end());
        const auto result = solveCamera(fixture.calibration, observations);
        require(result.cameras.size() == 8 && result.points.size() == 64 && result.residuals.size() == 512,
                "Camera/landmark/observation coverage was lost");
        require(result.unregisteredFrames.empty() && result.rmsPixels < 1e-5, "Exact reprojection failed");
        const auto& end = fixture.centers.back();
        const double baseline = std::sqrt(end[0] * end[0] + end[1] * end[1] + end[2] * end[2]);
        for (size_t i = 0; i < result.cameras.size(); ++i) {
            const auto& camera = result.cameras[i];
            require(camera.frame == 1001 + i * 0.5, "Frame/subframe identity changed");
            for (int axis = 0; axis < 3; ++axis) {
                const double sign = axis == 0 ? 1 : -1;
                require(std::abs(camera.transform[12 + axis] - sign * fixture.centers[i][axis] / baseline) < 1e-5,
                        "Recovered camera position differs from ground truth up to similarity");
                for (int column = 0; column < 3; ++column) {
                    const double otherSign = column == 0 ? 1 : -1;
                    require(std::abs(camera.transform[column * 4 + axis] - sign * otherSign * fixture.rotations[i][column * 3 + axis]) < 1e-5,
                            "Recovered camera orientation differs from ground truth");
                }
            }
        }
        for (size_t i = 0; i < result.points.size(); ++i) {
            const auto& point = result.points[i];
            require(point.trackId == (int64_t(1) << 40) + int64_t(i) * 37, "64-bit track identity changed");
            for (int axis = 0; axis < 3; ++axis) {
                require(std::abs(point.position[axis] - (axis == 0 ? 1 : -1) * fixture.positions[i][axis] / baseline) < 1e-4,
                        "Recovered landmark differs from ground truth up to similarity");
            }
        }
        CameraSolveOptions manualKeys;
        manualKeys.keyframes = {1001.5, 1003};
        const auto manual = solveCamera(fixture.calibration, fixture.observations, manualKeys);
        require(manual.cameras.size() == 8 && manual.rmsPixels < 1e-5, "Explicit keyframe reconstruction failed");
        const auto& anchor = manual.cameras[1].transform;
        for (int i = 0; i < 16; ++i) {
            require(std::abs(anchor[size_t(i)] - (i % 5 == 0 ? 1 : 0)) < 1e-8,
                    "Explicit first keyframe did not define the solve coordinate frame");
        }
        const auto noisy = make(0.15);
        std::vector<TrackObservation> training, heldOut;
        for (size_t i = 0; i < noisy.observations.size(); ++i) {
            const size_t frame = i / 64, point = i % 64;
            (frame > 0 && frame < 7 && point % 8 == frame ? heldOut : training).push_back(noisy.observations[i]);
        }
        const auto refined = solveCamera(noisy.calibration, training);
        require(refined.cameras.size() == 8 && refined.points.size() == 64 && refined.rmsPixels < 0.25,
                "Noisy-track bundle adjustment failed");
        require(refined.residuals.size() == training.size(), "Held-out observations leaked into optimization");
        // Clean ground-truth projections independently check the noisy solve.
        double heldOutSquared = 0;
        for (const auto& o : fixture.observations) {
            const auto& camera = refined.cameras[size_t((o.frame - 1001) * 2)];
            const auto& point = refined.points[size_t((o.trackId - (int64_t(1) << 40)) / 37)];
            double p[3]{};
            for (int axis = 0; axis < 3; ++axis) {
                for (int world = 0; world < 3; ++world) {
                    p[axis] += camera.transform[axis * 4 + world] * (point.position[world] - camera.transform[12 + world]);
                }
            }
            const double dx = noisy.calibration.fx * p[0] / -p[2] + noisy.calibration.cx - o.x;
            const double dy = noisy.calibration.fy * -p[1] / -p[2] + noisy.calibration.cy - o.y;
            heldOutSquared += dx * dx + dy * dy;
        }
        require(std::sqrt(heldOutSquared / fixture.observations.size()) < 0.15, "Noisy solve did not recover the independent clean projections");
        double heldOutError = 0;
        for (const auto& o : heldOut) {
            const auto& camera = refined.cameras[size_t((o.frame - 1001) * 2)];
            const auto& point = refined.points[size_t((o.trackId - (int64_t(1) << 40)) / 37)];
            double p[3]{};
            for (int axis = 0; axis < 3; ++axis) {
                for (int world = 0; world < 3; ++world) {
                    p[axis] += camera.transform[axis * 4 + world] * (point.position[world] - camera.transform[12 + world]);
                }
            }
            const double dx = noisy.calibration.fx * p[0] / -p[2] + noisy.calibration.cx - o.x;
            const double dy = noisy.calibration.fy * -p[1] / -p[2] + noisy.calibration.cy - o.y;
            heldOutError += dx * dx + dy * dy;
        }
        require(std::sqrt(heldOutError / heldOut.size()) < 0.3, "Held-out reprojection failed");
        CameraSolveOptions strict;
        strict.maximumRmsPixels = 0.001;
        fails([&] { solveCamera(noisy.calibration, training, strict); }, CameraSolveFailure::Reprojection);
        auto duplicate = fixture.observations;
        duplicate.push_back(duplicate.front());
        fails([&] { solveCamera(fixture.calibration, duplicate); }, CameraSolveFailure::InvalidInput);
        auto invalid = fixture.calibration;
        invalid.fx = 0;
        fails([&] { solveCamera(invalid, fixture.observations); }, CameraSolveFailure::InvalidInput);
        invalid.fx = std::numeric_limits<double>::min();
        fails([&] { solveCamera(invalid, fixture.observations); }, CameraSolveFailure::InvalidInput);
        fails([&] { solveCamera(fixture.calibration, {}); }, CameraSolveFailure::InvalidInput);
        const auto rotation = make(0.01, true);
        fails([&] { solveCamera(rotation.calibration, rotation.observations); }, CameraSolveFailure::InsufficientParallax);
        const auto plane = make(0.1, false, true);
        fails([&] { solveCamera(plane.calibration, plane.observations); }, CameraSolveFailure::InsufficientParallax);
        CameraSolveOptions invalidKeys;
        invalidKeys.keyframes = {1001, 9000};
        fails([&] { solveCamera(fixture.calibration, observations, invalidKeys); }, CameraSolveFailure::InvalidInput);
        auto partial = fixture.observations;
        for (int i = 0; i < 4; ++i) {
            auto o = fixture.observations[size_t(i)];
            o.frame = 1002.25;
            partial.push_back(o);
        }
        const auto incomplete = solveCamera(fixture.calibration, partial);
        require(incomplete.cameras.size() == 8 && incomplete.unregisteredFrames == std::vector<double>{1002.25},
                "Unregistered frame was hidden or a camera pose fabricated");
        CameraSolveOptions cancelled;
        cancelled.cancelled = [] { return true; };
        fails([&] { solveCamera(fixture.calibration, observations, cancelled); }, CameraSolveFailure::Cancelled);
        bool completionStarted = false;
        cancelled.cancelled = [&] { return completionStarted; };
        cancelled.progress = [&](double value, const std::string&) { if (value >= 0.15) completionStarted = true; };
        fails([&] { solveCamera(fixture.calibration, observations, cancelled); }, CameraSolveFailure::Cancelled);
        std::cout << "libmv/Ceres camera solve passed: 8 cameras, 64 landmarks, 512 observations; exact RMS "
                  << result.rmsPixels << " px, noisy RMS " << refined.rmsPixels << " px, clean validation RMS "
                  << std::sqrt(heldOutSquared / fixture.observations.size()) << " px; held-out RMS "
                  << std::sqrt(heldOutError / heldOut.size()) << " px (" << heldOut.size() << " observations).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
