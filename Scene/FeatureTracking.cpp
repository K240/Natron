// SPDX-License-Identifier: GPL-2.0-or-later
#include "Scene/FeatureTracking.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <libmv/simple_pipeline/detect.h>
#include <libmv/tracking/track_region.h>
#include <libmv/multiview/fundamental.h>

namespace Natron { namespace Scene {
namespace {
using Pyramid = std::vector<libmv::FloatImage>;
void progress(const CameraSolveOptions& c, double value, const std::string& message)
{
    if (c.cancelled && c.cancelled()) {
        throw CameraSolveError(CameraSolveFailure::Cancelled, "Camera tracking cancelled");
    }
    if (c.progress) c.progress(value, message);
}
bool included(const TrackingImage& image, double x, double y, int margin)
{
    if (!std::isfinite(x) || !std::isfinite(y) || x < margin || y < margin ||
        x >= image.width - margin || y >= image.height - margin) return false;
    if (image.mask.empty()) return true;
    for (int dy : {-margin, 0, margin}) for (int dx : {-margin, 0, margin}) {
        const auto value=image.mask[size_t(int(y) + dy) * image.width + int(x) + dx];
        if (!std::isfinite(value) || value < 0.5f) return false;
    }
    return true;
}
Pyramid pyramid(const TrackingImage& image, int levels)
{
    if (image.width < 32 || image.height < 32 || image.pixels.size() != size_t(image.width) * image.height ||
        (!image.mask.empty() && image.mask.size() != image.pixels.size())) {
        throw std::invalid_argument("Invalid tracking image or mask");
    }
    std::vector<float> sample;
    for (size_t i = 0; i < image.pixels.size(); i += 17) if (std::isfinite(image.pixels[i])) sample.push_back(image.pixels[i]);
    if (sample.empty()) throw std::invalid_argument("Tracking image contains no finite pixels");
    std::sort(sample.begin(), sample.end());
    const float low = sample[sample.size() / 50], high = sample[sample.size() * 49 / 50];
    const float scale = high > low + 1e-6f ? 1.f / (high - low) : 1.f;
    Pyramid result(1);
    result[0].Resize(image.height, image.width, 1);
    for (int y = 0; y < image.height; ++y) for (int x = 0; x < image.width; ++x) {
        const float value = image.pixels[size_t(y) * image.width + x];
        result[0](y, x) = std::isfinite(value) ? std::clamp((value - low) * scale, 0.f, 1.f) : 0;
    }
    for (int level = 1; level < levels; ++level) {
        const auto& previous = result.back();
        if (previous.Width() < 48 || previous.Height() < 48) break;
        libmv::FloatImage next(previous.Height() / 2, previous.Width() / 2, 1);
        for (int y = 0; y < next.Height(); ++y) for (int x = 0; x < next.Width(); ++x) {
            next(y, x) = (previous(2*y, 2*x) + previous(2*y+1, 2*x) + previous(2*y, 2*x+1) + previous(2*y+1, 2*x+1)) * .25f;
        }
        result.push_back(std::move(next));
    }
    return result;
}
libmv::FloatImage crop(const libmv::FloatImage& image, int x, int y, int width, int height)
{
    libmv::FloatImage result(height, width, 1);
    for (int row = 0; row < height; ++row) for (int column = 0; column < width; ++column) result(row, column) = image(y + row, x + column);
    return result;
}
bool follow(const Pyramid& a, const Pyramid& b, double x, double y, double& u, double& v,
            const FeatureTrackingOptions& config, double& correlation)
{
    const int last = int(std::min(a.size(), b.size())) - 1;
    for (int level = last; level >= 0; --level) {
        const double scale = double(1 << level), sx = x / scale, sy = y / scale;
        const double guessX = u / scale, guessY = v / scale;
        const int radius = config.patchRadius, pad = radius + 4;
        const int ax = int(std::floor(sx)) - pad, ay = int(std::floor(sy)) - pad;
        const int bx = std::max(0, int(std::floor(guessX)) - pad - 8), by = std::max(0, int(std::floor(guessY)) - pad - 8);
        const int bw = std::min(b[level].Width() - bx, 2 * (pad + 8) + 2), bh = std::min(b[level].Height() - by, 2 * (pad + 8) + 2);
        if (ax < 0 || ay < 0 || ax + 2*pad+2 > a[level].Width() || ay + 2*pad+2 > a[level].Height() || bw < 2*pad || bh < 2*pad) return false;
        const auto source = crop(a[level], ax, ay, 2*pad+2, 2*pad+2);
        const auto target = crop(b[level], bx, by, bw, bh);
        const double px = sx - ax, py = sy - ay, qx = guessX - bx, qy = guessY - by;
        double x1[]{px-radius, px+radius, px+radius, px-radius, px};
        double y1[]{py-radius, py-radius, py+radius, py+radius, py};
        double x2[]{qx-radius, qx+radius, qx+radius, qx-radius, qx};
        double y2[]{qy-radius, qy-radius, qy+radius, qy+radius, qy};
        libmv::TrackRegionOptions options;
        options.mode = libmv::TrackRegionOptions::TRANSLATION;
        options.max_iterations = 25;
        options.minimum_correlation = config.minimumCorrelation;
        options.num_extra_points = 1;
        options.use_normalized_intensities = true;
        options.use_brute_initialization = level == last;
        options.attempt_refine_before_brute = true;
        options.minimum_corner_shift_tolerance_pixels = .01;
        libmv::TrackRegionResult result;
        libmv::TrackRegion(source, target, x1, y1, options, x2, y2, &result);
        if (!result.is_usable() || !std::isfinite(x2[4]) || !std::isfinite(y2[4])) return false;
        u = (x2[4] + bx) * scale;
        v = (y2[4] + by) * scale;
        correlation = result.correlation;
    }
    return true;
}
struct Track {
    int64_t id;
    double x, y, vx = 0, vy = 0;
    std::vector<TrackObservation> observations;
};
} // namespace

FeatureTracks trackFeatures(const std::vector<double>& frames, const TrackingImageProvider& images,
                            const FeatureTrackingOptions& options, const CameraSolveOptions& control)
{
    if (!images || frames.size() < 2 || options.maximumFeatures < 8 || options.maximumFeatures > 5000 ||
        options.minimumDistance < 2 || options.patchRadius < 3 || options.patchRadius > 15 ||
        options.pyramidLevels < 1 || options.pyramidLevels > 5 || options.minimumTrackLength < 2 ||
        options.replenishInterval < 1 || !std::isfinite(options.minimumCorrelation) || options.minimumCorrelation <= 0 || options.minimumCorrelation > 1 ||
        !std::isfinite(options.maximumForwardBackwardError) || options.maximumForwardBackwardError <= 0) {
        throw std::invalid_argument("Invalid automatic tracking configuration");
    }
    for (size_t i = 0; i < frames.size(); ++i) if (!std::isfinite(frames[i]) || (i && frames[i] <= frames[i-1])) throw std::invalid_argument("Tracking frames must be finite and increasing");
    FeatureTracks result;
    result.frames = frames;
    std::vector<Track> tracks;
    std::vector<size_t> active;
    Pyramid previous;
    for (size_t frame = 0; frame < frames.size(); ++frame) {
        progress(control, double(frame) / frames.size(), "Tracking frame " + std::to_string(frames[frame]));
        auto image = images(frames[frame]);
        auto current = pyramid(image, options.pyramidLevels);
        if (frame && (image.width != result.width || image.height != result.height)) throw std::invalid_argument("Tracking image format changed within the sequence");
        result.width = image.width; result.height = image.height;
        std::vector<size_t> next;
        for (const size_t index : active) {
            progress(control, double(frame) / frames.size(), "Tracking features");
            auto& track = tracks[index];
            double u = track.x + track.vx, v = track.y + track.vy, score = 0;
            if (!follow(previous, current, track.x, track.y, u, v, options, score) || !included(image, u, v, options.patchRadius + 3)) continue;
            double bx = track.x, by = track.y, backScore = 0;
            if (!follow(current, previous, u, v, bx, by, options, backScore) || std::hypot(bx-track.x, by-track.y) > options.maximumForwardBackwardError) continue;
            track.vx = u-track.x; track.vy = v-track.y; track.x = u; track.y = v;
            track.observations.push_back({frames[frame], track.id, u+.5, v+.5});
            next.push_back(index);
        }
        active = std::move(next);
        if (frame % options.replenishInterval == 0 || active.size() < size_t(options.maximumFeatures / 2)) {
            libmv::DetectOptions detector;
            detector.type = libmv::DetectOptions::HARRIS;
            detector.margin = (options.patchRadius + 5) * (1 << (int(current.size()) - 1));
            detector.min_distance = options.minimumDistance;
            detector.harris_threshold = 1e-6;
            libmv::vector<libmv::Feature> features;
            libmv::Detect(current.front(), detector, &features);
            std::stable_sort(features.begin(), features.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
            for (const auto& feature : features) {
                if (active.size() >= size_t(options.maximumFeatures)) break;
                if (!included(image, feature.x, feature.y, detector.margin)) continue;
                bool near = false;
                for (const auto i : active) if (std::hypot(tracks[i].x-feature.x, tracks[i].y-feature.y) < options.minimumDistance) { near = true; break; }
                if (near) continue;
                Track track{int64_t(tracks.size()+1), feature.x, feature.y};
                track.observations.push_back({frames[frame], track.id, feature.x+.5, feature.y+.5});
                active.push_back(tracks.size()); tracks.push_back(std::move(track));
            }
        }
        previous = std::move(current);
    }
    for (const auto& track : tracks) if (track.observations.size() >= size_t(options.minimumTrackLength)) {
        ++result.trackCount;
        result.observations.insert(result.observations.end(), track.observations.begin(), track.observations.end());
    }
    progress(control, 1, "Automatic feature tracking complete");
    return result;
}

namespace {
struct Pair { double first, second, score; size_t common; };
std::set<int64_t> pairInliers(const CameraCalibration& c, const FeatureTracks& tracks, const Pair& pair,
                            const TrackedSolveOptions& options)
{
    std::map<int64_t, const TrackObservation*> a, b;
    for (const auto& o : tracks.observations) if (o.enabled && o.weight > 0) {
        if (o.frame == pair.first) a[o.trackId] = &o;
        if (o.frame == pair.second) b[o.trackId] = &o;
    }
    std::vector<int64_t> ids;
    std::vector<libmv::Vec3> x, y;
    for (const auto& p : a) if (b.count(p.first)) {
        ids.push_back(p.first);
        const auto u=undistortPixel(c,p.second->x,p.second->y),v=undistortPixel(c,b.at(p.first)->x,b.at(p.first)->y);
        x.emplace_back((u[0]-c.cx)/c.fx, (u[1]-c.cy)/c.fy, 1);
        y.emplace_back((v[0]-c.cx)/c.fx, (v[1]-c.cy)/c.fy, 1);
    }
    if (ids.size() < 12) return {};
    std::mt19937 random(9173);
    std::vector<size_t> order(ids.size()); std::iota(order.begin(), order.end(), 0);
    std::set<int64_t> best;
    libmv::Mat3 model;
    auto score = [&](const libmv::Mat3& f) {
        std::set<int64_t> inliers;
        for (size_t i = 0; i < ids.size(); ++i) {
            const libmv::Vec3 l2 = f*x[i], l1 = f.transpose()*y[i];
            const double numerator = y[i].dot(l2);
            const double denominator = std::pow(l2.x()/c.fx,2)+std::pow(l2.y()/c.fy,2)+std::pow(l1.x()/c.fx,2)+std::pow(l1.y()/c.fy,2);
            if (denominator > 1e-30 && numerator*numerator/denominator < options.inlierThresholdPixels*options.inlierThresholdPixels) inliers.insert(ids[i]);
        }
        return inliers;
    };
    for (int trial = 0; trial < options.ransacIterations; ++trial) {
        if (trial % 16 == 0) progress(options.solve, .02, "Rejecting keyframe outliers");
        std::shuffle(order.begin(), order.end(), random);
        libmv::Mat u(2,8), v(2,8);
        for (int i = 0; i < 8; ++i) { u.col(i)=x[order[i]].head<2>(); v.col(i)=y[order[i]].head<2>(); }
        libmv::Mat3 f;
        libmv::NormalizedEightPointSolver(u,v,&f);
        if (!f.allFinite()) continue;
        auto inliers=score(f);
        if (inliers.size()>best.size()) { best=std::move(inliers); model=f; }
    }
    if (best.size()<12) return {};
    libmv::Mat u(2,best.size()),v(2,best.size()); size_t column=0;
    for (size_t i=0;i<ids.size();++i) if (best.count(ids[i])) {u.col(column)=x[i].head<2>();v.col(column)=y[i].head<2>();++column;}
    libmv::NormalizedEightPointSolver(u,v,&model);
    return model.allFinite()?score(model):std::set<int64_t>();
}
}

CameraReconstruction solveTrackedCamera(const CameraCalibration& calibration, const FeatureTracks& tracks,
                                       const TrackedSolveOptions& options)
{
    if(options.solve.intrinsics!=CameraSolveOptions::Intrinsics::Fixed)
        return estimateTrackedCamera(calibration,tracks,options);
    if(calibration.width<=0 || calibration.height<=0 || !std::isfinite(calibration.fx) || calibration.fx<=0 ||
       !std::isfinite(calibration.fy) || calibration.fy<=0 || !std::isfinite(calibration.cx) || !std::isfinite(calibration.cy) ||
       tracks.width!=calibration.width || tracks.height!=calibration.height)
        throw CameraSolveError(CameraSolveFailure::InvalidInput,"Invalid camera calibration or track image domain");
    if (tracks.frames.size()<2 || tracks.observations.empty() || !std::isfinite(options.inlierThresholdPixels) || options.inlierThresholdPixels<=0 ||
        !std::isfinite(options.solve.maximumRmsPixels) || options.solve.maximumRmsPixels<=0 ||
        options.ransacIterations<1 || options.ransacIterations>10000) throw std::invalid_argument("Invalid tracked solve configuration");
    std::set<double> frameSet;
    for(double f:tracks.frames)if(!std::isfinite(f)||!frameSet.insert(f).second)
        throw CameraSolveError(CameraSolveFailure::InvalidInput,"Invalid or duplicate tracking frame");
    std::map<double,std::map<int64_t,const TrackObservation*>> samples;
    for (const auto& o:tracks.observations) if(o.enabled && o.weight>0) {
        if(!std::isfinite(o.frame)||!frameSet.count(o.frame)||!std::isfinite(o.x)||!std::isfinite(o.y)||!std::isfinite(o.weight)||o.trackId<0 || samples[o.frame].count(o.trackId))
            throw CameraSolveError(CameraSolveFailure::InvalidInput,"Invalid or duplicate automatic track observation");
        samples[o.frame][o.trackId]=&o;
    }
    std::vector<Pair> pairs;
    if (!options.automaticKeyframes && options.solve.keyframes) {
        pairs.push_back({options.solve.keyframes->first,options.solve.keyframes->second,0,0});
    } else {
        const size_t step=std::max<size_t>(1,(tracks.frames.size()-1)/6);
        for(size_t ai=0;ai+1<tracks.frames.size();ai+=step) for(size_t bi=ai+1;bi<tracks.frames.size();++bi) {
            if(bi+1!=tracks.frames.size() && bi%step!=0)continue;
            size_t common=0;double motion=0;
            for(const auto& a:samples[tracks.frames[ai]]) {auto b=samples[tracks.frames[bi]].find(a.first);if(b!=samples[tracks.frames[bi]].end()) {++common;motion+=std::hypot(a.second->x-b->second->x,a.second->y-b->second->y);}}
            if(common>=16 && motion/common>=3) pairs.push_back({tracks.frames[ai],tracks.frames[bi],double(common)*std::sqrt(motion/common),common});
        }
        std::stable_sort(pairs.begin(),pairs.end(),[](const Pair& a,const Pair& b){return a.score>b.score;});
        if(pairs.size()>8)pairs.resize(8);
    }
    std::string diagnostic="No keyframe pair has enough common tracks and camera motion";
    for(const auto& pair:pairs) {
        progress(options.solve,0,"Selecting reconstruction keyframes");
        try {
            const auto inliers=pairInliers(calibration,tracks,pair,options);
            if(inliers.size()<12){diagnostic="Not enough geometrically consistent keyframe tracks";continue;}
            std::set<int64_t> common;
            for(const auto& a:samples[pair.first])if(samples[pair.second].count(a.first))common.insert(a.first);
            std::vector<TrackObservation> observations;
            for(const auto& o:tracks.observations)if(!common.count(o.trackId)||inliers.count(o.trackId))observations.push_back(o);
            auto config=options.solve;config.keyframes={{pair.first,pair.second}};config.maximumRmsPixels=1e6;
            CameraReconstruction result;
            for(int refinement=0;refinement<3;++refinement) {
                result=solveCamera(calibration,observations,config);
                std::set<std::pair<double,int64_t>> rejected;
                for(const auto& r:result.residuals)if(r.errorPixels>options.inlierThresholdPixels*2)rejected.emplace(r.frame,r.trackId);
                if(rejected.empty())break;
                observations.erase(std::remove_if(observations.begin(),observations.end(),[&](const auto& o){return rejected.count({o.frame,o.trackId})!=0;}),observations.end());
            }
            if(result.rmsPixels>options.solve.maximumRmsPixels) {diagnostic="Tracked solve exceeds the requested reprojection tolerance";continue;}
            bool complete=result.unregisteredFrames.empty();
            for(double frame:tracks.frames) if(std::none_of(result.cameras.begin(),result.cameras.end(),[&](const auto& c){return c.frame==frame;}))complete=false;
            if(!complete){diagnostic="Some image frames could not be registered; shorten the range or improve tracking";continue;}
            progress(options.solve,1,"Automatic camera solve complete");return result;
        }catch(const CameraSolveError& e){if(e.reason==CameraSolveFailure::Cancelled)throw;diagnostic=e.what();}
    }
    throw CameraSolveError(CameraSolveFailure::Reconstruction,diagnostic);
}
}} // namespace Natron::Scene
