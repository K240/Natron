// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/ReconstructionUsd.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/xform.h>

PXR_NAMESPACE_USING_DIRECTIVE

namespace Natron { namespace Scene {
std::string reconstructionUsdLayer(const CameraReconstruction& result,
                                   const ReconstructionUsdOptions& options)
{
    const auto& c = result.calibration;
    if (!validLens(c) || options.solveId.empty() || !std::isfinite(options.framesPerSecond) || options.framesPerSecond <= 0 ||
        !std::isfinite(options.imagePixelAspect) || options.imagePixelAspect <= 0 || !std::isfinite(options.clipScale) || options.clipScale<=0 ||
        !SdfPath::IsValidPathString(options.rootPath) ||
        c.width <= 0 || c.height <= 0 || !std::isfinite(c.fx) || c.fx <= 0 ||
        !std::isfinite(c.fy) || c.fy <= 0 || !std::isfinite(c.cx) || !std::isfinite(c.cy) ||
        result.cameras.size() < 2 || result.points.empty() || !result.unregisteredFrames.empty()) {
        throw std::invalid_argument("USD reconstruction needs valid calibration, identity, FPS and a complete solve");
    }
    const SdfPath rootPath(options.rootPath);
    if (!rootPath.IsAbsoluteRootOrPrimPath() || rootPath.IsAbsoluteRootPath() || rootPath.ContainsPrimVariantSelection()) {
        throw std::invalid_argument("Reconstruction root must be an absolute prim path");
    }
    const auto stage = UsdStage::CreateInMemory();
    UsdGeomSetStageUpAxis(stage, UsdGeomTokens->y);
    UsdGeomSetStageMetersPerUnit(stage, 1);
    stage->SetTimeCodesPerSecond(options.framesPerSecond);
    stage->SetFramesPerSecond(options.framesPerSecond);
    const auto root = UsdGeomXform::Define(stage, rootPath).GetPrim();
    stage->SetDefaultPrim(stage->GetPrimAtPath(rootPath.GetPrefixes().front()));
    root.SetCustomDataByKey(TfToken("natron:solveId"), VtValue(options.solveId));
    root.SetCustomDataByKey(TfToken("natron:owner"), VtValue(options.solveId));
    root.SetCustomDataByKey(TfToken("natron:scaleStatus"), VtValue(std::string(options.metricScale?"metric":"arbitrary")));
    root.SetCustomDataByKey(TfToken("natron:backend"), VtValue(result.backend));
    const bool distorted=c.k1!=0 || c.k2!=0;
    root.SetCustomDataByKey(TfToken("natron:observationDomain"), VtValue(std::string(distorted?"distorted-top-left-pixel-edge":"undistorted-top-left-pixel-edge")));
    root.SetCustomDataByKey(TfToken("natron:plateRequiresUndistort"),VtValue(distorted));
    root.SetCustomDataByKey(TfToken("natron:estimatedIntrinsics"),VtValue(result.estimatedIntrinsics));
    root.SetCustomDataByKey(TfToken("natron:estimatedPrincipalPoint"),VtValue(result.estimatedPrincipalPoint));
    root.SetCustomDataByKey(TfToken("natron:principalPointValidationGain"),VtValue(result.principalPointValidationGain));
    root.SetCustomDataByKey(TfToken("natron:calibrationAmbiguous"),VtValue(result.calibrationAmbiguous));
    root.SetCustomDataByKey(TfToken("natron:radialK1"),VtValue(c.k1));
    root.SetCustomDataByKey(TfToken("natron:radialK2"),VtValue(c.k2));
    root.SetCustomDataByKey(TfToken("natron:imageWidth"), VtValue(c.width));
    root.SetCustomDataByKey(TfToken("natron:imageHeight"), VtValue(c.height));
    root.SetCustomDataByKey(TfToken("natron:pixelAspect"), VtValue(options.imagePixelAspect));
    root.SetCustomDataByKey(TfToken("natron:reprojectionRmsPixels"), VtValue(result.rmsPixels));

    const auto camera = UsdGeomCamera::Define(stage, rootPath.AppendChild(TfToken("Camera")));
    // USD focal/aperture values are in tenths of a scene unit. Here a scene
    // unit is a meter, so one millimeter is 0.01 camera attribute units.
    const double apertureMm = 36;
    const double focalMm = c.fx * apertureMm / c.width;
    const double verticalMm = focalMm * c.height / c.fy;
    camera.CreateProjectionAttr().Set(UsdGeomTokens->perspective);
    camera.CreateFocalLengthAttr().Set(float(focalMm * 0.01));
    camera.CreateHorizontalApertureAttr().Set(float(apertureMm * 0.01));
    camera.CreateVerticalApertureAttr().Set(float(verticalMm * 0.01));
    camera.CreateHorizontalApertureOffsetAttr().Set(float((c.width * 0.5 - c.cx) * apertureMm / c.width * 0.01));
    camera.CreateVerticalApertureOffsetAttr().Set(float((c.cy - c.height * 0.5) * verticalMm / c.height * 0.01));
    camera.CreateClippingRangeAttr().Set(GfVec2f(float(.001*options.clipScale),float(1000000*options.clipScale)));
    const auto transform = camera.AddTransformOp();
    std::set<double> frames;
    for (const auto& pose : result.cameras) {
        if (!std::isfinite(pose.frame) || !frames.insert(pose.frame).second ||
            !std::all_of(pose.transform.begin(), pose.transform.end(), [](double v) { return std::isfinite(v); }) ||
            pose.transform[3] != 0 || pose.transform[7] != 0 || pose.transform[11] != 0 || pose.transform[15] != 1) {
            throw std::invalid_argument("Invalid reconstructed camera sample");
        }
        GfMatrix4d matrix(1);
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                matrix[row][column] = pose.transform[row * 4 + column];
            }
        }
        transform.Set(matrix, UsdTimeCode(pose.frame));
    }
    stage->SetStartTimeCode(*frames.begin());
    stage->SetEndTimeCode(*frames.rbegin());

    auto cloud = UsdGeomPoints::Define(stage, rootPath.AppendChild(TfToken("Landmarks")));
    cloud.CreatePurposeAttr().Set(UsdGeomTokens->guide);
    cloud.CreateWidthsAttr().Set(VtFloatArray{0.01f});
    cloud.SetWidthsInterpolation(UsdGeomTokens->constant);
    cloud.CreateDisplayColorAttr().Set(VtVec3fArray{GfVec3f(1, 0.6f, 0.1f)});
    VtVec3fArray positions;
    VtInt64Array ids;
    VtFloatArray errors, angles;
    VtIntArray support;
    std::set<int64_t> uniqueIds;
    GfVec3f minimum(std::numeric_limits<float>::max()), maximum(-std::numeric_limits<float>::max());
    for (const auto& point : result.points) {
        if (point.trackId < 0 || !uniqueIds.insert(point.trackId).second ||
            !std::isfinite(point.rmsPixels) || !std::isfinite(point.parallaxDegrees) ||
            !std::all_of(point.position.begin(), point.position.end(), [](double v) { return std::isfinite(v) && std::abs(v) <= std::numeric_limits<float>::max(); })) {
            throw std::invalid_argument("Invalid reconstructed landmark");
        }
        const GfVec3f p(point.position[0], point.position[1], point.position[2]);
        positions.push_back(p);
        ids.push_back(point.trackId);
        errors.push_back(float(point.rmsPixels));
        angles.push_back(float(point.parallaxDegrees));
        support.push_back(int(point.observations));
        for (int i = 0; i < 3; ++i) {
            minimum[i] = std::min(minimum[i], p[i]);
            maximum[i] = std::max(maximum[i], p[i]);
        }
    }
    cloud.CreatePointsAttr().Set(positions);
    cloud.CreateIdsAttr().Set(ids);
    cloud.CreateExtentAttr().Set(VtVec3fArray{minimum, maximum});
    UsdGeomPrimvarsAPI primvars(cloud);
    primvars.CreatePrimvar(TfToken("natron:reprojectionErrorPixels"), SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).Set(errors);
    primvars.CreatePrimvar(TfToken("natron:parallaxDegrees"), SdfValueTypeNames->FloatArray, UsdGeomTokens->vertex).Set(angles);
    primvars.CreatePrimvar(TfToken("natron:observationCount"), SdfValueTypeNames->IntArray, UsdGeomTokens->vertex).Set(support);
    std::string layer;
    if (!stage->GetRootLayer()->ExportToString(&layer)) {
        throw std::runtime_error("Cannot serialize reconstructed USD scene");
    }
    return layer;
}
}} // namespace Natron::Scene
