// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_TESTS_CAMERASOLVEFIXTURE_H
#define NATRON_TESTS_CAMERASOLVEFIXTURE_H
#include "Scene/CameraSolve.h"
#include <cmath>
#include <stdexcept>

namespace CameraSolveFixture {
using namespace Natron::Scene;
struct Fixture {
    CameraCalibration calibration{1024, 768, 900, 970, 483.25, 373.75};
    std::vector<TrackObservation> observations;
    std::vector<std::array<double, 3>> positions, centers;
    std::vector<std::array<double, 9>> rotations; // world-to-camera, row-major
};
inline Fixture make(double noise = 0, bool rotationOnly = false, bool planar = false)
{
    Fixture f;
    for (int i = 0; i < 64; ++i) {
        f.positions.push_back({(i % 8 - 3.5) * 0.5, (i / 8 - 3.5) * 0.35,
                               5 + (planar ? 0 : ((i * 17) % 23) * 0.16)});
    }
    for (int image = 0; image < 8; ++image) {
        const double yaw = image * 0.016, pitch = std::sin(image * 0.5) * 0.02;
        const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        const std::array<double, 9> r{cy, sy * sp, sy * cp, 0, cp, -sp, -sy, cy * sp, cy * cp};
        const std::array<double, 3> center{rotationOnly ? 0 : image * 0.16,
                                         rotationOnly ? 0 : std::sin(image) * 0.03,
                                         rotationOnly ? 0 : image * 0.025};
        f.centers.push_back(center);
        f.rotations.push_back(r);
        for (size_t track = 0; track < f.positions.size(); ++track) {
            std::array<double, 3> p{};
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    p[row] += r[row * 3 + column] * (f.positions[track][column] - center[column]);
                }
            }
            f.observations.push_back({1001 + image * 0.5, (int64_t(1) << 40) + int64_t(track) * 37,
                f.calibration.fx * p[0] / p[2] + f.calibration.cx + noise * std::sin(image * 13 + track * 0.7),
                f.calibration.fy * p[1] / p[2] + f.calibration.cy + noise * std::cos(image * 7 + track * 0.9)});
        }
    }
    return f;
}
inline void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template<class F> void fails(F function, CameraSolveFailure reason)
{
    try {
        function();
    } catch (const CameraSolveError& error) {
        require(error.reason == reason, "Unexpected camera solve failure category");
        return;
    }
    throw std::runtime_error("Invalid camera solve was accepted");
}
} // namespace CameraSolveFixture
#endif
