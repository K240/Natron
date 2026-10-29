// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/FeatureTracking.h"
#include "Tests/CameraSolveFixture.h"
#include <QCoreApplication>
#include <QDir>
#include <QImage>
#include <cmath>
#include <iostream>
#include <map>
#include <limits>
#include <stdexcept>
using namespace Natron::Scene;
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        std::vector<double> frames;for(int i=1;i<=25;++i)frames.push_back(i);
        FeatureTrackingOptions options;options.maximumFeatures=180;options.minimumTrackLength=8;
        const bool real=argc==3 && std::string(argv[1])=="--real";
        auto provider=[&](double frame) {
            TrackingImage image;
            if(real) {
                QImage source(QDir(QString::fromLocal8Bit(argv[2])).filePath(QStringLiteral("plate.%1.png").arg(int(frame),4,10,QChar(u'0'))));
                if(source.isNull())throw std::runtime_error("Cannot read real plate frame");
                source=source.convertToFormat(QImage::Format_RGB888);image.width=source.width();image.height=source.height();
                image.pixels.resize(size_t(image.width)*image.height);
                for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){const auto p=source.constScanLine(y)+x*3;image.pixels[size_t(y)*image.width+x]=float((.2126*p[0]+.7152*p[1]+.0722*p[2])/255.);}
            } else {
                image.width=320;image.height=240;image.pixels.resize(320*240);
                for(int y=0;y<240;++y)for(int x=0;x<320;++x){const double u=x-(frame-1)*.4,v=y-(frame-1)*.15;
                    image.pixels[size_t(y)*320+x]=float(.5+.15*std::sin(u*.43)*std::cos(v*.31)+.15*std::sin(u*.13+v*.19)+.1*std::cos(u*.27-v*.37));}
            }
            return image;
        };
        const auto tracks=trackFeatures(frames,provider,options);
        if(tracks.trackCount<20)throw std::runtime_error("Automatic tracker did not retain enough long tracks");
        std::cout<<"Automatic tracks: "<<tracks.trackCount<<", observations: "<<tracks.observations.size()<<'\n';
        if(real) {
            CameraCalibration c{640,480,535.4,539.2,320.6,248.1};
            TrackedSolveOptions solve;solve.solve.maximumRmsPixels=1.5;
            solve.solve.progress=[](double,const std::string& text){if(text!="Rejecting keyframe outliers")std::cout<<text<<'\n';};
            const auto result=solveTrackedCamera(c,tracks,solve);
            if(result.cameras.size()!=frames.size() || result.points.size()<20)throw std::runtime_error("Incomplete real-plate reconstruction");
            std::cout<<"REAL_PLATE cameras="<<result.cameras.size()<<" points="<<result.points.size()<<" rms="<<result.rmsPixels<<" keyframes="<<result.keyframe1<<','<<result.keyframe2<<'\n';
        }else{
            std::map<int64_t,TrackObservation> first;
            double error=0;size_t count=0;
            for(const auto& o:tracks.observations){if(!first.count(o.trackId))first[o.trackId]=o;const auto& a=first[o.trackId];error+=std::hypot(o.x-a.x-(o.frame-a.frame)*.4,o.y-a.y-(o.frame-a.frame)*.15);++count;}
            if(error/count>.15)throw std::runtime_error("Subpixel tracking drift exceeds tolerance");
            std::cout<<"Synthetic independent flow error="<<error/count<<" px\n";
            using CameraSolveFixture::require;
            CameraSolveOptions cancel;cancel.cancelled=[] {return true;};
            CameraSolveFixture::fails([&]{trackFeatures(frames,provider,options,cancel);},CameraSolveFailure::Cancelled);
            auto maskedProvider=[&](double frame){auto image=provider(frame);image.mask.resize(image.pixels.size());
                for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x)image.mask[size_t(y)*image.width+x]=x<160?1.f:std::numeric_limits<float>::quiet_NaN();return image;};
            auto maskOptions=options;maskOptions.minimumTrackLength=4;
            const auto masked=trackFeatures({1,2,3,4,5,6,7},maskedProvider,maskOptions);
            require(masked.trackCount>=5,"Mask excluded all valid features");
            for(const auto& o:masked.observations)require(o.x<160,"Tracker crossed the excluded mask");
            const auto fixture=CameraSolveFixture::make(.08);
            FeatureTracks observed;observed.width=fixture.calibration.width;observed.height=fixture.calibration.height;
            observed.observations=fixture.observations;for(int i=0;i<8;++i)observed.frames.push_back(1001+i*.5);
            // Mismatched features have no consistent epipolar geometry. The
            // remaining landmarks and all poses must survive robust fitting.
            for(size_t i=0;i<observed.observations.size();++i)if(i%64<9){observed.observations[i].x+=50*std::sin(i*1.3);observed.observations[i].y+=60*std::cos(i*.7);}
            TrackedSolveOptions robust;robust.solve.maximumRmsPixels=.5;
            const auto solved=solveTrackedCamera(fixture.calibration,observed,robust);
            require(solved.cameras.size()==8 && solved.points.size()>=45 && solved.rmsPixels<.2,"Robust solve lost the valid camera/point geometry");
            std::cout<<"Outlier solve: "<<solved.points.size()<<" points, RMS "<<solved.rmsPixels<<" px; mask and cancellation passed\n";
            auto duplicate=observed;duplicate.observations.push_back(duplicate.observations.front());
            CameraSolveFixture::fails([&]{solveTrackedCamera(fixture.calibration,duplicate,robust);},CameraSolveFailure::InvalidInput);
            robust.solve.cancelled=[] {return true;};
            CameraSolveFixture::fails([&]{solveTrackedCamera(fixture.calibration,observed,robust);},CameraSolveFailure::Cancelled);
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
