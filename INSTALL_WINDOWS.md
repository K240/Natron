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

Vulkan SDK 1.4.363.0 is also required for the migration foundation. The build
script defaults to `C:\VulkanSDK\1.4.363.0`; override `-VulkanSdkRoot` if needed.
It selects this SDK explicitly instead of relying on inherited environment
variables. The Viewer, Engine image processing and GPU-enabled shipped OFX
nodes use Vulkan. Editing overlays use OFX DrawSuite; other Qt widgets and the
CPU fallback Viewer use raster painting. The application's OpenGL renderer and
build dependencies have been retired.

Pass `-ConfigureOnly` to configure without compiling, `-SkipDependencies`
to reuse installed native dependencies, or `-Jobs N` to change concurrency.
The runner configures DLL, Python, Qt plugin, OCIO and OpenFX paths. For tests,
it builds the pinned openfx-io sources if the IO test bundle is absent.

GUI smoke and CPU/renderer tests use a fresh profile under `build/test-profiles`, with isolated
INI settings, caches, temporary files and autosaves. Update and OCIO startup
checks are disabled in that test profile. An external watchdog records the
startup/run/shutdown phase and stops only the test process on failure. Use
`-GuiTimeoutSeconds N` to change the default 60-second limit. These tests exit
through QApplication after checking their output; normal save/discard/cancel
confirmation behavior needs a separate test.

Build and run the independent Vulkan tests with:

```powershell
.\tools\build-windows-vulkan.ps1
.\tools\test-windows-vulkan.ps1
.\tools\test-windows-vulkan-natron.ps1
```

The default test selection checks GPU indices 0 and 1. Pass `-GpuIndices 0`
on a single-GPU machine. Results, validation diagnostics and Vulkan frame
readbacks are written under `build/vulkan-tests`. To check scaling, set
`QT_SCALE_FACTOR=1.5` for the test process and use a separate report directory.
The main CMake build can also build these tests with
`-DNATRON_BUILD_GPU_TESTS=ON`; they require a Vulkan GPU and an interactive
display. Standard CPU tests remain available independently.

The integrated Natron Viewer uses the same Vulkan core. Its test matrix covers
GPU 0/1, 8u/32f, GUI/OFX graphs, native Roto/transform/Corner Pin/Tracker editing,
and timeline/histogram/native custom parameter/curve/Dope Sheet drawing and input (36 cases),
including pan/zoom events and shared device ownership. Results are in
`build/vulkan-natron-tests`. Tracker previews also check explicit sRGB input,
image orientation, and magnifier drag/resize. `-Program DrawSuiteSmoke` loads
an independent OFX plugin and checks V2 selection, alpha compositing, physical
hairline width, pen editing and draw-context lifetime. The normal CMake build
places this test bundle in `build/ofx-tests`; only its test runner adds that
directory to the plugin path. `-Program TimelineSmoke` checks the raster timeline:
cache/keyframe markers, scrubbing, playback bounds, zoom/pan, range selection
and timecode text. The timeline uses QWidget/QPainter without an OpenGL context.
`-Program HistogramSmoke` checks raster histogram bars in RGB/A/Y/R/G/B modes,
additive RGB overlap, async mode changes, smoothing, pan/zoom/fit, Viewer pickers
and current/named Viewer and input selection. CPU binning is retained; the
unused OpenGL binning implementation is retired.
`-Program ParamDrawSuiteSmoke` checks the Natron custom-parameter DrawSuite
extension: native entry-point selection, alpha and hairline pixels, fractional
pen editing, UTF-8 keys and repeat, resize and deferred modal focus. Standard
GL-only custom controls are retired; ordinary parameter controls remain available.
`-Program CurveSmoke` checks native parametric background selection, gradient
and additive pixels, expired contexts, key/tangent edits, undo/redo and zoom/pan.
It also checks ColorLookup's ramp/histogram, both HueCorrect major versions,
the sheared Hue Vs Hue guide, HueKeyer, and ordinary animation curves with
timeline scrubbing. Curve widgets use QWidget/QPainter without a GL context;
parametric backgrounds use the separate Natron DrawSuite extension.
`-Program DopeSheetSmoke` checks raster key icons and editing, Reader trim/slip,
time and Group ranges, hierarchy visibility, resize and modal drag interruption.
The shipped OFX editing overlays also use DrawSuite. The GUI matrix compares
a read-only fingerprint of user preferences before and after. Current recovery,
memory-pressure and 4K/8K processing/presentation evidence is described in
[the Vulkan migration guide](Documentation/source/maintainers/vulkan-migration.rst).
Windows is the completion target for this task; Linux/macOS validation is separate.
The guide records the remaining Windows acceptance conditions.

Executables are in `build/windows-qt6/App`, `Renderer`, `PythonBin` and `Tests`.
This builds Release executables. A portable ZIP can be assembled separately
with the commands below; this script does not generate an installer.
OpenFX plugin binaries are separate dependencies, not rebuilt by this script.

## Portable Release candidate

Build Natron and all shipped OFX bundles first, then run:

```powershell
uv run --frozen --with pefile==2024.8.26 python tools/package-windows-portable.py --destination build/portable --zip build/Natron-windows-qt6-vulkan.zip
.\tools\test-windows-portable.ps1 -Archive build/Natron-windows-qt6-vulkan.zip -ReportDirectory build/portable-tests
```

Use new destination, ZIP and report paths for each candidate. Extract the ZIP
and double-click `Natron.cmd` to launch the GUI. `NatronRenderer.cmd` and
`NatronPython.cmd` start the other executables and accept command-line arguments.
The launchers retain the caller's working directory. The package includes Qt,
Python, the five rebuilt OFX bundles, codec DLLs, configuration data and license
notices. Qt, Python, uv and the Vulkan SDK are not needed on the target machine.
The target is Windows 10 1903 or newer / Windows 11 x64, with a Vulkan 1.3 driver.

The package tests use a relocated directory containing spaces and Japanese
characters, an isolated profile and a restricted DLL search environment. They
check bundled Python, CPU OFX rendering, Shadertoy Vulkan rendering and actual
Viewer presentation. Pass `-SoftwareDriverDirectory` with a directory provisioned
by `tools/install-windows-software-vulkan.ps1` to run the same tests on SwiftShader.
Tests preserve the user's Natron settings. Same-input ZIP assembly is deterministic;
the rolling MSYS2 dependencies still need a separately pinned environment for
reproducible clean builds. The local candidate is unsigned, and local path-isolation
tests do not establish acceptance on a clean machine or a remote CI runner.

## Building the remaining OpenFX plugins

```powershell
.\tools\build-windows-ofx.ps1
.\tools\run-windows-qt6.ps1 -Program OfxSmoke
.\tools\run-windows-qt6.ps1 -Program OfxGuiSmoke
.\tools\run-windows-qt6.ps1
```

This builds Misc, CImg, Arena (including AudioCurve), G'MIC and IO from the pinned
upstream sources in `build-configs/windows-ofx/sources.json`. The OFX C interface
allows these UCRT64 plugins to run in the MSVC / Qt 6 Natron host. The locally
validated environment uses GCC 16.2, CMake 4.4.4 and Ninja 1.13.2. Builds use
C++17, or C++20 for Arena and IO, and CMake throughout.

The script provisions a local MSYS2 environment under `build/msys64`, installs
current UCRT64 dependencies, initializes submodules, downloads the additional
CImg / G'MIC sources, applies the checked-in compatibility patches and builds
the bundles into `build/Plugins`. Runtime DLL dependencies, ImageMagick modules
and data files are copied into `build/ofx-runtime`; the Natron runner sets their
search paths. MSYS2's rolling package versions are recorded in
`build/ofx-package-versions.txt`. Pass `-SkipDependencies` for an incremental
build, `-PluginSets io` (or `misc,arena,gmic`) to select sets, or `-Jobs N` for concurrency.

IO is rebuilt with OpenColorIO 2.6.0, OpenImageIO 3.1.18.1, FFmpeg 9.0.2 and
SeExpr 2.11. Its 22 plugin identifiers and versions are preserved. The six OCIO
nodes use Natron's Vulkan shader suite when GPU processing is enabled; their
CPU option remains available. The obsolete OCIO GL shader backend is excluded
from this build. DLLs left beside IO.ofx by old downloaded bundles are backed up
under `build/ofx-legacy-runtime-backup/IO` and removed from the loader search
path; the rebuilt bundle uses `build/ofx-runtime`.
Run `tools/test-windows-vulkan-ocio.ps1` to check both GPUs against OCIO's CPU
Float reference, actual OCIO node renders and project save/reload.

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
