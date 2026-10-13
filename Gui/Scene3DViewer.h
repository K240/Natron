// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef NATRON_GUI_SCENE3DVIEWER_H
#define NATRON_GUI_SCENE3DVIEWER_H
#include <Python.h>
#include <QWidget>
#include <QImage>
#include <QPolygonF>
#include <QVariant>
#include <QStringList>
#include "Engine/NativeScene3D.h"
#include "Gui/GuiFwd.h"

class QUndoStack;

NATRON_NAMESPACE_ENTER
class ViewerTab;
class Scene3DViewer : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString selectedNode READ selectedNode)
    Q_PROPERTY(QStringList selectedNodes READ selectedNodes)
    Q_PROPERTY(QString renderError READ renderError)
    Q_PROPERTY(QImage capturedFrame READ capturedFrame)
    Q_PROPERTY(QString rendererBackend READ rendererBackend)
    Q_PROPERTY(bool lookThroughCamera READ lookThroughCamera WRITE setLookThroughCamera)
    Q_PROPERTY(int gizmoMode READ gizmoMode WRITE setGizmoMode)
    Q_PROPERTY(QPointF gizmoCenter READ gizmoCenter)
    Q_PROPERTY(QPolygonF gizmoHandles READ gizmoHandles)
    Q_PROPERTY(QVariantList gizmoRings READ gizmoRings)
    Q_PROPERTY(QVariantList sceneObjects READ sceneObjects)
    Q_PROPERTY(QVariantList inspectionPivot READ inspectionPivot)
    Q_PROPERTY(double inspectionDistance READ inspectionDistance)
public:
    explicit Scene3DViewer(ViewerTab* tab);
    ~Scene3DViewer() override;
    QString selectedNode() const;
    QStringList selectedNodes() const;
    QString renderError() const {return _error;}
    QImage capturedFrame() const {return _frame;}
    QString rendererBackend() const {return _backend;}
    bool lookThroughCamera() const {return _lookThrough;}
    int gizmoMode() const {return _gizmoMode;}
    QPointF gizmoCenter() const {return _handleCenter;}
    QPolygonF gizmoHandles() const;
    QVariantList gizmoRings() const;
    QVariantList sceneObjects() const;
    QVariantList inspectionPivot() const {return {_pivot.x,_pivot.y,_pivot.z};}
    double inspectionDistance() const {return _distance;}
public Q_SLOTS:
    void setGizmoMode(int mode);
    void setLookThroughCamera(bool value);
    void resetView();
    void frameSelection();
    void refreshScene();
Q_SIGNALS:
    void lookThroughCameraChanged(bool value);
    void gizmoModeChanged(int mode);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void hideEvent(QHideEvent*) override;
    void focusOutEvent(QFocusEvent*) override;
private:
    ViewerTab* _tab;
    NodeWPtr _source, _selected;
    std::vector<NodeWPtr> _selection, _graphSelection;
    Scene3DSnapshot _scene;
    std::vector<Gpu::Cards::Card> _cards;
    Gpu::Cards::Camera _camera;
    Gpu::Cards::Camera _dragCamera;
    Gpu::Cards::Settings _settings;
    std::unique_ptr<Gpu::Cards::Renderer> _renderer;
    std::shared_ptr<Gpu::VulkanDevice> _device;
    std::unique_ptr<Gpu::Cards::UsdSceneBridge> _usdBridge;
    QImage _frame;
    QString _error, _backend;
    QRectF _imageRect;
    QRectF _dragImageRect;
    double _yaw = 25, _pitch = -15, _distance = 7;
    Gpu::Cards::Vec3 _pivot;
    bool _lookThrough = false, _dirty = true, _snapshotValid = false;
    U64 _hash = 0;
    double _time = -1;
    int _view = -1, _lut = -1;
    double _gain = -1, _gamma = -1;
    QPointF _lastMouse;
    QPointF _dragStart, _dragTangent;
    int _dragAxis = -1;
    int _gizmoMode = 0, _selectedObject = -1;
    bool _handlesVisible = false, _geometricRotation = false;
    Gpu::Cards::Vec3 _dragWorldCenter, _dragWorldAxis;
    Gpu::Cards::Matrix _gizmoViewInverse;
    QPointF _dragRotationCenter, _dragRotationU, _dragRotationV;
    double _dragLineStart = 0, _dragAngle = 0, _dragRotation = 0, _dragTime = 0;
    std::array<double,3> _dragValues{};
    KnobDoubleWPtr _dragKnob;
    struct DragTarget {
        KnobDoubleWPtr knob;
        KnobGuiWPtr gui;
        std::array<double,3> values{};
        Gpu::Cards::Matrix parentInverse;
    };
    std::vector<DragTarget> _dragTargets;
    std::shared_ptr<QUndoStack> _dragUndoStack;
    std::array<QPointF,3> _handleEnd;
    QPointF _handleCenter;
    std::array<QPolygonF,3> _rotationRings;
    std::array<Gpu::Cards::Matrix,3> _rotationBases;
    struct ObjectDisplay {
        QPointF icon;
        bool visible = false;
        QPolygonF nearPlane, farPlane;
    };
    std::vector<ObjectDisplay> _objectDisplay;
    bool _marquee = false, _marqueeAdd = false;
    int _marqueeStartHit = -1;
    QPointF _marqueeEnd;
    QPointF project(Gpu::Cards::Vec3 world, bool* visible = nullptr) const;
    QPointF projectGizmoVector(Gpu::Cards::Vec3 world, double pixels) const;
    void selectAt(QPointF position);
    int hitObject(QPointF position) const;
    const Scene3DObject* selectedObject() const;
    std::vector<const Scene3DObject*> selectedObjects() const;
    bool isSelected(const NodePtr& node) const;
    Gpu::Cards::Vec3 selectionCenter() const;
    std::vector<int> graphSelectionIndices() const;
    void selectObjects(const std::vector<int>& indices, bool updateGraph);
    void selectObject(int index, bool updateGraph);
    void finishMarquee();
    bool syncGraphSelection();
    void updateObjects();
    void updateHandles();
    bool rotationAngle(QPointF position, double* angle) const;
    bool lineParameter(QPointF position, double* parameter) const;
    void endDrag();
};
NATRON_NAMESPACE_EXIT
#endif
