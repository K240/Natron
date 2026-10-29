// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/FeatureTracking.h"
#include "Tests/CameraSolveFixture.h"
#include <cmath>
#include <iostream>
#include <map>
#include <algorithm>
#include <limits>
using namespace Natron::Scene;
using CameraSolveFixture::require;

int main(){try{
    // Independent cameras span rotation and translation on three axes. Landmarks
    // cover the image radius; a short lateral-only move cannot constrain a lens.
    CameraCalibration truth{960,720,810,810,480,360,-.12,.035};
    FeatureTracks tracks;tracks.width=960;tracks.height=720;
    for(int frame=0;frame<12;++frame){
        tracks.frames.push_back(frame+1);const double yaw=(frame-5)*.035,pitch=std::sin(frame*.6)*.06;
        const double cy=std::cos(yaw),sy=std::sin(yaw),cp=std::cos(pitch),sp=std::sin(pitch);
        const double center[]{(frame-5)*.16,std::sin(frame*.7)*.22,std::cos(frame*.4)*.3};
        for(int point=0;point<160;++point){
            const double d[]{(point%16-7.5)*.38-center[0],(point/16-4.5)*.35-center[1],5+(point*17%31)*.1-center[2]};
            const double px=cy*d[0]+sy*sp*d[1]+sy*cp*d[2],py=cp*d[1]-sp*d[2],pz=-sy*d[0]+cy*sp*d[1]+cy*cp*d[2];
            const double u=px/pz,v=py/pz,r2=u*u+v*v,radial=1-.12*r2+.035*r2*r2;
            const double x=480+810*u*radial+.025*std::sin(frame*13+point*.7),y=360+810*v*radial+.025*std::cos(frame*7+point*.9);
            tracks.observations.push_back({double(frame+1),int64_t(point)+10000000000LL,x,y});
        }
    }
    auto unknown=truth;unknown.fx=unknown.fy=960;unknown.k1=unknown.k2=0;
    TrackedSolveOptions options;options.solve.intrinsics=CameraSolveOptions::Intrinsics::FocalAndRadial;options.solve.maximumRmsPixels=.5;
    options.solve.progress=[](double,const std::string& s){std::cout<<s<<'\n';};
    std::vector<TrackObservation> heldOut;FeatureTracks training=tracks;training.observations.clear();
    for(size_t i=0;i<tracks.observations.size();++i)if(i%17==0)heldOut.push_back(tracks.observations[i]);else training.observations.push_back(tracks.observations[i]);
    const auto result=solveTrackedCamera(unknown,training,options);
    std::cout<<"Estimated fx="<<result.calibration.fx<<" k1="<<result.calibration.k1<<" k2="<<result.calibration.k2<<" RMS="<<result.rmsPixels<<" points="<<result.points.size()<<" coverage="<<result.calibrationCoverage<<'\n';
    require(result.cameras.size()==12 && result.points.size()>120,"Estimated solve lost views or landmarks");
    require(std::abs(result.calibration.fx/810-1)<.02,"Focal length did not recover the independent truth");
    require(std::abs(result.calibration.k1+.12)<.02 && std::abs(result.calibration.k2-.035)<.03,"Radial distortion did not recover the independent truth");
    require(result.rmsPixels<.1,"Estimated lens reprojection is inaccurate");
    require(!result.calibrationAmbiguous,"Well-excited synthetic calibration was marked ambiguous");
    TrackedSolveOptions known;known.solve.maximumRmsPixels=.5;
    const auto supplied=solveTrackedCamera(truth,training,known);
    require(supplied.cameras.size()==12 && supplied.rmsPixels<.05 && supplied.calibration.k1==truth.k1,"Supplied radial calibration did not solve in the distorted observation domain");
    std::map<int64_t,SolvedPoint> points;for(const auto& p:result.points)points.emplace(p.trackId,p);
    double squared=0;size_t count=0;
    for(const auto& o:heldOut)if(points.count(o.trackId)){
        const auto& matrix=result.cameras[size_t(o.frame-1)].transform;const auto& p=points.at(o.trackId).position;double local[3]{};
        for(int a=0;a<3;++a)for(int w=0;w<3;++w)local[a]+=matrix[a*4+w]*(p[w]-matrix[12+w]);
        const double u=local[0]/-local[2],v=-local[1]/-local[2],r2=u*u+v*v;const auto& c=result.calibration;
        const double radial=1+c.k1*r2+c.k2*r2*r2;
        squared+=std::pow(c.fx*u*radial+c.cx-o.x,2)+std::pow(c.fy*v*radial+c.cy-o.y,2);++count;
    }
    require(count>90 && std::sqrt(squared/count)<.15,"Estimated camera/lens fails withheld observations");
    std::cout<<"Held-out RMS="<<std::sqrt(squared/count)<<" px ("<<count<<" observations)\n";
    auto principalOptions=options;principalOptions.solve.refinePrincipalPoint=true;
    auto shiftedTraining=training;for(auto& o:shiftedTraining.observations){o.x-=20;o.y+=20;}
    const auto shifted=solveTrackedCamera(unknown,shiftedTraining,principalOptions);
    std::cout<<"Shifted principal: fx="<<shifted.calibration.fx<<" cx="<<shifted.calibration.cx<<" cy="<<shifted.calibration.cy
             <<" k1="<<shifted.calibration.k1<<" k2="<<shifted.calibration.k2<<" validation gain="<<shifted.principalPointValidationGain<<'\n';
    require(shifted.estimatedPrincipalPoint,"Decentered camera kept the biased center model");
    require(std::abs(shifted.calibration.cx-460)<2 && std::abs(shifted.calibration.cy-380)<2,"Principal point did not recover the independent shifted truth");
    require(std::abs(shifted.calibration.fx/810-1)<.02 && shifted.rmsPixels<.1,"Principal refinement biased focal length or image fit");
    require(shifted.calibration.cx>960*.35 && shifted.calibration.cx<960*.65 && shifted.calibration.cy>720*.35 && shifted.calibration.cy<720*.65,"Principal bounds were ignored");
    std::map<int64_t,SolvedPoint> shiftedPoints;for(const auto& p:shifted.points)shiftedPoints.emplace(p.trackId,p);
    double shiftedSquared=0;size_t shiftedCount=0;
    for(const auto& o:heldOut)if(shiftedPoints.count(o.trackId)){
        const auto& m=shifted.cameras[size_t(o.frame-1)].transform;const auto& p=shiftedPoints.at(o.trackId).position;double local[3]{};
        for(int a=0;a<3;++a)for(int w=0;w<3;++w)local[a]+=m[a*4+w]*(p[w]-m[12+w]);
        const double u=local[0]/-local[2],v=-local[1]/-local[2],r2=u*u+v*v;const auto& c=shifted.calibration;
        const double radial=1+c.k1*r2+c.k2*r2*r2;
        shiftedSquared+=std::pow(c.fx*u*radial+c.cx-o.x+20,2)+std::pow(c.fy*v*radial+c.cy-o.y-20,2);++shiftedCount;
    }
    require(shiftedCount>90 && std::sqrt(shiftedSquared/shiftedCount)<.15,"Refined principal point fails independent withheld observations");
    std::cout<<"Shifted independent held-out RMS="<<std::sqrt(shiftedSquared/shiftedCount)<<" px\n";
    auto reordered=shiftedTraining;std::reverse(reordered.observations.begin(),reordered.observations.end());
    reordered.observations.push_back({std::numeric_limits<double>::quiet_NaN(),-1,0,0,1,false});
    const auto reorderedResult=solveTrackedCamera(unknown,reordered,principalOptions);
    require(reorderedResult.estimatedPrincipalPoint && std::abs(reorderedResult.calibration.cx-shifted.calibration.cx)<.5 &&
            std::abs(reorderedResult.calibration.cy-shifted.calibration.cy)<.5,"Principal validation depends on observation order or disabled markers");
    const auto centered=solveTrackedCamera(unknown,training,principalOptions);
    require(!centered.estimatedPrincipalPoint && centered.calibration.cx==480 && centered.calibration.cy==360,"Validation accepted an unnecessary principal-point model");
    auto focalTracks=tracks;
    for(auto& o:focalTracks.observations){const auto u=undistortPixel(truth,o.x,o.y);o.x=u[0];o.y=360+(u[1]-360)*1.2;}
    auto nonSquare=unknown;nonSquare.fy=nonSquare.fx*1.2;
    auto focalOptions=options;focalOptions.solve.intrinsics=CameraSolveOptions::Intrinsics::Focal;
    const auto focal=solveTrackedCamera(nonSquare,focalTracks,focalOptions);
    require(std::abs(focal.calibration.fx/810-1)<.02 && std::abs(focal.calibration.fy/focal.calibration.fx-1.2)<1e-10,"Focal-only solve lost fixed pixel aspect");
    require(focal.calibration.k1==0 && focal.calibration.k2==0,"Focal-only solve introduced lens distortion");
    FeatureTracks critical;critical.width=1024;critical.height=768;
    for(int frame=0;frame<8;++frame){critical.frames.push_back(frame+1);for(int p=0;p<64;++p){
        const double z=5+(p*17%23)*.16;
        critical.observations.push_back({double(frame+1),p,512+900*((p%8-3.5)*.5-frame*.15)/z,384+900*(p/8-3.5)*.35/z});}}
    const auto ambiguous=solveTrackedCamera({1024,768,1024,1024,512,384},critical,focalOptions);
    require(ambiguous.calibrationAmbiguous,"Translation-only motion falsely claimed an observable focal length");
    std::cout<<"Critical-motion focal range="<<ambiguous.focalMinimum<<".."<<ambiguous.focalMaximum<<" px, correctly marked ambiguous\n";
    for(const auto p:std::vector<std::array<double,2>>{{30,40},{480,360},{800,600}}){const auto d=distortPixel(truth,p[0],p[1]);const auto u=undistortPixel(truth,d[0],d[1]);require(std::hypot(u[0]-p[0],u[1]-p[1])<1e-6,"Lens inverse does not round-trip");}
    options.solve.cancelled=[] {return true;};
    CameraSolveFixture::fails([&]{solveTrackedCamera(unknown,tracks,options);},CameraSolveFailure::Cancelled);
    auto invalid=truth;invalid.k1=-5;require(!validLens(invalid),"Folding lens accepted");
    auto principalCancel=principalOptions;bool reachedPrincipal=false;
    principalCancel.solve.progress=[&](double,const std::string& message){if(message=="Validating principal point refinement")reachedPrincipal=true;};
    principalCancel.solve.cancelled=[&]{return reachedPrincipal;};
    CameraSolveFixture::fails([&]{solveTrackedCamera(unknown,shiftedTraining,principalCancel);},CameraSolveFailure::Cancelled);
    require(reachedPrincipal,"Principal refinement cancellation did not reach its phase");
    std::cout<<"Camera intrinsics estimation passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
