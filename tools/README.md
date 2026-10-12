# Contents of the `tools` Directory

This directory contains various tools to help building and maintaining Natron.

`node-size-gui-smoke.py` checks Reader node dimensions across 256 timeline
changes, 64 label changes, preview toggles and a project save/reload. It also
checks that toggling the preview repairs a previously enlarged node. Run it
with a fresh owned profile, for example:

```powershell
./tools/run-windows-owned-gui-test.ps1 -Script tools/node-size-gui-smoke.py -Profile build/test-profiles/NodeSize-new -Result build/node-size-new/result.json -TimeoutSeconds 100
```

The Reader settings panel must remain open during the timeline changes; without
it, the filename sublabel refresh that caused the original growth is skipped.

`dot-insertion-gui-smoke.py` uses real graph key/mouse events to insert a Dot
into a Reader/Viewer connection with Ctrl-click. It verifies the inserted
connections, 16 undo/redo cycles, direct Dot creation and project save/reload.
Run it with `run-windows-owned-gui-test.ps1` and a fresh profile/result directory
as above. A stage report and bend-point screenshot survive a native crash.

`test-windows-renderer-api-comparison.ps1 -ReportDirectory <fresh-directory>`
records 20 unique timed frames per CPU/GPU case at 4K and 8K, using the same
full-size CPU source, shipped OCIO Linear-to-sRGB node and dispersed CPU PNG
probe. It compares the preserved RTX OpenGL build with current RTX/AMD Vulkan
builds. `windows-gpu-adapters.py` maps DXGI adapter identities to the PDH counters;
`windows-gpu-memory.py` samples adapter-wide memory at a requested 20 ms interval.
These measurements include other processes and do not establish an instantaneous
or per-process peak. The legacy and current IO bundles use different OCIO versions.
Run `verify-renderer-api-comparison.py --matrix <report.json> --adapters <adapters.json>`
to check all 240 samples, source/probe/native callbacks, 737,280 reference pixels,
input hashes and memory records. Legacy accuracy failures remain in the report.
For historical results, `--input-snapshot` accepts hash-checked copies preserved
before the implementation changed; it does not waive mismatched inputs.
`verify-vulkan-optimization.py` rechecks the before/after matrices, including the
preserved baseline, and reports median/p95 changes for CPU and GPU cases on both
devices. It requires GPU median improvement at both resolutions and retains
measurement-time variation, CPU regressions and legacy accuracy differences.

Set `NATRON_VULKAN_PROFILE=1` only for diagnostic runs to log wall times for
transfers, submissions, fragment processing, Engine image operations, cache bitmap
scans and OFX actions. Nested durations overlap. Normal rendering does not read
the profiling clock; final performance measurements leave profiling disabled.

`install-windows-vulkan-sdk.ps1` and `install-windows-software-vulkan.ps1` provision
the pinned Windows SDK and SwiftShader driver described by `build-configs/vulkan`.
`test-windows-vulkan-software.ps1 -IcdPath <manifest> -ReportDirectory <fresh-directory>`
checks nine headless rendering-core configurations with Vulkan validation.
Prepend the provisioned driver's `bin` directory to PATH for its runtime DLLs.
The Windows CI workflow builds Natron and all shipped OFX bundles and exercises
software Vulkan and relocated packaging; authoring the workflow does not prove
a successful remote CI run.

`package-windows-portable.py --destination <fresh-directory> --zip <new.zip>`
assembles the Release application, Python, Qt, rebuilt OFX, codec data and licenses,
checks PE dependency closure and emits per-file hashes and provenance. Run it with
`uv run --frozen --with pefile==2024.8.26 python`. Repeating with the same inputs
produces the same ZIP bytes. `test-windows-portable.ps1 -Archive <zip> -ReportDirectory
<fresh-directory>` checks packaged Python, Renderer and visible Viewer under an
isolated profile and restricted runtime environment, in a relocated Unicode path.
Add `-SoftwareDriverDirectory <directory>` for SwiftShader. `verify-windows-portable.py`
joins hardware/software results, a repeated archive, software-core results and
current source/import audits, checking independent rendered pixels and actual
module origins. This proves local packaging acceptance, not a clean-machine test,
signing or remote CI completion.
Pass `--optimization-proof <report>` to include current renderer performance
evidence in the package verification.

`uv run --frozen python tools/audit-native-ofx-compatibility.py --cache <test-profile-cache.xml>`
compares every shipped OFX ID and version with the pre-migration CSV inventory.
It writes a registration/declaration table under `build/native-ofx-compatibility`.
Pointer properties such as overlay entry points are not persisted in that cache;
their availability and editing behavior require the live interact tests. A matching
registration inventory does not by itself prove every node's functional compatibility.

`audit-windows-ofx-interacts.ps1 -ReportDirectory <fresh-directory>` builds a
standalone CMake descriptor inspector against Natron's current HostSupport.
It reads live effect/context/parameter entry points in native and legacy-custom
capability profiles, without creating an effect/interact instance or graphics
context. `plugins.csv` classifies DrawSuite declarations, unsupported legacy UI
and nodes that need no DrawSuite. It also records CPU/native render declarations;
`unsupported-render.csv` lists OpenGL-required contexts without native support.
Vulkan suite availability matches the application; diagnostic runtime functions
reject calls and record them, without initializing a GPU. An empty unsupported list proves declarations,
not rendering correctness. The inspector also accepts a custom `-PluginDirectory`.

`test-windows-vulkan-ofx-interacts.ps1` exercises all eight Generator overlays
and independent V1-only/no-UI CPU fixtures across RTX/AMD, 100%/200%, 8u/32f and
Vulkan/CPU raster display. It checks pen input, focus reset, Undo/Redo, visible
nonmodal compatibility explanations, lifecycle callback counts and CPU pixels.
`verify-ofx-interact-migration.py` joins before/after declarations, current input
hashes and the complete 32-process matrix. It does not claim full migration or
all shipped render functions. Python commands run through `uv`.
Consecutive Generator edits merge into one undo command; Undo restores the value
before both drags. If a verification-only correction follows a completed GUI run,
`--recorded-verifier <preserved-script>` validates that script against the recorded
hash while reporting the current verifier's hash separately. All other recorded
inputs must still match their current files. New runs do not need this option.

`test-windows-ofx-retired-render.ps1` checks six independent ABI variants through
actual CPU and Vulkan output, visible creation errors, and owned v1/v2 OFX caches.
It covers two GPUs, 100%/200%, 8u/32f, Vulkan/raster display, and disabled Vulkan.
`verify-ofx-retired-render.py` verifies the complete 22-process matrix and current
source/runtime inputs. OpenGL-required declarations are retained as metadata;
they cannot enable the retired backend. CPU-compatible effects remain available.
The owned test runner can seed cache XML only from its own test-profile tree.
`test-windows-ofx-retired-render-headless.ps1` and its verifier test NatronRenderer
on both GPUs and with Vulkan disabled. The owned renderer process receives
interpreter input followed by stdin EOF; completed pixels alone are insufficient
without its completion marker, successful exit and absence of a watchdog timeout.

`audit-native-engine-source.py` checks owned C++ sources for legacy GL calls,
the 25 retired shader/context/texture/GLAD files and the current Qt 6 Ninja
build for GL dependencies. `audit-native-ofx-runtime.py` separately audits normal
and delay imports in the application, OFX and codec binaries. Run Python tools
through `uv`; the PE audit needs `--with pefile==2024.8.26`.
The OFX host disables the OpenGL Render Suite. Viewer and Engine GPU work use
Vulkan; ordinary Qt drawing and CPU OFX image processing remain supported.

`test-windows-vulkan-memory.ps1` verifies bounded VMA allocations, real allocation
refusals, OFX CPU retries, GPU-only failure and runtime Viewer raster fallback on
both GPUs. It preserves logs, captures, input hashes and the user settings hash.
`verify-vulkan-memory.py --matrix <report.json> --output <proof.json>` independently
checks the 14-case matrix, every CPU retry pixel and the central 60% of all eight
recovered Viewer captures against the current binaries and test definitions.
`NATRON_VULKAN_MEMORY_LIMIT_MB` limits VMA-managed device-local heap blocks;
swapchain and driver-private allocations are outside that limit. Zero or an
unset value uses no explicit cap. Memory budget reporting is enabled when the
driver supports `VK_EXT_memory_budget`. The normal Vulkan test runner also
accepts `-Tests MemoryBudget`.

`test-windows-vulkan-pressure.ps1` keeps a proven native cache input resident,
fills the shared device heap with real image allocations under a 32 MiB limit,
and checks CPU retry against a reference PNG. A small native probe avoids
creating a full CPU snapshot before pressure. It also checks native rendering
after the pressure images are released, validation errors, graphics modules,
artifact hashes and unchanged user settings. RTX, AMD and SwiftShader passed
this separate matrix. Supply `-GpuIndices 0 -IcdPath <software-ICD.json>` for a
software driver and a fresh `-ReportDirectory` to preserve previous evidence.
Device loss is checked separately by `test-windows-vulkan-device-loss.ps1`.

`test-windows-vulkan-concurrent-memory.ps1` checks two visible Viewers sharing
one device under a 64 MiB cap. Real allocation refusals switch both to raster
display; after the memory released by that transition is filled again, two
independent OFX actions overlap and retry on CPU. It compares every RGBA value
in the central 60% of seven captures, then checks native presentation on a new
Viewer using the same device. The default matrix covers both GPUs, 8u/32f and
display scales 1/2; `-GpuIndices 0 -IcdPath <software-ICD.json>` selects a software
driver. Owned test windows are placed on the primary screen so a changed scale
does not leave their native surfaces unexposed.

`verify-vulkan-memory-pressure.py` rehashes the four cached/Viewer hardware/software
reports, live inputs, copied captures, source/import audits and fixture imports.
Pass `--cached-hardware`, `--cached-software`, `--viewer-hardware`, `--viewer-software`,
`--source-audit`, `--runtime-audit` and `--fixture-imports` with the corresponding
JSON paths. The current 15-case proof is `build/vulkan-memory-pressure-proof.json`.
It preserves the remaining migration gates and does not mark full migration complete.
After recovery tests pass on the same build, `--device-loss-proof <proof.json>`
links their independently checked source/runtime and result hashes. Pressure
regressions must be rerun after implementation changes; older hashes are retained
as historical checkpoints rather than replaced in their original reports.

`test-windows-vulkan-device-loss.ps1` tests native cache recovery, a failure inside
an OFX render action, persistent OCIO resources, GPU-required Shadertoy and two
sharing Viewers. Its default matrix covers both GPUs and Viewer 8u/32f at scales
1/2. Use `-GpuIndices 0 -IcdPath <software-ICD.json>` for software Vulkan and a
fresh `-ReportDirectory`. Loss-result injection is exposed only inside owned
test profiles and does not reset the desktop driver. Old device/image/program
handles must reject operations while allowing cleanup. Replacements must use a
larger device generation, render matching pixels and report no validation errors.

`verify-vulkan-device-loss.py` checks these hardware/software reports and the core
loss tests against current source/binary hashes, copied PNGs, profile markers,
shutdown logs, user settings and source/PE audits. Supply `--hardware`, `--software`,
`--core-hardware`, `--core-software`, `--source-audit`, `--runtime-audit` and
`--fixture-imports`. The combined proof is `build/vulkan-device-loss-proof.json`;
it retains the remaining migration gates. Core `-Tests DeviceLoss` additionally
tests a concurrent retirement during command recording before submission.

`test-windows-vulkan-performance.ps1` runs full-size 4K/8K procedural sources
through the shipped OCIO CDL effect in `NatronRenderer`, comparing CPU and native
graphs on RTX/AMD with validation enabled/disabled. Each graph has three warmups
and twenty samples. Engine image caches are purged outside each timed call; PNG
output samples the full image into 64x48 pixels. Every full-size native pixel is
also compared with a CPU formula in a separate, untimed readback.
Use a fresh `-ReportDirectory` to retain reports, PNGs and immutable input hashes.

`NatronOfxVulkanTestSuite` V2 is available only to owned test profiles. It reports
device-wide successful image-copy counts/texel bytes, callback-tracked lifetime
VMA device-local block peaks, and sampled driver memory usage/budget. Uniforms,
LUT buffers and GPU copies are outside the pixel counters; driver/swapchain memory
is outside the VMA peak. Compare snapshots after render work has completed.
Test profiles validate by default. `NATRON_TEST_VULKAN_VALIDATION=0` permits
production-cost measurements inside a profile; explicit validation requests win.

`verify-vulkan-performance.py --report <report.json> --source-audit <source.json>
--runtime-audit <runtime.json> --fixture-imports <fixture.json>` rechecks current
source/binary hashes, all saved probe pixels, transfer deltas, sample percentiles,
managed peaks, profile markers/shutdown logs and unchanged user settings. Its
output is `build/vulkan-application-performance-proof.json` and a CSV table.
The proof covers cold-cache headless processing; Viewer playback/scrubbing, WSI,
driver VRAM peaks and the old GL performance baseline remain separate gates.
Parsec testing is excluded from the current Windows completion criteria by user
request. Existing `parsecTested=false` fields record the measurement scope, rather
than a remaining acceptance gate.

`test-windows-vulkan-viewer-performance.ps1` measures real timeline seeks through
one or two visible, independent Natron Viewers. The default production matrix has
48 cases: RTX/AMD, 4K/8K source images, 8u/32f Viewer textures and native/CPU/Raster
processing. Each case has three warmups and twenty samples. Original source images
are generated at full resolution; the Viewer requests mipmap 2/3 for its 960x540
surface, including the OCIO effect at that display scale. Cache purges and PNG
readbacks are outside timing. The profile-only `framePresented` signal records
frame time, source format, texture bounds, viewport, transfer counts and successful
native present/queue wait or completed Raster paint; it does not measure scanout.
Run the validation matrix with `-ViewerCounts 2 -Modes native,cpu -ValidationModes 1`
and a fresh `-ReportDirectory`. Both matrices preserve settings and launch variables.

`verify-vulkan-viewer-performance.py --reports <production.json> <validation.json>
--source-audit <source.json> --runtime-audit <runtime.json> --fixture-imports <fixture.json>`
checks the 64 cases against current inputs, presentation metadata, counters, logs,
percentiles and user settings. It compares all saved pixels with an independent
procedural/downsample/display formula and compares native/CPU/Raster captures.
Output is `build/vulkan-viewer-performance-proof.json` and a CSV. The test captures
frames 2 and 24 after their timed intervals. Controlled cold-cache seeks do not
establish continuous playback, physical scanout, Parsec performance or a GL baseline.
GUI Python stdout is redirected to the Script Editor; benchmark phase diagnostics
therefore use the owned profile's `progress.json`, and the exit marker uses the
process descriptor directly. Shutdown runs after the presentation callback returns.

`test-windows-vulkan-playback.ps1` exercises the real playback scheduler with two
visible Viewers, RTX/AMD, 4K/8K sources, 8u/32f storage and Vulkan/Raster display.
The seven stages cover forward, backward, loop, bounce, cancellation, restart in
the other Viewer and backward playback there. The requested rate is 8 fps; this
is a functional regression, not a playback speed or physical scanout measurement.
After stopping, both displayed frames and the timeline must stay stable. Captures
from both Viewers are checked against the independent display reference.
Use a fresh report directory. `run-windows-owned-gui-test.ps1` isolates the test
through child-process environment variables, settings, a startup marker and a PID
watchdog. It does not mutate the launching shell's runtime paths or user settings.
`verify-vulkan-playback.py --report <report.json>` checks all 16 configurations,
112 stages, input hashes, validation/shutdown logs and all saved pixels. Its output
is `build/vulkan-windows-playback-proof.json`.

`verify-vulkan-application-integration.py --performance-proof <performance.json>
--device-loss-proof <loss.json> --memory-proof <memory.json>` joins those separately
checked results only when live source/binary hashes and source/runtime audit hashes
identify the same build and user settings match. It writes
`build/vulkan-application-integration-proof.json` and retains the remaining gates.
The user-selected completion platform is Windows (`--completion-platform windows`,
the default). Linux/macOS validation is outside this task. Use a fresh `--output`
path for a new checkpoint; earlier archived proofs retain their original scope.
`--completion-platform all` explicitly retains the earlier cross-platform plan.
Add `--viewer-performance-proof <viewer.json>` to include the independently
checked 64 real-Viewer seek cases. It requires the same source/runtime audits,
build hashes and user-settings fingerprint and adds 1280 timed samples.
Add `--playback-proof <playback.json>` for the separately checked Windows scheduler
matrix. This satisfies the continuous-playback functional gate while preserving
the remaining performance, compatibility and packaging gates.
The concurrent-pressure GUI harness explicitly requests both Viewers after graph
edits reach the event loop, since connection notifications can supersede an earlier
automatic render. It still requires distinct request tokens and two overlapping
CPU actions; a barrier timeout fails the test.

`test-windows-vulkan-software.ps1 -IcdPath <software-ICD.json>` forces a single
CPU Vulkan 1.3 device and runs nine headless core tests with validation. It checks
the ICD DLL, test executables and source hashes before and after the run, and
restores process environment variables. Compute, bounded allocation recovery,
Viewer display math, image operations, fragment programs and LUTs are covered.
Supply the manifest from an installed software driver such as SwiftShader and
use a fresh `-ReportDirectory`. This is local core evidence; Natron GUI tests,
CI execution and packaged driver distribution require separate validation.

`test-windows-raster-fallback.ps1` checks CPU-only headless rendering and the
integrated raster Viewer with Vulkan explicitly disabled and with no usable
driver manifest. It tests both texture depths and display scales 1 and 2,
image pixels, pan/wheel input, graphics modules and startup/shutdown watchdogs.
The CPU IO overlay cases also check four native DrawSuite handles, focus reset
and undo/redo at both display scales. The matrix has 14 configurations.
Reports and captures go to `build/raster-fallback-tests`. Each test uses disposable
settings; GUI test scripts exit their own process without invoking a save dialog.
`NATRON_VULKAN_DISABLED=1` selects CPU processing and display without loading Vulkan.
The `RasterViewerNoGpu` CTest checks the display/frame contract without a graphics
driver. `Tests/NatronRasterGpuCompare.exe <gpu-index>` is an explicit hardware
comparison of the raster and Vulkan display math.
`uv run --frozen python tools/verify-raster-fallback.py --fallback-report <report.json>`
rehashes the copied artifacts, current source/import audits and native GPU
regression reports before writing `build/vulkan-raster-fallback-proof.json`.
It records the remaining migration gates instead of marking the full migration complete.
Use `--native-prefix <path-prefix>`, `--source-audit <json>` and `--runtime-audit <json>`
to verify a fresh regression checkpoint without replacing earlier reports.

`test-windows-vulkan-ocio.ps1` also runs `OcioVulkanFailureTest` before its GPU
matrix. It injects failures at device information, shader creation, image allocation,
upload, render, readback and release. The 88 cases include RGB/RGBA, padded rows
and negative strides; 84 retries verify that a temporary failure leaves the caller's
pixels unchanged and permits the existing OCIO CPU transform to run exactly once.
This does not simulate a real driver reset or prove Engine recovery from device loss.

`test-windows-vulkan-io-overlay.ps1` verifies the shipped IO DrawSuite overlays
on both GPUs at screen scale 1 and 2. It covers rectangle, transform, ramp and
Text position drawing, pen edits, focus modifier reset and undo/redo, retains
52 captures and checks that IO has no direct OpenGL import. A separate adapter
test checks canonical transforms, pixel-sized points, text and invalid calls.
The launcher uses disposable settings and owned-PID timeouts to detect startup
or shutdown dialogs. `verify-io-vulkan-source.py` verifies both saved IO patches
and the pinned SupportExt/OpenFX sources; run it through `uv run --frozen python`.

The current Natron build uses `build-windows-qt6.ps1` and
`run-windows-qt6.ps1`; see [BUILDING.md](../BUILDING.md). The former
Jenkins/buildmaster Natron entry points are retired. SDK and installer
scripts below are historical tooling and need a Qt 6 port before reuse.

`build-windows-ofx.ps1` builds the remaining standard OpenFX sets with CMake and
MSYS2 UCRT64. `run-windows-qt6.ps1 -Program OfxSmoke` checks their rendering, and
`-Program OfxGuiSmoke` checks a GUI graph. See [INSTALL_WINDOWS.md](../INSTALL_WINDOWS.md).

`probe-windows-vulkan.ps1 -SdkRoot C:\VulkanSDK\1.4.363.0` records Vulkan GPU
features, image formats, Win32 presentation queues, and shader toolchain checks
under `build/vulkan-probe`. It uses the SDK explicitly even when the desktop
process inherited an older `VULKAN_SDK`. See the
[Vulkan migration plan](../Documentation/source/maintainers/vulkan-migration.rst).

`build-windows-vulkan.ps1` builds the reusable Vulkan core and Qt presenter with
independent compute, Viewer renderer and native presentation tests.
`test-windows-vulkan.ps1` checks both local
GPUs with validation, CPU references and a process watchdog; frame readbacks
and diagnostics go to `build/vulkan-tests`. Use `-Tests Renderer` for only the
CPU/GPU display and cache tile comparisons, or `-Tests Viewer` for native
presentation. The Natron GUI smoke runner also
isolates its settings and autosaves, monitors its own PID, and fails on an
unexpected modal or a startup/run/shutdown timeout.
`-GuiTextureDepth 8u` or `32f` selects the isolated GUI profile's texture depth.

`test-windows-vulkan-natron.ps1` checks the integrated Natron Viewer on both
GPUs, at both texture depths, using GUI, OFX, native overlay, DrawSuite and
timeline/histogram/native custom parameter/curve/Dope Sheet tests (36 cases). It checks frame pixels,
native pan/wheel input, shared device ownership, and validation through shutdown.
Reports and frame PNGs are saved per case under `build/vulkan-natron-tests`.
User preferences are fingerprinted before and after. CPU and renderer tests
also receive their own settings, cache, and autosave profile.
The matrix report records executable and preference hashes, and rejects an
executable change during the run.
Each completed case also records hashes of the test definitions, JSON, PNGs
and log. `-Resume` reuses a checkpoint only when those hashes, the executable,
preferences, GPU, depth and display scale still match. Interrupted runs retain
`report.partial.json`; only a completed matrix writes a passing `report.json`.
The main Viewer uses Vulkan. `-Program OverlaySmoke` checks Roto contour pixels
and native Roto, transform, Corner Pin and Tracker drag operations. Tracker
magnifier/keyframe previews check row/channel order and sRGB conversion;
magnifier resizing and dragging are exercised too. `-Program DrawSuiteSmoke`
checks an independent OFX C ABI plugin: V2 selection over a deliberately failing
V1 entry point, alpha compositing, a one-physical-pixel hairline, native pen
editing and rejection of expired draw contexts. Its bundle has no Qt or
graphics API imports and is added to the plugin path only for this test.
Tracker tests also check held modifiers, interrupted drags and temporary/manual
Add Track modes. The DrawSuite plugin opens an automatically dismissed OFX
message dialog to check that focus callbacks wait for the modal action to
return and cancel the interrupted drag before following input.
`-Program TimelineSmoke` checks QWidget/QPainter timeline rendering without an
OpenGL widget, cache/keyframe marker pixels, scrubbing, playback bounds,
wheel zoom, middle-button pan, right-button range selection and timecode text.
Native Viewer cache markers come from resident node images; the test also
clears the per-node cache, refills it and switches input to reject stale markers.
`-Program HistogramSmoke` checks QWidget/QPainter histogram bars in all six
channel modes, additive RGB overlap, rapid asynchronous mode changes,
smoothing, current/named Viewer selection, Viewer RGB/alpha pickers, out-of-range
picker values, input A/B, and pan/zoom/fit. Histogram bars retain the original
unsmoothed strokes so partial coverage does not tint saturated additive overlaps.
`-Program ParamDrawSuiteSmoke` loads a second independent OFX C ABI plugin for
Natron's custom-parameter DrawSuite extension. It checks native selection over
V1, ordinary controls for a V1-only parameter, alpha/hairline pixels, logical
viewport size and DPR, fractional pen edits, UTF-8 keys/repeat, resize and focus.
Its controlled OFX message dialog verifies that drawing, keys, pen and focus do
not reenter the plugin's modal loop. Pending focus loss cancels drag/modifier state
after the dialog returns. `Global/OpenFX/ParamInteractDrawSuite.h` describes the
extension; standard OpenFX custom parameter V1 remains GL-only and is retired.
`-Program CurveSmoke` loads a third independent OFX C ABI plugin for native
parametric backgrounds. It checks gradients (including skew), additive blending,
native selection over a failing GL callback, expired contexts, key/tangent edits,
undo/redo, zoom/pan and ordinary animation curves with timeline scrubbing.
It also verifies shipped ColorLookup ramp/histogram backgrounds, both HueCorrect
major versions, Hue Vs Hue and HueKeyer against pixel references.
`Global/OpenFX/NatronDrawSuite.h` defines the optional gradient/blend/antialias
extension; `ParamInteractDrawSuite.h` defines the separate background entry point.
Both use the standard draw context and lifetime rules. Parameter backgrounds
receive Draw only; their curve editing stays in the host.
`-Program DopeSheetSmoke` checks the raster Dope Sheet's key icons, selection,
move/scale, interpolation, copy/paste/delete, undo/redo, scrub, zoom/pan,
Reader trim/slip/move, TimeOffset/FrameRange/Group editing and Retime display.
It also checks collapsed/hidden/scrolled rows, resize and a modal interrupted
drag. The known test dialog closes automatically; unexpected dialogs fail.
Shipped OFX overlays await their port, along with the remaining
Engine OpenGL paths. Qt OpenGL and OpenGLWidgets are no longer GUI build
dependencies. Standard scripting startup uses native PySide6 `QtCore`, `QtGui`
and `QtWidgets`; `GuiSmoke` also audits the loaded Qt OpenGL DLLs on Windows.

`test-windows-vulkan-engine.ps1` checks native image copy, ROI gain/offset,
channel restoration and mask/mix against CPU references on both configured
GPUs, then renders through an independent OFX plugin and NatronRenderer.
It also runs `-Program ImageStorageSmoke`: 106 Engine Image/CacheEntry cases
cover native allocation, ROI copies, CPU transfers, mask/mix, channel restoration,
resize, bitmap state, cache accounting/release and reciprocal concurrent copies.
Eight NaN cases inspect and repair RGBA on GPU with four-byte flag readback,
preserving infinities, pixel bits during read-only inspection and bitmap/ROI state.
Three runtime-sharing cases cover concurrent acquisition by eight workers,
native copies between owners and release after the last image/owner expires.
OFX contexts with the same device selection and validation mode share a weakly
pooled logical device/runtime; their opaque handle namespaces remain separate.
The Vulkan backing currently accepts Float RGBA images. Opt-in effects and
Readers render a single RGBA color plane through native Engine cache images
and GPU temporary output buffers. Engine mipmaps and full-scale fallback
resampling also stay on the GPU. Other components, multiple simultaneous planes
and analysis renders keep their CPU render path.
`-Program ClipBorrowSmoke` runs 115 checks on native OFX clip borrowing,
read-only input, ROI-restricted output, cropped negative-origin images,
expired handles across contexts, persistent shaders, bitmap validity and release
order, including a cache entry published before its GPU allocation completes.
Borrowing and GPU operations perform no CPU pixel transfers. The test runner
removes each exited test's disposable Viewer cache parts while retaining its
isolated settings, logs and proof artifacts.
`test-windows-vulkan-image-convert.ps1` runs 1218 CPU-reference comparisons
per GPU for format and color-space conversion: 384 CPU-to-Vulkan, 768
Vulkan-to-CPU, 64 native conversions and two explicit cross-context transfers.
CPU boundaries support Byte/Short/Float with Alpha/XY/RGB/RGBA components;
native images remain Float RGBA. Linear/sRGB/Rec.709/BT.1886 conversions preserve
alpha, bitmap validity and pixels outside the ROI. Aliased native conversions
use GPU temporaries and transfer counters verify no CPU upload/download.
`-Program ImageConvertSmoke` runs this standalone Engine test with an isolated
profile and PID watchdog. The report records source/binary and output hashes,
post-exit stderr and unchanged user preferences.
`-Program EngineVulkanSmoke` runs the OFX test alone with an isolated profile
and process watchdog. The public `NatronVulkanImageSuite.h` defines an opt-in
Natron C ABI using opaque RGBA32F images. `NatronVulkanRenderSuite.h` adds the
native render capability and an action flag, and borrows standard clip handles
into an existing plugin context on the shared runtime. Native clips expose null
CPU data and zero row bytes; clip release invalidates every borrowed token.
Plugin-owned temporary images and persistent shader programs remain independent.
The Engine test renders Reader → two native effects → Writer, compares all
output pixels to the CPU-clip path, checks native cache reuse and renders again
after Unicode project reload and a changed input path. With no-op Linear IO
color management, each render uploads once at the Reader boundary and downloads
once at the Writer boundary; the intermediate native nodes transfer no CPU
pixels. IO's existing OCIO GPU staging is measured separately by the OCIO suite.
Signed/padded row strides, context isolation, concurrent calls and aliased outputs
are also covered. Shipped OCIO and Shadertoy effects now borrow Float RGBA clips.
Render-scale-capable effects also select native Float RGBA clips for proxy
renders. Completed higher-resolution native cache entries are now downscaled
on the GPU with the same coordinate-aware 2x2 box average as CPU mipmaps.
Full-scale-only effects use native nearest-neighbor input expansion followed by
native output reduction. Formats without a native operation use CPU boundaries;
the legacy GL paths have been retired.

`test-windows-vulkan-proxy.ps1` compares two-node native and shipped OCIO graphs
to CPU clips in the actual Viewer at full size and 1/2, 1/4, 1/8, 1/16 and 1/32.
It checks native cache reuse without another render, input/output clip scale
metadata, and Shadertoy's scaled resolution/render-scale uniforms. It also warms
full-size native/OCIO cache entries, switches to all five proxy scales and
requires zero native rerenders, CPU uploads or downloads and a native GPU Viewer
copy. Full-scale-only test effects also compare proxy or full-resolution
inputs, host channel restoration/mask/mix, cached full-resolution reductions and
separate cache entries for each proxy input quality. Their render actions and
clip metadata must remain at scale 1. Each GPU's 8u and 32f presentation paths run
60 comparisons with isolated profiles and PID
watchdogs. `-Program VulkanProxySmoke` runs one matrix instance. Source, binary,
post-exit artifact hashes and user preference fingerprints are recorded under
`build/vulkan-proxy-tests`. The CPU mipmap boundary handles odd/negative bounds
and one-pixel dimensions; ImageMipmapTest checks 162 layouts plus an incomplete
corner bitmap. Native ImageStorageSmoke additionally checks 360 mipmap cases:
nine odd/negative/single-pixel shapes, two source levels, partial/full ROIs,
five reductions and bitmap copying on/off. ROI padding and incomplete bitmaps
must match CPU and native generation must transfer no pixel data.
ImageUpscaleTest covers 48 CPU layouts across byte/short/float, Alpha/RGBA,
negative bounds and partial ROIs; ImageStorageSmoke checks 540 nearest-neighbor
expansions across native/native, CPU/native and native/CPU boundaries, with 180
cases each. Native expansion must transfer no CPU pixels; boundary cases must
upload or download once. ROI padding and bitmap state must be retained.
ImageKeyTest checks cache-key serialization version 3, proxy-input quality
separation and rejection of older cache entries with unknown input quality.
Proxy cache display must match exactly for both 8u and 32f using the deterministic
GPU conversion. CPU references still verify the displayed pixels independently.
The earlier 72-comparison checkpoint, including the existing Engine,
OCIO, Shadertoy, conversion, Viewer/overlay and CTest regressions, was recorded in
`build/vulkan-native-proxy-validation-proof.json`.
The subsequent GPU mipmap checkpoint passed all 120 presentation comparisons
(51000 sampled pixels), including 40 reductions from native cache with zero
rerenders/uploads and one Viewer-boundary download each. Both GPUs passed
360 native mipmap comparisons, the existing Engine/OCIO/Shadertoy/conversion/GUI
regressions, CTest and the OFX project roundtrip. Source/binary/artifact hashes,
zero unexpected dialogs/validation errors and unchanged user preferences were
independently checked in `build/vulkan-native-mipmap-validation-proof.json`.
The full-scale-only checkpoint passed all 240 presentation comparisons (102000
sampled pixels), including 120 full-scale-only comparisons and 60 reductions
from completed native caches with zero rerenders/uploads and one Viewer-boundary
download. Both GPUs passed 540 expansions, including 180 native-only cases
without pixel transfers, plus 360 native reductions and all existing regressions.
Display error against CPU was at most one byte; cached 32f display matched
exactly. Compute covered 666 reference cases and 22 invalid inputs. GUI/overlay,
CTest and the OFX project roundtrip passed with no unexpected dialogs or Vulkan
validation errors and unchanged preferences. That checkpoint's source/binary/artifact
hashes were independently checked in
`build/vulkan-native-fullscale-validation-proof.json`.

`test-windows-vulkan-reader.ps1` validates stock Reader native color/alpha work
at the decoder boundary, the Reader-to-OCIO native chain, project roundtrips,
216 internal crop/proxy/alpha cases with signed padded rows and ROI guards,
and 42 Viewer comparisons per GPU/texture-depth pair. Stock Readers retain
their full-resolution decode cache; Engine performs native proxy reduction.
The tests use isolated profiles, PID watchdogs, shader validation and preference
fingerprints. Headless renders require one upload and one encoder download,
while cached Viewer renders require no Reader rerender or upload. The Writer
checkpoint below adds native internal OCIO for Float/RGBA; other native component
formats remain migration work.

The 2026-10-11 Reader checkpoint passed on RTX 5080 and AMD Radeon Graphics:
70 stock comparisons, 168 Viewer comparisons (71,400 sampled pixels), and
216 internal processing cases per GPU. All 504 Viewer captures were 1400x600;
1,016 saved Reader artifacts were hash-verified. Maximum Viewer error was one
byte step, with zero validation errors or unexpected dialogs. The existing
proxy/OCIO/Shadertoy/Engine/GUI/CTest/OFX checks passed again, including the
115-check pending-cache-allocation regression. The combined independently
verified proof is `build/vulkan-native-reader-validation-proof.json`.

`test-windows-vulkan-writer.ps1` compares Float/RGBA Writers on both GPUs.
First run `uv run --frozen python tools/reader-vulkan-fixtures.py` to prepare the
input fixtures. It checks EXR half/float, PNG, float TIFF, JPEG, mixed CPU/native
boundaries, the normal Write container, repeated encoding and project reloads.
`compare-writer-vulkan.py` independently reads every saved channel with OIIO and
checks identity EXR against the input pixels. The 2026-10-11 checkpoint passed
154 comparisons and 240 internal alpha/ROI/row-layout cases per GPU, with one
download per encoder, immutable inputs and unchanged user preferences. A cached
Reader feeding two Writers requires zero uploads and two downloads on a repeat
render, and recreates both files. The saved proof is
`build/vulkan-native-writer-tests/report.json`.
Reader's 70 saved-image comparisons and 168 Viewer comparisons also passed
on the updated binaries, along with Engine/OCIO/CTest/OFX regressions. The
independently verified combined proof, including all 630 Writer artifacts and
504 fixed-size Viewer captures, is `build/vulkan-native-writer-validation-proof.json`.

Viewer and Engine now use the same Vulkan instance and logical device. Automatic
and explicit selection of the same physical device reuse one Engine context;
headless instances without surface extensions do not enable swapchain support.
The float RGBA/RGB display path copies the resident Engine image into a display
snapshot on the GPU and reuses the Engine cache. Histogram and color-picker CPU
snapshots are created lazily when requested. The Reader GUI matrix disables
project `autoPreviews` to measure Viewer transfers separately from node thumbnails.
The other channel/contrast/partial-update paths remain migration
work, along with native formats, planes and the remaining OpenGL paths.
The 2026-10-12 Reader matrix passed 168 comparisons and 504 captures on RTX 5080
and AMD Radeon Graphics. Its 84 float display cases use zero Engine image
downloads; cache redisplay uses zero uploads and downloads. Maximum float-display
differences from CPU processing were one byte value on RTX and zero on AMD.
The 146 native clip/snapshot checks per GPU also cover normalized selection,
snapshot independence, padding and opaque NaN alpha. Native-image histogram and
picker tests passed on both GPUs.
The final build also passed Engine, Reader, OCIO, UI, Writer, CTest and OFX
regressions. The combined proof is
`build/vulkan-native-viewer-validation-proof.json`: 308 Writer cases,
1,461,484 independently compared channel values, 630 verified Writer artifacts,
504 Viewer captures, and unchanged user preferences. GUI profiles, startup/exit
logs and watchdog results are included in the verification.

`test-windows-vulkan-viewer-display.ps1` checks the native byte display path on
both GPUs. The GPU converter applies gain/offset, the existing gamma and display
LUT tables, byte alpha and row error diffusion without downloading Engine pixels.
Its stable row starts keep dithering unchanged on cache redisplay. The backend
matrix checks 432 cases, including negative/zero gamma, HDR values, opaque alpha,
negative origins and narrow ROIs. A full-frame grid test compares cropped
snapshots and padded cache tiles with nearest/bilinear sampling, PAR and mipmaps.
The real Viewer then compares 32 LUT/exposure/gamma/premultiplication settings per
GPU against forced CPU display processing in isolated profiles. Reader regressions
also compare native 8bit and float display with the CPU Viewer conversion.
On 2026-10-12 the final build passed all 64 display-setting cases with a maximum
CPU difference of one byte value and identical cache redisplay. Reader tests
passed all 168 byte/float cases with zero Engine image downloads and zero cached
uploads/downloads. The combined GUI matrices saved 696 captures. Per GPU, the
byte converter passed 432 cases/580,032 channels, and the grid comparison passed
eight full frames/26,880,000 channels with zero difference.
The same final build passed Engine, byte/float UI and histogram/picker, OCIO,
Writer, CTest and OFX regressions. The combined proof with verified source,
binary, report and artifact hashes is `build/vulkan-byte-viewer-validation-proof.json`.
It also verifies all 308 Writer cases/1,461,484 channel values and 630 artifacts,
zero validation errors, and unchanged user preferences. The current dependency
audit for that build is `build/vulkan-opengl-retirement-audit.json`; at that
stage Arena, IO and Misc retained direct OpenGL imports. The later native OFX
build described below removes those imports.

`test-windows-vulkan-shadertoy.ps1` checks the runtime fragment shader backend
and the shipped Shadertoy node on both GPUs with validation and isolated profiles.
The backend checks 40 CPU reference cases, seven invalid inputs, four concurrent
workers, sampler wrap/filter/mipmaps, derivatives, aliased ROIs and resource
reflection. Each GPU then renders 19 Shadertoy cases and compares 236873 pixels,
including built-in uniforms, all six parameter types, Auto Params, four inputs,
filter/wrap modes, the shipped Gaussian Blur preset and a Unicode project reload.
It checks stdin EOF, output hashes after exit and unchanged user preferences.
Reports and copied artifacts are saved under `build/shadertoy-vulkan-tests`.
`-Program ShadertoyVulkanSmoke` runs the node checks alone with the renderer
watchdog. `NatronVulkanShaderSuite.h` defines the opt-in V1 C ABI for compiling,
rendering, releasing and inspecting active shader resources in an Image Suite
context. Shadertoy renders directly into native Float RGBA clips and borrows
compatible inputs. Transfer counters require only Reader uploads and a Writer
download. Other formats and effects without render-scale support retain explicit
CPU boundaries. Its active target excludes the retired OpenGL/Mesa renderer and
context code. General Engine dispatch still contains legacy OpenGL branches.

`test-windows-vulkan-ocio.ps1` checks Shader Suite V2, immutable 1D/2D/3D LUTs
and all six shipped OCIO nodes on both GPUs. The native LUT test checks 19 CPU
reference cases and nine invalid layouts. The independent OFX C ABI test uses
OCIO 2.6's CPU Float processor for 64 cases with signed/padded RGB/RGBA rows,
four workers, seven simultaneous 3D LUTs, large 1D LUTs stored as 2D textures
and all five dynamic uniform types. Twenty-two shipped render comparisons cover
the six nodes, nearest/linear/tetrahedral LUTs, premult on all six nodes,
shared RGBA and separate Alpha masks, inverted mask, mix, a two-node native
chain and a Unicode project reload. Transfer counters require one Reader upload,
one Writer download and an additional mask upload when used. Native cache reuse
requires zero renders/uploads and one Writer download. Each comparison uses 1247 pixels and an
absolute plus relative tolerance of 3e-5. Linear LUT sampling is evaluated in
shader float arithmetic to avoid hardware interpolation precision differences.
The native Viewer test also starts after a headless GPU context and overlaps
four workers creating 16 contexts with both validation settings. It checks
actual float image transfers while presentation initializes. The active Qt
platform's required surface extension is requested directly, and Volk's
device resolver is selected from the owning instance under the loader lock.
The script verifies validation errors, interpreter EOF, post-exit artifact
hashes, runtime DLL/source/binary hashes and unchanged user preferences.
Results are stored in `build/ocio-vulkan-tests`. `-Program OcioVulkanSmoke`
runs the renderer checks alone with an isolated profile and PID watchdog.
Shader Suite V2 adds explicit descriptor bindings and immutable LUT data while
preserving V1. IO is built from pinned sources with OCIO 2.6, OIIO 3.1,
FFmpeg 9 and SeExpr 2.11; its OCIO GL shader implementation is excluded.
General Engine dispatch still contains legacy OpenGL branches.

`test-windows-vulkan-plugin-overlay.ps1` checks the native Misc and Arena overlays
on both GPUs at screen scales 1 and 2. It exercises CopyRectangle, Transform,
Ramp, Position, CornerPin, TrackerPM, Arena Text and ContactSheet. The interactive
cases verify actual dragging, focus modifier reset, Edit-menu Undo/Redo and
captured pixels; ContactSheet verifies its selection outline. The script also
checks 26 drawing-adapter cases, all five bundles' direct PE imports, saved
patches, artifact hashes, validation errors and unchanged user preferences.
`-Program PluginOverlaySmoke` runs one GUI instance in an isolated profile with
a process watchdog. `test-windows-vulkan-io-overlay.ps1` checks the four IO
overlay cases with the same shared harness.

The authoritative Misc changes, including native parametric and ShaderToy
support, are consolidated in `openfx-misc.patch`; Arena uses `openfx-arena.patch`.
Each bundle's SupportExt changes have a separate `*-supportext-vulkan.patch`.
`build-windows-ofx.ps1` applies these patches idempotently.
`uv run --frozen python tools/verify-native-ofx-source.py` checks the parent pins,
submodule gitlinks and exact patch contents for Misc, Arena and IO. Apply only
the patches selected by the build script; the earlier incremental Misc patches
are superseded.

Engine startup and per-frame TLS no longer initialize or acquire OpenGL
contexts. Legacy OpenGL preferences are hidden; the graphics backend field
reports Vulkan. Undo history is independent of whether a parameter evaluates
the image, and merged animated edits restore the original key or static value.
Engine's old image/render/storage branches and their GLAD/context sources still
need removal before the source migration is complete.

GUI tests disable the OBS implicit Vulkan layer only in their own process via
`DISABLE_VULKAN_OBS_CAPTURE=1`, restoring the caller's environment afterward.
An observed `opengl32.dll` load on NVIDIA was traced through the Vulkan loader
and NVIDIA driver. Module presence alone therefore does not identify an
application OpenGL call. The harness records module loads; direct imports,
source paths and actual Vulkan rendering are audited separately.

`build-windows-ofx-codecs.ps1` builds the pinned libultrahdr 1.5.1 with
`UHDR_ENABLE_GLES=OFF` and the existing `libuhdr-1.dll` ABI name. The OFX build
runs it automatically, and runtime installation prefers its DLL over the MSYS2
package. Unreferenced EGL/GLES DLLs are moved out of the runtime bin directory.
`uv run --no-project --python .venv/Scripts/python.exe --with pefile==2024.8.26 python tools/test-windows-ofx-codecs.py`
compares six HDR decode cases and both encoded JPEGs against the package's CPU
implementation in separate processes. All 18,432 pixels and all output bytes
match, and all 30 exports required by OIIO and the ImageMagick UHDR coder exist.
`audit-native-ofx-runtime.py`, run through the same pinned pefile environment,
checks normal and delay imports of the applications, every bundle and every
codec DLL. The current runtime has 184 DLLs and the audit covers 332 PE files
without a direct OpenGL/EGL/GLES import.

`audit-ofx-vulkan.ps1 -CacheFile <test-profile-cache.xml>` inventories every cached
OFX plugin version and each module's direct DLL imports. The CSV and JSON in
`build/ofx-vulkan-audit` retain unverified CPU/overlay migration status. Native
import inspection falls back to `uv` with pinned `pefile==2024.8.26` for the
older stripped IO binary. Transitive imports and runtime loads need separate checks.

- **MacPorts**
  
  Repository for Natron-specific ports for MacPorts. Some ports are not in MacPorts (e.g. openimageio), some have extra patches that fix specific issues (licensing, universal builds, etc.). Next to each `Portfile`, we always store the original MacPorts `Portfile.orig` and the `Portfile.patch` to apply. The `check.sh` script helps checking which `Portfile`s have to be updated after a `sudo port selfupdate`.

- **genStaticDocs.sh**

  This is the script used to generate the HTML documentation which is shippend with Natron binaries. This script is executed by the buildmaster and jenkins scripts, but it can also be used to preview the result while writing documentation.
  
  The script requires [pandoc](https://pandoc.org/) and [Sphinx](http://sphinx-doc.org/).
  
  For example, on macOS, if a Natron snapshot or release is installed as `/Applications/Natron.app`, The static docs can be generated in the `html` subdirectory with the following commands:
  ```
  cd <Natron source directory>/Documentation
  natron="/Applications/Natron.app"
  env FONTCONFIG_FILE="$natron/Contents/Resources/etc/fonts/fonts.conf" \
  OCIO="$natron/Contents/Resources/OpenColorIO-Configs/blender/config.ocio" \
  OFX_PLUGIN_PATH="$natron/Contents/Plugins" \
  NATRON_PLUGIN_PATH="$natron/Contents/Plugins/PyPlugs" \
  bash ../tools/genStaticDocs.sh "$natron/Contents/MacOS/NatronRenderer" /var/tmp/natrondocs .
  sphinx-build -b html source html
  ```

- **jenkins**

  These scripts are used to:
  
  - build the Natron SDK on Linux 32bit and Linux64bit (any distribution):
	
    `./include/scripts/build-Linux-sdk.sh` builds and install all Natron dependencies to `/opt/Natron-sdk`, which must be a user-writable directory. This includes a patched Qt4, a [mangled OSMesa](https://github.com/devernay/osmesa-install) (with LLVM 4.0.1), OpenImageIO, and all other dependencies.

  - build the Natron SDK on Windows 7 with [MSYS2](https://www.msys2.org/) installed:
	
	  - `MSYSTEM=MINGW64 BITS=64 ./include/scripts/build-Windows-sdk.sh` builds and installs the 64bit versions of the required packages.
	  - `MSYSTEM=MINGW32 BITS=32 ./include/scripts/build-Windows-sdk.sh` builds and installs the 32bit versions of the required packages.
	  
	  Note that the system should also be regularly updated. Updates usually go this way:
	  
      - pull updates from your local clone of the official [MINGW-packages](https://github.com/Alexpux/MINGW-packages) repository.
	  - go to the `tools/MINGW-packages` directory in the Natron sources (detailed below), and execute `./check.sh` to see if any package needs updating. Update `PKGCONFIG` for these packages accordingly.
	  - execute `pacman -Syu` to update MSYS2

	    Answer "n" to those two questions, if `pacman -Syu` asks:

	    ```
		:: Replace mingw-w64-i686-x264 with mingw32/mingw-w64-i686-x264-git? [Y/n] n
	    :: Replace mingw-w64-x86_64-x264 with mingw64/mingw-w64-x86_64-x264-git? [Y/n] n
		``

	    After execution, `pacman -Syu` may ask you to close and relaunch the terminal.
	  
	  - execute `MSYSTEM=MINGW64 BITS=64 ./include/scripts/build-Windows-sdk.sh` to rebuild and install the Natron-specific packages.
	  
  - build the Natron binaries and execute the unit tests on OS X 10.9 (`macStartupJenkins.sh`), Linux CentOS 6.4 32bit and 64bit (`linuxStartupJenkins.sh`) and Windows 10 32bit and 64bit (`winStartupJenkins.bat`). These build scripts are basically updated versions of the scripts from the `buildmaster`directory, but were never tested to produce actual releases. Also note that the OS X script does not produce universal 32/64 bits binaries when run on OS X 10.9.

- **buildmaster**

  The directory, given for reference, contains the old scripts that were used to build Natron binaries on the Natron build farm. These scripts require a complicated infrastructure (build farm with one machine of each arch, database to store the build results, web server to store the binaries and build symbols, etc.).
  
  These scripts are here mainly for reference, and the jenkins scripts are probably more up-to-date, although they were never used to produce releases.
  
  The main script is `buildmaster.sh`, and we launch it that way:
  - on OS X 10.6, Linux CentOS 6.4 32bit, Linux CentOS 64bit: `env NATRON_LICENSE=GPL ./buildmaster.sh`
  - on Window 7 64bit, we run in parallel the scripts to build the 32bit and the 64bit versions:
    - `env NATRON_LICENSE=GPL MSYSTEM=MINGW32 BITS=32 MKJOBS=2  ./buildmaster.sh`
    - `env NATRON_LICENSE=GPL MSYSTEM=MINGW64 BITS=64 MKJOBS=2  ./buildmaster.sh`
  The script that builds the Windows packages is in `buildmaster/include/scripts/build-Windows-installer.sh`, and must be updated every type a dependency DLL filename changes (this was made automatic in the corresponding jenkins script).
  

- **docker**

  Contains tools to build a docker image that contains the Natron SDK installed on top of CentOS6.

  The `natron-sdk` docker image is also available from dockerhub, see https://hub.docker.com/r/natrongithub/natron-sdk
  
- **MINGW-packages**

  This directory contains Natron-specific packages for MSYS2. For example, therer are ffmpeg and libraw versions with different licences, a patched version of Qt4, and a version of OpenImageIO 1.7 (because later versions hang when quitting Natron, see [this OpenImageIO issue](https://github.com/OpenImageIO/oiio/issues/1795)).
  
  The `check.sh` script may be used to check whether packages need to be updated, supposing that `../../../MINGW-packages` contains a clone of [MINGW-packages](https://github.com/Alexpux/MINGW-packages)  (note that the `mingw-w64-pyside-qt4` and `mingw-w64-shiboken-qt4` have not been updated for a long time, because they just work).

- **license**

  This directory contains license files for Natron and all its dependencies (including the plugins dependencies). The main Natron distribution is GPL2, so please make sure that no dependencies is GPL3. Natron could be distributed with another licence (LGPL or commercial), if the code owners allow it, but in this case GPL2 components can not be used (for example, only LGPL versions of ffmpeg and libraw may be used, and poppler can not be used).
  
  These files are included by `Gui/GuiResources.qrc` in the Natron sources, and are available through the About window in Natron.
  
- **mkTarballs.sh**

  A script to make tarballs of Natron and plugins sources (probably needs to be updated or fixed).
  
- **normalize**

  The source for the `normalize` utility from Qt4, which normalizes the signatures in SIGNAL() and SLOT() macros through the Natron source code. This gives a very small speedup at execution. Most code-formatting tools usually break normalized signatures, so this tool should be run afterwards.
  
- **travis**

  Scripts used for [Travis CI builds of Natron](https://travis-ci.org/NatronGitHub/Natron). See also the hidden file `.travis.yml` at the top source directory.

- **uncrustify**

  Configuration file for [uncrustify](http://uncrustify.sourceforge.net/). Unfortunately, uncrustify also breaks things, so the source code needs to be fixed after running it:
  - run uncrustify first
  
    `uncrustify -c tools/uncrustify/uncrustify.cfg --replace */*.cpp */*.h`

  - fix the output of uncrustify using the provided sed script

    `sed -f tools/uncrustify/uncrustify-post.sed -i .bak */*.cpp */*.h`

  - run Qt's normalize utility to fix signals/slots
  
    `for d in Gui Engine App Global Renderer Tests; do tools/normalize/normalize --modify $d; done`

- **utils**

  A few more maintenance utilities:
  
  - `fixpngs.sh` removes any date reference from the PNGs in the source tree, and also runs [optipng](http://optipng.sourceforge.net/)
  
  - `runPostShiboken.sh` is used to fix source files after running [shiboken](https://github.com/pyside/Shiboken) to generate the Python bindings. See the section *Generating Python bindings* in `INSTALL_MACOS.md` and `INSTALL_LINUX.md`.
valgrind

- **valgrind**

  [valgrind](http://valgrind.org/) suppression files.
  
  There is also a suppression file for Python in `jenkins/include/natron/valgrind-python.supp`and `buildmaster/include/natron/valgrind-python.supp`
