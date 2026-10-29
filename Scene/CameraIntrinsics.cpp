// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/FeatureTracking.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <ceres/ceres.h>
#include <ceres/rotation.h>
#include <Eigen/Core>

namespace Natron { namespace Scene {
std::array<double,2> distortPixel(const CameraCalibration& c,double x,double y)
{
    const double u=(x-c.cx)/c.fx,v=(y-c.cy)/c.fy,r2=u*u+v*v;
    const double radial=1+c.k1*r2+c.k2*r2*r2;
    return {c.cx+c.fx*u*radial,c.cy+c.fy*v*radial};
}
std::array<double,2> undistortPixel(const CameraCalibration& c,double x,double y)
{
    const double u=(x-c.cx)/c.fx,v=(y-c.cy)/c.fy,rd=std::hypot(u,v);
    if(rd<1e-15 || (c.k1==0 && c.k2==0))return {x,y};
    double r=rd;
    for(int i=0;i<40;++i){
        const double r2=r*r,f=r*(1+c.k1*r2+c.k2*r2*r2)-rd,d=1+3*c.k1*r2+5*c.k2*r2*r2;
        if(!std::isfinite(d)||d<=.01)break;
        if(std::abs(f)<1e-12)return {c.cx+c.fx*u*r/rd,c.cy+c.fy*v*r/rd};
        r=std::max(0.,r-std::clamp(f/d,-.25,.25));
    }
    throw std::invalid_argument("Lens distortion cannot be inverted at this observation");
}
bool validLens(const CameraCalibration& c)
{
    if(c.width<=0 || c.height<=0 || !std::isfinite(c.fx)||c.fx<=0 || !std::isfinite(c.fy)||c.fy<=0 ||
       !std::isfinite(c.cx)||!std::isfinite(c.cy)||!std::isfinite(c.k1)||!std::isfinite(c.k2))return false;
    const double radius=std::hypot(std::max(std::abs(c.cx),std::abs(c.width-c.cx))/c.fx,
                                   std::max(std::abs(c.cy),std::abs(c.height-c.cy))/c.fy);
    if(!std::isfinite(radius) || radius>1e6)return false;
    const double maximum=radius*radius;
    auto derivative=[&](double r2){return 1+3*c.k1*r2+5*c.k2*r2*r2;};
    auto factor=[&](double r2){return 1+c.k1*r2+c.k2*r2*r2;};
    double minimumDerivative=std::min(1.,derivative(maximum)),maximumFactor=std::max(1.,factor(maximum));
    if(c.k2>0){const double at=-3*c.k1/(10*c.k2);if(at>0 && at<maximum)minimumDerivative=std::min(minimumDerivative,derivative(at));}
    if(c.k2<0){const double at=-c.k1/(2*c.k2);if(at>0 && at<maximum)maximumFactor=std::max(maximumFactor,factor(at));}
    if(!std::isfinite(derivative(maximum)) || !std::isfinite(factor(maximum)) || minimumDerivative<.1 || maximumFactor>4)return false;
    return true;
}
namespace {
void checkpoint(const CameraSolveOptions& o,double p,const std::string& text)
{
    if(o.cancelled && o.cancelled())throw CameraSolveError(CameraSolveFailure::Cancelled,"Camera calibration cancelled");
    if(o.progress)o.progress(p,text);
}
struct Cancel : ceres::IterationCallback {
    const CameraSolveOptions& options;
    explicit Cancel(const CameraSolveOptions& o):options(o){}
    ceres::CallbackReturnType operator()(const ceres::IterationSummary&)override{
        return options.cancelled && options.cancelled()?ceres::SOLVER_ABORT:ceres::SOLVER_CONTINUE;
    }
};
struct Residual {
    double x,y,width,height,aspect,weight;
    template<class T> bool operator()(const T* lens,const T* camera,const T* point,T* residual)const{
        T p[3];ceres::AngleAxisRotatePoint(camera,point,p);for(int i=0;i<3;++i)p[i]+=camera[3+i];
        const T u=p[0]/p[2],v=p[1]/p[2],r2=u*u+v*v;
        const T f=exp(lens[0]),radial=T(1)+lens[1]*r2+lens[2]*r2*r2;
        residual[0]=T(weight)*(f*u*radial+T(width)*lens[3]-T(x));
        residual[1]=T(weight)*(f*T(aspect)*v*radial+T(height)*lens[4]-T(y));return true;
    }
};
using Pose=std::array<double,6>;
Pose encode(const SolvedCamera& c){
    double r[9];for(int col=0;col<3;++col)for(int row=0;row<3;++row)r[col*3+row]=(row? -1.:1.)*c.transform[row*4+col];
    Pose p{};ceres::RotationMatrixToAngleAxis(r,p.data());
    for(int row=0;row<3;++row)for(int col=0;col<3;++col)p[3+row]-=r[col*3+row]*c.transform[12+col];
    return p;
}
void decode(const Pose& p,SolvedCamera& c){
    double r[9];ceres::AngleAxisToRotationMatrix(p.data(),r);c.transform.fill(0);c.transform[15]=1;
    for(int col=0;col<3;++col)for(int row=0;row<3;++row){
        c.transform[col*4+row]=(col?-1.:1.)*r[row*3+col];
        c.transform[12+row]-=r[row*3+col]*p[3+col];
    }
}
std::array<double,2> projection(const CameraCalibration& c,const Pose& pose,const std::array<double,3>& point,double& depth){
    double p[3];ceres::AngleAxisRotatePoint(pose.data(),point.data(),p);for(int i=0;i<3;++i)p[i]+=pose[3+i];depth=p[2];
    return distortPixel(c,c.cx+c.fx*p[0]/p[2],c.cy+c.fy*p[1]/p[2]);
}
double refine(CameraReconstruction& result,const FeatureTracks& tracks,const TrackedSolveOptions& options,bool radial,bool principal=false)
{
    std::map<double,Pose> cameras;for(const auto& c:result.cameras)cameras.emplace(c.frame,encode(c));
    std::map<int64_t,std::array<double,3>> points;for(const auto& p:result.points)points.emplace(p.trackId,p.position);
    auto& c=result.calibration;const double aspect=c.fy/c.fx;
    double lens[]{std::log(c.fx),c.k1,c.k2,c.cx/c.width,c.cy/c.height};
    ceres::Problem problem;
    size_t count=0;
    for(const auto& o:tracks.observations)if(o.enabled && o.weight>0 && cameras.count(o.frame) && points.count(o.trackId)){
        auto* cost=new ceres::AutoDiffCostFunction<Residual,2,5,6,3>(new Residual{o.x,o.y,double(c.width),double(c.height),aspect,std::sqrt(o.weight)});
        problem.AddResidualBlock(cost,new ceres::HuberLoss(options.inlierThresholdPixels),lens,cameras.at(o.frame).data(),points.at(o.trackId).data());++count;
    }
    if(count<80 || cameras.size()<3)throw CameraSolveError(CameraSolveFailure::InvalidInput,"Auto calibration needs at least three registered views and 80 observations");
    problem.SetParameterLowerBound(lens,0,std::log(c.width*.25));problem.SetParameterUpperBound(lens,0,std::log(c.width*4.));
    for(int i=1;i<3;++i){problem.SetParameterLowerBound(lens,i,-1);problem.SetParameterUpperBound(lens,i,1);}
    std::vector<int> fixed;
    if(!radial){fixed.push_back(1);fixed.push_back(2);}
    if(!principal){fixed.push_back(3);fixed.push_back(4);}
    if(principal)for(int i=3;i<5;++i){problem.SetParameterLowerBound(lens,i,.35);problem.SetParameterUpperBound(lens,i,.65);}
    if(!fixed.empty())problem.SetParameterization(lens,new ceres::SubsetParameterization(5,fixed));
    problem.SetParameterBlockConstant(cameras.at(result.keyframe1).data());
    auto& second=cameras.at(result.keyframe2);int axis=0;for(int i=1;i<3;++i)if(std::abs(second[3+i])>std::abs(second[3+axis]))axis=i;
    problem.SetParameterization(second.data(),new ceres::SubsetParameterization(6,{3+axis}));
    ceres::Solver::Options settings;settings.linear_solver_type=ceres::DENSE_SCHUR;settings.max_num_iterations=100;
    settings.function_tolerance=1e-9;settings.gradient_tolerance=1e-10;settings.num_threads=1;
    Cancel cancel(options.solve);settings.callbacks.push_back(&cancel);
    ceres::Solver::Summary summary;ceres::Solve(settings,&problem,&summary);
    checkpoint(options.solve,.9,"Checking estimated lens and camera geometry");
    c.fx=std::exp(lens[0]);c.fy=c.fx*aspect;c.k1=lens[1];c.k2=lens[2];c.cx=lens[3]*c.width;c.cy=lens[4]*c.height;
    if(!summary.IsSolutionUsable() || !validLens(c) || c.fx<c.width*.255 || c.fx>c.width*3.95)
        throw CameraSolveError(CameraSolveFailure::Reconstruction,"Focal length or lens distortion is underconstrained; use more camera motion and depth variation");
    for(auto& camera:result.cameras)decode(cameras.at(camera.frame),camera);
    result.residuals.clear();result.points.clear();result.maximumErrorPixels=0;double squared=0,score=0;size_t total=0,accepted=0;
    std::map<int64_t,std::vector<ReprojectionResidual>> residuals;
    std::map<int64_t,double> errors;
    for(const auto& o:tracks.observations)if(o.enabled && o.weight>0){
        ++total;double error=options.inlierThresholdPixels*4;
        if(cameras.count(o.frame) && points.count(o.trackId)){
            double depth;const auto p=projection(c,cameras.at(o.frame),points.at(o.trackId),depth);
            if(depth>1e-6)error=std::hypot(p[0]-o.x,p[1]-o.y);
            if(std::isfinite(error) && error<options.inlierThresholdPixels*2){
                residuals[o.trackId].push_back({o.frame,o.trackId,error});errors[o.trackId]+=error*error;
            }
        }
        score+=std::isfinite(error)?std::min(error*error,options.inlierThresholdPixels*options.inlierThresholdPixels*16):options.inlierThresholdPixels*options.inlierThresholdPixels*16;
    }
    for(const auto& p:points)if(residuals[p.first].size()>=3){
        const auto& r=residuals[p.first];double parallax=0;std::vector<Eigen::Vector3d> rays;
        for(const auto& e:r){const auto camera=std::find_if(result.cameras.begin(),result.cameras.end(),[&](const auto& v){return v.frame==e.frame;});
            Eigen::Vector3d ray(p.second[0]-camera->transform[12],p.second[1]-camera->transform[13],p.second[2]-camera->transform[14]);ray.normalize();
            for(const auto& previous:rays)parallax=std::max(parallax,std::acos(std::clamp(previous.dot(ray),-1.,1.))*180/3.14159265358979323846);rays.push_back(ray);}
        if(parallax<options.solve.minimumParallaxDegrees)continue;
        result.points.push_back({p.first,p.second,r.size(),std::sqrt(errors[p.first]/r.size()),parallax});
        for(const auto& e:r){result.residuals.push_back(e);result.maximumErrorPixels=std::max(result.maximumErrorPixels,e.errorPixels);}
        squared+=errors[p.first];accepted+=r.size();
    }
    if(accepted<80 || result.points.size()<16)throw CameraSolveError(CameraSolveFailure::Reconstruction,"Too few consistent tracks for auto calibration");
    std::map<double,size_t> support;for(const auto& r:result.residuals)++support[r.frame];
    for(double frame:tracks.frames)if(support[frame]<6)throw CameraSolveError(CameraSolveFailure::Reconstruction,"Some frames lack enough calibrated inlier landmarks");
    result.rmsPixels=std::sqrt(squared/accepted);result.estimatedIntrinsics=true;result.calibrationCoverage=double(accepted)/total;
    result.estimatedPrincipalPoint=principal;
    return score/total;
}

double validationRms(const CameraReconstruction& result,const std::vector<TrackObservation>& observations,double threshold)
{
    std::map<double,Pose> cameras;for(const auto& c:result.cameras)cameras.emplace(c.frame,encode(c));
    std::map<int64_t,const SolvedPoint*> points;for(const auto& p:result.points)points.emplace(p.trackId,&p);
    double squared=0;size_t count=0;
    for(const auto& o:observations)if(o.enabled && o.weight>0){
        double error=threshold*4;
        if(cameras.count(o.frame) && points.count(o.trackId)){
            double depth;const auto p=projection(result.calibration,cameras.at(o.frame),points.at(o.trackId)->position,depth);
            if(depth>1e-6)error=std::hypot(p[0]-o.x,p[1]-o.y);
        }
        squared+=std::isfinite(error)?std::min(error*error,threshold*threshold*16):threshold*threshold*16;++count;
    }
    return count?std::sqrt(squared/count):std::numeric_limits<double>::infinity();
}

void refinePrincipalPoint(CameraReconstruction& best,const FeatureTracks& tracks,const TrackedSolveOptions& options,bool radial)
{
    checkpoint(options.solve,.92,"Validating principal point refinement");
    auto training=tracks;training.observations.clear();std::vector<TrackObservation> validation;
    std::vector<TrackObservation> ordered;
    for(const auto& o:tracks.observations)if(o.enabled && o.weight>0)ordered.push_back(o);
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return std::tie(a.frame,a.trackId)<std::tie(b.frame,b.trackId);});
    size_t active=0;
    for(const auto& o:ordered){
        if(o.enabled && o.weight>0 && ++active%11==0)validation.push_back(o);else training.observations.push_back(o);
    }
    if(validation.size()<30)return;
    try{
        // The competing models see the same fitting/validation partition.
        // Validation observations are omitted from these refinement objectives.
        auto reference=best;
        const bool referenceRadial=best.calibration.k1!=0 || best.calibration.k2!=0;
        refine(reference,training,options,referenceRadial);
        const double baseline=validationRms(reference,validation,options.inlierThresholdPixels);
        CameraReconstruction selected;double score=baseline;bool selectedRadial=false;
        for(bool withRadial:{false,true}){
            if(withRadial && !radial)continue;
            try{
                auto candidate=best;
                if(!withRadial)candidate.calibration.k1=candidate.calibration.k2=0;
                refine(candidate,training,options,withRadial,true);
                const double error=validationRms(candidate,validation,options.inlierThresholdPixels);
                if(error+.01<score){selected=std::move(candidate);score=error;selectedRadial=withRadial;}
            }catch(const CameraSolveError& e){if(e.reason==CameraSolveFailure::Cancelled)throw;}
        }
        if(!selected.cameras.empty()){
            refine(selected,tracks,options,selectedRadial,true);
            if(selected.rmsPixels<=options.solve.maximumRmsPixels){
                selected.principalPointValidationGain=baseline-score;best=std::move(selected);
            }
        }
    }catch(const CameraSolveError& e){if(e.reason==CameraSolveFailure::Cancelled)throw;}
}
}
CameraReconstruction estimateTrackedCamera(const CameraCalibration& domain,const FeatureTracks& tracks,const TrackedSolveOptions& options)
{
    if(!validLens(domain) || tracks.width!=domain.width || tracks.height!=domain.height || tracks.frames.size()<3 ||
       !std::isfinite(options.inlierThresholdPixels) || options.inlierThresholdPixels<=0 ||
       !std::isfinite(options.solve.maximumRmsPixels) || options.solve.maximumRmsPixels<=0)
        throw CameraSolveError(CameraSolveFailure::InvalidInput,"Auto calibration needs a valid image domain and at least three views");
    const bool radial=options.solve.intrinsics==CameraSolveOptions::Intrinsics::FocalAndRadial;
    CameraReconstruction best;double bestScore=std::numeric_limits<double>::infinity();std::string diagnostic;
    const double aspect=domain.fy/domain.fx;
    const double starts[]{.45,.65,.9,1.3,1.9,2.8};
    std::vector<std::pair<CameraCalibration,double>> candidates;
    for(size_t i=0;i<6;++i){
        checkpoint(options.solve,double(i)/6,"Estimating focal length and radial distortion (start "+std::to_string(i+1)+"/6)");
        try{
            auto initial=domain;initial.fx=domain.width*starts[i];initial.fy=initial.fx*aspect;initial.k1=initial.k2=0;
            auto bootstrap=options;bootstrap.solve.intrinsics=CameraSolveOptions::Intrinsics::Fixed;bootstrap.solve.maximumRmsPixels=10;
            bootstrap.inlierThresholdPixels=radial?4:options.inlierThresholdPixels;
            bootstrap.solve.progress={};
            auto result=solveTrackedCamera(initial,tracks,bootstrap);
            auto score=refine(result,tracks,options,false);
            if(radial){try{auto withLens=result;const double lensScore=refine(withLens,tracks,options,true);
                // Do not introduce a lens warp to explain tracking noise when
                // the more complex model has no meaningful fit improvement.
                if(std::sqrt(lensScore)+.02<std::sqrt(score)){result=std::move(withLens);score=lensScore;}}
                catch(const CameraSolveError& e){if(e.reason==CameraSolveFailure::Cancelled)throw;}}
            if(result.rmsPixels<=options.solve.maximumRmsPixels){candidates.emplace_back(result.calibration,score);if(score<bestScore){best=std::move(result);bestScore=score;}}
        }catch(const CameraSolveError& e){if(e.reason==CameraSolveFailure::Cancelled)throw;diagnostic=e.what();}
    }
    if(best.cameras.empty())throw CameraSolveError(CameraSolveFailure::Reconstruction,"Auto calibration failed: "+diagnostic);
    if(options.solve.refinePrincipalPoint)refinePrincipalPoint(best,tracks,options,radial);
    best.focalMinimum=best.focalMaximum=best.calibration.fx;
    const auto bestCorner=distortPixel(best.calibration,0,0);
    for(const auto& candidate:candidates)if(candidate.second<bestScore+std::max(.0025,bestScore*.1)){
        best.focalMinimum=std::min(best.focalMinimum,candidate.first.fx);best.focalMaximum=std::max(best.focalMaximum,candidate.first.fx);
        const auto corner=distortPixel(candidate.first,0,0);
        if(std::hypot(corner[0]-bestCorner[0],corner[1]-bestCorner[1])>2)best.calibrationAmbiguous=true;
    }
    best.calibrationAmbiguous=best.calibrationAmbiguous || best.focalMaximum/best.focalMinimum>1.2;
    if(best.estimatedPrincipalPoint && (best.calibration.cx<domain.width*.36 || best.calibration.cx>domain.width*.64 ||
       best.calibration.cy<domain.height*.36 || best.calibration.cy>domain.height*.64))best.calibrationAmbiguous=true;
    const auto first=std::find_if(best.cameras.begin(),best.cameras.end(),[&](const auto& c){return c.frame==best.keyframe1;});
    const auto last=std::find_if(best.cameras.begin(),best.cameras.end(),[&](const auto& c){return c.frame==best.keyframe2;});
    double rotationSpan=0;
    for(const auto& camera:best.cameras){double trace=0;for(int col=0;col<3;++col)for(int row=0;row<3;++row)trace+=first->transform[col*4+row]*camera.transform[col*4+row];
        rotationSpan=std::max(rotationSpan,std::acos(std::clamp((trace-1)*.5,-1.,1.))*180/3.14159265358979323846);}
    // Small rotations are a critical self-calibration motion even when local
    // optimizers happen to agree. This is a conservative observability flag,
    // not a statistical confidence interval.
    if(rotationSpan<8)best.calibrationAmbiguous=true;
    double baseline=0;for(int i=0;i<3;++i)baseline+=std::pow(first->transform[12+i]-last->transform[12+i],2);baseline=std::sqrt(baseline);
    if(!std::isfinite(baseline)||baseline<1e-8)throw CameraSolveError(CameraSolveFailure::InsufficientParallax,"Estimated camera baseline collapsed");
    for(auto& c:best.cameras)for(int i=0;i<3;++i)c.transform[12+i]/=baseline;
    for(auto& p:best.points)for(double& v:p.position)v/=baseline;
    checkpoint(options.solve,1,"Focal length, lens distortion and camera solved");return best;
}
}} // namespace Natron::Scene
