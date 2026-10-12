[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build\ocio-vulkan-tests',
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
    'build/windows-vulkan/GpuTests/NatronVulkanFragmentLut.exe',
    'build/windows-vulkan/GpuTests/NatronVulkanFragmentProgram.exe',
    'build/windows-vulkan/GpuTests/NatronVulkanViewer.exe',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'build/ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64/OcioVulkanSuiteSmoke.ofx',
    'Global/OpenFX/NatronVulkanImageSuite.h', 'Global/OpenFX/NatronVulkanShaderSuite.h',
    'Global/OpenFX/NatronVulkanRenderSuite.h', 'Engine/OfxClipInstance.cpp', 'Engine/EffectInstanceRenderRoI.cpp',
    'Engine/EffectInstance.cpp', 'Engine/EffectInstance.h', 'Engine/ParallelRenderArgs.cpp',
    'Engine/OfxHost.cpp', 'Engine/OfxVulkanImageSuite.cpp', 'Engine/OfxVulkanImageSuite.h',
    'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h',
    'Engine/VulkanImageContextPool.cpp', 'Engine/VulkanImageContextPool.h',
    'Gpu/ImageOperations.cpp', 'Gpu/ImageOperations.h', 'Gpu/shaders/image-operations.comp',
    'Gpu/FragmentProgram.cpp', 'Gpu/FragmentProgram.h', 'Gpu/LutTexture.cpp', 'Gpu/LutTexture.h',
    'Gpu/VulkanDevice.cpp', 'Gpu/VulkanDevice.h', 'Gpu/Qt/VulkanCanvas.cpp', 'Gpu/Qt/VulkanCanvas.h',
    'Gpu/tests/Viewer.cpp',
    'Gui/ViewerVulkan.cpp', 'Gui/ViewerGL.cpp', 'Gui/ViewerGLPrivate.h',
    'Gpu/CMakeLists.txt', 'Gpu/tests/FragmentLut.cpp', 'Gpu/tests/CMakeLists.txt',
    'Tests/OcioVulkanSuitePlugin.cpp', 'Tests/VulkanImageSuitePlugin.cpp',
    'build-configs/windows-ofx/CMakeLists.txt', 'build-configs/windows-ofx/sources.json',
    'build-configs/windows-ofx/native/OcioVulkan.cpp', 'build-configs/windows-ofx/native/OcioVulkan.h',
    'build-configs/windows-ofx/native/OcioVulkanAdapter.cpp', 'build-configs/windows-ofx/native/OcioVulkanAdapter.h',
    'build/windows-ofx/io/OcioVulkanFailureTest.exe', 'build-configs/windows-ofx/native/tests/OcioFailures.cpp',
    'build-configs/windows-ofx/native/IOVulkan.def', 'build-configs/windows-ofx/openfx-io.patch',
    'build-configs/windows-ofx/native/NatronOverlayInteract.h', 'build-configs/windows-ofx/openfx-io-supportext-vulkan.patch',
    'Gui/Gui15.cpp', 'Gui/GuiPrivate.h',
    'build/ofx-src/openfx-io/CMakeLists.txt', 'build/ofx-src/openfx-io/IOSupport/GenericOCIO.cpp',
    'build/ofx-src/openfx-io/IOSupport/GenericOCIO.h', 'build/ofx-src/openfx-io/FFmpeg/WriteFFmpeg.cpp',
    'tools/build-windows-ofx.ps1', 'tools/install-ofx-runtime.ps1', 'tools/run-windows-qt6.ps1',
    'tools/ocio-vulkan-smoke.py', 'tools/test-windows-vulkan.ps1', 'tools/test-windows-vulkan-ocio.ps1',
    'tools/test-settings-fingerprint.py'
)
$proofFiles += Get-ChildItem -LiteralPath 'build/ofx-src/openfx-io/OCIO' -File -Filter '*.cpp' |
    ForEach-Object { [IO.Path]::GetRelativePath($repoRoot, $_.FullName).Replace('\', '/') }
$proofFiles += Get-ChildItem -LiteralPath 'build/ofx-runtime/bin' -File -Filter '*.dll' |
    ForEach-Object { [IO.Path]::GetRelativePath($repoRoot, $_.FullName).Replace('\', '/') }
$hashes = @{}
foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferencesBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$cases = @()
function Save-Proof([bool]$complete) {
    $path = if ($complete) { $reportPath } else { Join-Path $ReportDirectory 'report.partial.json' }
    @{passed=$complete;fileSha256=$hashes;preferencesSha256=$preferencesBefore;cases=$cases;callbackFailureTransactions=$callbackFailures;
      finishedAt=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$path.tmp" -Encoding utf8
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
}
try {
    $previousPath = $env:PATH
    try {
        $env:PATH = (Join-Path $repoRoot 'build/ofx-runtime/bin') + ';' + $env:PATH
        $failureOutput = & build/windows-ofx/io/OcioVulkanFailureTest.exe
        if ($LASTEXITCODE -ne 0) { throw 'OCIO callback fault injection failed.' }
        $callbackFailures = $failureOutput | ConvertFrom-Json
        if (-not $callbackFailures.passed -or $callbackFailures.cases -ne 88 -or
            $callbackFailures.transient_retries -ne 84 -or $callbackFailures.gpu_required) {
            throw 'Incomplete OCIO callback failure evidence.'
        }
        $failureOutput | Set-Content -LiteralPath (Join-Path $ReportDirectory 'callback-failures.json') -Encoding utf8
    } finally {
        $env:PATH = $previousPath
    }
    Save-Proof $false
    & tools/test-windows-vulkan.ps1 -Tests FragmentLut,FragmentProgram,Viewer -GpuIndices $GpuIndices -ReportDirectory (Join-Path $ReportDirectory 'native') -TimeoutSeconds $TimeoutSeconds
    if ($LASTEXITCODE -ne 0) { throw 'Native shader/LUT tests failed.' }
    foreach ($index in $GpuIndices) {
        if ($index -lt 0) { throw 'GPU index must be nonnegative.' }
        $native = @{}
        foreach ($test in @('FragmentLut', 'FragmentProgram')) {
            $path = Join-Path $ReportDirectory "native/gpu$index/$test.stdout.log"
            $result = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            $expectedCases = if ($test -eq 'FragmentLut') { 19 } else { 40 }
            $expectedInvalid = if ($test -eq 'FragmentLut') { 9 } else { 7 }
            if (-not $result.passed -or $result.referenceCases -ne $expectedCases -or $result.invalidCases -ne $expectedInvalid -or
                $result.validationErrors -ne 0 -or $result.openGLLoaded) { throw "Invalid native $test proof." }
            $native[$test] = @{result=$result;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}
        }
        $viewerPath = Join-Path $ReportDirectory "native/gpu$index/Viewer.stdout.log"
        $viewer = Get-Content -LiteralPath $viewerPath -Raw | ConvertFrom-Json
        if (-not $viewer.passed -or $viewer.validationErrors -ne 0 -or -not $viewer.headlessBeforePresentation -or
            $viewer.headlessWorkers -ne 4 -or $viewer.headlessTransfers -ne 16 -or $viewer.headlessValidationErrors -ne 0) {
            throw 'Invalid concurrent headless/presentation proof.'
        }
        $native.Viewer = @{result=$viewer;sha256=(Get-FileHash -LiteralPath $viewerPath).Hash.ToLowerInvariant()}
        $directory = Join-Path $ReportDirectory "gpu$index"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $env:NATRON_VULKAN_DEVICE = [string]$index
        & tools/run-windows-qt6.ps1 -Program OcioVulkanSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
        if ($LASTEXITCODE -ne 0) { throw "Shipped OCIO GPU $index failed." }
        $resultPath = Join-Path $repoRoot 'build/ocio-vulkan-smoke/result.json'
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -AsHashtable
        $reference = $result.ocio_reference
        if (-not $result.ok -or $result.device -ne $native.FragmentLut.result.device -or $result.gpu_index -ne [string]$index -or
            $result.validation_errors -ne 0 -or -not $result.validation_enabled -or $result.cases.Count -ne 22 -or
            -not $result.project_roundtrip -or $result.native_renders -lt 25 -or $result.native_clip_renders -ne $result.native_renders) { throw 'Invalid shipped OCIO proof.' }
        if ($result.native_chain_length -ne 2 -or -not $result.native_cache_reuse -or
            ($result.cache_native_renders | ConvertTo-Json -Compress) -ne '[2,0]' -or
            ($result.cache_boundary_transfers | ConvertTo-Json -Compress) -ne '[[1,1],[0,1]]') { throw 'Incomplete shipped OCIO native chain/cache proof.' }
        if (-not $reference.passed -or $reference.referenceCases -ne 64 -or $reference.processors -ne 15 -or
            $reference.hostWorkers -ne 4 -or $reference.validationErrors -ne 0 -or -not $reference.signedPaddedRows -or
            $reference.textures1D -lt 1 -or $reference.textures2D -lt 1 -or $reference.textures3D -lt 7 -or
            $reference.uniformTypeMask -ne 31 -or $reference.device -ne $result.device) { throw 'Incomplete OCIO CPU reference coverage.' }
        foreach ($case in $result.cases) {
            if ($case.native_renders -lt 1 -or $case.pixels_compared -ne 1247) { throw 'Incomplete shipped OCIO pixel comparison.' }
            if ($case.native_clip_renders -ne $case.native_renders -or $case.boundary_transfers.Count -ne 2 -or
            $case.boundary_transfers[0] -ne (1 + $case.mask_uploads) -or $case.boundary_transfers[1] -ne (1 + $case.mask_downloads)) { throw 'OCIO native clips incurred unexpected CPU transfers.' }
        }
        $stdout = Get-Content -LiteralPath (Join-Path $result.profile 'stdout.log') -Raw
        if (-not $stdout.Contains('NATRON_INTERPRETER_EOF_OK')) { throw 'OCIO interpreter did not finish its input/EOF test.' }
        foreach ($file in $result.artifact_sha256.Keys) {
            $artifact = Join-Path 'build/ocio-vulkan-smoke' $file
            if ((Get-FileHash -LiteralPath $artifact).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$file]) {
                throw "OCIO artifact changed after exit: $file"
            }
            Copy-Item -LiteralPath $artifact -Destination (Join-Path $directory $file)
        }
        Copy-Item -LiteralPath $resultPath -Destination (Join-Path $directory 'result.json')
        foreach ($file in @('stdout.log', 'stderr.log', 'stdin.txt')) {
            Copy-Item -LiteralPath (Join-Path $result.profile $file) -Destination (Join-Path $directory $file)
        }
        $artifacts = @{}
        Get-ChildItem -LiteralPath $directory -File | ForEach-Object { $artifacts[$_.Name] = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
        $cases += @{gpu=$index;device=$result.device;passed=$true;native=$native;ocioReferenceCases=64;
            shippedOcioCases=$result.cases.Count;nativeRenders=$result.native_renders;nativeClipRenders=$result.native_clip_renders;
            interpreterInputAndEOF=$true;directory=$directory;fileSha256=$artifacts}
        Save-Proof $false
        Write-Output "Shipped OCIO Vulkan GPU $index passed: $($result.device)."
    }
    foreach ($file in $proofFiles) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Test source/binary changed: $file" }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferencesBefore) { throw 'Preferences changed during OCIO testing.' }
    Save-Proof $true
} finally { $env:NATRON_VULKAN_DEVICE = $previousGpu }
exit 0
