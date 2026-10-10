# Building Natron on Windows

Use a 64-bit MSVC toolchain, CMake, Ninja, uv, vcpkg and a matching Qt 6 SDK.
The checked environment uses Qt / PySide6 / Shiboken6 6.11.2, CPython 3.14
and Visual Studio 2026 (MSVC 19.51). Qt 5 and qmake builds are retired.

```powershell
.\tools\build-windows-qt6.ps1
.\tools\run-windows-qt6.ps1 -Program NatronRenderer -ProgramArguments @('--version')
.\tools\run-windows-qt6.ps1 -Program CTest
.\tools\run-windows-qt6.ps1 -Program Smoke
.\tools\run-windows-qt6.ps1 -Program GuiSmoke
.\tools\run-windows-qt6.ps1
```

The build script defaults to `D:\Qt\6.11.2\msvc2022_64` and
`D:\work\apps\vcpkg`. Override `-QtRoot` and `-VcpkgRoot` for other
locations. Visual Studio is located with vswhere. `uv sync --frozen` prepares
Python, and the pinned vcpkg manifest in `build-configs/windows-qt6` builds
the native dependencies. OpenFX submodules, Clang 20 resource headers and
OpenColorIO configs are provisioned automatically.

Pass `-ConfigureOnly` to configure without compiling, `-SkipDependencies`
to reuse installed native dependencies, or `-Jobs N` to change concurrency.
The runner configures DLL, Python, Qt plugin, OCIO and OpenFX paths. For tests,
it fetches the upstream openfx-io test bundle if it is absent.

Executables are in `build/windows-qt6/App`, `Renderer`, `PythonBin` and `Tests`.
This is a development build; it does not generate a redistributable installer.
OpenFX plugin binaries are separate dependencies, not rebuilt by this script.

## Building the remaining OpenFX plugins

```powershell
.\tools\build-windows-ofx.ps1
.\tools\run-windows-qt6.ps1 -Program OfxSmoke
.\tools\run-windows-qt6.ps1 -Program OfxGuiSmoke
.\tools\run-windows-qt6.ps1
```

This builds Misc, CImg, Arena (including AudioCurve) and G'MIC from the pinned
upstream sources in `build-configs/windows-ofx/sources.json`. The OFX C interface
allows these UCRT64 plugins to run in the MSVC / Qt 6 Natron host. The locally
validated environment uses GCC 16.2, CMake 4.4.4 and Ninja 1.13.2. Builds use
C++17, or C++20 for Arena's current Poppler headers, and CMake throughout.

The script provisions a local MSYS2 environment under `build/msys64`, installs
current UCRT64 dependencies, initializes submodules, downloads the additional
CImg / G'MIC sources, applies the checked-in compatibility patches and builds
the bundles into `build/Plugins`. Runtime DLL dependencies, ImageMagick modules
and data files are copied into `build/ofx-runtime`; the Natron runner sets their
search paths. MSYS2's rolling package versions are recorded in
`build/ofx-package-versions.txt`. Pass `-SkipDependencies` for an incremental
build, `-PluginSets misc,arena,gmic` to select sets, or `-Jobs N` for concurrency.

The existing IO bundle continues to provide image and video readers / writers.
Arena includes OpenColorIO 2 color management. Its legacy ReadMisc formats and
legacy Poppler path are disabled. Optional
Arena OpenCL effects and the experimental RichText / MIDI plugins are outside
the standard bundle. G'MIC's OFX wrapper is built from its current upstream
revision but retains the upstream-compatible G'MIC 2.8.4 engine. Misc's CImg
source pin is 2.9.9; this does not upgrade those engines to their newest major release.
The upstream wrapper documents its engine compatibility in its
[source Makefile](https://github.com/NatronGitHub/openfx-gmic/blob/292ac55c7c84f6eff56c1764a5a5037b96895d12/Makefile).

Validation checks recognition of all rebuilt sets, actual Invert / CImg Blur /
ImageMagick Modulate / G'MIC Gaussian renders, SVG and PDF readers, Text
rendering, project save / reload and a GUI Viewer graph using all four bundles.
