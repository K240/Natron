.. _maint-building:

Building Natron
===============

Natron uses CMake and Qt 6.6 or newer. The Qt 5 / PySide2 and qmake build
paths have been retired. The dependency graph is HostSupport -> Engine ->
Gui, with App and Renderer providing the graphical and headless entry points.

The repository's ``BUILDING.md`` contains the current CMake configuration.
``INSTALL_WINDOWS.md`` describes the tested MSVC workflow and the
``tools/build-windows-qt6.ps1`` and ``tools/run-windows-qt6.ps1`` scripts.

Python tools and dependencies use uv and the checked-in ``uv.lock``.
Qt, PySide6 and Shiboken6 must have matching versions. Bindings are generated
by Shiboken as part of the CMake build. The GUI uses Qt Widgets and Vulkan;
Qt OpenGL and OpenGLWidgets are not required. Engine and OFX GL paths are
still being migrated; this does not yet make the whole application GL-free.
``NATRON_BUILD_TESTS`` enables the native suite (default ON).

Runtime dependencies include an OpenColorIO config and OpenFX plugin bundles.
Run CTest through the Windows runner to set the Python, Qt and plugin paths.
The IO plugin test bundle is downloaded on demand. Plugin compilation and
installer packaging are separate workflows.

Standard scripting startup exposes PySide6 ``QtCore``, ``QtGui`` and
``QtWidgets`` directly. QtPy remains available for explicit user-script imports;
it is not loaded by the host because its compatibility imports load optional
Qt OpenGL dependencies.
