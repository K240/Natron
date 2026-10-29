[CmdletBinding()]
param(
    [ValidateSet('Natron', 'NatronRenderer', 'NatronPython', 'NatronUsdSceneBridge', 'NatronCameraSolve', 'NatronCameraSolveUsd', 'NatronCompositing', 'NatronReflection', 'NatronFeatureTracking', 'Tests', 'CTest', 'Smoke', 'RasterGuiSmoke', 'RasterIOOverlaySmoke', 'HeadlessCpuSmoke', 'MemoryFailureSmoke', 'MemoryPressureSmoke', 'DeviceLossSmoke', 'PerformanceRender', 'EngineVulkanSmoke', 'ShadertoyVulkanSmoke', 'OcioVulkanSmoke', 'ImageStorageSmoke', 'ImageConvertSmoke', 'ClipBorrowSmoke', 'VulkanProxySmoke', 'ReaderVulkanSmoke', 'ReaderEngineVulkanSmoke', 'WriterEngineVulkanSmoke', 'GuiSmoke', 'MemoryGuiSmoke', 'ConcurrentMemoryGuiSmoke', 'DeviceLossGuiSmoke', 'ViewerPerformance', 'OfxSmoke', 'OfxGuiSmoke', 'OverlaySmoke', 'IOOverlaySmoke', 'PluginOverlaySmoke', 'DrawSuiteSmoke', 'ParamDrawSuiteSmoke', 'CurveSmoke', 'DopeSheetSmoke', 'TimelineSmoke', 'HistogramSmoke', 'Scene3DGuiSmoke', 'Scene3DFbxDemoGuiSmoke', 'Scene3DFbxAnimationDemoGuiSmoke', 'Scene3DFbxCameraDemoGuiSmoke', 'Scene3DSmoke', 'Scene3DLightingSmoke', 'Scene3DLightingDemoGuiSmoke', 'Scene3DEnvironmentSmoke', 'Scene3DEnvironmentDemoGuiSmoke', 'Scene3DHdNvpDemoGuiSmoke', 'Scene3DUsdDemoGuiSmoke', 'Scene3DUsdSmoke', 'Scene3DStageOutlineGuiSmoke')]
    [string]$Program = 'Natron',
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$VulkanSdkRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$BuildDirectory = 'build\windows-qt6',
    [string]$OfxPluginDirectory = 'build\Plugins',
    [ValidateRange(1, 600)]
    [int]$GuiTimeoutSeconds = 60,
    [ValidateSet('8u', '32f')]
    [string]$GuiTextureDepth = '8u',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ProgramArguments
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$pythonRoot = & uv run --frozen python -c 'import sys; print(sys.base_prefix)'
if ($LASTEXITCODE -ne 0) { throw 'Could not locate the uv Python environment.' }
$sitePackages = Join-Path $repoRoot '.venv\Lib\site-packages'
$depsBin = Join-Path $repoRoot 'build\vcpkg_installed\x64-windows\bin'
$ofxRuntime = Join-Path $repoRoot 'build\ofx-runtime'
$env:PATH = "$QtRoot\bin;$sitePackages\PySide6;$sitePackages\shiboken6;$pythonRoot;$depsBin;$ofxRuntime\bin;$env:PATH"
$env:PYTHONHOME = $pythonRoot
$env:PYTHONPATH = $sitePackages
$env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
$env:OCIO = Join-Path $repoRoot 'OpenColorIO-Configs\blender\config.ocio'
$env:OFX_PLUGIN_PATH = if ([IO.Path]::IsPathRooted($OfxPluginDirectory)) { $OfxPluginDirectory } else { Join-Path $repoRoot $OfxPluginDirectory }
$env:FONTCONFIG_PATH = Join-Path $repoRoot 'build\vcpkg_installed\x64-windows\etc\fonts'
$env:FONTCONFIG_FILE = Join-Path $env:FONTCONFIG_PATH 'fonts.conf'
if (Test-Path -LiteralPath $ofxRuntime) {
    $env:MAGICK_CONFIGURE_PATH = Join-Path $ofxRuntime 'etc\ImageMagick-7'
    $coderDirectory = Get-ChildItem -LiteralPath (Join-Path $ofxRuntime 'lib') -Directory -Filter 'ImageMagick-*' | Select-Object -First 1
    if ($coderDirectory) {
        $env:MAGICK_CODER_MODULE_PATH = Join-Path $coderDirectory.FullName 'modules-Q16HDRI\coders'
        $env:MAGICK_FILTER_MODULE_PATH = Join-Path $coderDirectory.FullName 'modules-Q16HDRI\filters'
    }
}
if ($Program -in @('Tests', 'CTest', 'Smoke', 'RasterGuiSmoke', 'RasterIOOverlaySmoke', 'HeadlessCpuSmoke', 'MemoryFailureSmoke', 'MemoryPressureSmoke', 'DeviceLossSmoke', 'PerformanceRender', 'EngineVulkanSmoke', 'ShadertoyVulkanSmoke', 'OcioVulkanSmoke', 'GuiSmoke', 'MemoryGuiSmoke', 'ConcurrentMemoryGuiSmoke', 'DeviceLossGuiSmoke', 'ViewerPerformance', 'OfxSmoke', 'OfxGuiSmoke', 'OverlaySmoke') -and -not (Test-Path -LiteralPath "$env:OFX_PLUGIN_PATH\IO.ofx.bundle")) {
    & (Join-Path $PSScriptRoot 'build-windows-ofx.ps1') -PluginSets io
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath "$env:OFX_PLUGIN_PATH\IO.ofx.bundle")) {
        throw 'Could not build the current IO bundle. For a custom OfxPluginDirectory, install the source-built IO bundle there first.'
    }
}
$smokeResult = $null
$previousTestVulkanValidation = $env:NATRON_TEST_VULKAN_VALIDATION
if ($Program -eq 'Smoke') {
    $smokeResult = Join-Path $repoRoot 'build\qt6-smoke\result.json'
    $ProgramArguments = @('--no-settings', 'tools/qt6-smoke.py', '1')
} elseif ($Program -eq 'Scene3DSmoke') {
    $sceneDevice = if ($env:NATRON_VULKAN_DEVICE) { $env:NATRON_VULKAN_DEVICE } else { 'auto' }
    if ($env:NATRON_VULKAN_DISABLED -eq '1') { $sceneDevice += '-cpu-only' }
    $smokeResult = Join-Path $repoRoot "build/scene3d-smoke-$sceneDevice/result.json"
    $ProgramArguments = @('--no-settings', '--interpreter', 'tools/scene3d-smoke.py')
} elseif ($Program -eq 'Scene3DUsdSmoke') {
    $smokeResult = Join-Path $repoRoot ('build/scene3d-usd-smoke-' + $(if ($env:NATRON_VULKAN_DISABLED -eq '1') { 'cpu' } else { 'gpu' }) + '/result.json')
    $ProgramArguments = @('--no-settings', '--interpreter', 'tools/scene3d-usd-smoke.py')
} elseif ($Program -eq 'Scene3DStageOutlineGuiSmoke') {
    $smokeResult = Join-Path $repoRoot $(if($env:NATRON_STAGE_OUTLINE_SCENE){'build/scene3d-stage-outline-user/gui-result.json'}else{'build/scene3d-stage-outline/gui-result.json'})
    $ProgramArguments = @('tools/scene3d-stage-outline-gui-smoke.py')
} elseif ($Program -eq 'Scene3DHdNvpDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-hdnvp-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-hdnvp-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DUsdDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-usd-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-usd-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DLightingSmoke') {
    $smokeResult = Join-Path $repoRoot ('build/scene3d-lighting-smoke-' + $(if ($env:NATRON_VULKAN_DISABLED -eq '1') { 'cpu' } else { 'gpu' }) + '/result.json')
    $ProgramArguments = @('--no-settings', '--interpreter', 'tools/scene3d-lighting-smoke.py')
} elseif ($Program -eq 'Scene3DEnvironmentSmoke') {
    $smokeResult = Join-Path $repoRoot ('build/scene3d-environment-smoke-' + $(if ($env:NATRON_VULKAN_DISABLED -eq '1') { 'cpu' } else { 'gpu' }) + '/result.json')
    $ProgramArguments = @('--no-settings', '--interpreter', 'tools/scene3d-environment-smoke.py')
} elseif ($Program -eq 'Scene3DEnvironmentDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-environment-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-environment-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DLightingDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-lighting-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-lighting-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DFbxCameraDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-fbx-camera-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-fbx-camera-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DFbxAnimationDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-fbx-animation-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-fbx-animation-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DFbxDemoGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-fbx-demo/gui-result.json'
    $ProgramArguments = @('tools/scene3d-fbx-demo-gui-smoke.py')
} elseif ($Program -eq 'Scene3DGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/scene3d-gui-result.json'
    $ProgramArguments = @('tools/scene3d-gui-smoke.py')
} elseif ($Program -eq 'RasterGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\raster-gui-smoke\result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/raster-gui-smoke.py')
} elseif ($Program -eq 'RasterIOOverlaySmoke') {
    $smokeResult = Join-Path $repoRoot 'build\raster-io-overlay-gui-smoke\result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/io-overlay-gui-smoke.py')
} elseif ($Program -eq 'HeadlessCpuSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\headless-cpu-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/headless-cpu-smoke.py')
} elseif ($Program -eq 'MemoryFailureSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-memory-failure-smoke/result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/vulkan-memory-failure-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'MemoryPressureSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-memory-pressure-smoke/result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/vulkan-memory-pressure-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'DeviceLossSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-device-loss-smoke/result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/vulkan-device-loss-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'PerformanceRender') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-performance-render/result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/vulkan-performance-render.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = if ($env:NATRON_BENCHMARK_VALIDATION -eq '0') { '0' } else { '1' }
    $env:NATRON_TEST_VULKAN_VALIDATION = $env:NATRON_VULKAN_VALIDATION
} elseif ($Program -eq 'ViewerPerformance') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-viewer-performance/result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/vulkan-viewer-performance.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = if ($env:NATRON_BENCHMARK_VALIDATION -eq '0') { '0' } else { '1' }
    $env:NATRON_TEST_VULKAN_VALIDATION = $env:NATRON_VULKAN_VALIDATION
} elseif ($Program -eq 'DeviceLossGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-device-loss-gui-smoke/result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/vulkan-device-loss-gui-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'EngineVulkanSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\engine-vulkan-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/engine-vulkan-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'ShadertoyVulkanSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\shadertoy-vulkan-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/shadertoy-vulkan-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'OcioVulkanSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ocio-vulkan-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/ocio-vulkan-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
} elseif ($Program -eq 'ImageStorageSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\image-storage-smoke\result.json'
    $selection = if ($env:NATRON_VULKAN_DEVICE) { $env:NATRON_VULKAN_DEVICE } else { '-1' }
    $ProgramArguments = @('--gpu', $selection)
} elseif ($Program -eq 'ImageConvertSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\image-convert-smoke\result.json'
    $selection = if ($env:NATRON_VULKAN_DEVICE) { $env:NATRON_VULKAN_DEVICE } else { '-1' }
    $ProgramArguments = @('--gpu', $selection)
} elseif ($Program -eq 'ClipBorrowSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\clip-borrow-smoke\result.json'
    $ProgramArguments = @()
} elseif ($Program -eq 'VulkanProxySmoke') {
    $smokeResult = Join-Path $repoRoot 'build\vulkan-proxy-gui-smoke\result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/vulkan-proxy-gui-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'ReaderEngineVulkanSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\reader-vulkan-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/reader-vulkan-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
} elseif ($Program -eq 'WriterEngineVulkanSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\writer-vulkan-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', '--interpreter', 'tools/writer-vulkan-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
} elseif ($Program -eq 'ReaderVulkanSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\reader-vulkan-gui-smoke\result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/reader-vulkan-gui-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'MemoryGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-memory-gui-smoke/result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/vulkan-memory-gui-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
} elseif ($Program -eq 'ConcurrentMemoryGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build/vulkan-concurrent-memory-gui-smoke/result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/vulkan-concurrent-memory-gui-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build/ofx-tests')
} elseif ($Program -eq 'GuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\gui-qt6-result.json'
    $ProgramArguments = @('tools/qt6-gui-smoke.py')
} elseif ($Program -eq 'OfxSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ofx-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', 'tools/ofx-smoke.py', '1')
} elseif ($Program -eq 'OfxGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ofx-gui-result.json'
    $ProgramArguments = @('tools/ofx-gui-smoke.py')
} elseif ($Program -eq 'OverlaySmoke') {
    $smokeResult = Join-Path $repoRoot 'build\overlay-gui-result.json'
    $ProgramArguments = @('tools/overlay-gui-smoke.py')
} elseif ($Program -eq 'PluginOverlaySmoke') {
    $smokeResult = Join-Path $repoRoot 'build\plugin-overlay-gui-smoke\result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/plugin-overlay-gui-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'IOOverlaySmoke') {
    $smokeResult = Join-Path $repoRoot 'build\io-overlay-gui-smoke\result.json'
    $ProgramArguments = @('--clear-openfx-cache', 'tools/io-overlay-gui-smoke.py')
    $env:NATRON_VULKAN_VALIDATION = '1'
} elseif ($Program -eq 'DrawSuiteSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ofx-drawsuite-gui-result.json'
    $ProgramArguments = @('tools/ofx-drawsuite-gui-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
} elseif ($Program -eq 'HistogramSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\histogram-gui-result.json'
    $ProgramArguments = @('tools/histogram-gui-smoke.py')
} elseif ($Program -eq 'ParamDrawSuiteSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ofx-param-drawsuite-gui-result.json'
    $ProgramArguments = @('tools/ofx-param-drawsuite-gui-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
} elseif ($Program -eq 'CurveSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\curve-gui-result.json'
    $ProgramArguments = @('tools/curve-gui-smoke.py')
    $env:OFX_PLUGIN_PATH += ';' + (Join-Path $repoRoot 'build\ofx-tests')
} elseif ($Program -eq 'DopeSheetSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\dope-sheet-gui-result.json'
    $ProgramArguments = @('tools/dope-sheet-gui-smoke.py')
} elseif ($Program -eq 'TimelineSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\timeline-gui-result.json'
    $ProgramArguments = @('tools/timeline-gui-smoke.py')
}
if ($smokeResult -and (Test-Path -LiteralPath $smokeResult)) {
    Remove-Item -LiteralPath $smokeResult
}
if ($Program -eq 'CTest') {
    $executable = (Get-Command ctest -ErrorAction Stop).Source
    $ProgramArguments = @('--test-dir', $BuildDirectory, '--output-on-failure') + $ProgramArguments
} else {
    $binary = @{ RasterGuiSmoke = 'Natron'; RasterIOOverlaySmoke = 'Natron'; HeadlessCpuSmoke = 'NatronRenderer'; Smoke = 'NatronRenderer'; MemoryFailureSmoke = 'NatronRenderer'; MemoryPressureSmoke = 'NatronRenderer'; DeviceLossSmoke = 'NatronRenderer'; PerformanceRender = 'NatronRenderer'; EngineVulkanSmoke = 'NatronRenderer'; ShadertoyVulkanSmoke = 'NatronRenderer'; OcioVulkanSmoke = 'NatronRenderer'; VulkanProxySmoke = 'Natron'; ReaderVulkanSmoke = 'Natron'; ReaderEngineVulkanSmoke = 'NatronRenderer'; WriterEngineVulkanSmoke = 'NatronRenderer'; MemoryGuiSmoke = 'Natron'; ConcurrentMemoryGuiSmoke = 'Natron'; DeviceLossGuiSmoke = 'Natron'; ViewerPerformance = 'Natron'; GuiSmoke = 'Natron'; OfxSmoke = 'NatronRenderer'; OfxGuiSmoke = 'Natron'; OverlaySmoke = 'Natron'; IOOverlaySmoke = 'Natron'; PluginOverlaySmoke = 'Natron'; DrawSuiteSmoke = 'Natron'; ParamDrawSuiteSmoke = 'Natron'; CurveSmoke = 'Natron'; TimelineSmoke = 'Natron'; DopeSheetSmoke = 'Natron'; HistogramSmoke = 'Natron'; Scene3DGuiSmoke = 'Natron'; Scene3DFbxDemoGuiSmoke = 'Natron'; Scene3DFbxAnimationDemoGuiSmoke = 'Natron'; Scene3DFbxCameraDemoGuiSmoke = 'Natron'; Scene3DSmoke = 'NatronRenderer'; Scene3DLightingSmoke = 'NatronRenderer'; Scene3DEnvironmentSmoke = 'NatronRenderer'; Scene3DEnvironmentDemoGuiSmoke = 'Natron'; Scene3DLightingDemoGuiSmoke = 'Natron'; Scene3DUsdSmoke = 'NatronRenderer'; Scene3DUsdDemoGuiSmoke = 'Natron'; Scene3DHdNvpDemoGuiSmoke = 'Natron'; Scene3DStageOutlineGuiSmoke = 'Natron' }[$Program]
    if (-not $binary) { $binary = $Program }
    if ($Program -eq 'ImageStorageSmoke') { $binary = 'NatronVulkanImageStorage' }
    if ($Program -eq 'ImageConvertSmoke') { $binary = 'NatronVulkanImageConvert' }
    if ($Program -eq 'ClipBorrowSmoke') { $binary = 'NatronVulkanClipBorrow' }
    $module = @{ Natron = 'App'; NatronRenderer = 'Renderer'; NatronPython = 'PythonBin'; Tests = 'Tests'; NatronVulkanImageStorage = 'Tests'; NatronVulkanImageConvert = 'Tests'; NatronVulkanClipBorrow = 'Tests'; NatronUsdSceneBridge = 'Tests'; NatronCameraSolve = 'Tests'; NatronCameraSolveUsd = 'Tests'; NatronCompositing = 'Tests'; NatronReflection = 'Tests'; NatronFeatureTracking = 'Tests' }[$binary]
    $binaryName = if ($binary -eq 'NatronPython') { 'natron-python' } else { $binary }
    $executable = Join-Path $BuildDirectory "$module\$binaryName.exe"
}
if (-not (Test-Path -LiteralPath $executable)) { throw "Build executable not found: $executable" }
# USD plugins must resolve to the same applocal DLLs as the executable.
$usdExecutableDirectory = if ($Program -eq 'CTest') { Join-Path $BuildDirectory 'Tests' } else { Split-Path -Parent $executable }
$env:PXR_PLUGINPATH_NAME = Join-Path (Join-Path $repoRoot $usdExecutableDirectory) 'usd/*/resources/'

if ($Program -in @('RasterGuiSmoke', 'RasterIOOverlaySmoke', 'HeadlessCpuSmoke', 'MemoryFailureSmoke', 'MemoryPressureSmoke', 'DeviceLossSmoke', 'PerformanceRender', 'EngineVulkanSmoke', 'ShadertoyVulkanSmoke', 'OcioVulkanSmoke', 'ImageStorageSmoke', 'ImageConvertSmoke', 'ClipBorrowSmoke', 'VulkanProxySmoke', 'ReaderVulkanSmoke', 'ReaderEngineVulkanSmoke', 'WriterEngineVulkanSmoke', 'GuiSmoke', 'MemoryGuiSmoke', 'ConcurrentMemoryGuiSmoke', 'DeviceLossGuiSmoke', 'ViewerPerformance', 'OfxGuiSmoke', 'OverlaySmoke', 'IOOverlaySmoke', 'PluginOverlaySmoke', 'DrawSuiteSmoke', 'ParamDrawSuiteSmoke', 'CurveSmoke', 'DopeSheetSmoke', 'TimelineSmoke', 'HistogramSmoke', 'Scene3DGuiSmoke', 'Scene3DFbxDemoGuiSmoke', 'Scene3DFbxAnimationDemoGuiSmoke', 'Scene3DFbxCameraDemoGuiSmoke', 'Scene3DSmoke', 'Scene3DLightingSmoke', 'Scene3DLightingDemoGuiSmoke', 'Scene3DEnvironmentSmoke', 'Scene3DEnvironmentDemoGuiSmoke', 'Scene3DHdNvpDemoGuiSmoke', 'Scene3DUsdDemoGuiSmoke', 'Scene3DUsdSmoke', 'Scene3DStageOutlineGuiSmoke')) {
    $testProfile = Join-Path $repoRoot ("build\test-profiles\" + $Program + '-' + [Guid]::NewGuid().ToString('N'))
    $settingsDirectory = Join-Path $testProfile 'settings\INRIA'
    New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
    # This file belongs exclusively to the new test process. User startup
    # choices, appearance, shortcuts and autosave recovery are never edited.
    $testSettings = @('[General]', 'checkForUpdates=false', 'startupCheckOCIO=false', "texturesBitDepth=$GuiTextureDepth")
    Set-Content -LiteralPath (Join-Path $settingsDirectory 'Natron.ini') -Value $testSettings -Encoding utf8
    foreach ($directory in @('data', 'cache', 'temp')) {
        New-Item -ItemType Directory -Path (Join-Path $testProfile $directory) -Force | Out-Null
    }
    $previousViewerBackend = $env:NATRON_TEST_VIEWER_BACKEND
    $previousProfile = $env:NATRON_TEST_PROFILE
    $previousMarker = $env:NATRON_TEST_START_MARKER
    $previousLayerPath = $env:VK_LAYER_PATH
    $previousObsCapture = $env:DISABLE_VULKAN_OBS_CAPTURE
    $process = $null
    try {
        if ($Program -eq 'RasterIOOverlaySmoke') { $env:NATRON_TEST_VIEWER_BACKEND = 'Raster' }
        if ($Program -eq 'ViewerPerformance') {
            $env:NATRON_TEST_VIEWER_BACKEND = if ($env:NATRON_BENCHMARK_GUI_MODE -eq 'raster') { 'Raster' } else { $null }
        }
        $env:VK_LAYER_PATH = Join-Path $VulkanSdkRoot 'Bin'
        # OBS's implicit capture layer initializes unrelated graphics APIs.
        # Isolate this test process without changing OBS or global settings.
        $env:DISABLE_VULKAN_OBS_CAPTURE = '1'
        $env:NATRON_TEST_PROFILE = $testProfile
        $env:NATRON_TEST_START_MARKER = Join-Path $testProfile 'started.txt'
        $launch = @{
            FilePath = (Resolve-Path -LiteralPath $executable).Path
            ArgumentList = $ProgramArguments
            WorkingDirectory = $repoRoot
            PassThru = $true
            WindowStyle = 'Hidden'
            RedirectStandardOutput = (Join-Path $testProfile 'stdout.log')
            RedirectStandardError = (Join-Path $testProfile 'stderr.log')
        }
        if ($Program -in @('RasterGuiSmoke', 'RasterIOOverlaySmoke', 'HeadlessCpuSmoke', 'MemoryFailureSmoke', 'MemoryPressureSmoke', 'DeviceLossSmoke', 'PerformanceRender', 'EngineVulkanSmoke', 'ShadertoyVulkanSmoke', 'OcioVulkanSmoke', 'ReaderEngineVulkanSmoke', 'WriterEngineVulkanSmoke', 'Scene3DSmoke', 'Scene3DLightingSmoke', 'Scene3DEnvironmentSmoke', 'Scene3DUsdSmoke')) {
            # EOF closes the interpreter after the script; background autorun
            # would render saved Write nodes again and overwrite the fixtures.
            $interpreterInput = Join-Path $testProfile 'stdin.txt'
            [IO.File]::WriteAllText($interpreterInput, "assert app is app1`nprint('NATRON_INTERPRETER_EOF_OK')`n")
            $launch.RedirectStandardInput = $interpreterInput
        }
        $process = Start-Process @launch
        $deadline = [DateTime]::UtcNow.AddSeconds($GuiTimeoutSeconds)
        while (-not $process.WaitForExit(500)) {
            if ([DateTime]::UtcNow -ge $deadline) {
                $phase = if (Test-Path -LiteralPath $smokeResult) { 'shutdown' } elseif (Test-Path -LiteralPath $env:NATRON_TEST_START_MARKER) { 'run' } else { 'startup' }
                $process.Refresh()
                $diagnostic = @{
                    program = $Program
                    pid = $process.Id
                    phase = $phase
                    windowTitle = $process.MainWindowTitle
                    timeoutSeconds = $GuiTimeoutSeconds
                }
                $diagnostic | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testProfile 'timeout.json') -Encoding utf8
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                if (-not $process.WaitForExit(5000)) { throw "Could not terminate test PID $($process.Id). Logs: $testProfile" }
                throw "$Program timed out during $phase. A confirmation dialog may be blocking it. Logs: $testProfile"
            }
        }
        $process.Refresh()
        if ($process.ExitCode -ne 0) {
            Get-Content -LiteralPath (Join-Path $testProfile 'stderr.log')
            throw "$Program exited with code $($process.ExitCode). Logs: $testProfile"
        }
        if ($Program -in @('ImageStorageSmoke', 'ImageConvertSmoke', 'ClipBorrowSmoke')) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $smokeResult) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $testProfile 'stdout.log') -Destination $smokeResult
        }
        $graphicsFailures = Select-String -LiteralPath (Join-Path $testProfile 'stderr.log') -Pattern $(if ($Program -in @('RasterGuiSmoke', 'RasterIOOverlaySmoke', 'HeadlessCpuSmoke')) { 'Vulkan validation error:|VUID-' } else { 'Vulkan validation error:|Vulkan runtime error:|Vulkan Viewer:' })
        if ($graphicsFailures) {
            Write-Output $graphicsFailures.Line
            throw "$Program reported Vulkan errors, including shutdown. Logs: $testProfile"
        }
        Write-Output "GUI smoke profile and logs: $testProfile"
    } finally {
        # Keep proof artifacts and logs, but release the closed test's large
        # disposable Viewer cache so repeated validation cannot fill the disk.
        if ($process -and $process.HasExited) {
            $viewerCache = Join-Path $testProfile 'cache\ViewerCache'
            if (Test-Path -LiteralPath $viewerCache) {
                foreach ($file in Get-ChildItem -LiteralPath $viewerCache -File -Filter 'CachePart*') {
                    if ($file.Name -match '^CachePart[0-9]+$') {
                        $absolute = (Resolve-Path -LiteralPath $file.FullName).Path
                        if (-not $absolute.StartsWith($testProfile + '\', [StringComparison]::OrdinalIgnoreCase)) {
                            throw 'Disposable cache path escaped its test profile.'
                        }
                        Remove-Item -LiteralPath $absolute
                    }
                }
            }
        }
        $env:NATRON_TEST_VIEWER_BACKEND = $previousViewerBackend
        $env:NATRON_TEST_PROFILE = $previousProfile
        $env:NATRON_TEST_START_MARKER = $previousMarker
        $env:VK_LAYER_PATH = $previousLayerPath
        $env:DISABLE_VULKAN_OBS_CAPTURE = $previousObsCapture
        $env:NATRON_TEST_VULKAN_VALIDATION = $previousTestVulkanValidation
    }
} else {
    $isHeadlessTest = $Program -in @('Tests', 'CTest', 'Smoke', 'OfxSmoke')
    $previousProfile = $env:NATRON_TEST_PROFILE
    if ($isHeadlessTest) {
        $testProfile = Join-Path $repoRoot ("build\test-profiles\" + $Program + '-' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $testProfile -Force | Out-Null
        $env:NATRON_TEST_PROFILE = $testProfile
    }
    try {
        & uv run --frozen -- $executable @ProgramArguments
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    } finally {
        if ($isHeadlessTest) { $env:NATRON_TEST_PROFILE = $previousProfile }
    }
}
if ($smokeResult) {
    if (-not (Test-Path -LiteralPath $smokeResult)) { throw 'Smoke test did not finish successfully. See the preceding Python diagnostics.' }
    $report = Get-Content -LiteralPath $smokeResult -Raw | ConvertFrom-Json
    if ($Program -in @('RasterGuiSmoke', 'RasterIOOverlaySmoke', 'HeadlessCpuSmoke', 'MemoryFailureSmoke', 'MemoryPressureSmoke', 'DeviceLossSmoke', 'PerformanceRender', 'EngineVulkanSmoke', 'ShadertoyVulkanSmoke', 'OcioVulkanSmoke', 'ImageStorageSmoke', 'ImageConvertSmoke', 'ClipBorrowSmoke', 'VulkanProxySmoke', 'ReaderVulkanSmoke', 'ReaderEngineVulkanSmoke', 'WriterEngineVulkanSmoke', 'GuiSmoke', 'MemoryGuiSmoke', 'ConcurrentMemoryGuiSmoke', 'DeviceLossGuiSmoke', 'ViewerPerformance', 'OfxSmoke', 'OfxGuiSmoke', 'OverlaySmoke', 'IOOverlaySmoke', 'PluginOverlaySmoke', 'DrawSuiteSmoke', 'ParamDrawSuiteSmoke', 'CurveSmoke', 'DopeSheetSmoke', 'TimelineSmoke', 'HistogramSmoke', 'Scene3DGuiSmoke', 'Scene3DFbxDemoGuiSmoke', 'Scene3DFbxAnimationDemoGuiSmoke', 'Scene3DFbxCameraDemoGuiSmoke', 'Scene3DSmoke', 'Scene3DLightingSmoke', 'Scene3DLightingDemoGuiSmoke', 'Scene3DEnvironmentSmoke', 'Scene3DEnvironmentDemoGuiSmoke', 'Scene3DHdNvpDemoGuiSmoke', 'Scene3DUsdDemoGuiSmoke', 'Scene3DUsdSmoke', 'Scene3DStageOutlineGuiSmoke') -and -not $report.ok) { throw "$Program failed." }
    Write-Output (Get-Content -LiteralPath $smokeResult -Raw)
}
exit $LASTEXITCODE
