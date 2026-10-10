# Building Natron

Natron uses CMake, C++17, Qt 6.6 or newer, PySide6 and Shiboken6. Qt 5,
PySide2 and qmake builds have been retired. Match the Qt SDK and PySide6 /
Shiboken6 versions exactly. Python build tools and dependencies are managed
with uv; `pyproject.toml` and `uv.lock` define the environment.

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
