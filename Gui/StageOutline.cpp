// SPDX-License-Identifier: GPL-2.0-or-later
#include <Python.h>
#include "Gui/StageOutline.h"
#include <QTreeWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QSignalBlocker>
#include <QTreeWidgetItemIterator>
#include <QSplitter>
#include <QComboBox>
#include <QTimer>
#include <QToolButton>
#include <QShowEvent>
#include <QScopedValueRollback>
#include <QMenu>
#include <QHBoxLayout>
#include <QUndoStack>
#include <map>
#include <set>
#include <algorithm>
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Format.h"
#include "Engine/TimeLine.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Gui/Gui.h"
#include "Gui/GuiAppInstance.h"
#include "Gui/ViewerTab.h"
#include "Gui/Scene3DViewer.h"
#include "Gui/NodeGui.h"
#include "Gui/NodeGraph.h"
#include "Gui/TabWidget.h"
#include "Gui/NodeGraphUndoRedo.h"
#include "Gui/KnobUndoCommand.h"
NATRON_NAMESPACE_ENTER
using namespace Gpu::Cards;
namespace {
constexpr int PathRole=Qt::UserRole,FetchRole=Qt::UserRole+1;
struct PageReply {UsdPrimPage page;QString error;};
struct SelectionReply {std::vector<UsdPrimInfo> prims;QString error;};
}
StageOutline::StageOutline(Gui* gui):QWidget(gui),PanelWidget(this,gui) {
    setObjectName("StageOutline");setMinimumWidth(200);
    auto layout=new QVBoxLayout(this);layout->setContentsMargins(4,4,4,4);layout->setSpacing(4);
    _sourceChoice=new QComboBox(this);_sourceChoice->setObjectName("StageOutlineSource");
    _sourceChoice->setToolTip(tr("Follow the active Viewer, or select a Render3D node to keep inspecting its stage."));
    _sourceChoice->addItem(tr("Follow active Viewer"),QString());layout->addWidget(_sourceChoice);
    _status=new QLabel(tr("Connect Render3D to inspect its composed USD stage."),this);_status->setObjectName("StageOutlineStatus");_status->setWordWrap(true);layout->addWidget(_status);
    _filter=new QLineEdit(this);_filter->setObjectName("StageOutlineFilter");_filter->setPlaceholderText(tr("Filter expanded prims"));_filter->setToolTip(tr("Filters fetched rows by path, name or type. Expand branches to inspect their children; payloads are not loaded by search."));layout->addWidget(_filter);
    _tree=new QTreeWidget(this);_tree->setObjectName("StageOutlineTree");_tree->setColumnCount(3);_tree->setHeaderLabels({tr("Prim"),tr("Type"),tr("State")});
    _tree->setSelectionMode(QAbstractItemView::ExtendedSelection);_tree->setUniformRowHeights(true);_tree->header()->setStretchLastSection(false);_tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);_tree->header()->setSectionResizeMode(1,QHeaderView::Fixed);_tree->header()->resizeSection(1,75);_tree->header()->setSectionResizeMode(2,QHeaderView::Fixed);_tree->header()->resizeSection(2,90);
    auto split=new QSplitter(Qt::Vertical,this);split->addWidget(_tree);layout->addWidget(split,1);
    _details=new QPlainTextEdit(split);_details->setObjectName("StageOutlineProperties");_details->setReadOnly(true);_details->setLineWrapMode(QPlainTextEdit::NoWrap);_details->setMinimumHeight(30);_details->setPlaceholderText(tr("Select a Prim to inspect composed properties."));split->addWidget(_details);split->setStretchFactor(0,1);split->setStretchFactor(1,0);split->setSizes({300,90});
    _frameButton=new QPushButton(tr("Frame selection"),this);_frameButton->setObjectName("StageOutlineFrame");_frameButton->setEnabled(false);layout->addWidget(_frameButton);connect(_frameButton,&QPushButton::clicked,this,&StageOutline::frameSelectionRequested);
    auto cameraRow=new QHBoxLayout;
    _lookButton=new QPushButton(tr("Look through"),this);_lookButton->setObjectName("StageOutlineLookThrough");_lookButton->setEnabled(false);cameraRow->addWidget(_lookButton);
    _renderCameraButton=new QPushButton(tr("Set render camera"),this);_renderCameraButton->setObjectName("StageOutlineRenderCamera");_renderCameraButton->setEnabled(false);cameraRow->addWidget(_renderCameraButton);layout->addLayout(cameraRow);
    connect(_lookButton,&QPushButton::clicked,this,&StageOutline::lookThroughSelectedCamera);connect(_renderCameraButton,&QPushButton::clicked,this,&StageOutline::useSelectedRenderCamera);
    _editButton=new QToolButton(this);_editButton->setText(tr("Add USD edit"));_editButton->setObjectName("StageOutlineEdit");_editButton->setPopupMode(QToolButton::InstantPopup);_editButton->setEnabled(false);
    auto edits=new QMenu(_editButton);edits->addAction(tr("Transform"),this,&StageOutline::addTransformEdit);edits->addAction(tr("Visibility"),this,&StageOutline::addVisibilityEdit);edits->addAction(tr("Bind material"),this,&StageOutline::addMaterialBinding);edits->addAction(tr("Compositing"),this,&StageOutline::addCompositingEdit);_editButton->setMenu(edits);layout->addWidget(_editButton);
    connect(this,&StageOutline::selectionReady,this,&StageOutline::syncSelection);
    connect(this,&StageOutline::frameSelectionRequested,this,&StageOutline::frameSelection);
    connect(_sourceChoice,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){
        setPinnedSource(_sourceChoice->itemData(index).toString());
        if(getGui())getGui()->getApp()->triggerAutoSave();
    });
    _composer=new QFutureWatcher<ComposeReply>(this);
    _timer=new QTimer(this);_timer->setInterval(200);
    connect(_timer,&QTimer::timeout,this,[this]{if(isVisible())refreshSource();});_timer->start();
    connect(_filter,&QLineEdit::textChanged,this,[this]{filterRows();});
    connect(_tree,&QTreeWidget::itemExpanded,this,[this](QTreeWidgetItem* item){
        const auto path=item->data(0,PathRole).toString();if(path.isEmpty())return;_expanded.insert(path);
        if(item->childCount()==1 && item->child(0)->data(0,FetchRole).toString()==QStringLiteral("unfetched"))fetch(item);
    });
    connect(_tree,&QTreeWidget::itemCollapsed,this,[this](QTreeWidgetItem* item){_expanded.remove(item->data(0,PathRole).toString());});
    connect(_tree,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem* item,int){
        if(item->data(0,FetchRole).toString()==QStringLiteral("more")){auto parent=item->parent();const size_t offset=item->data(1,FetchRole).toULongLong();delete item;fetch(parent,offset);}
    });
    connect(_tree,&QTreeWidget::itemSelectionChanged,this,[this]{_ownershipDirty=true;inspectSelection();});
}
QString StageOutline::sourceNode()const {
    auto node=_source.lock();return node?QString::fromStdString(node->getFullyQualifiedName()):QString();
}
void StageOutline::setPinnedSource(const QString& identity) {
    if(_pinnedId==identity)return;
    _pinnedId=identity;_sceneKey.clear();refreshSource();
}
void StageOutline::resetSource() {
    _pinnedId.clear();_source.reset();_scene={};_sceneKey.clear();_requestedKey.clear();
    _expanded.clear();_restorePaths.clear();
    {QSignalBlocker block(_sourceChoice);_sourceChoice->clear();_sourceChoice->addItem(tr("Follow active Viewer"),QString());}
    setScene({},tr("Connect Render3D to inspect its composed USD stage."));
    _frameButton->setEnabled(false);
}
void StageOutline::registerViewer(Scene3DViewer* viewer) {
    connect(viewer,&Scene3DViewer::primPathsSelected,this,[this,viewer](const QStringList& paths){
        if(!_framingSelection && _source.lock() && viewer->stageSource()==_source.lock())selectPrimPaths(paths);
    });
    connect(viewer,&Scene3DViewer::stageValueChanged,this,[this,viewer]{
        if(isVisible() && _source.lock() && viewer->stageSource()==_source.lock())viewer->setPrimSelection(_selected);
    });
    connect(viewer,&Scene3DViewer::sceneSnapshotChanged,this,[this,viewer]{
        // A hidden Viewer had no object snapshot when Outline revealed the
        // owning graph nodes. Reconcile it before graph-selection polling.
        if(isVisible() && _source.lock() && viewer->stageSource()==_source.lock())viewer->setPrimSelection(_selected);
    });
}
void StageOutline::refreshSource() {
    auto gui=getGui();if(!gui || gui->isAboutToClose())return;
    auto project=gui->getApp()->getProject();if(project->isLoadingProject() || !gui->getApp()->getNodesBeingCreated().empty())return;
    NodesList nodes;project->getActiveNodesExpandGroups(&nodes);
    std::vector<std::pair<QString,QString>> choices;
    NodePtr pinned;
    for(const auto& node:nodes)if(auto native=dynamic_cast<NativeScene3D*>(node->getEffectInstance().get())) {
        if(native->kind()!=Scene3DKind::Render3D)continue;
        const auto id=QString::fromStdString(native->sceneId());
        choices.emplace_back(QString::fromStdString(node->getLabel())+QStringLiteral(" (")+QString::fromStdString(node->getFullyQualifiedName())+QStringLiteral(")"),id);
        if(id==_pinnedId)pinned=node;
    }
    std::sort(choices.begin(),choices.end());
    {QSignalBlocker block(_sourceChoice);
        bool changed=_sourceChoice->count()!=int(choices.size())+1+(!_pinnedId.isEmpty() && !pinned?1:0);
        for(size_t i=0;i<choices.size() && !changed;++i)changed=_sourceChoice->itemText(int(i)+1)!=choices[i].first || _sourceChoice->itemData(int(i)+1).toString()!=choices[i].second;
        if(changed){_sourceChoice->clear();_sourceChoice->addItem(tr("Follow active Viewer"),QString());for(const auto& choice:choices)_sourceChoice->addItem(choice.first,choice.second);
            if(!_pinnedId.isEmpty() && !pinned)_sourceChoice->addItem(tr("Missing Render3D (%1)").arg(_pinnedId),_pinnedId);
        }
        _sourceChoice->setCurrentIndex(std::max(0,_sourceChoice->findData(_pinnedId)));
    }
    const auto viewers=gui->getViewersList();
    auto active=gui->getActiveViewer();
    if(std::find(viewers.begin(),viewers.end(),active)==viewers.end())active=viewers.empty()?nullptr:viewers.front();
    const auto source=_pinnedId.isEmpty()?Scene3DViewer::rendererForViewer(active):pinned;
    _frameButton->setEnabled(source && !_selected.empty() && std::any_of(viewers.begin(),viewers.end(),[&](ViewerTab* tab){return Scene3DViewer::rendererForViewer(tab)==source;}));
    if(!source){
        _source.reset();_scene={};_sceneKey.clear();_requestedKey.clear();
        setScene({},_pinnedId.isEmpty()?tr("Connect Render3D to the active Viewer, or select a Render3D source."):tr("The pinned Render3D node is missing."));return;
    }
    const double time=gui->getApp()->getTimeLine()->currentFrame();
    // A pinned stage uses the active Viewer's view; its source stays fixed.
    const ViewIdx view=active?active->getCurrentView():ViewIdx(0);
    Format format;project->getProjectDefaultFormat(&format);
    const auto id=QString::fromStdString(dynamic_cast<NativeScene3D*>(source->getEffectInstance().get())->sceneId());
    auto sceneKey=[&]{
        QString labels=QString::fromStdString(source->getLabel());
        for(const auto& object:_scene.objects)labels+=QStringLiteral(";")+QString::fromStdString(object.owner->getLabel());
        return QStringLiteral("%1/%2/%3/%4/%5/%6/%7/%8").arg(id).arg(source->getHashValue()).arg(time,0,'g',17).arg(view.value()).arg(format.width()).arg(format.height()).arg(format.getPixelAspectRatio(),0,'g',17).arg(labels);
    };
    auto key=sceneKey();
    _requestedKey=key;
    if(source!=_source.lock()) {
        // Selection belongs to a stage branch, not just to a coincident prim path.
        _restorePaths.clear();_expanded.clear();{QSignalBlocker block(_tree);_tree->clear();}
        _source=source;_scene={};_sceneKey.clear();setScene({},tr("Composing stage..."));
    }
    if(_composing || key==_sceneKey)return;
    _sceneKey=key;
    const auto branch=QString::fromStdString(source->getLabel())+tr(" · frame %1 · view %2").arg(time).arg(view.value());
    try {
        auto native=dynamic_cast<NativeScene3D*>(source->getEffectInstance().get());
        _scene=native->sceneAt(time,view,true);
        key=sceneKey();_sceneKey=_requestedKey=key;
        const auto cards=native->texturedScene(_scene,view,true);native->textureLights(_scene,view,true);
        if(!_bridge)_bridge=std::make_shared<UsdSceneBridge>();
        Gpu::Cards::Settings settings;settings.width=format.width();settings.height=format.height();settings.pixelAspect=format.getPixelAspectRatio();settings.lights=_scene.lights;settings.edits=_scene.usdEdits;
        const auto bridge=_bridge;const auto camera=_scene.camera;const auto sources=_scene.usdSources;
        _composing=true;
        disconnect(_composer,nullptr,this,nullptr);
        connect(_composer,&QFutureWatcher<ComposeReply>::finished,this,[this,key,branch]{
            _composing=false;auto reply=_composer->result();
            if(getGui() && key==_requestedKey){
                if(reply.error.isEmpty())setScene(std::move(reply.value),branch);
                else setScene({},branch+QStringLiteral("\n")+reply.error);
            }
            if(getGui() && isVisible())refreshSource();
        });
        _composer->setFuture(QtConcurrent::run([bridge,cards,camera,settings,sources]{
            ComposeReply reply;try{bridge->exportScene(cards,camera,settings,sources);reply.value=bridge->sceneValue();}catch(const std::exception& e){reply.error=QString::fromUtf8(e.what());}return reply;
        }));
    }catch(const std::exception& e){setScene({},branch+QStringLiteral("\n")+QString::fromUtf8(e.what()));}
}
void StageOutline::syncSelection() {
    auto gui=getGui();auto source=_source.lock();
    const bool camera=source && _selected.size()==1 && _selected.front().type=="Camera";
    const bool usd=source && dynamic_cast<NativeScene3D*>(source->getEffectInstance().get())->rendererBackendAt(source->getApp()->getTimeLine()->currentFrame(),ViewIdx(0))==3;
    _lookButton->setEnabled(camera);_renderCameraButton->setEnabled(camera && usd);_editButton->setEnabled(usd && !_selected.empty());
    if(!gui || !source || !isVisible())return;
    // Graph ownership remains usable with no visible 3D Viewer.
    if(!_selected.empty() && _ownershipDirty) {
        std::map<NodeGraph*,NodesGuiList> graphs;std::set<Node*> seen;
        for(const auto& prim:_selected)for(const auto& object:_scene.objects)if(auto native=dynamic_cast<NativeScene3D*>(object.owner->getEffectInstance().get())) {
            if(native->sceneId()==prim.ownerId && seen.insert(object.owner.get()).second)if(auto nodeGui=std::dynamic_pointer_cast<NodeGui>(object.owner->getNodeGui()))graphs[nodeGui->getDagGui()].push_back(nodeGui);
        }
        for(const auto& pair:graphs)if(pair.first)pair.first->setSelection(pair.second);
        _ownershipDirty=false;
    }
    for(auto tab:gui->getViewersList())if(Scene3DViewer::rendererForViewer(tab)==source)if(auto viewer=tab->findChild<Scene3DViewer*>())viewer->setPrimSelection(_selected);
    _frameButton->setEnabled(!_selected.empty() && std::any_of(gui->getViewersList().begin(),gui->getViewersList().end(),[&](ViewerTab* tab){return Scene3DViewer::rendererForViewer(tab)==source;}));
}
void StageOutline::lookThroughSelectedCamera() {
    if(!_value || _selected.size()!=1 || _selected.front().type!="Camera")return;
    const auto path=QString::fromStdString(_selected.front().path);const auto value=_value;
    try {
        const auto camera=value->camera(path.toStdString());auto gui=getGui();auto source=_source.lock();if(!gui || !source)return;
        auto viewers=gui->getViewersList();auto active=gui->getActiveViewer();if(std::find(viewers.begin(),viewers.end(),active)!=viewers.end()){viewers.remove(active);viewers.push_front(active);}
        QScopedValueRollback<bool> framing(_framingSelection,true);
        for(auto tab:viewers)if(Scene3DViewer::rendererForViewer(tab)==source) {
            if(auto pane=tab->getParentPane()){pane->makeCurrentTab(pane->getTabScriptNames().indexOf(QString::fromStdString(tab->getScriptName())));pane->window()->raise();}
            gui->setActiveViewer(tab);if(auto toggle=tab->findChild<QToolButton*>(QStringLiteral("Viewer3DToggle")))toggle->setChecked(true);
            if(auto viewer=tab->findChild<Scene3DViewer*>())viewer->lookThroughPrim(path,camera);return;
        }
    }catch(const std::exception& e){_details->setPlainText(QString::fromUtf8(e.what()));}
}
void StageOutline::useSelectedRenderCamera() {
    auto source=_source.lock();if(!source || !_value || _selected.size()!=1 || _selected.front().type!="Camera")return;
    auto knob=std::dynamic_pointer_cast<KnobString>(source->getKnobByName("usdCamera"));auto gui=std::dynamic_pointer_cast<NodeGui>(source->getNodeGui());if(!knob || !gui)return;
    gui->ensurePanelCreated();
    for(const auto& entry:gui->getKnobs())if(entry.first.lock()==knob) {
        entry.second->pushUndoCommand(new KnobUndoCommand<std::string>(entry.second,knob->getValue(),_selected.front().path));return;
    }
}
void StageOutline::addTransformEdit(){insertEdit("USDTransform");}
void StageOutline::addVisibilityEdit(){insertEdit("USDVisibility");}
void StageOutline::addMaterialBinding(){insertEdit("BindMaterial");}
void StageOutline::addCompositingEdit(){insertEdit("USDCompositing");}
void StageOutline::insertEdit(const std::string& plugin) {
    auto source=_source.lock();auto gui=getGui();if(!source || !gui || _selected.empty() || _composing)return;
    auto renderGui=std::dynamic_pointer_cast<NodeGui>(source->getNodeGui());if(!renderGui || !renderGui->getInputArrow(0))return;
    // Bind the exact inspected branch; a later source-combo change cannot
    // redirect an existing node's targets or insertion point.
    const auto paths=selectedPrimPaths().join(QLatin1Char('\n')).toStdString();
    CreateNodeArgs args("fr.inria.built-in."+plugin,source->getGroup());args.setProperty(kCreateNodeArgsPropAutoConnect,false);args.setProperty(kCreateNodeArgsPropAddUndoRedoCommand,false);
    args.setPropertyN<std::string>(kCreateNodeArgsPropNodeInitialParamValues,{"primPaths"});args.setProperty<std::string>(std::string(kCreateNodeArgsPropParamValue)+"_primPaths",paths,0,false);
    auto node=gui->getApp()->createNode(args);if(!node)return;auto nodeGui=std::dynamic_pointer_cast<NodeGui>(node->getNodeGui());if(!nodeGui)return;
    NodesGuiList added{nodeGui};
    if(plugin=="BindMaterial") {
        CreateNodeArgs materialArgs("fr.inria.built-in.PBRMaterial",source->getGroup());materialArgs.setProperty(kCreateNodeArgsPropAutoConnect,false);materialArgs.setProperty(kCreateNodeArgsPropAddUndoRedoCommand,false);
        if(auto material=gui->getApp()->createNode(materialArgs)){node->connectInput(material,1);added.push_back(std::dynamic_pointer_cast<NodeGui>(material->getNodeGui()));}
    }
    auto graph=renderGui->getDagGui();auto command=new QUndoCommand(tr("Add %1 to %2").arg(QString::fromStdString(plugin),QString::fromStdString(source->getLabel())));
    new AddMultipleNodesCommand(graph,added,command);new InsertNodeCommand(graph,renderGui->getInputArrow(0),nodeGui,command);graph->pushUndoCommand(command);
    graph->setSelection({nodeGui});nodeGui->ensurePanelCreated();gui->getApp()->triggerAutoSave();_sceneKey.clear();refreshSource();
}
void StageOutline::frameSelection() {
    auto gui=getGui();auto source=_source.lock();if(!gui || !source || _selected.empty())return;
    const auto prims=_selected;
    QScopedValueRollback<bool> framing(_framingSelection,true);
    auto viewers=gui->getViewersList();auto active=gui->getActiveViewer();
    if(std::find(viewers.begin(),viewers.end(),active)!=viewers.end()){viewers.remove(active);viewers.push_front(active);}
    for(auto tab:viewers)if(Scene3DViewer::rendererForViewer(tab)==source) {
        if(auto pane=tab->getParentPane()) {
            pane->makeCurrentTab(pane->getTabScriptNames().indexOf(QString::fromStdString(tab->getScriptName())));
            pane->window()->raise();
        }
        gui->setActiveViewer(tab);
        if(auto toggle=tab->findChild<QToolButton*>(QStringLiteral("Viewer3DToggle")))toggle->setChecked(true);
        if(auto viewer=tab->findChild<Scene3DViewer*>()){viewer->setPrimSelection(prims);viewer->frameSelection();}return;
    }
}
void StageOutline::notifyGuiClosing(){_timer->stop();_requestedKey.clear();}
void StageOutline::enterEvent(QtCompat::QEnterEvent* event){enterEventBase();QWidget::enterEvent(event);}
void StageOutline::leaveEvent(QEvent* event){leaveEventBase();QWidget::leaveEvent(event);}
void StageOutline::showEvent(QShowEvent* event){QWidget::showEvent(event);refreshSource();}
QString StageOutline::stageStatus()const{return _status->text();}
QStringList StageOutline::selectedPrimPaths()const {
    QStringList paths;for(auto item:_tree->selectedItems()){const auto p=item->data(0,PathRole).toString();if(!p.isEmpty())paths<<p;}paths.sort();return paths;
}
void StageOutline::setScene(std::shared_ptr<const UsdSceneValue> value,const QString& branch,bool resetTree) {
    if(value==_value){if(!value)_status->setText(branch);return;}
    if(value && _value && !resetTree) {
        _value=std::move(value);_status->setText(tr("%1 · USD time %2 · revision %3").arg(branch).arg(_value->timeCode()).arg(_value->revision()));
        refreshInspectionCameras();
        if(!selectedPrimPaths().isEmpty())inspectSelection();return;
    }
    _restorePaths=selectedPrimPaths();++_generation;++_selectionSerial;_value=std::move(value);_selected.clear();
    {QSignalBlocker block(_tree);_tree->clear();}_details->clear();Q_EMIT selectionReady();
    if(!_value){_status->setText(branch);return;}
    refreshInspectionCameras();
    _status->setText(tr("%1 · USD time %2 · revision %3").arg(branch).arg(_value->timeCode()).arg(_value->revision()));
    _expanded.insert(QStringLiteral("/Natron"));_expanded.insert(QStringLiteral("/Natron/Geometry"));_expanded.insert(QStringLiteral("/Natron/Assets"));
    fetch(nullptr);
}
void StageOutline::refreshInspectionCameras() {
    auto gui=getGui();auto source=_source.lock();if(!gui || !source || !_value)return;
    for(auto tab:gui->getViewersList())if(Scene3DViewer::rendererForViewer(tab)==source)if(auto viewer=tab->findChild<Scene3DViewer*>()) {
        const auto path=viewer->property("lookThroughPrimPath").toString();if(path.isEmpty())continue;
        try{viewer->lookThroughPrim(path,_value->camera(path.toStdString()));}catch(const std::exception&){viewer->setLookThroughCamera(false);}
    }
}
void StageOutline::fetch(QTreeWidgetItem* parent,size_t offset) {
    if(!_value)return;const auto value=_value;const auto generation=_generation;
    const auto path=parent?parent->data(0,PathRole).toString():QStringLiteral("/");
    if(parent && offset==0){qDeleteAll(parent->takeChildren());auto loading=new QTreeWidgetItem(parent,{tr("Loading...")});loading->setFlags(Qt::NoItemFlags);}
    auto watcher=new QFutureWatcher<PageReply>(this);
    connect(watcher,&QFutureWatcher<PageReply>::finished,this,[this,watcher,parent,offset,generation]{
        auto reply=watcher->result();watcher->deleteLater();if(generation!=_generation)return;
        if(parent && offset==0)qDeleteAll(parent->takeChildren());
        if(!reply.error.isEmpty()){_details->setPlainText(reply.error);return;}
        {QSignalBlocker block(_tree);
            for(const auto& prim:reply.page.prims) {
                const auto path=QString::fromStdString(prim.path);QStringList state;
                if(!prim.active)state<<tr("inactive");if(!prim.loaded)state<<tr("unloaded");if(prim.instance)state<<tr("instance");if(prim.instanceProxy)state<<tr("proxy");if(prim.hasPayload)state<<tr("payload");if(prim.visibility=="invisible")state<<tr("hidden");
                auto item=new QTreeWidgetItem({prim.displayName.empty()?QString::fromStdString(prim.name):QString::fromStdString(prim.displayName),QString::fromStdString(prim.type),state.join(QStringLiteral(", "))});
                item->setData(0,PathRole,path);item->setToolTip(0,path);item->setToolTip(2,state.join(QStringLiteral(", ")));if(parent)parent->addChild(item);else _tree->addTopLevelItem(item);
                if(prim.hasChildren){auto placeholder=new QTreeWidgetItem(item,{tr("Expand to inspect")});placeholder->setData(0,FetchRole,QStringLiteral("unfetched"));placeholder->setFlags(Qt::NoItemFlags);}
            }
            if(reply.page.more){auto more=new QTreeWidgetItem({tr("More prims (double-click)")});more->setData(0,FetchRole,QStringLiteral("more"));more->setData(1,FetchRole,qulonglong(offset+reply.page.prims.size()));more->setFlags(Qt::ItemIsEnabled);if(parent)parent->addChild(more);else _tree->addTopLevelItem(more);}
        }
        restoreSelection();filterRows();
    });
    watcher->setFuture(QtConcurrent::run([value,path,offset]{PageReply reply;try{reply.page=value->children(path.toStdString(),offset);}catch(const std::exception& e){reply.error=QString::fromUtf8(e.what());}return reply;}));
}
void StageOutline::selectPrimPaths(const QStringList& paths) {
    _ownershipDirty=true;
    _restorePaths=paths;{QSignalBlocker block(_tree);_tree->clearSelection();}restoreSelection();if(paths.isEmpty())inspectSelection();
}
void StageOutline::restoreSelection() {
    const auto targets=_restorePaths;
    bool changed=false;
    for(QTreeWidgetItemIterator it(_tree);*it;++it) {
        auto item=*it;const auto path=item->data(0,PathRole).toString();if(path.isEmpty())continue;
        bool needExpansion=_expanded.contains(path);
        for(const auto& target:_restorePaths)if(target.startsWith(path+QStringLiteral("/")))needExpansion=true;
        if(needExpansion && !item->isExpanded())item->setExpanded(true);
        if(_restorePaths.contains(path) && !item->isSelected()){QSignalBlocker block(_tree);item->setSelected(true);_tree->scrollToItem(item);changed=true;}
    }
    if(changed){inspectSelection();_restorePaths=targets;}
}
void StageOutline::inspectSelection() {
    if(!_value)return;const auto paths=selectedPrimPaths();_restorePaths=paths;
    const auto generation=_generation,serial=++_selectionSerial;_selected.clear();Q_EMIT selectionReady();
    if(paths.isEmpty()){_details->clear();return;}_details->setPlainText(tr("Inspecting selection..."));
    const auto value=_value;auto watcher=new QFutureWatcher<SelectionReply>(this);
    connect(watcher,&QFutureWatcher<SelectionReply>::finished,this,[this,watcher,generation,serial]{
        auto reply=watcher->result();watcher->deleteLater();if(generation!=_generation || serial!=_selectionSerial)return;
        if(!reply.error.isEmpty()){_details->setPlainText(reply.error);return;}_selected=std::move(reply.prims);
        QStringList text;
        for(const auto& p:_selected) {
            text<<QString::fromStdString(p.path)<<tr("Type: %1").arg(QString::fromStdString(p.type))<<tr("Owner identity: %1").arg(QString::fromStdString(p.ownerId));
            if(!p.material.empty())text<<tr("Material: %1").arg(QString::fromStdString(p.material));
            text<<tr("Visibility: %1").arg(QString::fromStdString(p.visibility));
            for(const auto& api:p.apis)text<<tr("API: %1").arg(QString::fromStdString(api));
            for(const auto& layer:p.layers)text<<tr("Layer: %1").arg(QString::fromStdString(layer));
            for(const auto& property:p.properties)text<<QString::fromStdString(property.first)+QStringLiteral(": ")+QString::fromStdString(property.second);
            text<<QString();
        }_details->setPlainText(text.join(QLatin1Char('\n')));Q_EMIT selectionReady();
    });
    watcher->setFuture(QtConcurrent::run([value,paths]{SelectionReply reply;try{for(const auto& path:paths)reply.prims.push_back(value->inspect(path.toStdString()));}catch(const std::exception& e){reply.error=QString::fromUtf8(e.what());}return reply;}));
}
void StageOutline::filterRows() {
    const auto query=_filter->text();
    std::function<bool(QTreeWidgetItem*)> visible=[&](QTreeWidgetItem* item){
        bool match=query.isEmpty() || item->data(0,PathRole).toString().contains(query,Qt::CaseInsensitive) || item->text(0).contains(query,Qt::CaseInsensitive) || item->text(1).contains(query,Qt::CaseInsensitive);
        for(int i=0;i<item->childCount();++i)match=visible(item->child(i)) || match;item->setHidden(!match);return match;
    };for(int i=0;i<_tree->topLevelItemCount();++i)visible(_tree->topLevelItem(i));
}
NATRON_NAMESPACE_EXIT
#include "moc_StageOutline.cpp"
