// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/SceneFrame.h"
#include "Tests/CameraSolveFixture.h"
#include <cmath>
#include <iostream>
using namespace Natron::Scene;
using CameraSolveFixture::require;
template <class F> void invalid(F f)
{
    try
    {
        f();
    }
    catch (const std::invalid_argument&)
    {
        return;
    }
    throw std::runtime_error("Invalid scene frame operation accepted");
}
std::array<double, 2> project(
    const CameraCalibration& c, const SolvedCamera& camera, const std::array<double, 3>& p)
{
    double v[3] {};
    for (int a = 0; a < 3; ++a)
        for (int w = 0; w < 3; ++w)
            v[a] += camera.transform[a * 4 + w] * (p[w] - camera.transform[12 + w]);
    return { c.cx + c.fx * v[0] / -v[2], c.cy - c.fy * v[1] / -v[2] };
}
int main()
{
    try
    {
        const auto fixture = CameraSolveFixture::make();
        const auto raw = solveCamera(fixture.calibration, fixture.observations);
        std::vector<std::array<double, 3>> plane { raw.points[0].position, raw.points[7].position,
            raw.points[56].position };
        SceneFrame frame = groundFrame({}, plane);
        for (const auto& p : plane)
            require(std::abs(framePoint(frame, p)[1]) < 1e-8, "Ground does not map to Y=0");
        frame = originFrame(frame, { plane[0] });
        const auto origin = framePoint(frame, plane[0]);
        require(std::hypot(origin[0], origin[2]) < 1e-8, "Origin is not on selected ground point");
        frame = distanceFrame(frame, plane[0], plane[1], 2.5);
        require(frame.groundAligned && frame.metricScale, "Frame constraints were lost");
        const auto a = framePoint(frame, plane[0]), b = framePoint(frame, plane[1]);
        double distance = 0;
        for (int i = 0; i < 3; ++i)
            distance += std::pow(a[i] - b[i], 2);
        require(std::abs(std::sqrt(distance) - 2.5) < 1e-8, "Known-distance scale is incorrect");
        const auto aligned = applySceneFrame(raw, frame);
        double maximum = 0;
        for (size_t c = 0; c < raw.cameras.size(); ++c)
            for (size_t p = 0; p < raw.points.size(); ++p)
            {
                const auto before = project(raw.calibration, raw.cameras[c], raw.points[p].position),
                           after
                    = project(aligned.calibration, aligned.cameras[c], aligned.points[p].position);
                maximum = std::max(maximum, std::hypot(before[0] - after[0], before[1] - after[1]));
            }
        require(maximum < 1e-8, "Alignment changed camera/point reprojection");
        invalid([] { groundFrame({}, { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }); });
        invalid([] { groundFrame({}, { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }); });
        invalid([] { distanceFrame({}, { 0, 0, 0 }, { 0, 0, 0 }, 1); });
        invalid([] { distanceFrame({}, { 0, 0, 0 }, { 1, 0, 0 }, -1); });
        invalid(
            []
            {
                SceneFrame f;
                f.matrix[0] = -1;
                frameScale(f);
            });
        invalid(
            []
            {
                SceneFrame f;
                f.matrix[4] = .2;
                frameScale(f);
            });
        std::cout << "SceneFrame passed: ground/origin/metric scale; maximum reprojection change " << maximum
                  << " px\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
