// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GPU_CARDSCENE_H
#define NATRON_GPU_CARDSCENE_H
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "Gpu/FragmentProgram.h"

namespace Natron { namespace Gpu { namespace Cards {
struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3 operator+(Vec3 b) const { return {x+b.x,y+b.y,z+b.z}; }
    Vec3 operator-(Vec3 b) const { return {x-b.x,y-b.y,z-b.z}; }
    Vec3 operator*(double s) const { return {x*s,y*s,z*s}; }
};
struct Matrix {
    // Column-major, right-handed coordinates, cameras look down -Z.
    std::array<double,16> m{1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    static Matrix transform(Vec3 translate, Vec3 rotateDegrees, Vec3 scale = {1,1,1});
    Matrix operator*(const Matrix& b) const;
    Matrix inverse() const;
    Vec3 point(Vec3 p) const;
    Vec3 vector(Vec3 p) const;
};
struct Camera {
    Matrix transform = Matrix::transform({0,0,5},{});
    double focal = 50, aperture = 36, nearClip = .001, farClip = 100000;
    bool orthographic = false;
    double orthoWidth = 4;
    // Both film offsets use horizontal aperture units. Vertical offsets stay
    // physically consistent when the output aspect conforms the film gate.
    double offsetX = 0, offsetY = 0;
    // Calibrated cameras can retain their full film gate. Zero keeps the
    // historical horizontal-fit behavior for native and imported cameras.
    double apertureAspect = 0;
    double filmAspect(double outputAspect) const { return apertureAspect > 0 ? apertureAspect : outputAspect; }
    std::string ownerId,label;
};
enum class TextureAddress { Black = 0, Clamp = 1, Repeat = 2, Mirror = 3 };
struct Texture {
    int width = 0, height = 0;
    // Linear premultiplied RGBA, row zero at the bottom; material samplers
    // instead hold straight color and premultiply after filtering. Data maps
    // retain straight signed components without premultiplication.
    std::vector<float> pixels;
    // Animated material UVs share the decoded pixels without copying images.
    std::shared_ptr<const std::vector<float>> sharedPixels;
    const std::vector<float>& pixelData() const {return sharedPixels?*sharedPixels:pixels;}
    // Map normalized source-format coordinates into this texture's data window.
    std::array<double,2> imageUvScale{1,1}, imageUvOffset{0,0};
    // USD material textures filter straight linear color, then premultiply.
    // Ordinary compositing images retain their existing data-window sampling.
    bool materialSampling = false;
    // Data maps retain signed values and independent alpha channels.
    bool dataSampling = false;
    TextureAddress wrapS = TextureAddress::Black, wrapT = TextureAddress::Black;
    // Row-major affine UV transform (two rows of three coefficients).
    std::array<double,6> uvTransform{1,0,0, 0,1,0};
    std::array<float,4> sample(double u, double v) const;
};
// Shared by imported surfaces and native materials. Existing scenes stay unlit.
struct Material {
    bool lit = false;
    std::array<float,4> baseColor{1,1,1,1};
    Vec3 emission;
    double metallic = 0, roughness = .5, ior = 1.5;
};
// Normal (RGB tangent space), roughness and metallic (selected RGBA channel).
struct MaterialMap {
    std::shared_ptr<const Texture> texture;
    unsigned channel = 0;
    // Native materials use the mesh's ordinary UVs; USD maps carry their own.
    bool useMeshUv = false;
};
enum class LightType { Distant, Point, Spot, Environment };
struct Light {
    LightType type = LightType::Distant;
    Matrix transform = Matrix::transform({0,0,5},{});
    Vec3 color{1,1,1};
    double intensity = 1, exposure = 0;
    // Spot angles are half angles about local -Z, in degrees.
    double coneAngle = 30, coneSoftness = .2;
    bool shadows = true;
    double shadowBias = .0001;
    std::string ownerId,label,instanceId;
    // Straight linear radiance in a latitude-longitude image. Alpha is ignored;
    // a missing image emits the uniform light color from every direction.
    std::shared_ptr<const Texture> environment;
    unsigned environmentSamples = 32; // samples per diffuse/specular lobe
};
struct MeshVertex {
    Vec3 position, normal;
    std::array<double,2> uv{.5,.5};
    std::array<float,4> color{1,1,1,1};
    std::array<std::array<double,2>,3> mapUvs{};
};
struct MeshTriangle {
    std::array<MeshVertex,3> vertices;
    // Straight linear base color. Image input overrides this material color.
    std::array<float,4> color{.7f,.7f,.7f,1};
    Material material;
};
struct MeshHit {unsigned triangle=0;double depth=0,b1=0,b2=0;};
struct Mesh {
    struct Bounds {Vec3 minimum,maximum;};
    struct BvhNode {Bounds bounds;unsigned begin=0,count=0,left=0,right=0;};
    std::vector<MeshTriangle> triangles;
    std::vector<unsigned> triangleOrder;
    std::vector<BvhNode> bvh;
    Bounds bounds;
    size_t objectCount=0;
    // Build once before sharing the immutable mesh across renders.
    void buildAcceleration();
    void intersect(Vec3 origin,Vec3 direction,double nearClip,double farClip,std::vector<MeshHit>& hits) const;
};
enum class GeometryRole { Surface = 0, ShadowCatcher = 1, Occluder = 2, ReflectionCatcher = 3, ShadowReflectionCatcher = 4 };
inline bool catchesShadows(GeometryRole role) {return role==GeometryRole::ShadowCatcher || role==GeometryRole::ShadowReflectionCatcher;}
inline bool catchesReflections(GeometryRole role) {return role==GeometryRole::ReflectionCatcher || role==GeometryRole::ShadowReflectionCatcher;}
struct Compositing {
    GeometryRole role = GeometryRole::Surface;
    bool cameraVisible = true, shadowVisible = true, reflectionVisible = true, holdout = false;
    double shadowDensity = 1;
    Vec3 shadowTint;
    double reflectionStrength = .25, reflectionRoughness = .2;
};
struct Card {
    Matrix transform;
    double width = 2, height = 2, opacity = 1;
    std::shared_ptr<const Texture> texture;
    std::array<MaterialMap,3> maps;
    // Optional triangle geometry; otherwise this is a rectangular Card.
    std::shared_ptr<const Mesh> mesh;
    Material material;
    bool overrideMaterial = false;
    bool projected = false;
    Camera projector;
    double projectionAspect = 1;
    bool projectionFrozen=false;
    double projectionReferenceTime=1,projectionImageTime=1;
    std::string ownerId,label,instanceId;
    // A material-only card authors bindings on these composed USD paths.
    std::vector<std::string> bindingPaths;
    Compositing compositing;
};

struct UsdPrimEdit {
    enum class Kind { Transform, Visibility, Material, Compositing };
    Kind kind = Kind::Transform;
    std::string identity;
    std::vector<std::string> paths;
    Matrix transform; // world delta in right-handed Y-up meters
    bool visible = true;
    std::string materialPath;
    bool cameraVisible = true, shadowVisible = true, reflectionVisible = true, holdout = false;
    GeometryRole role = GeometryRole::Surface;
    double shadowDensity = 1;
    Vec3 shadowTint;
    double reflectionStrength = .25, reflectionRoughness = .2;
};
enum class RenderPass { Color = 0, Depth = 1, Shadow = 2, Occlusion = 3, Reflection = 4 };
struct Settings {
    int width = 1, height = 1;
    double pixelAspect = 1;
    int samples = 2; // samples per axis: 1, 2 or 4
    RenderPass pass = RenderPass::Color;
    std::vector<Light> lights;
    std::vector<UsdPrimEdit> edits;
    // Viewer inspection uses a separate camera without changing the authored
    // render camera or its USD edit opinions.
    std::shared_ptr<const Camera> stageCamera;
    double stageCameraAspect=0; // zero conforms to the requested output
    bool transparentBackground=false; // retain lighting while hiding dome backgrounds
};
struct Hit { int card = -1; double depth = 0, u = 0, v = 0;
    std::array<float,4> tint{1,1,1,1};unsigned primitive=0;
    Vec3 normal{0,0,1};
    Vec3 tangent, bitangent;
    std::array<std::array<double,2>,3> mapUvs{};
    Material material;
};
Hit intersect(const Card& card, const Camera& camera, const Settings& settings, double x, double y);
std::vector<float> renderCpu(const std::vector<Card>& cards, const Camera& camera, const Settings& settings,
                             const std::function<bool()>& abort = {});
// Depth output is camera-local -Z replicated in RGB, with sample coverage in A.
// Empty pixels are zero. Convert it to premultiplied near-white/far-black RGBA.
void makeDepthPreview(std::vector<float>& pixels, double nearDepth, double farDepth);
class Renderer {
public:
    explicit Renderer(std::shared_ptr<VulkanDevice> device);
    std::vector<float> render(const std::vector<Card>& cards, const Camera& camera, const Settings& settings);
private:
    std::shared_ptr<VulkanDevice> _device;
    std::unique_ptr<FragmentProgram> _program;
    std::unique_ptr<FragmentProgram> _meshProgram;
    std::vector<float> renderMeshes(const std::vector<Card>& cards, const Camera& camera, const Settings& settings);
};
}}}
#endif
