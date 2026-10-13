// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/CardScene.h"
#include "Gpu/Lighting.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace Natron { namespace Gpu { namespace Cards {
namespace {
double axis(Vec3 p,int a) {return a==0?p.x:a==1?p.y:p.z;}
double dot(Vec3 a,Vec3 b) {return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 cross(Vec3 a,Vec3 b) {return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
Mesh::Bounds emptyBounds() {
    const double inf=std::numeric_limits<double>::infinity();return {{inf,inf,inf},{-inf,-inf,-inf}};
}
void expand(Mesh::Bounds& b,Vec3 p) {
    b.minimum={std::min(b.minimum.x,p.x),std::min(b.minimum.y,p.y),std::min(b.minimum.z,p.z)};
    b.maximum={std::max(b.maximum.x,p.x),std::max(b.maximum.y,p.y),std::max(b.maximum.z,p.z)};
}
bool intersects(const Mesh::Bounds& b,Vec3 o,Vec3 d,double nearClip,double farClip) {
    for(int a=0;a<3;++a) {
        const double origin=axis(o,a),direction=axis(d,a),lo=axis(b.minimum,a),hi=axis(b.maximum,a);
        if(direction==0) {if(origin<lo || origin>hi)return false;continue;}
        double t0=(lo-origin)/direction,t1=(hi-origin)/direction;if(t0>t1)std::swap(t0,t1);
        nearClip=std::max(nearClip,t0);farClip=std::min(farClip,t1);if(nearClip>farClip)return false;
    }
    return true;
}
}
void Mesh::buildAcceleration() {
    if(triangles.empty())throw std::invalid_argument("FBX contains no visible polygon meshes.");
    if(triangles.size()>std::numeric_limits<unsigned>::max()/2)throw std::invalid_argument("Mesh has too many triangles.");
    for(const auto& triangle:triangles) {
        validateMaterial(triangle.material);
        for(const auto& vertex:triangle.vertices) {
            for(double v:{vertex.position.x,vertex.position.y,vertex.position.z,vertex.normal.x,vertex.normal.y,vertex.normal.z,vertex.uv[0],vertex.uv[1]})
                if(!std::isfinite(v))throw std::invalid_argument("FBX mesh contains non-finite vertex data.");
            for(float v:vertex.color)if(!std::isfinite(v))throw std::invalid_argument("FBX mesh contains non-finite vertex colors.");
        }
        for(float v:triangle.color)if(!std::isfinite(v))throw std::invalid_argument("FBX mesh contains non-finite material colors.");
    }
    triangleOrder.resize(triangles.size());std::iota(triangleOrder.begin(),triangleOrder.end(),0u);
    bvh.clear();bvh.reserve(triangles.size()*2);
    auto centroid=[&](unsigned i){const auto& t=triangles[i];return (t.vertices[0].position+t.vertices[1].position+t.vertices[2].position)*(1./3);};
    std::function<unsigned(unsigned,unsigned)> build=[&](unsigned begin,unsigned count) {
        const unsigned index=unsigned(bvh.size());bvh.emplace_back();Bounds box=emptyBounds(),centers=emptyBounds();
        for(unsigned i=begin;i<begin+count;++i) {
            for(const auto& v:triangles[triangleOrder[i]].vertices)expand(box,v.position);
            expand(centers,centroid(triangleOrder[i]));
        }
        bvh[index].bounds=box;
        if(count<=8){bvh[index].begin=begin;bvh[index].count=count;return index;}
        const Vec3 extent=centers.maximum-centers.minimum;
        const int a=extent.x>=extent.y && extent.x>=extent.z?0:extent.y>=extent.z?1:2;
        const unsigned mid=begin+count/2;
        std::nth_element(triangleOrder.begin()+begin,triangleOrder.begin()+mid,triangleOrder.begin()+begin+count,
            [&](unsigned i,unsigned j){const double x=axis(centroid(i),a),y=axis(centroid(j),a);return x<y || (x==y && i<j);});
        const unsigned left=build(begin,mid-begin),right=build(mid,begin+count-mid);
        bvh[index].left=left;bvh[index].right=right;return index;
    };
    build(0,unsigned(triangles.size()));bounds=bvh[0].bounds;
}
void Mesh::intersect(Vec3 origin,Vec3 direction,double nearClip,double farClip,std::vector<MeshHit>& hits) const {
    if(triangles.empty())return;
    if(bvh.empty())throw std::invalid_argument("Mesh acceleration structure was not built.");
    std::array<unsigned,64> stack{};unsigned size=1;
    while(size) {
        const auto& node=bvh[stack[--size]];
        if(!intersects(node.bounds,origin,direction,nearClip,farClip))continue;
        if(!node.count){stack[size++]=node.left;stack[size++]=node.right;continue;}
        for(unsigned i=node.begin;i<node.begin+node.count;++i) {
            const unsigned index=triangleOrder[i];const auto& t=triangles[index];
            const Vec3 e1=t.vertices[1].position-t.vertices[0].position,e2=t.vertices[2].position-t.vertices[0].position;
            const Vec3 p=cross(direction,e2);const double determinant=dot(e1,p);
            if(std::abs(determinant)<1e-14)continue;
            const Vec3 offset=origin-t.vertices[0].position;const double b1=dot(offset,p)/determinant;
            if(b1<0 || b1>1)continue;
            const Vec3 q=cross(offset,e1);const double b2=dot(direction,q)/determinant;
            if(b2<0 || b1+b2>1)continue;
            const double depth=dot(e2,q)/determinant;
            if(depth>=nearClip && depth<=farClip)hits.push_back({index,depth,b1,b2});
        }
    }
}
}}}
