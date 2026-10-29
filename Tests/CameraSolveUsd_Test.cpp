// SPDX-License-Identifier: GPL-2.0-or-later
#include "Tests/CameraSolveFixture.h"
#include "Scene/ReconstructionUsd.h"
#include "Scene/SceneFrame.h"
#include "Gpu/UsdStageValue.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>
#include <pxr/base/gf/camera.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace CameraSolveFixture;
namespace {
void write(const QString& filename, const std::string& text)
{
    QFile file(filename);
    require(file.open(QIODevice::WriteOnly), "Cannot write reconstruction fixture");
    require(file.write(text.data(), qint64(text.size())) == qint64(text.size()), "Truncated reconstruction fixture");
}
template<class F> void invalid(F function)
{
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid USD reconstruction was exported");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        const auto fixture = make();
        const auto result = solveCamera(fixture.calibration, fixture.observations);
        ReconstructionUsdOptions options;
        options.solveId = "synthetic-solve";
        options.imagePixelAspect=1.2;
        const auto text = reconstructionUsdLayer(result, options);
        QTemporaryDir directory;
        require(directory.isValid(), "No temporary directory");
        const auto filename = directory.filePath(QStringLiteral("reconstruction.usda"));
        write(filename, text);
        const auto stage = UsdStage::Open(filename.toStdString());
        require(bool(stage), "Cannot reopen reconstructed USD scene");
        require(stage->GetDefaultPrim().GetPath() == SdfPath("/Reconstruction"), "Reconstruction default prim lost");
        require(UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->y && UsdGeomGetStageMetersPerUnit(stage) == 1,
                "Native scene coordinate metadata changed");
        require(stage->GetDefaultPrim().GetCustomDataByKey(TfToken("natron:scaleStatus")).Get<std::string>() == "arbitrary",
                "Uncalibrated solve falsely claims metric scale");
        require(stage->GetDefaultPrim().GetCustomDataByKey(TfToken("natron:pixelAspect")).Get<double>()==1.2,
                "Image pixel aspect was confused with unequal focal calibration");
        const UsdGeomCamera camera(stage->GetPrimAtPath(SdfPath("/Reconstruction/Camera")));
        const UsdGeomPoints cloud(stage->GetPrimAtPath(SdfPath("/Reconstruction/Landmarks")));
        require(bool(camera) && bool(cloud), "Camera or Points prim missing");
        TfToken purpose;
        cloud.GetPurposeAttr().Get(&purpose);
        require(purpose == UsdGeomTokens->guide, "Landmarks must default to guide purpose");
        VtInt64Array ids;
        VtVec3fArray points;
        cloud.GetIdsAttr().Get(&ids);
        cloud.GetPointsAttr().Get(&points);
        require(ids.size() == 64 && points.size() == 64, "USD point coverage changed");
        VtIntArray support;
        UsdGeomPrimvarsAPI(cloud).GetPrimvar(TfToken("natron:observationCount")).Get(&support);
        require(support.size() == 64 && support.front() == 8, "Point support data lost");
        double maximumPixelError = 0;
        // USD's own camera/frustum implementation is independent of the solve
        // adapter. It checks both basis conversion and off-center fx/fy optics.
        for (size_t frame = 0; frame < result.cameras.size(); ++frame) {
            const auto frustum = camera.GetCamera(UsdTimeCode(result.cameras[frame].frame)).GetFrustum();
            const auto viewProjection = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix();
            for (size_t point = 0; point < points.size(); ++point) {
                require(ids[point] == result.points[point].trackId, "USD int64 point IDs changed");
                const auto ndc = viewProjection.Transform(GfVec3d(points[point]));
                const double x = (ndc[0] + 1) * fixture.calibration.width * 0.5;
                const double y = (1 - ndc[1]) * fixture.calibration.height * 0.5;
                const auto& observation = fixture.observations[frame * points.size() + point];
                maximumPixelError = std::max(maximumPixelError, std::hypot(x - observation.x, y - observation.y));
            }
        }
        require(maximumPixelError < 0.001, "USD camera/points no longer reproject onto the plate observations");

        auto frame=groundFrame({}, {result.points[0].position,result.points[7].position,result.points[56].position});
        frame=distanceFrame(frame,result.points[0].position,result.points[7].position,2.5);
        auto alignedOptions=options;alignedOptions.metricScale=true;alignedOptions.clipScale=frameScale(frame);
        const auto alignedFile=directory.filePath(QStringLiteral("aligned.usda"));
        write(alignedFile,reconstructionUsdLayer(applySceneFrame(result,frame),alignedOptions));
        const auto alignedStage=UsdStage::Open(alignedFile.toStdString());
        require(alignedStage->GetDefaultPrim().GetCustomDataByKey(TfToken("natron:scaleStatus")).Get<std::string>()=="metric","Known-distance USD lost metric status");
        const UsdGeomCamera alignedCamera(alignedStage->GetPrimAtPath(SdfPath("/Reconstruction/Camera")));
        VtVec3fArray alignedPoints;UsdGeomPoints(alignedStage->GetPrimAtPath(SdfPath("/Reconstruction/Landmarks"))).GetPointsAttr().Get(&alignedPoints);
        GfVec2f alignedClip;alignedCamera.GetClippingRangeAttr().Get(&alignedClip);
        require(std::abs(alignedClip[0]-.001*alignedOptions.clipScale)<1e-6,"Scene scale did not preserve relative camera clipping");
        for(size_t c=0;c<result.cameras.size();++c){
            const auto before=camera.GetCamera(UsdTimeCode(result.cameras[c].frame)).GetFrustum();
            const auto after=alignedCamera.GetCamera(UsdTimeCode(result.cameras[c].frame)).GetFrustum();
            const auto a=before.ComputeViewMatrix()*before.ComputeProjectionMatrix(),b=after.ComputeViewMatrix()*after.ComputeProjectionMatrix();
            const auto basis=alignedCamera.GetCamera(UsdTimeCode(result.cameras[c].frame)).GetTransform();
            require(std::abs(GfVec3d(basis[0][0],basis[0][1],basis[0][2]).GetLength()-1)<1e-9,"Metric scale leaked into the USD camera basis");
            for(size_t p=0;p<points.size();++p){const auto u=a.Transform(GfVec3d(points[p])),v=b.Transform(GfVec3d(alignedPoints[p]));
                require(std::hypot((u[0]-v[0])*fixture.calibration.width/2,(u[1]-v[1])*fixture.calibration.height/2)<.001,"Aligned USD changed reconstruction reprojection");}
        }
        auto invalidScale=options;invalidScale.clipScale=0;invalid([&]{reconstructionUsdLayer(result,invalidScale);});

        // Exercise the current branch-private composition/Outline/camera seam,
        // not just standalone USD serialization. No renderer or GPU is started.
        using namespace Natron::Gpu::Cards;
        UsdSceneSource source;
        source.filename = filename.toStdString();
        source.ownerId = options.solveId;
        source.mountPath = "/Natron/Assets/solve";
        source.timeCode = 1002.5;
        const std::string authored = "#usda 1.0\n(metersPerUnit = 1; upAxis = \"Y\"; timeCodesPerSecond = 24)\ndef Xform \"Natron\" {}\n";
        const auto composed = UsdSceneValue::compose(authored, {source}, 73);
        const auto page = composed->children("/Natron/Assets/solve/Source");
        require(page.prims.size() == 2, "Composed stage lost camera/point cloud hierarchy");
        require(composed->inspect("/Natron/Assets/solve/Source/Landmarks").type == "Points", "Outline cannot inspect the point cloud prim");
        const auto pickedCamera = composed->camera("/Natron/Assets/solve/Source/Camera");
        const auto& expected = result.cameras[3].transform;
        for (size_t i = 0; i < expected.size(); ++i) {
            require(std::abs(pickedCamera.transform.m[i] - expected[i]) < 1e-8, "Scene camera selector lost the solved world transform");
        }
        require(std::abs(pickedCamera.aperture / pickedCamera.focal - double(fixture.calibration.width) / fixture.calibration.fx) < 1e-7,
                "Scene camera selector lost calibrated horizontal framing");
        const auto calibrated=composed->camera("/Natron/Assets/solve/Source/Camera",true);
        Settings image;image.width=fixture.calibration.width;image.height=fixture.calibration.height;
        // Check the actual CPU render rays against off-center unequal fx/fy,
        // independently of USD's frustum computation above.
        for(size_t p=0;p<result.points.size();++p){
            const auto& o=fixture.observations[3*result.points.size()+p];
            const auto& point=result.points[p].position;
            Card card;card.transform=calibrated.transform;card.width=card.height=.05;
            for(int axis=0;axis<3;++axis)card.transform.m[12+axis]=point[axis];
            const auto hit=intersect(card,calibrated,image,o.x,image.height-o.y);
            require(hit.card>=0 && std::hypot(hit.u-.5,hit.v-.5)<1e-5,"Render3D ray lost calibrated vertical/off-center framing");
        }
        const auto assembly = directory.filePath(QStringLiteral("assembly.usda"));
        auto lensResult=result;lensResult.calibration.k1=-.12;lensResult.calibration.k2=.035;lensResult.estimatedIntrinsics=true;lensResult.calibrationAmbiguous=true;lensResult.estimatedPrincipalPoint=true;
        const auto lensFile=directory.filePath(QStringLiteral("lens.usda"));write(lensFile,reconstructionUsdLayer(lensResult,options));
        const auto lensStage=UsdStage::Open(lensFile.toStdString());const auto lensRoot=lensStage->GetPrimAtPath(SdfPath("/Reconstruction"));
        require(lensRoot.GetCustomDataByKey(TfToken("natron:plateRequiresUndistort")).Get<bool>(),"USD lost the required plate correction");
        require(lensRoot.GetCustomDataByKey(TfToken("natron:estimatedPrincipalPoint")).Get<bool>(),"USD lost the principal point estimation diagnostic");
        require(lensRoot.GetCustomDataByKey(TfToken("natron:radialK1")).Get<double>()==-.12 &&
                lensRoot.GetCustomDataByKey(TfToken("natron:calibrationAmbiguous")).Get<bool>(),"USD lost lens coefficients/observability status");
        const UsdGeomCamera lensCamera(lensStage->GetPrimAtPath(SdfPath("/Reconstruction/Camera")));
        require(lensCamera.GetCamera(UsdTimeCode(result.cameras.front().frame)).GetFrustum().ComputeProjectionMatrix()==
                camera.GetCamera(UsdTimeCode(result.cameras.front().frame)).GetFrustum().ComputeProjectionMatrix(),"Lens metadata changed USD's ideal pinhole projection");
        composed->saveAssembly(assembly.toStdString());
        require(bool(UsdStage::Open(assembly.toStdString())->GetPrimAtPath(SdfPath("/Natron/Assets/solve/Source/Landmarks"))),
                "Composed reconstruction did not survive assembly save/reload");
        auto incomplete = result;
        incomplete.unregisteredFrames.push_back(1002.25);
        invalid([&] { reconstructionUsdLayer(incomplete); });
        auto invalidPoints = result;
        invalidPoints.points[1].trackId = invalidPoints.points[0].trackId;
        invalid([&] { reconstructionUsdLayer(invalidPoints); });
        if (argc == 3 && std::string(argv[1]) == "--output") {
            const QString output = QString::fromLocal8Bit(argv[2]);
            require(QDir().mkpath(QFileInfo(output).absolutePath()), "Cannot create output directory");
            write(output, text);
            std::cout << "Reconstructed USD scene: " << output.toStdString() << '\n';
        } else {
            require(argc == 1, "Usage: NatronCameraSolveUsd [--output reconstruction.usda]");
        }
        std::cout << "Reconstruction USD passed: Camera + 64 guide Points, 64-bit IDs, 8 time samples, "
                  << "maximum USD reprojection error " << maximumPixelError
                  << " px; composed stage/Outline/camera and save/reload passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
