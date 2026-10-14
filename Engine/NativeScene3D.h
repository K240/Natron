// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_NATIVESCENE3D_H
#define NATRON_ENGINE_NATIVESCENE3D_H
#include <Python.h>
#include "Engine/EffectInstance.h"
#include "Gpu/CardScene.h"
#include "Gpu/FbxMesh.h"
#include "Gpu/UsdScene.h"
#include "Gpu/UsdSceneBridge.h"
#include <mutex>

NATRON_NAMESPACE_ENTER
enum class Scene3DKind { Card, Camera, Axis, TransformGeo, Scene, Project3D, Render3D, ReadGeo, ReadCamera, Light, PBRMaterial, ReadUSD, ReadUSDCamera, USDTransform, USDVisibility, BindMaterial, USDCompositing };
struct Scene3DMapSource {
    EffectInstancePtr source;
    unsigned channel = 0;
    int normalEncoding = -1; // -1 scalar, 0 unsigned RGB, 1 signed RGB
    bool flipY = false;
    Gpu::Cards::TextureAddress wrap = Gpu::Cards::TextureAddress::Repeat;
};
struct Scene3DItem {
    Gpu::Cards::Card card;
    Gpu::Cards::Matrix editParentTransform;
    NodePtr owner;
    EffectInstancePtr imageSource;
    double imageTime = 0;
    std::array<Scene3DMapSource,3> mapSources;
    double mapTime = 0;
};
// Editor objects are separate from the cards that produce image pixels.
struct Scene3DObject {
    Scene3DKind kind = Scene3DKind::Card;
    NodePtr owner;
    Gpu::Cards::Matrix transform, editParentTransform;
    Gpu::Cards::Camera camera;
    Gpu::Cards::Light light;
    double time = 0, cameraAspect = 1;
    int itemIndex = -1;
    bool worldSpace = false, renderCamera = false, projectionCamera = false;
};
struct Scene3DSnapshot {
    struct EnvironmentSource {size_t lightIndex=0;EffectInstancePtr source;double time=0;};
    std::vector<Scene3DItem> items;
    std::vector<Scene3DObject> objects;
    std::vector<Gpu::Cards::Light> lights;
    std::vector<EnvironmentSource> environmentSources;
    std::vector<Gpu::Cards::UsdSceneSource> usdSources;
    std::vector<Gpu::Cards::UsdPrimEdit> usdEdits;
    Gpu::Cards::Camera camera;
};
class NativeScene3D : public EffectInstance {
public:
    NativeScene3D(NodePtr node, Scene3DKind kind);
    int getMajorVersion() const override {return 1;}
    int getMinorVersion() const override {return 0;}
    std::string getPluginID() const override;
    std::string getPluginLabel() const override;
    std::string getPluginDescription() const override;
    void getPluginGrouping(std::list<std::string>* groups) const override;
    int getNInputs() const override;
    bool isInputOptional(int input) const override;
    std::string getInputLabel(int input) const override;
    void addAcceptedComponents(int, std::list<ImagePlaneDesc>* comps) override;
    void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const override;
    RenderSafetyEnum renderThreadSafety() const override {return eRenderSafetyInstanceSafe;}
    bool supportsTiles() const override {return false;}
    bool supportsMultiResolution() const override {return true;}
    bool supportsMultipleClipPARs() const override {return true;}
    bool supportsMultipleClipFPSs() const override {return true;}
    bool isViewAware() const override {return true;}
    bool doesTemporalClipAccess() const override {return true;}
    bool getCreateChannelSelectorKnob() const override {return false;}
    bool isMultiPlanar() const override {return _kind==Scene3DKind::Render3D;}
    PassThroughEnum isPassThroughForNonRenderedPlanes() const override {return ePassThroughBlockNonRenderedPlanes;}
    NodeDataType getOutputDataType() const override;
    bool acceptsInputDataType(int input, NodeDataType type) const override;
    void initializeKnobs() override;
    void purgeCaches() override;
    void onKnobsLoaded() override;
    bool knobChanged(KnobI* knob, ValueChangedReasonEnum reason, ViewSpec view, double time, bool mainThread) override;
    StatusEnum getRegionOfDefinition(U64, double, const RenderScale&, ViewIdx, RectD* rod) override;
    Scene3DKind kind() const {return _kind;}
    std::string sceneId() const;
    Gpu::Cards::Matrix transformAt(double time, ViewIdx view) const;
    Gpu::Cards::Camera cameraAt(double time, ViewIdx view) const;
    Scene3DSnapshot sceneAt(double time, ViewIdx view, bool preserveUsd=false) const;
    std::vector<Gpu::Cards::Card> texturedScene(const Scene3DSnapshot& scene, ViewIdx view, bool draft) const;
    void textureLights(Scene3DSnapshot& scene, ViewIdx view, bool draft) const;
    int rendererBackendAt(double time,ViewIdx view) const;
    Gpu::Cards::HdNvpSettings hdNvpSettingsAt(double time,ViewIdx view,bool draft) const;
    KnobDoublePtr translationKnob() const {return _translate.lock();}
    static const ImagePlaneDesc& depthPlane();
    static const ImagePlaneDesc& albedoPlane();
    static const ImagePlaneDesc& normalPlane();
    static const ImagePlaneDesc& positionPlane();
    bool depthPreviewAt(double time, ViewIdx view) const;
    std::array<double,2> depthPreviewRangeAt(double time, ViewIdx view) const;
protected:
    StatusEnum render(const RenderActionArgs& args) override;
    StatusEnum getPreferredMetadata(NodeMetadata& metadata) override;
    FramesNeededMap getFramesNeeded(double, ViewIdx) override {return {};}
    void getRegionsOfInterest(double, const RenderScale&, const RectD&, const RectD&, ViewIdx, RoIMap*) override {}
    void getComponentsNeededAndProduced(double time, ViewIdx view, ComponentsNeededMap* comps,
                                        double* passTime, int* passView, int* passInput) override;
private:
    std::shared_ptr<Gpu::Cards::UsdSceneAsset> usdAssetAt(ViewIdx view) const;
    std::shared_ptr<const Gpu::Cards::UsdScenePose> usdPoseAt(double time, ViewIdx view) const;
    std::shared_ptr<Gpu::Cards::FbxMeshAsset> fbxAssetAt(ViewIdx view) const;
    std::shared_ptr<const Gpu::Cards::Mesh> meshAt(double time, ViewIdx view) const;
    double fbxSecondsAt(double time, ViewIdx view) const;
    Gpu::Cards::Camera importedCameraAt(double time, ViewIdx view) const;
    Gpu::Cards::Material materialAt(double time, ViewIdx view) const;
    Gpu::Cards::Light lightAt(double time, ViewIdx view) const;
    void refreshAnimationInfo();
    Scene3DObject editorObjectAt(double time, ViewIdx view) const;
    void appendParentObjects(Scene3DSnapshot& scene, double time, ViewIdx view, bool worldSpace = false) const;
    Scene3DKind _kind;
    KnobDoubleWPtr _translate, _rotate, _scale, _size, _opacity, _focal, _aperture, _near, _far, _orthoWidth;
    KnobBoolWPtr _orthographic, _freeze;
    KnobBoolWPtr _visible, _cameraVisible, _shadowVisible, _reflectionVisible, _holdout;
    KnobChoiceWPtr _imageTiming;
    KnobStringWPtr _usdTargets, _usdCamera;
    KnobIntWPtr _reference;
    KnobChoiceWPtr _backend, _samples, _output, _pathDenoiser;
    KnobIntWPtr _pathSamples, _pathBounces;
    KnobDoubleWPtr _depthRange;
    KnobFileWPtr _file;
    KnobBoolWPtr _readAnimation, _applySkinTransform;
    KnobStringWPtr _animationTake, _animationInfo;
    KnobDoubleWPtr _animationStart, _animationSpeed, _animationOffset;
    KnobStringWPtr _cameraName, _cameraInfo;
    KnobStringWPtr _primPath, _variants;
    KnobStringWPtr _sceneId;
    KnobChoiceWPtr _usdAssemblyMode;
    KnobBoolWPtr _loadPayloads;
    mutable std::shared_ptr<Gpu::Cards::UsdSceneAsset> _usdAsset, _usdPoseAsset;
    mutable std::shared_ptr<const Gpu::Cards::UsdScenePose> _usdPose;
    mutable double _usdTime=0;
    mutable bool _usdAnimated=false, _usdPayloads=true;
    mutable std::string _usdVariantSelection, _usdPrimPath;
    KnobDoubleWPtr _focalMultiplier;
    KnobColorWPtr _baseColor, _emission, _lightColor;
    KnobDoubleWPtr _metallic, _roughness, _ior, _materialOpacity, _intensity, _exposure, _coneAngle, _coneSoftness, _shadowBias;
    KnobChoiceWPtr _lightType, _environmentSamples;
    KnobChoiceWPtr _normalEncoding, _roughnessChannel, _metallicChannel, _mapWrap;
    KnobBoolWPtr _normalFlipY;
    KnobBoolWPtr _shadows;
    mutable std::shared_ptr<Gpu::Cards::FbxMeshAsset> _cameraAsset;
    mutable Gpu::Cards::Camera _importedCamera;
    mutable double _cameraTime=0;
    mutable bool _cameraAnimated=false;
    mutable std::string _cameraSelection, _cameraTake;
    mutable std::mutex _meshMutex;
    mutable std::shared_ptr<Gpu::Cards::FbxMeshAsset> _fbxAsset, _meshAsset;
    mutable std::shared_ptr<const Gpu::Cards::Mesh> _mesh;
    mutable double _meshTime=0;
    mutable std::string _meshTake;
    mutable bool _meshSkinTransform=false;
    mutable std::string _meshPath;
    mutable qint64 _meshStamp=-1,_meshSize=-1;
    std::unique_ptr<Gpu::Cards::Renderer> _renderer;
    std::shared_ptr<Gpu::VulkanDevice> _rendererDevice;
    std::unique_ptr<Gpu::Cards::UsdSceneBridge> _usdBridge;
    std::shared_ptr<const Gpu::Cards::HdNvpFrame> _usdFrame;
    std::string _usdFrameKey;
};
template<Scene3DKind K> class BuiltinScene3D : public NativeScene3D {
public:
    explicit BuiltinScene3D(NodePtr n):NativeScene3D(n,K) {}
    static EffectInstance* BuildEffect(NodePtr n) {return new BuiltinScene3D(n);}
};
NATRON_NAMESPACE_EXIT
#endif
