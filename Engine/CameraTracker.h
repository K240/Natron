// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_ENGINE_CAMERATRACKER_H
#define NATRON_ENGINE_CAMERATRACKER_H
#include <Python.h>
#include "Engine/EffectInstance.h"
#include "Scene/FeatureTracking.h"
#include <memory>

NATRON_NAMESPACE_ENTER
class CameraTracker : public EffectInstance
{
    Q_OBJECT
public:
    explicit CameraTracker(NodePtr node);
    ~CameraTracker() override;
    static EffectInstance* BuildEffect(NodePtr node) {return new CameraTracker(node);}
    std::string getPluginID() const override {return "fr.inria.built-in.CameraTracker";}
    std::string getPluginLabel() const override {return "CameraTracker";}
    std::string getPluginDescription() const override;
    void getPluginGrouping(std::list<std::string>* groups) const override {groups->push_back("3D");}
    int getMajorVersion() const override {return 1;}
    int getMinorVersion() const override {return 0;}
    int getNInputs() const override {return 2;}
    std::string getInputLabel(int input) const override {return input==0?"Source":"tracking mask";}
    bool isInputOptional(int input) const override {return input==1;}
    NodeDataType getOutputDataType() const override {return NodeDataType::Image;}
    bool acceptsInputDataType(int,NodeDataType type) const override {return type==NodeDataType::Image || type==NodeDataType::Unknown;}
    bool supportsTiles() const override {return true;}
    bool supportsMultiResolution() const override {return true;}
    bool isViewAware() const override {return true;}
    bool doesTemporalClipAccess() const override {return true;}
    bool getCreateChannelSelectorKnob() const override {return false;}
    bool isHostChannelSelectorSupported(bool*,bool*,bool*,bool*) const override {return false;}
    RenderSafetyEnum renderThreadSafety() const override {return eRenderSafetyFullySafeFrame;}
    void addAcceptedComponents(int,std::list<ImagePlaneDesc>* components) override;
    void addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const override;
    void initializeKnobs() override;
    bool knobChanged(KnobI*,ValueChangedReasonEnum,ViewSpec,double,bool) override;
    void onKnobsLoaded() override;
    void onInputChanged(int) override;
    bool supportsOverlayDrawList() const override {return true;}
    bool hasOverlay() const override {return true;}
    std::shared_ptr<const Scene::CameraReconstruction> reconstruction() const;
    std::string usdAssetPath() const;
    bool analysisRunning() const;
protected:
    bool isIdentity(double,const RenderScale&,const RectI&,ViewIdx,double*,ViewIdx*,int*) override;
    void drawOverlay(double,const RenderScale&,ViewIdx) override;
private:
    struct Private;
    std::unique_ptr<Private> _imp;
    void startAnalysis(bool solve);
    void restoreAnalysis();
    void createScene();
    void updateStatus();
    std::string configuration(bool solve) const;
};

// Image correction remains a graph operation with an explicit analysis-data
// dependency. USD cameras and geometry stay in the ideal pinhole domain.
class CameraTrackerUndistort : public EffectInstance
{
public:
    explicit CameraTrackerUndistort(NodePtr n):EffectInstance(n){setSupportsRenderScaleMaybe(eSupportsYes);}
    static EffectInstance* BuildEffect(NodePtr n){return new CameraTrackerUndistort(n);}
    std::string getPluginID()const override{return "fr.inria.built-in.CameraTrackerUndistort";}
    std::string getPluginLabel()const override{return "UndistortPlate";}
    std::string getPluginDescription()const override{return "Remove the accepted CameraTracker Brown radial lens distortion. Source and CameraTracker are explicit inputs. The ideal plate retains its image size and pixel aspect, with black outside the source.";}
    void getPluginGrouping(std::list<std::string>* g)const override{g->push_back("3D");}
    int getMajorVersion()const override{return 1;}
    int getMinorVersion()const override{return 0;}
    int getNInputs()const override{return 2;}
    std::string getInputLabel(int i)const override{return i==0?"Source":"CameraTracker";}
    bool isInputOptional(int)const override{return false;}
    NodeDataType getOutputDataType()const override{return NodeDataType::Image;}
    bool acceptsInputDataType(int,NodeDataType t)const override{return t==NodeDataType::Image || t==NodeDataType::Unknown;}
    bool supportsTiles()const override{return true;}
    bool supportsMultiResolution()const override{return true;}
    bool getCreateChannelSelectorKnob()const override{return false;}
    bool isHostChannelSelectorSupported(bool*,bool*,bool*,bool*)const override{return false;}
    RenderSafetyEnum renderThreadSafety()const override{return eRenderSafetyFullySafeFrame;}
    void addAcceptedComponents(int,std::list<ImagePlaneDesc>*)override;
    void addSupportedBitDepth(std::list<ImageBitDepthEnum>*)const override;
    StatusEnum getPreferredMetadata(NodeMetadata&)override;
    StatusEnum getRegionOfDefinition(U64,double,const RenderScale&,ViewIdx,RectD*)override;
    StatusEnum render(const RenderActionArgs&)override;
};
NATRON_NAMESPACE_EXIT
#endif
