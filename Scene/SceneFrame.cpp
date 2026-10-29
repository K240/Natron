// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/SceneFrame.h"
#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <Eigen/Geometry>
#include <algorithm>
#include <cmath>
namespace Natron
{
namespace Scene
{
    namespace
    {
        using Eigen::Matrix3d;
        using Eigen::Matrix4d;
        using Eigen::Vector3d;
        Matrix4d matrix(const SceneFrame& f)
        {
            return Eigen::Map<const Matrix4d>(f.matrix.data());
        }
        SceneFrame withDelta(const SceneFrame& f, const Matrix3d& r, const Vector3d& t)
        {
            Matrix4d d = Matrix4d::Identity();
            d.block<3, 3>(0, 0) = r;
            d.block<3, 1>(0, 3) = t;
            SceneFrame result = f;
            Eigen::Map<Matrix4d>(result.matrix.data()) = d * matrix(f);
            frameScale(result);
            return result;
        }
        Vector3d centroid(const SceneFrame& f, const std::vector<std::array<double, 3>>& points)
        {
            if (points.empty())
                throw std::invalid_argument("Select reconstruction landmarks first");
            Vector3d c = Vector3d::Zero();
            for (const auto& p : points)
            {
                for (double v : p)
                    if (!std::isfinite(v))
                        throw std::invalid_argument("Landmarks must be finite");
                const auto q = framePoint(f, p);
                c += Vector3d(q.data());
            }
            return c / points.size();
        }
    }
    double frameScale(const SceneFrame& f)
    {
        const auto m = matrix(f);
        const Matrix3d a = m.block<3, 3>(0, 0);
        const double scale = a.col(0).norm();
        if (!m.allFinite() || scale < 1e-9 || scale > 1e9 || a.determinant() <= 0
            || (a.transpose() * a - Matrix3d::Identity() * scale * scale).norm() > 1e-7 * scale * scale
            || std::abs(m(3, 0)) + std::abs(m(3, 1)) + std::abs(m(3, 2)) + std::abs(m(3, 3) - 1) > 1e-10)
            throw std::invalid_argument(
                "Scene frame must be a finite, positive uniform similarity transform");
        return scale;
    }
    std::array<double, 3> framePoint(const SceneFrame& f, const std::array<double, 3>& p)
    {
        const auto& m = f.matrix;
        return { m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] };
    }
    SceneFrame groundFrame(const SceneFrame& f, const std::vector<std::array<double, 3>>& points)
    {
        frameScale(f);
        if (points.size() < 3)
            throw std::invalid_argument("Select at least three non-collinear ground landmarks");
        const Vector3d c = centroid(f, points);
        Matrix3d covariance = Matrix3d::Zero();
        for (const auto& p : points)
        {
            const auto q = framePoint(f, p);
            const Vector3d d = Vector3d(q.data()) - c;
            covariance += d * d.transpose();
        }
        Eigen::SelfAdjointEigenSolver<Matrix3d> fit(covariance);
        const auto values = fit.eigenvalues();
        if (fit.info() != Eigen::Success || values[2] < 1e-16 || values[1] < values[2] * 1e-6)
            throw std::invalid_argument("Ground landmarks are coincident or collinear");
        if (values[0] > values[1] * .02)
            throw std::invalid_argument("Ground landmarks do not form a sufficiently flat plane");
        Vector3d y = fit.eigenvectors().col(0);
        if (y.y() < 0)
            y = -y;
        const auto a = framePoint(f, points[0]), b = framePoint(f, points[1]);
        Vector3d x = Vector3d(b.data()) - Vector3d(a.data());
        x -= y * y.dot(x);
        if (x.norm() < 1e-9)
            throw std::invalid_argument("Choose distinct first and second ground landmarks");
        x.normalize();
        const Vector3d z = x.cross(y).normalized();
        Matrix3d r;
        r.row(0) = x.transpose();
        r.row(1) = y.transpose();
        r.row(2) = z.transpose();
        auto result = withDelta(f, r, -r * c);
        result.groundAligned = true;
        return result;
    }
    SceneFrame originFrame(const SceneFrame& f, const std::vector<std::array<double, 3>>& points)
    {
        frameScale(f);
        Vector3d t = -centroid(f, points);
        if (f.groundAligned)
            t.y() = 0;
        return withDelta(f, Matrix3d::Identity(), t);
    }
    SceneFrame distanceFrame(
        const SceneFrame& f, const std::array<double, 3>& a, const std::array<double, 3>& b, double meters)
    {
        frameScale(f);
        const auto p = framePoint(f, a), q = framePoint(f, b);
        const double distance = (Vector3d(p.data()) - Vector3d(q.data())).norm();
        if (!std::isfinite(meters) || meters <= 0 || distance < 1e-9)
            throw std::invalid_argument(
                "Select two distinct landmarks and a positive known distance in meters");
        auto result = withDelta(f, Matrix3d::Identity() * (meters / distance), Vector3d::Zero());
        result.metricScale = true;
        return result;
    }
    CameraReconstruction applySceneFrame(const CameraReconstruction& raw, const SceneFrame& f)
    {
        const double scale = frameScale(f);
        auto result = raw;
        for (auto& point : result.points)
            point.position = framePoint(f, point.position);
        for (auto& camera : result.cameras)
        {
            const auto original = camera.transform;
            for (int col = 0; col < 3; ++col)
                for (int row = 0; row < 3; ++row)
                {
                    camera.transform[col * 4 + row] = 0;
                    for (int k = 0; k < 3; ++k)
                        camera.transform[col * 4 + row]
                            += f.matrix[k * 4 + row] * original[col * 4 + k] / scale;
                }
            const auto center = framePoint(f, { original[12], original[13], original[14] });
            for (int i = 0; i < 3; ++i)
                camera.transform[12 + i] = center[i];
        }
        return result;
    }
}
}
