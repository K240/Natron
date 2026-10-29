// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Engine/NativeScene3D.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSaveFile>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <limits>
#include "Engine/AppManager.h"
#include "Engine/AppInstance.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoCommand.h"
#ifdef NATRON_HAS_USD
#include "Scene/ReconstructionUsd.h"
#endif
NATRON_NAMESPACE_ENTER
using namespace Gpu::Cards;
struct ReconstructionLayoutState
{
    KnobStringPtr selection, frame, status, renderer;
    KnobDoublePtr distance, size, assetScale;
    KnobIntPtr reference;
    KnobFilePtr asset;
    std::map<std::string, KnobButtonPtr> actions;
    QTemporaryDir files;
    std::mutex mutex;
};
namespace
{
struct FrameRecord
{
    Scene::SceneFrame frame;
    std::string solve;
};
FrameRecord record(const std::string& text)
{
    FrameRecord r;
    if (text.empty())
        return r;
    const auto o = QJsonDocument::fromJson(QByteArray::fromStdString(text)).object();
    const auto m = o.value(QStringLiteral("matrix")).toArray();
    if (o.value(QStringLiteral("version")).toInt() != 1 || m.size() != 16)
        throw std::runtime_error("Invalid saved scene frame");
    for (int i = 0; i < 16; ++i)
        r.frame.matrix[i] = m[i].toDouble(std::numeric_limits<double>::quiet_NaN());
    r.frame.groundAligned = o.value(QStringLiteral("ground")).toBool();
    r.frame.metricScale = o.value(QStringLiteral("metric")).toBool();
    r.solve = o.value(QStringLiteral("solve")).toString().toStdString();
    Scene::frameScale(r.frame);
    return r;
}
std::string encoded(const FrameRecord& r)
{
    QJsonArray m;
    for (double v : r.frame.matrix)
        m.append(v);
    return QJsonDocument(QJsonObject { { QStringLiteral("version"), 1 }, { QStringLiteral("matrix"), m },
                             { QStringLiteral("ground"), r.frame.groundAligned },
                             { QStringLiteral("metric"), r.frame.metricScale },
                             { QStringLiteral("solve"), QString::fromStdString(r.solve) } })
        .toJson(QJsonDocument::Compact)
        .toStdString();
}
NativeScene3D* native(const NodePtr& n)
{
    return n ? dynamic_cast<NativeScene3D*>(n->getEffectInstance().get()) : nullptr;
}
NativeScene3D* inputScene(const NativeScene3D* n)
{
    auto p = n->getInput(0);
    for (int i = 0; p && p->getPluginID() == PLUGINID_NATRON_DOT && i < 1024; ++i)
        p = p->getInput(0);
    return p ? dynamic_cast<NativeScene3D*>(p.get()) : nullptr;
}
Matrix gpu(const Scene::SceneFrame& f)
{
    Matrix m;
    m.m = f.matrix;
    return m;
}
Vec3 vec(const std::array<double, 3>& p)
{
    return { p[0], p[1], p[2] };
}
Camera frameCamera(Camera c, const Scene::SceneFrame& f)
{
    const double scale = Scene::frameScale(f);
    const auto m = gpu(f);
    c.transform = m * c.transform;
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row)
            c.transform.m[col * 4 + row] /= scale;
    c.nearClip *= scale;
    c.farClip *= scale;
    return c;
}
class FrameUndo : public UndoCommand
{
    KnobStringWPtr knob;
    std::string before, after;

public:
    FrameUndo(const KnobStringPtr& k, std::string a, std::string b, const std::string& label)
        : UndoCommand(label)
        , knob(k)
        , before(std::move(a))
        , after(std::move(b))
    {
    }
    void redo() override
    {
        if (auto k = knob.lock())
            k->setValue(after);
    }
    void undo() override
    {
        if (auto k = knob.lock())
            k->setValue(before);
    }
};
class LayoutUndo : public UndoCommand
{
    std::vector<NodePtr> nodes;
    NodeWPtr renderer, before, after;
    bool first = true, undone = false;
    KnobStringWPtr frame;
    std::string frameBefore, frameAfter;

public:
    LayoutUndo(std::vector<NodePtr> n, const NodePtr& r, const NodePtr& a, const NodePtr& b,
        const KnobStringPtr& f, std::string fa, std::string fb, const std::string& label)
        : UndoCommand(label)
        , nodes(std::move(n))
        , renderer(r)
        , before(a)
        , after(b)
        , frame(f)
        , frameBefore(std::move(fa))
        , frameAfter(std::move(fb))
    {
    }
    ~LayoutUndo() override
    {
        if (undone)
            for (auto i = nodes.rbegin(); i != nodes.rend(); ++i)
                (*i)->destroyNode(false, false);
    }
    void redo() override
    {
        if (first)
        {
            first = false;
            return;
        }
        for (const auto& n : nodes)
            n->activate({}, false, false);
        if (auto r = renderer.lock())
        {
            r->disconnectInput(0);
            r->connectInput(after.lock(), 0);
        }
        if (auto f = frame.lock())
            f->setValue(frameAfter);
        undone = false;
    }
    void undo() override
    {
        if (auto r = renderer.lock())
        {
            r->disconnectInput(0);
            r->connectInput(before.lock(), 0);
        }
        for (auto i = nodes.rbegin(); i != nodes.rend(); ++i)
            (*i)->deactivate({}, true, false, true, false, false);
        if (auto f = frame.lock())
            f->setValue(frameBefore);
        undone = true;
    }
};
std::vector<int64_t> ids(const std::string& text)
{
    std::istringstream stream(text);
    std::vector<int64_t> result;
    int64_t id;
    while (stream >> id)
        if (id >= 0 && std::find(result.begin(), result.end(), id) == result.end())
            result.push_back(id);
    if (!stream.eof())
        throw std::runtime_error("Landmark IDs must be whitespace-separated nonnegative integers");
    return result;
}
std::vector<std::array<double, 3>> selected(const Scene3DSnapshot& scene, const std::vector<int64_t>& ids)
{
    std::vector<std::array<double, 3>> points;
    for (auto id : ids)
    {
        const auto p = std::find_if(scene.guidePoints.begin(), scene.guidePoints.end(),
            [&](const auto& v) { return v.trackId == id; });
        if (p == scene.guidePoints.end())
            throw std::runtime_error("A selected landmark no longer exists in this solve");
        points.push_back({ p->position.x, p->position.y, p->position.z });
    }
    return points;
}
Vec3 mean(const std::vector<std::array<double, 3>>& points)
{
    if (points.empty())
        throw std::runtime_error("Select landmarks in Points mode first");
    Vec3 p;
    for (const auto& v : points)
        p = p + vec(v);
    return p * (1. / points.size());
}
Vec3 euler(const Matrix& r)
{
    const double y = std::asin(std::clamp(-r.m[2], -1., 1.)), c = std::cos(y);
    double x, z;
    if (std::abs(c) > 1e-8)
    {
        x = std::atan2(r.m[6], r.m[10]);
        z = std::atan2(r.m[1], r.m[0]);
    }
    else
    {
        x = std::atan2(-r.m[9], r.m[5]);
        z = 0;
    }
    return { x * 180 / 3.14159265358979323846, y * 180 / 3.14159265358979323846,
        z * 180 / 3.14159265358979323846 };
}
void vectorKnob(const NodePtr& n, const char* name, Vec3 v)
{
    auto k = std::dynamic_pointer_cast<KnobDouble>(n->getEffectInstance()->getKnobByName(name));
    if (!k)
        throw std::runtime_error(std::string("Missing layout control: ") + name);
    k->setValue(v.x, ViewSpec::all(), 0);
    k->setValue(v.y, ViewSpec::all(), 1);
    k->setValue(v.z, ViewSpec::all(), 2);
}
}
void NativeScene3D::initializeLayoutKnobs(const KnobPagePtr& page)
{
    _layout = std::make_shared<ReconstructionLayoutState>();
    auto text = [&](const char* name, const char* label, bool secret = false)
    {
        auto k = AppManager::createKnob<KnobString>(this, tr(label));
        k->setName(name);
        k->setAnimationEnabled(false);
        k->setSecret(secret);
        page->addKnob(k);
        return k;
    };
    auto number = [&](const char* name, const char* label, double value)
    {
        auto k = AppManager::createKnob<KnobDouble>(this, tr(label));
        k->setName(name);
        k->setAnimationEnabled(false);
        k->setDefaultValue(value);
        k->setMinimum(.000001);
        k->setDisplayMinimum(.01);
        k->setDisplayMaximum(10);
        k->setIncrement(.01);
        k->setDecimals(4);
        k->setEvaluateOnChange(false);
        page->addKnob(k);
        return k;
    };
    auto button = [&](const char* name, const char* label)
    {
        auto k = AppManager::createKnob<KnobButton>(this, tr(label));
        k->setName(name);
        page->addKnob(k);
        _layout->actions[name] = k;
    };
    _layout->selection = text("selectedLandmarks", "Selected landmark IDs");
    _layout->selection->setEvaluateOnChange(false);
    _layout->selection->setHintToolTip(tr(
        "Enable Points in the 3D Viewer. Click a landmark; Shift-click adds or removes it. The first two "
        "ground points define the X direction. You can also enter whitespace-separated landmark IDs here."));
    button("setGround", "Set ground from selection");
    button("setOrigin", "Set origin from selection");
    _layout->actions["setGround"]->setHintToolTip(
        tr("Fit a plane through at least three non-collinear landmarks and map it to Y=0. The existing up "
           "direction chooses the plane normal. Camera, points and placed geometry move together."));
    _layout->actions["setOrigin"]->setHintToolTip(
        tr("Place the origin at the selection centroid. After ground alignment, move only X and Z to "
           "preserve the ground at Y=0."));
    _layout->distance = number("knownDistance", "Known distance (m)", 1);
    button("setScale", "Set scale from two landmarks");
    button("resetFrame", "Reset scene frame");
    _layout->size = number("placementSize", "Card / ground size", 2);
    button("createAxis", "Create axis at selection");
    button("createCard", "Create card at selection");
    button("createShadowCatcher", "Create shadow catcher ground");
    button("createReflectionCatcher", "Create reflection catcher ground");
    _layout->actions["createReflectionCatcher"]->setHintToolTip(
        tr("Create an aligned reflection receiver. To add reflections to an existing shadow ground, enable "
           "Catch reflections on that ShadowCatcher instead of adding another coplanar card."));
    button("createOccluder", "Create occluder card");
    button("createKeyLight", "Create key light");
    _layout->reference = AppManager::createKnob<KnobInt>(this, tr("Projection reference frame"));
    _layout->reference->setName("projectionFrame");
    _layout->reference->setDefaultValue(1);
    _layout->reference->setAnimationEnabled(false);
    _layout->reference->setEvaluateOnChange(false);
    page->addKnob(_layout->reference);
    button("createProjectedGround", "Create projected ground");
    _layout->asset = AppManager::createKnob<KnobFile>(this, tr("USD asset"));
    _layout->asset->setName("assetFile");
    _layout->asset->setAnimationEnabled(false);
    _layout->asset->setEvaluateOnChange(false);
    page->addKnob(_layout->asset);
    _layout->assetScale = number("assetScale", "Asset scale", 1);
    button("placeAsset", "Place USD asset on ground");
    _layout->actions["placeAsset"]->setHintToolTip(
        tr("Create an editable ReadUSD asset reference. Its bounds are centered at the selected landmarks in "
           "X/Z, and its lowest point rests at Y=0 when ground is aligned. Source asset files remain "
           "unchanged."));
    _layout->actions["createProjectedGround"]->setHintToolTip(
        tr("Create a ground Card with a frozen Project3D using the target renderer's corrected background "
           "plate. The projection camera reads the scene before SceneSetup, so alignment remains shared "
           "without a feedback loop."));
    _layout->frame = text("sceneFrame", "Scene frame", true);
    _layout->renderer = text("layoutRenderer", "Target renderer", true);
    _layout->renderer->setEvaluateOnChange(false);
    _layout->status = text("layoutStatus", "Scene setup status");
    _layout->status->setAsMultiLine();
    _layout->status->setAllDimensionsEnabled(false);
    _layout->status->setIsPersistent(false);
    _layout->status->setEvaluateOnChange(false);
    _layout->status->setValue(
        "Enable Points in the 3D Viewer. Select 3+ ground points, then set ground; select two points to "
        "establish meters. Point selection order defines the ground X axis.");
}
std::vector<int64_t> NativeScene3D::selectedLandmarks() const
{
    try
    {
        return _layout ? ids(_layout->selection->getValue()) : std::vector<int64_t>();
    }
    catch (const std::exception&)
    {
        return {};
    }
}
void NativeScene3D::selectLandmarks(const std::vector<int64_t>& points)
{
    if (!_layout)
        return;
    std::ostringstream s;
    for (auto p : points)
        s << p << ' ';
    _layout->selection->setValue(s.str());
}
void NativeScene3D::updateLayoutStatus()
{
    if (!_layout)
        return;
    try
    {
        const auto r = record(_layout->frame->getValue());
        _layout->status->setValue(
            std::string(r.frame.groundAligned ? "Ground aligned. " : "Ground not aligned. ")
            + (r.frame.metricScale ? "Scale established in meters." : "Scale remains arbitrary.")
            + " Select landmarks in the 3D Viewer's Points mode.");
    }
    catch (const std::exception& e)
    {
        _layout->status->setValue(e.what());
    }
}
bool NativeScene3D::layoutKnobChanged(KnobI* knob)
{
    if (!_layout)
        return false;
    if (knob == _layout->frame.get())
    {
        updateLayoutStatus();
        return true;
    }
    for (const auto& a : _layout->actions)
        if (a.second.get() == knob)
        {
            layoutAction(a.first);
            return true;
        }
    return false;
}
Scene3DSnapshot NativeScene3D::layoutSceneAt(double time, ViewIdx view, bool preserveUsd) const
{
    auto input = inputScene(this);
    if (!input)
        throw std::runtime_error("SceneSetup needs a reconstruction scene input");
    auto scene = input->sceneAt(time, view, preserveUsd);
    if (scene.reconstructions.size() != 1)
        throw std::runtime_error("SceneSetup needs exactly one CameraTracker reconstruction branch");
    const auto r = record(_layout->frame->getValue());
    auto& recipe = scene.reconstructions.front();
    if (!r.solve.empty() && r.solve != recipe.fingerprint)
        throw std::runtime_error(
            "The camera solve changed. Reset the scene frame before setting up this reconstruction again");
    const Matrix m = gpu(r.frame);
    const double scale = Scene::frameScale(r.frame);
    for (auto& point : scene.guidePoints)
    {
        point.position = m.point(point.position);
        point.owner = getNode();
    }
    for (auto& source : scene.usdSources)
    {
        Scene::SceneFrame combined = r.frame;
        combined.matrix = (m * source.transform).m;
        if (source.ownerId == native(recipe.owner)->sceneId())
        {
#ifdef NATRON_HAS_USD
            const double clipScale = Scene::frameScale(combined) * recipe.clipScale;
            const auto aligned = std::make_shared<Scene::CameraReconstruction>(
                Scene::applySceneFrame(*recipe.value, combined));
            Scene::ReconstructionUsdOptions options;
            options.framesPerSecond = getApp()->getProjectFrameRate();
            options.imagePixelAspect = recipe.imagePixelAspect;
            options.metricScale = r.frame.metricScale || recipe.metricScale;
            options.clipScale = clipScale;
            options.solveId = source.ownerId;
            std::ostringstream key;
            key.precision(17);
            key << source.filename << '/' << options.framesPerSecond << '/' << options.imagePixelAspect << '/'
                << options.metricScale << '/' << clipScale;
            for (double v : combined.matrix)
                key << '/' << v;
            const auto hash
                = QCryptographicHash::hash(QByteArray::fromStdString(key.str()), QCryptographicHash::Sha256)
                      .toHex();
            std::lock_guard<std::mutex> lock(_layout->mutex);
            if (!_layout->files.isValid())
                throw std::runtime_error("Cannot create aligned reconstruction cache");
            const auto path = _layout->files.filePath(QString::fromLatin1(hash) + QStringLiteral(".usda"));
            if (!QFileInfo::exists(path))
            {
                const auto text = Scene::reconstructionUsdLayer(*aligned, options);
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly)
                    || file.write(text.data(), qint64(text.size())) != qint64(text.size()) || !file.commit())
                    throw std::runtime_error("Cannot publish aligned reconstruction");
            }
            source.filename = path.toStdString();
            source.transform = Matrix();
            source.revision = std::hash<std::string> {}(source.filename);
            recipe.value = aligned;
            recipe.metricScale = options.metricScale;
            recipe.clipScale = clipScale;
#else
            throw std::runtime_error("SceneSetup camera publication requires OpenUSD");
#endif
        }
        else
            source.transform = m * source.transform;
    }
    for (auto& item : scene.items)
    {
        item.card.transform = m * item.card.transform;
        item.editParentTransform = m * item.editParentTransform;
        if (item.card.projected)
            item.card.projector = frameCamera(item.card.projector, r.frame);
    }
    for (auto& object : scene.objects)
    {
        object.transform = m * object.transform;
        object.editParentTransform = m * object.editParentTransform;
        if (object.kind == Scene3DKind::Camera || object.kind == Scene3DKind::SceneCamera
            || object.kind == Scene3DKind::ReadCamera || object.kind == Scene3DKind::ReadUSDCamera)
            object.camera = frameCamera(object.camera, r.frame);
        if (object.kind == Scene3DKind::Light)
            object.light.transform = m * object.light.transform;
    }
    for (auto& light : scene.lights)
        light.transform = m * light.transform;
    for (auto& edit : scene.usdEdits)
        if (edit.kind == UsdPrimEdit::Kind::Transform)
            edit.transform = m * edit.transform * m.inverse();
    scene.camera = frameCamera(scene.camera, r.frame);
    return scene;
}
void NativeScene3D::layoutAction(const std::string& action)
{
    if (!_layout)
        return;
    std::vector<NodePtr> created;
    NodePtr rollbackInput;
    std::string rollbackFrame;
    bool graphChanged = false;
    try
    {
        if (action == "resetFrame")
        {
            getNode()->pushUndoCommand(std::make_shared<FrameUndo>(
                _layout->frame, _layout->frame->getValue(), "", "Reset scene frame"));
            _layout->status->setValue(
                "Scene frame reset. Placed geometry remains in the shared scene coordinate system.");
            return;
        }
        auto input = inputScene(this);
        if (!input)
            throw std::runtime_error("Connect the reconstruction scene first");
        const double time = getApp()->getTimeLine()->currentFrame();
        const ViewIdx view(0);
        const auto raw = input->sceneAt(time, view, true);
        if (raw.reconstructions.size() != 1)
            throw std::runtime_error("SceneSetup needs one reconstruction");
        auto r = record(_layout->frame->getValue());
        const auto& recipe = raw.reconstructions.front();
        if (!r.solve.empty() && r.solve != recipe.fingerprint)
            throw std::runtime_error("The camera solve changed; reset the scene frame first");
        const auto points = selected(raw, ids(_layout->selection->getValue()));
        if (action == "setGround" || action == "setOrigin" || action == "setScale")
        {
            if (action == "setGround")
                r.frame = Scene::groundFrame(r.frame, points);
            else if (action == "setOrigin")
                r.frame = Scene::originFrame(r.frame, points);
            else
            {
                if (points.size() != 2)
                    throw std::runtime_error("Select exactly two landmarks to set scale");
                r.frame = Scene::distanceFrame(r.frame, points[0], points[1], _layout->distance->getValue());
            }
            r.solve = recipe.fingerprint;
            getNode()->pushUndoCommand(std::make_shared<FrameUndo>(
                _layout->frame, _layout->frame->getValue(), encoded(r), "Align reconstruction"));
            _layout->status->setValue(std::string(r.frame.groundAligned ? "Ground aligned. " : "")
                + (r.frame.metricScale ? "Scale established in meters." : "Scale remains arbitrary."));
            return;
        }
        NodePtr render;
        for (const auto& n : getNode()->getGroup()->getNodes())
            if (auto p = native(n))
                if (p->kind() == Scene3DKind::Render3D && p->sceneId() == _layout->renderer->getValue())
                    render = n;
        if (!render)
            throw std::runtime_error("Create this SceneSetup with CameraTracker's Create USD scene, or set "
                                     "its target renderer identity");
        auto downstream = native(render)->getInput(0);
        for (int i = 0; downstream && downstream.get() != this && i < 1024; ++i)
        {
            const auto effect = dynamic_cast<NativeScene3D*>(downstream.get());
            if (downstream->getPluginID() != PLUGINID_NATRON_DOT
                && (!effect
                    || (effect->kind() != Scene3DKind::USDTransform
                        && effect->kind() != Scene3DKind::USDVisibility
                        && effect->kind() != Scene3DKind::USDCompositing
                        && effect->kind() != Scene3DKind::BindMaterial)))
                break;
            downstream = downstream->getInput(0);
        }
        if (downstream.get() != this)
            throw std::runtime_error(
                "The target Render3D must read this SceneSetup, optionally through USD edit nodes");
        const auto before = getInput(0)->getNode();
        double x, y, beforeX, beforeY;
        getNode()->getPosition(&x, &y);
        before->getPosition(&beforeX, &beforeY);
        x = std::min(x, beforeX) - 260;
        rollbackInput = before;
        rollbackFrame = _layout->frame->getValue();
        auto create = [&](const char* plugin)
        {
            CreateNodeArgs args(plugin, getNode()->getGroup());
            args.setProperty<bool>(kCreateNodeArgsPropAutoConnect, false);
            args.setProperty<bool>(kCreateNodeArgsPropAddUndoRedoCommand, false);
            auto n = getApp()->createNode(args);
            if (!n)
                throw std::runtime_error("Cannot create layout node");
            n->setPosition(action == "createProjectedGround" ? x - 180 : x, y - 420 + created.size() * 140);
            created.push_back(n);
            return n;
        };
        const Matrix frame = gpu(r.frame);
        const double scale = Scene::frameScale(r.frame);
        const Matrix inverse = frame.inverse();
        Matrix rotation = inverse;
        rotation.m[12] = rotation.m[13] = rotation.m[14] = 0;
        for (int c = 0; c < 3; ++c)
            for (int a = 0; a < 3; ++a)
                rotation.m[c * 4 + a] *= scale;
        Vec3 anchor = frame.point(mean(points));
        NodePtr geometry;
        if (action == "createProjectedGround")
        {
            if (!r.frame.groundAligned)
                throw std::runtime_error("Set ground from three or more landmarks first");
            const auto src = std::find_if(raw.usdSources.begin(), raw.usdSources.end(),
                [&](const auto& s) { return s.ownerId == native(recipe.owner)->sceneId(); });
            if (src == raw.usdSources.end())
                throw std::runtime_error("Missing reconstruction camera source");
            const auto projector = create("fr.inria.built-in.SceneCamera");
            projector->connectInput(before, 0);
            std::dynamic_pointer_cast<KnobString>(projector->getEffectInstance()->getKnobByName("cameraPath"))
                ->setValue(src->mountPath + "/Source/Camera");
            const auto image = native(render)->getInput(2);
            if (!image)
                throw std::runtime_error("Connect the undistorted plate to Render3D's background");
            const auto projection = create("fr.inria.built-in.Project3D");
            projection->connectInput(image->getNode(), 0);
            projection->connectInput(projector, 1);
            std::dynamic_pointer_cast<KnobInt>(
                projection->getEffectInstance()->getKnobByName("referenceFrame"))
                ->setValue(_layout->reference->getValue());
            std::dynamic_pointer_cast<KnobBool>(projection->getEffectInstance()->getKnobByName("freeze"))
                ->setValue(true);
            geometry = create("fr.inria.built-in.Card");
            geometry->connectInput(projection, 0);
            anchor.y = 0;
            rotation = rotation * Matrix::transform({}, { -90, 0, 0 });
        }
        else if (action == "createAxis")
            geometry = create("fr.inria.built-in.Axis");
        else if (action == "createCard"
            || (action == "createShadowCatcher" || action == "createReflectionCatcher")
            || action == "createOccluder")
            geometry = create("fr.inria.built-in.Card");
        else if (action == "createKeyLight")
            geometry = create("fr.inria.built-in.Light");
        else if (action == "placeAsset")
        {
            std::string filename = _layout->asset->getValue();
            getApp()->getProject()->canonicalizePath(filename);
            if (filename.empty() || !QFileInfo::exists(QString::fromStdString(filename)))
                throw std::runtime_error("Choose an existing USD asset file");
            geometry = create("fr.inria.built-in.ReadUSD");
            std::dynamic_pointer_cast<KnobFile>(geometry->getEffectInstance()->getKnobByName("file"))
                ->setValue(filename);
            std::dynamic_pointer_cast<KnobChoice>(geometry->getEffectInstance()->getKnobByName("usdAssembly"))
                ->setValue(1);
            const auto asset = native(geometry)->sceneAt(time, view, true);
            UsdSceneBridge bridge;
            Gpu::Cards::Settings settings;
            bridge.exportScene({}, asset.camera, settings, asset.usdSources);
            const auto bounds
                = bridge.sceneValue()->inspect("/Natron/Assets/n_" + native(geometry)->sceneId() + "/Source");
            if (!bounds.hasBounds)
                throw std::runtime_error("USD asset has no drawable bounds");
            const double assetScale = _layout->assetScale->getValue();
            if (!std::isfinite(assetScale) || assetScale <= 0)
                throw std::runtime_error("Asset scale must be positive");
            if (r.frame.groundAligned)
                anchor.y = 0;
            anchor = anchor
                - Vec3 { (bounds.minimum.x + bounds.maximum.x) * .5, bounds.minimum.y,
                      (bounds.minimum.z + bounds.maximum.z) * .5 }
                    * assetScale;
        }
        else
            throw std::runtime_error("Unknown scene setup action");
        if ((action == "createShadowCatcher" || action == "createReflectionCatcher"))
        {
            if (!r.frame.groundAligned)
                throw std::runtime_error("Set ground from three or more landmarks first");
            anchor.y = 0;
            rotation = rotation * Matrix::transform({}, { -90, 0, 0 });
        }
        if (action == "createOccluder")
        {
            auto camera = dynamic_cast<NativeScene3D*>(native(render)->getInput(1).get());
            if (!camera)
                throw std::runtime_error("Connect a render camera before creating a proxy card");
            auto basis = camera->cameraAt(time, view).transform;
            basis.m[12] = basis.m[13] = basis.m[14] = 0;
            rotation = rotation * basis;
        }
        if (action == "createKeyLight")
            rotation = rotation * Matrix::transform({}, { -55, -30, 0 });
        geometry->setPosition(x, y - 140);
        vectorKnob(geometry, "translate", inverse.point(anchor));
        vectorKnob(geometry, "rotate", euler(rotation));
        const double factor = (action == "placeAsset" ? _layout->assetScale->getValue() : 1) / scale;
        vectorKnob(geometry, "scale", { factor, factor, factor });
        if (action == "createCard" || action == "createProjectedGround"
            || (action == "createShadowCatcher" || action == "createReflectionCatcher")
            || action == "createOccluder")
        {
            const double size = _layout->size->getValue();
            if (!std::isfinite(size) || size <= 0)
                throw std::runtime_error("Card size must be positive");
            auto k
                = std::dynamic_pointer_cast<KnobDouble>(geometry->getEffectInstance()->getKnobByName("size"));
            k->setValue(size, ViewSpec::all(), 0);
            k->setValue(size, ViewSpec::all(), 1);
        }
        if ((action == "createShadowCatcher" || action == "createReflectionCatcher")
            || action == "createOccluder")
        {
            auto role = create(action == "createReflectionCatcher" ? "fr.inria.built-in.ReflectionCatcher"
                    : action == "createShadowCatcher"              ? "fr.inria.built-in.ShadowCatcher"
                                                                   : "fr.inria.built-in.Occluder");
            role->setPosition(x, y - 80);
            if (!role->connectInput(geometry, 0))
                throw std::runtime_error("Cannot connect proxy geometry");
            geometry = role;
        }
        const auto merge = create("fr.inria.built-in.Scene");
        merge->setPosition(x, y - 30);
        if (!merge->connectInput(before, 0) || !merge->connectInput(geometry, 1))
            throw std::runtime_error("Cannot merge placed geometry into the scene");
        getNode()->disconnectInput(0);
        graphChanged = true;
        if (!getNode()->connectInput(merge, 0))
            throw std::runtime_error("Cannot connect placed geometry to SceneSetup");
        r.solve = recipe.fingerprint;
        const auto afterFrame = encoded(r);
        _layout->frame->setValue(afterFrame);
        // The undo target is SceneSetup's input, so all new nodes and the
        // branch connection are restored as one graph edit.
        getNode()->pushUndoCommand(std::make_shared<LayoutUndo>(created, getNode(), before, merge,
            _layout->frame, rollbackFrame, afterFrame, "Place reconstruction geometry"));
        _layout->status->setValue(
            "Geometry placed in the shared scene frame. Use its transform controls or 3D gizmo to edit it.");
    }
    catch (const std::exception& e)
    {
        if (graphChanged)
        {
            getNode()->disconnectInput(0);
            getNode()->connectInput(rollbackInput, 0);
            _layout->frame->setValue(rollbackFrame);
        }
        for (auto i = created.rbegin(); i != created.rend(); ++i)
            (*i)->destroyNode(true, false);
        _layout->status->setValue(e.what());
    }
}
NATRON_NAMESPACE_EXIT
