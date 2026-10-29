// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GUI_STAGEOUTLINE_H
#define NATRON_GUI_STAGEOUTLINE_H
#include <Python.h>
#include <QWidget>
#include <QStringList>
#include <QSet>
#include <QFutureWatcher>
#include "Global/QtCompat.h"
#include "Global/Macros.h"
#include "Gui/PanelWidget.h"
#include "Engine/NativeScene3D.h"
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QPushButton;
class QTimer;
class QToolButton;
NATRON_NAMESPACE_ENTER
class StageOutline : public QWidget, public PanelWidget {
    Q_OBJECT
    Q_PROPERTY(QStringList selectedPrimPaths READ selectedPrimPaths)
    Q_PROPERTY(QString stageStatus READ stageStatus)
    Q_PROPERTY(QString pinnedSource READ pinnedSource WRITE setPinnedSource)
    Q_PROPERTY(QString sourceNode READ sourceNode)
    Q_PROPERTY(bool composing READ composing)
public:
    explicit StageOutline(Gui* gui);
    void setScene(std::shared_ptr<const Gpu::Cards::UsdSceneValue> value,const QString& branch,bool resetTree=true);
    QStringList selectedPrimPaths()const;
    QString stageStatus()const;
    std::vector<Gpu::Cards::UsdPrimInfo> selectedPrims()const{return _selected;}
    QString pinnedSource()const{return _pinnedId;}
    QString sourceNode()const;
    bool composing()const{return _composing;}
    void registerViewer(Scene3DViewer* viewer);
    void resetSource();
public Q_SLOTS:
    void selectPrimPaths(const QStringList& paths);
    void setPinnedSource(const QString& identity);
    void refreshSource();
    void lookThroughSelectedCamera();
    void useSelectedRenderCamera();
    void addTransformEdit();
    void addVisibilityEdit();
    void addMaterialBinding();
    void addCompositingEdit();
    void addShadowCatcherEdit();
    void addReflectionCatcherEdit();
    void addOccluderEdit();
Q_SIGNALS:
    void selectionReady();
    void frameSelectionRequested();
private:
    void syncSelection();
    void refreshInspectionCameras();
    void frameSelection();
    void insertEdit(const std::string& plugin,int compositingRole=0);
    void notifyGuiClosing() override;
    void enterEvent(QtCompat::QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void fetch(QTreeWidgetItem* parent,size_t offset=0);
    void inspectSelection();
    void restoreSelection();
    void filterRows();
    QTreeWidget* _tree;
    QLabel* _status;
    QPlainTextEdit* _details;
    QLineEdit* _filter;
    QComboBox* _sourceChoice;
    QPushButton* _frameButton;
    QPushButton* _lookButton;
    QPushButton* _renderCameraButton;
    QToolButton* _editButton;
    QTimer* _timer;
    QString _pinnedId, _sceneKey, _requestedKey;
    NodeWPtr _source;
    Scene3DSnapshot _scene;
    std::shared_ptr<Gpu::Cards::UsdSceneBridge> _bridge;
    struct ComposeReply {std::shared_ptr<const Gpu::Cards::UsdSceneValue> value;QString error;};
    QFutureWatcher<ComposeReply>* _composer;
    bool _composing=false;
    bool _framingSelection=false;
    bool _ownershipDirty=true;
    std::shared_ptr<const Gpu::Cards::UsdSceneValue> _value;
    uint64_t _generation=0,_selectionSerial=0;
    QSet<QString> _expanded;
    QStringList _restorePaths;
    std::vector<Gpu::Cards::UsdPrimInfo> _selected;
};
NATRON_NAMESPACE_EXIT
#endif
