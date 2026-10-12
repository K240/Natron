# Building Natron

Natron uses CMake, C++17, Qt 6.6 or newer, PySide6 and Shiboken6. Qt 5,
PySide2 and qmake builds have been retired. Match the Qt SDK and PySide6 /
Shiboken6 versions exactly. Python build tools and dependencies are managed
with uv; `pyproject.toml` and `uv.lock` define the environment.

The Vulkan migration foundation requires Vulkan SDK 1.4.363 or newer,
including Volk, Vulkan Memory Allocator, glslangValidator and spirv-val. Set
`VULKAN_SDK` or pass `-DNATRON_VULKAN_SDK_ROOT=/path/to/sdk`. The runtime API
minimum is Vulkan 1.3; the loader is loaded dynamically. This foundation is
used by the native Natron Viewer, Roto/native host overlays, and Tracker
markers, magnifier and keyframe previews. The host also supports OFX 1.5
DrawSuite V1 and Overlay Interact V2 through this native Viewer. The
timeline now uses QWidget/QPainter without an OpenGL context; `TimelineSmoke`
checks its marker pixels, frame/range editing, zoom/pan and timecode text.
The histogram also uses QWidget/QPainter, retaining CPU binning and additive
RGB bars; `HistogramSmoke` checks channel modes, picker and input changes.
Full OFX custom parameter controls now use QWidget/QPainter and the Natron
`ParamInteractDrawSuite.h` extension. The standard GL-only custom interact is
retired; plugins without the extension receive ordinary parameter controls.
`ParamDrawSuiteSmoke` checks drawing, editing, keys, resize and modal focus.
The curve editor also uses QWidget/QPainter, including parametric backgrounds
through the Natron extension. ColorLookup, HueCorrect (both major versions),
and HueKeyer backgrounds use native gradient and additive drawing commands.
`CurveSmoke` checks their pixels, key/tangent edits, undo/redo, zoom/pan and
ordinary animation curves with timeline scrubbing.
The Dope Sheet uses QWidget/QPainter and the same draw command list, with
keyframe icons in logical pixels. `DopeSheetSmoke` checks key selection,
move/scale, interpolation, copy/paste/delete, undo/redo, navigation, Reader
trim/slip, time and Group ranges, hierarchy visibility, resize and modal focus.
The GUI no longer requires Qt OpenGL or OpenGLWidgets. Its unused GL shader and
glyph renderer have been removed. Startup exposes native PySide6 `QtCore`,
`QtGui` and `QtWidgets` modules; it avoids QtPy's eager GL compatibility imports.
QtPy remains available for user scripts that explicitly request it. The Engine
and shipped OFX bundles use Vulkan image and draw suites, and their OpenGL
execution paths and build dependencies have been retired. Memory pressure and
device loss recovery, application performance, packaging and platform validation
are tracked in the [Vulkan migration plan](Documentation/source/maintainers/vulkan-migration.rst).

## Windows

See [INSTALL_WINDOWS.md](INSTALL_WINDOWS.md) for the supported MSVC build,
run and test commands.

## Linux and macOS

Install a C++17 compiler, CMake, Ninja, uv, matching Qt 6 development libraries,
Boost (headers and serialization), Expat, Cairo, Fontconfig and pkg-config.
Linux additionally needs Extra CMake Modules, X11 and optionally Wayland.

```sh
git submodule update --init --recursive
uv sync --frozen
cmake -S . -B build/qt6 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/Qt/6.11.2/kit;.venv/lib/python3.14/site-packages/PySide6;.venv/lib/python3.14/site-packages/shiboken6;.venv/lib/python3.14/site-packages/shiboken6_generator" \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build/qt6 --parallel
```

The Shiboken generator also needs matching Clang resource headers (Clang 20
for Shiboken 6.11.2); set `LLVM_INSTALL_DIR` to their installation root.
The Windows build script provisions these headers automatically.

Set `PYTHONHOME` to the uv interpreter's base prefix and `PYTHONPATH` to
the environment's site-packages when running an embedded Python build.
Set `OCIO` to an OpenColorIO config and `OFX_PLUGIN_PATH` to installed OpenFX
bundles. The test suite needs the openfx-io bundle (ReadOIIO, WriteOIIO and
SeNoise). OpenFX plugins are built and distributed separately from Natron.

Linux and macOS instructions describe the CMake configuration; this migration
has been validated on Windows. Distribution packaging needs a separate port.
