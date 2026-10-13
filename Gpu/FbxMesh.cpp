// SPDX-License-Identifier: GPL-2.0-or-later
#include "Gpu/FbxMesh.h"
#include "ufbx.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace Natron { namespace Gpu { namespace Cards {
namespace {
Vec3 vector(ufbx_vec3 v){return {v.x,v.y,v.z};}
using SceneOwner=std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)>;
const ufbx_anim* selectAnimation(const ufbx_scene* scene,const std::string& take) {
    if(take.empty())return scene->anim;
    for(const auto* stack:scene->anim_stacks)
        if(std::string(stack->name.data,stack->name.length)==take)return stack->anim;
    throw std::invalid_argument("FBX animation take not found: "+take);
}
SceneOwner evaluateScene(const ufbx_scene* scene,double seconds,const std::string& take,bool skinning) {
    if(!std::isfinite(seconds))throw std::invalid_argument("FBX animation time must be finite.");
    const auto animation=selectAnimation(scene,take);
    if(!animation)return {nullptr,ufbx_free_scene};
    const double time=animation->time_begin+seconds;
    if(!std::isfinite(time))throw std::invalid_argument("FBX animation time exceeds the supported range.");
    ufbx_evaluate_opts options{};options.evaluate_skinning=skinning;ufbx_error error{};
    SceneOwner evaluated(ufbx_evaluate_scene(scene,animation,time,&options,&error),ufbx_free_scene);
    if(!evaluated){char message[1024]{};ufbx_format_error(message,sizeof(message),&error);throw std::runtime_error(std::string("Cannot evaluate FBX animation: ")+message);}
    return evaluated;
}
Vec3 coordinateAxis(ufbx_coordinate_axis axis) {
    switch(axis) {
    case UFBX_COORDINATE_AXIS_POSITIVE_X:return {1,0,0};case UFBX_COORDINATE_AXIS_NEGATIVE_X:return {-1,0,0};
    case UFBX_COORDINATE_AXIS_POSITIVE_Y:return {0,1,0};case UFBX_COORDINATE_AXIS_NEGATIVE_Y:return {0,-1,0};
    case UFBX_COORDINATE_AXIS_POSITIVE_Z:return {0,0,1};case UFBX_COORDINATE_AXIS_NEGATIVE_Z:return {0,0,-1};
    default:throw std::invalid_argument("FBX camera has invalid projection axes.");
    }
}
Camera extractCamera(const ufbx_scene* scene,const ufbx_node* node) {
    const auto source=node->camera;if(!source)throw std::invalid_argument("FBX camera node is missing.");
    const auto& m=node->node_to_world;
    Matrix world;world.m={m.m00,m.m10,m.m20,0,m.m01,m.m11,m.m21,0,m.m02,m.m12,m.m22,0,m.m03,m.m13,m.m23,1};
    // ufbx leaves this field unset when no target camera axes were requested
    // (the mesh-loading path). Such FBX cameras use +X forward and +Y up.
    const auto axes=ufbx_coordinate_axes_valid(source->projection_axes)?source->projection_axes:
        ufbx_coordinate_axes{UFBX_COORDINATE_AXIS_POSITIVE_Z,UFBX_COORDINATE_AXIS_POSITIVE_Y,UFBX_COORDINATE_AXIS_NEGATIVE_X};
    const Vec3 right=coordinateAxis(axes.right),up=coordinateAxis(axes.up),front=coordinateAxis(axes.front);
    Matrix projection;projection.m={right.x,right.y,right.z,0,up.x,up.y,up.z,0,front.x,front.y,front.z,0,0,0,0,1};
    Camera result;result.transform=world*projection;
    // Unit conversion can reside in the node matrices or geometry properties.
    // Remove only the remaining unit scale from the camera's basis and apply
    // it to clip/orthographic distances, preserving actual inherited scaling.
    const double unit=scene->metadata.root_scale;
    if(!std::isfinite(unit) || unit<=0)throw std::invalid_argument("FBX camera has invalid units.");
    for(int column=0;column<3;++column)for(int row=0;row<3;++row)result.transform.m[column*4+row]/=unit;
    result.orthographic=source->projection_mode==UFBX_PROJECTION_MODE_ORTHOGRAPHIC;
    result.focal=std::isfinite(source->focal_length_mm) && source->focal_length_mm>0?source->focal_length_mm:50;
    if(!result.orthographic)result.aperture=2*result.focal*source->field_of_view_tan.x;
    result.orthoWidth=source->orthographic_size.x*unit;
    // Missing FBX clip properties are zero; retain the native camera defaults.
    if(source->near_plane>0)result.nearClip=source->near_plane*unit;
    if(source->far_plane>0)result.farClip=source->far_plane*unit;
    try{result.transform.inverse();}catch(const std::exception& error) {
        throw std::invalid_argument("Invalid FBX camera transform: "+std::string(node->name.data,node->name.length)+": "+error.what());
    }
    if(!std::isfinite(result.aperture) || result.aperture<=0 || !std::isfinite(result.nearClip) ||
       !std::isfinite(result.farClip) || result.farClip<=result.nearClip ||
       (result.orthographic && (!std::isfinite(result.orthoWidth) || result.orthoWidth<=0)))
        throw std::invalid_argument("FBX camera has invalid projection or clipping settings.");
    return result;
}
std::array<float,4> materialColor(const ufbx_material* material) {
    if(!material)return {.7f,.7f,.7f,1};
    const auto& base=material->pbr.base_color.has_value?material->pbr.base_color:material->fbx.diffuse_color;
    const auto& factor=material->pbr.base_color.has_value?material->pbr.base_factor:material->fbx.diffuse_factor;
    const double f=factor.has_value?factor.value_real:1;
    const ufbx_vec3 color=base.has_value?base.value_vec3:ufbx_vec3{.7,.7,.7};
    // FBX transparency is a color multiplied by a factor. Maya commonly
    // writes factor=1 with a black TransparentColor for an opaque material.
    // Collapse RGB transparency to its mean for our scalar compositing alpha.
    double transparency=material->fbx.transparency_factor.value_real;
    const auto& transparent=material->fbx.transparency_color;
    if(transparent.has_value) {
        const auto c=transparent.value_vec3;transparency*=(c.x+c.y+c.z)/3;
    }
    const auto& alpha=material->pbr.opacity;
    const double opacity=alpha.has_value?(alpha.value_components>=3?
        (alpha.value_vec3.x+alpha.value_vec3.y+alpha.value_vec3.z)/3:alpha.value_real):1-transparency;
    return {float(color.x*f),float(color.y*f),float(color.z*f),float(std::clamp(opacity,0.,1.))};
}
std::shared_ptr<const Mesh> extractMesh(const ufbx_scene* scene,bool deformed,bool applySkinTransform=false) {
    auto result=std::make_shared<Mesh>();
    bool hasPolygons=false;
    for(const ufbx_node* node:scene->nodes) {
        const ufbx_mesh* source=node->mesh;if(!source)continue;
        hasPolygons |= source->num_triangles>0;
        if(!node->visible)continue;
        bool visible=true;for(const ufbx_node* parent=node->parent;parent;parent=parent->parent)visible &= parent->visible;
        if(!visible)continue;
        std::vector<uint32_t> indices(source->max_face_triangles*3);
        bool local=!deformed || source->skinned_is_local;
        ufbx_matrix transform=node->geometry_to_world;
        if(!local && applySkinTransform && source->skin_deformers.count) {
            const ufbx_bone_pose* bind=ufbx_get_bone_pose(node->bind_pose,node);
            if(!bind)for(const auto* instance:source->instances) {
                bind=ufbx_get_bone_pose(instance->bind_pose,instance);if(bind)break;
            }
            if(!bind)throw std::runtime_error("Cannot apply FBX mesh transform to skin: mesh bind pose is missing.");
            const auto inverse=ufbx_matrix_invert(&bind->bone_to_world);
            transform=ufbx_matrix_mul(&node->geometry_to_world,&inverse);local=true;
        }
        const auto& positions=deformed?source->skinned_position:source->vertex_position;
        const auto& vertexNormals=deformed?source->skinned_normal:source->vertex_normal;
        const ufbx_matrix normals=ufbx_matrix_for_normals(&transform);
        size_t before=result->triangles.size();
        for(size_t faceIndex=0;faceIndex<source->faces.count;++faceIndex) {
            const auto& face=source->faces[faceIndex];
            if(face.num_indices<3 || (source->face_hole.count && source->face_hole[faceIndex]))continue;
            const uint32_t count=ufbx_triangulate_face(indices.data(),indices.size(),source,face);
            const unsigned mat=source->face_material.count?source->face_material[faceIndex]:0;
            const ufbx_material* material=mat<node->materials.count?node->materials[mat]:nullptr;
            for(uint32_t i=0;i<count;++i) {
                MeshTriangle triangle;triangle.color=materialColor(material);
                for(unsigned c=0;c<3;++c) {
                    const uint32_t index=indices[i*3+c];auto& vertex=triangle.vertices[c];
                    vertex.position=vector(local?ufbx_transform_position(&transform,positions[index]):positions[index]);
                    if(vertexNormals.exists) {
                        vertex.normal=vector(local?ufbx_transform_direction(&normals,vertexNormals[index]):vertexNormals[index]);
                        const double norm=std::sqrt(vertex.normal.x*vertex.normal.x+vertex.normal.y*vertex.normal.y+vertex.normal.z*vertex.normal.z);
                        if(norm>0)vertex.normal=vertex.normal*(1/norm);
                    }
                    if(source->vertex_uv.exists){const auto uv=source->vertex_uv[index];vertex.uv={uv.x,uv.y};}
                    if(source->vertex_color.exists){const auto color=source->vertex_color[index];vertex.color={float(color.x),float(color.y),float(color.z),float(std::clamp(color.w,0.,1.))};}
                }
                result->triangles.push_back(triangle);
            }
        }
        if(result->triangles.size()>before)++result->objectCount;
    }
    // Visibility can hide every mesh, including the unanimated file pose.
    // Keep that empty snapshot so another animation time can restore geometry.
    if(!hasPolygons)throw std::invalid_argument("FBX contains no polygon meshes.");
    if(!result->triangles.empty())result->buildAcceleration();
    return result;
}
}
struct FbxMeshAsset::Impl {
    std::unique_ptr<ufbx_scene,decltype(&ufbx_free_scene)> scene{nullptr,ufbx_free_scene};
    std::shared_ptr<const Mesh> mesh;
    std::vector<FbxAnimationTake> takes;
    std::vector<std::string> cameras;
    std::vector<uint32_t> cameraNodes;
    mutable std::mutex evaluateMutex;
};
FbxMeshAsset::FbxMeshAsset():_imp(std::make_unique<Impl>()) {}
FbxMeshAsset::~FbxMeshAsset()=default;
std::shared_ptr<FbxMeshAsset> FbxMeshAsset::load(const std::string& filename,bool loadGeometry) {
    if(filename.empty())throw std::invalid_argument("An FBX filename is required.");
    // std::filesystem's UTF-8 path preserves Japanese/non-ASCII filenames on Windows.
    std::ifstream file(std::filesystem::u8path(filename),std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("Cannot open FBX: "+filename);
    const auto length=file.tellg();if(length<=0)throw std::runtime_error("FBX file is empty: "+filename);
    std::vector<char> data(static_cast<size_t>(length));file.seekg(0);
    if(!file.read(data.data(),std::streamsize(data.size())))throw std::runtime_error("Cannot read FBX: "+filename);
    ufbx_load_opts options{};options.target_axes=ufbx_axes_right_handed_y_up;options.target_unit_meters=1;
    options.file_format=UFBX_FILE_FORMAT_FBX;options.generate_missing_normals=true;
    if(!loadGeometry){options.ignore_geometry=true;options.space_conversion=UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;options.target_camera_axes=ufbx_axes_right_handed_y_up;}
    // Image inputs remain under Natron's color management. External caches and
    // textures are not implicitly opened during loading or evaluation.
    options.load_external_files=false;ufbx_error error{};
    auto asset=std::shared_ptr<FbxMeshAsset>(new FbxMeshAsset);
    asset->_imp->scene.reset(ufbx_load_memory(data.data(),data.size(),&options,&error));
    if(!asset->_imp->scene){char message[1024]{};ufbx_format_error(message,sizeof(message),&error);throw std::runtime_error(std::string("Cannot load FBX: ")+message);}
    if(loadGeometry)asset->_imp->mesh=extractMesh(asset->_imp->scene.get(),false);
    for(const auto* node:asset->_imp->scene->nodes)if(node->camera) {
        std::string path(node->name.data,node->name.length);
        for(const auto* parent=node->parent;parent && !parent->is_root;parent=parent->parent)
            path=std::string(parent->name.data,parent->name.length)+"/"+path;
        asset->_imp->cameras.push_back(path);asset->_imp->cameraNodes.push_back(node->typed_id);
    }
    for(const auto* stack:asset->_imp->scene->anim_stacks)
        asset->_imp->takes.push_back({std::string(stack->name.data,stack->name.length),stack->time_begin,stack->time_end});
    return asset;
}
const std::vector<FbxAnimationTake>& FbxMeshAsset::takes() const {return _imp->takes;}
double FbxMeshAsset::frameRate() const {return _imp->scene->settings.frames_per_second;}
std::shared_ptr<const Mesh> FbxMeshAsset::staticMesh() const {
    if(!_imp->mesh)throw std::invalid_argument("FBX mesh geometry was not loaded.");return _imp->mesh;
}
std::shared_ptr<const Mesh> FbxMeshAsset::evaluate(double seconds,const std::string& take,bool applySkinTransform) const {
    staticMesh();
    std::lock_guard<std::mutex> lock(_imp->evaluateMutex);
    auto evaluated=evaluateScene(_imp->scene.get(),seconds,take,true);
    return evaluated?extractMesh(evaluated.get(),true,applySkinTransform):_imp->mesh;
}
const std::vector<std::string>& FbxMeshAsset::cameras() const {return _imp->cameras;}
Camera FbxMeshAsset::camera(const std::string& name,bool animated,double seconds,const std::string& take) const {
    if(_imp->cameras.empty())throw std::invalid_argument("FBX contains no cameras.");
    size_t selected=0;
    if(!name.empty()) {
        selected=_imp->cameras.size();size_t matches=0;
        for(size_t i=0;i<_imp->cameras.size();++i)if(_imp->cameras[i]==name){selected=i;++matches;}
        if(!matches) {
            for(size_t i=0;i<_imp->cameras.size();++i) {
                const auto* node=_imp->scene->nodes.data[_imp->cameraNodes[i]];
                if(std::string(node->name.data,node->name.length)==name){selected=i;++matches;}
            }
        }
        if(!matches)throw std::invalid_argument("FBX camera not found: "+name);
        if(matches>1)throw std::invalid_argument("FBX camera name is ambiguous; use a unique hierarchy path: "+name);
    }
    const auto index=_imp->cameraNodes[selected];
    if(!animated)return extractCamera(_imp->scene.get(),_imp->scene->nodes.data[index]);
    std::lock_guard<std::mutex> lock(_imp->evaluateMutex);
    auto evaluated=evaluateScene(_imp->scene.get(),seconds,take,false);
    const auto* scene=evaluated?evaluated.get():_imp->scene.get();
    return extractCamera(scene,scene->nodes.data[index]);
}
std::shared_ptr<const Mesh> loadFbxMesh(const std::string& filename) {return FbxMeshAsset::load(filename)->staticMesh();}
}}}
