[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build\shadertoy-vulkan-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
$proofFiles = @(
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-vulkan/GpuTests/NatronVulkanFragmentProgram.exe',
    'build/Plugins/Misc.ofx.bundle/Contents/Win64/Misc.ofx',
    'Global/OpenFX/NatronVulkanImageSuite.h', 'Global/OpenFX/NatronVulkanShaderSuite.h',
    'Global/OpenFX/NatronVulkanRenderSuite.h', 'Engine/OfxClipInstance.cpp', 'Engine/EffectInstanceRenderRoI.cpp',
    'Engine/EffectInstance.cpp', 'Engine/EffectInstance.h', 'Engine/ParallelRenderArgs.cpp',
    'Engine/OfxHost.cpp', 'Engine/OfxVulkanImageSuite.cpp', 'Engine/OfxVulkanImageSuite.h',
    'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h',
    'Engine/VulkanImageContextPool.cpp', 'Engine/VulkanImageContextPool.h',
    'Gpu/ImageOperations.cpp', 'Gpu/ImageOperations.h', 'Gpu/shaders/image-operations.comp',
    'Gpu/FragmentProgram.cpp', 'Gpu/FragmentProgram.h', 'Gpu/VulkanDevice.cpp', 'Gpu/VulkanDevice.h',
    'Gpu/CMakeLists.txt', 'Gpu/tests/FragmentProgram.cpp', 'Gpu/tests/CMakeLists.txt',
    'build-configs/windows-ofx/CMakeLists.txt', 'build-configs/windows-ofx/native/ShadertoyVulkan.cpp',
    'build-configs/windows-ofx/native/MiscVulkan.def', 'build-configs/windows-ofx/openfx-misc.patch',
    'build-configs/windows-ofx/openfx-misc-supportext-vulkan.patch', 'build-configs/windows-ofx/native/NatronOverlayInteract.h',
    'Engine/AppManager.cpp', 'Engine/AppManagerPrivate.cpp', 'Engine/AppManagerPrivate.h', 'Engine/Settings.cpp',
    'build/ofx-src/openfx-misc/Shadertoy/Shadertoy.cpp', 'build/ofx-src/openfx-misc/Shadertoy/Shadertoy.h',
    'build/Plugins/Misc.ofx.bundle/Contents/Resources/presets/default/gaussianblur.frag.glsl',
    'tools/build-windows-ofx.ps1', 'tools/run-windows-qt6.ps1', 'tools/shadertoy-vulkan-smoke.py',
    'tools/test-windows-vulkan.ps1', 'tools/test-windows-vulkan-shadertoy.ps1', 'tools/test-settings-fingerprint.py', 'tools/windows-module-audit.py'
)
$hashes = @{}
$proofFiles += @('build-configs/windows-ofx/sources.json', 'tools/build-windows-ofx-codecs.ps1', 'tools/install-ofx-runtime.ps1', 'tools/test-windows-ofx-codecs.py')
$proofFiles += Get-ChildItem -LiteralPath 'build/ofx-runtime/bin' -File -Filter '*.dll' |
    ForEach-Object { [IO.Path]::GetRelativePath($repoRoot, $_.FullName).Replace('\', '/') }
foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferencesBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$cases = @()
function Save-Proof([bool]$complete) {
    $path = if ($complete) { $reportPath } else { Join-Path $ReportDirectory 'report.partial.json' }
    @{passed=$complete;fileSha256=$hashes;preferencesSha256=$preferencesBefore;cases=$cases;
      finishedAt=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$path.tmp" -Encoding utf8
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
}
try {
    Save-Proof $false
    & tools/test-windows-vulkan.ps1 -Tests FragmentProgram -GpuIndices $GpuIndices -ReportDirectory (Join-Path $ReportDirectory 'fragment') -TimeoutSeconds $TimeoutSeconds
    if ($LASTEXITCODE -ne 0) { throw 'Native fragment tests failed.' }
    foreach ($index in $GpuIndices) {
        if ($index -lt 0) { throw 'GPU index must be nonnegative.' }
        $nativePath = Join-Path $ReportDirectory "fragment/gpu$index/FragmentProgram.stdout.log"
        $native = Get-Content -LiteralPath $nativePath -Raw | ConvertFrom-Json
        if (-not $native.passed -or $native.referenceCases -ne 40 -or $native.invalidCases -ne 7 -or
            $native.validationErrors -ne 0 -or $native.openGLLoaded) { throw 'Invalid native fragment proof.' }
        $directory = Join-Path $ReportDirectory "gpu$index"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $env:NATRON_VULKAN_DEVICE = [string]$index
        & tools/run-windows-qt6.ps1 -Program ShadertoyVulkanSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
        if ($LASTEXITCODE -ne 0) { throw "Shipped Shadertoy GPU $index failed." }
        $resultPath = Join-Path $repoRoot 'build/shadertoy-vulkan-smoke/result.json'
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -AsHashtable
        if (-not $result.ok -or $result.opengl_loaded -or $result.device -ne $native.device -or $result.gpu_index -ne [string]$index -or
            $result.validation_errors -ne 0 -or -not $result.validation_enabled -or $result.cases.Count -ne 19 -or
            -not $result.project_roundtrip -or $result.native_renders -lt 19 -or $result.native_clip_renders -ne $result.native_renders) { throw 'Invalid shipped Shadertoy proof.' }
        foreach ($case in $result.cases) {
            if ($case.native_renders -lt 1 -or $case.pixels_compared -ne 12467) { throw 'Incomplete shipped shader pixel comparison.' }
            if ($case.native_clip_renders -ne $case.native_renders -or $case.boundary_transfers.Count -ne 2 -or
                $case.boundary_transfers[1] -ne 1) { throw 'Shadertoy output incurred unexpected CPU transfers.' }
        }
        $stdout = Get-Content -LiteralPath (Join-Path $result.profile 'stdout.log') -Raw
        if (-not $stdout.Contains('NATRON_INTERPRETER_EOF_OK')) { throw 'Shadertoy interpreter did not finish its input/EOF test.' }
        foreach ($file in $result.artifact_sha256.Keys) {
            $artifact = Join-Path 'build/shadertoy-vulkan-smoke' $file
            if ((Get-FileHash -LiteralPath $artifact).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$file]) {
                throw "Shadertoy artifact changed after exit: $file"
            }
            Copy-Item -LiteralPath $artifact -Destination (Join-Path $directory $file)
        }
        Copy-Item -LiteralPath $resultPath -Destination (Join-Path $directory 'result.json')
        foreach ($file in @('stdout.log', 'stderr.log', 'stdin.txt')) {
            Copy-Item -LiteralPath (Join-Path $result.profile $file) -Destination (Join-Path $directory $file)
        }
        $artifacts = @{}
        Get-ChildItem -LiteralPath $directory -File | ForEach-Object { $artifacts[$_.Name] = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
        $cases += @{gpu=$index;device=$result.device;passed=$true;fragmentCases=$native.referenceCases;
            nativeMaxError=$native.maxAbsoluteError;shippedShaderCases=$result.cases.Count;nativeRenders=$result.native_renders;nativeClipRenders=$result.native_clip_renders;
            interpreterInputAndEOF=$true;directory=$directory;fileSha256=$artifacts;
            fragmentLogSha256=(Get-FileHash -LiteralPath $nativePath).Hash.ToLowerInvariant()}
        Save-Proof $false
        Write-Output "Shipped Shadertoy Vulkan GPU $index passed: $($result.device)."
    }
    foreach ($file in $proofFiles) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Shader source or binary changed: $file" }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferencesBefore) { throw 'Preferences changed during shader testing.' }
    Save-Proof $true
} finally { $env:NATRON_VULKAN_DEVICE = $previousGpu }
