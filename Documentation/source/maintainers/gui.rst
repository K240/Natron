.. _maint-gui:

The Gui Module
==============

``Gui`` is the Qt desktop application built on top of ``Engine``. Every class
here may use ``Engine``; several implement the ``…I`` interfaces the Engine
calls back into (see :ref:`maint-design`). Like some Engine classes, several
large Gui classes are split across numbered ``.cpp`` files
(``Gui20.cpp``, ``ViewerTab30.cpp`` …) that all implement one class.

Main window and application
---------------------------

- ``Gui`` (``Gui.cpp`` and ``Gui05``–``Gui50.cpp``, ``GuiPrivate.*``): the main
  window. Owns the docked panes, menus, the node graph, viewers and editors.
- ``GuiApplicationManager`` (``GuiApplicationManager*.cpp``): the
  ``AppManager`` subclass for GUI mode — shortcuts (``ActionShortcuts``),
  icons/resources, the application event loop, known plug-in menus.
- ``GuiAppInstance``: the ``AppInstance`` subclass that owns a ``Gui``.
- Docking/layout: ``TabWidget``, ``Splitter``, ``FloatingWidget``,
  ``SerializableWindow``, ``ProjectGuiSerialization`` (layouts are saved with
  the project).

The node graph
--------------

- ``NodeGraph`` (``NodeGraph.cpp`` and ``NodeGraph05``–``NodeGraph45.cpp``,
  ``NodeGraphPrivate.*``): the ``QGraphicsView`` canvas; implements
  ``NodeGraphI``.
- ``NodeGui`` (``NodeGui.cpp``): the on-canvas node item; implements
  ``NodeGuiI``. ``BackdropGui``, ``DotGui``, ``Edge`` (connections),
  ``NodeGraphTextItem``/``NodeGraphRectItem`` are the graphics items.
- ``NodeGraphUndoRedo`` and ``NodeCreationDialog`` handle edits and node
  creation. Node clipboard: ``NodeClipBoard``.

Node layout and serialization use the unscaled body rectangle, excluding the
outline. Reusing ``boundingRect()`` as a new body size adds the outline on each
Reader filename refresh and compounds temporary node magnification. The GUI
regression in ``tools/node-size-gui-smoke.py`` changes sequence time with the
Reader panel open, toggles previews, changes labels, and saves/reloads the
project. To reset a node enlarged by an older build, select it and toggle its
preview off and on (default shortcut: Alt+P).

``DotGui`` creates an ellipse instead of the base node's rectangular body.
The inherited size API uses its virtual geometry, and content resize refreshes
skip nodes that cannot be resized. ``tools/dot-insertion-gui-smoke.py`` presses
Ctrl in the actual node graph, clicks the displayed edge bend point, checks
the Reader/Dot/Viewer connections and 16 undo/redo cycles, then saves/reloads
a project containing two Dots.

The viewer
----------

- ``ViewerGL`` (``ViewerGL.cpp``, ``ViewerGLPrivate.*``, ``ViewerVulkan.cpp``):
  the ``QWidget`` viewport containing a native Vulkan surface. It implements
  the existing ``OpenGLViewerI`` interface and displays frames through the
  shared Vulkan device. Overlays use ``OverlayDrawList`` and interactions
  reach the widget through the native surface's event bridge.
- ``ViewerTab`` (``ViewerTab.cpp`` and ``ViewerTab10``–``40.cpp``,
  ``ViewerTabPrivate.*``): the panel around the viewport — playback controls,
  channel/view selectors, the timeline.
- ``InfoViewerWidget``, ``Histogram`` and ``TimeLineGui`` use Qt raster widgets.
  Histogram binning remains on the CPU.

Animation editors
-----------------

- ``CurveEditor`` / ``CurveWidget`` (``CurveWidget.cpp``,
  ``CurveWidgetPrivate.*``, ``CurveGui``): the keyframe curve editor
  (``QWidget`` / ``QPainter`` with the shared draw command list).
- ``DopeSheet`` / ``DopeSheetView`` / ``DopeSheetEditor`` /
  ``DopeSheetHierarchyView`` (with ``DopeSheetEditorUndoRedo``): the raster
  dope sheet, including key icons, clip ranges and editing handles.
- Undo/redo for curves: ``CurveEditorUndoRedo``.

Custom OFX parameter controls and parametric backgrounds use the Natron draw
suite extensions. Standard Viewer overlays use OFX DrawSuite / Overlay
Interact V2. The GUI does not link Qt OpenGL or OpenGLWidgets; remaining Engine
and shipped plugin GL paths are tracked in :ref:`maint-vulkan-migration`.

Parameter panels (Knob GUIs)
----------------------------

- ``DockablePanel`` (``DockablePanel.cpp``, ``DockablePanelPrivate.*``):
  a node's settings panel; implements ``DockablePanelI``.
- ``KnobGui`` (``KnobGui.cpp``, ``KnobGui10/20.cpp``, ``KnobGuiPrivate.*``):
  base class implementing ``KnobGuiI``; ``KnobGuiFactory`` builds the right
  widget for each knob type. Concrete widgets: ``KnobGuiValue`` (int/double),
  ``KnobGuiBool``, ``KnobGuiChoice``, ``KnobGuiColor``, ``KnobGuiString``,
  ``KnobGuiFile``, ``KnobGuiButton``, ``KnobGuiGroup``, ``KnobGuiParametric``,
  ``KnobGuiSeparator``, ``KnobGuiTable``.
- ``KnobUndoCommand`` (undo for parameter edits), ``KnobWidgetDnD`` (drag-and-
  drop linking), ``ManageUserParamsDialog``/``AddKnobDialog`` (user parameters),
  ``EditExpressionDialog``/``EditScriptDialog`` (Python expressions/scripts).

Tool panels
-----------

- ``RotoPanel`` (``RotoPanel.cpp``): the roto/paint item tree and tools.
- ``TrackerPanel`` (``TrackerPanel.cpp``) and ``MultiInstancePanel``: the
  tracker's marker table.
- ``PreferencesPanel``: the settings UI, generated from the ``Settings``
  knobs.
- ``HostOverlay`` (``HostOverlay.cpp``): draws the host-provided overlays
  (position/transform/corner-pin handles) for plug-ins that request them.

Custom widgets and helpers
--------------------------

Natron ships many hand-built widgets to get the look and behavior it wants:
``ComboBox``, ``SpinBox`` (+ ``SpinBoxValidator``), ``Button``, ``Label``,
``LineEdit``, ``ScaleSliderQWidget``, ``ColorSelectorWidget`` /
``QtColorTriangle``, ``Menu``, ``ClickableLabel``, ``AnimatedCheckBox``,
``TableModelView`` (a generic model/view table), ``ScriptTextEdit`` /
``ScriptEditor`` (the Python console), ``ProgressPanel`` /
``ProgressTaskInfo``, ``MessageBox`` / ``LogWindow`` / ``AboutWindow`` /
``SplashScreen``, and the file dialog ``SequenceFileDialog`` (sequence-aware,
backed by ``Engine``'s ``FileSystemModel``). ``QtEnumConvert`` translates
between Qt enums and Natron's toolkit-independent enums (``Global/Enums.h``,
``KeySymbols.h``) so Engine code never sees Qt key/modifier types.

GUI Python integration
----------------------

``PyGuiApp`` and ``PythonPanels`` expose the GUI to Python (custom panels and
menus), bound through Shiboken with ``typesystem_natronGui.xml``. See
:ref:`maint-python`.
