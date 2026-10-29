[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build\engine-vulkan-tests',
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$previousValidation = $env:NATRON_VULKAN_VALIDATION
$proofFiles = @(
    'build/ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/Win64/VulkanMemoryFailureSmoke.ofx',
    'Tests/VulkanMemoryFailurePlugin.cpp',
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Tests/Tests.exe', 'build/windows-vulkan/GpuTests/NatronVulkanImageOperations.exe',
    'build/windows-qt6/Tests/NatronVulkanImageStorage.exe',
    'build/windows-qt6/Tests/NatronVulkanClipBorrow.exe', 'Tests/VulkanClipBorrow.cpp',
    'build/ofx-tests/VulkanImageSuiteSmoke.ofx.bundle/Contents/Win64/VulkanImageSuiteSmoke.ofx',
    'Engine/AppManager.cpp', 'Engine/OfxHost.cpp', 'Engine/OfxVulkanImageSuite.cpp', 'Engine/OfxVulkanImageSuite.h', 'Engine/CMakeLists.txt',
    'Global/OpenFX/NatronVulkanImageSuite.h', 'Gpu/ImageOperations.cpp', 'Gpu/ImageOperations.h',
    'Global/OpenFX/NatronVulkanRenderSuite.h', 'Engine/OfxClipInstance.cpp',
    'Engine/OfxEffectInstance.cpp', 'Engine/OfxEffectInstance.h', 'Engine/OfxImageEffectInstance.cpp',
    'Engine/EffectInstance.cpp', 'Engine/EffectInstance.h', 'Engine/EffectInstanceRenderRoI.cpp',
    'Engine/ParallelRenderArgs.cpp', 'Engine/ParallelRenderArgs.h',
    'Global/OpenFX/NatronVulkanShaderSuite.h', 'Gpu/FragmentProgram.cpp', 'Gpu/FragmentProgram.h',
    'Global/Enums.h', 'Engine/Image.h', 'Engine/Image.cpp', 'Engine/ImageVulkan.cpp', 'Engine/ImageConvert.cpp', 'Engine/ImageMaskMix.cpp',
    'Engine/ImageCopyChannels.cpp', 'Engine/CacheEntry.h', 'Engine/Cache.h', 'Engine/NonKeyParams.h',
    'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h', 'Tests/VulkanImageStorage.cpp', 'Tests/Image_Test.cpp',
    'Engine/ImageKey.cpp', 'Engine/ImageKey.h', 'Engine/ImageSerialization.h',
    'Engine/VulkanImageContextPool.cpp', 'Engine/VulkanImageContextPool.h',
    'Gpu/VulkanDevice.cpp', 'Gpu/VulkanDevice.h', 'Gpu/shaders/image-operations.comp', 'Gpu/CMakeLists.txt',
    'Gpu/tests/ImageOperations.cpp', 'Gpu/tests/CMakeLists.txt', 'Tests/VulkanImageSuitePlugin.cpp', 'Tests/CMakeLists.txt',
    'tools/engine-vulkan-smoke.py', 'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan.ps1',
    'tools/test-windows-vulkan-engine.ps1', 'tools/test-settings-fingerprint.py'
)
$fileHashes = @{}
foreach ($file in $proofFiles) { $fileHashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferencesBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint user preferences.' }
$cases = @()
function Save-Proof([bool]$complete) {
    $path = if ($complete) { $reportPath } else { Join-Path $ReportDirectory 'report.partial.json' }
    @{passed=$complete;fileSha256=$fileHashes;preferencesSha256=$preferencesBefore;cases=$cases;
      finishedAt=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath "$path.tmp" -Encoding utf8
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
}
try {
    Save-Proof $false
    & tools/test-windows-vulkan.ps1 -GpuIndices $GpuIndices -Tests ImageOperations -ReportDirectory (Join-Path $ReportDirectory 'operations') -TimeoutSeconds $TimeoutSeconds
    if ($LASTEXITCODE -ne 0) { throw 'Native Vulkan image operations failed.' }
    foreach ($index in $GpuIndices) {
        if ($index -lt 0) { throw 'GPU index must be nonnegative.' }
        $env:NATRON_VULKAN_DEVICE = [string]$index
        $directory = Join-Path $ReportDirectory "gpu$index"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $operationsPath = Join-Path $ReportDirectory "operations/gpu$index/ImageOperations.stdout.log"
        $operations = Get-Content -LiteralPath $operationsPath -Raw | ConvertFrom-Json
        if (-not $operations.passed -or $operations.validationErrors -ne 0 -or $operations.openGLLoaded -or
            $operations.referenceCases -ne 666 -or $operations.invalidCases -ne 22) { throw 'Invalid native image operations proof.' }
        & tools/run-windows-qt6.ps1 -Program ImageStorageSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'storage-run.log')
        if ($LASTEXITCODE -ne 0) { throw "Native Engine image storage GPU $index failed." }
        $storage = Get-Content -LiteralPath build/image-storage-smoke/result.json -Raw | ConvertFrom-Json
        if (-not $storage.passed -or $storage.validationErrors -ne 0 -or $storage.device -ne $operations.device -or
            -not $storage.cacheLifetime -or -not $storage.contextIdentity -or $storage.reciprocalCopies -ne 64 -or $storage.referenceCases -ne 106 -or
            $storage.nanCases -ne 8 -or $storage.nanPixelTransfers -ne 0 -or $storage.nanDiagnosticBytes -ne 4 -or -not $storage.nanBitExact -or
            $storage.sharedRuntimeCases -ne 3 -or $storage.runtimeWorkers -ne 8 -or
            $storage.mipmapCases -ne 360 -or $storage.mipmapPixelTransfers -ne 0 -or
            $storage.mipmapMaxLevel -ne 5 -or -not $storage.mipmapBitmaps -or
            $storage.upscaleCases -ne 540 -or $storage.nativeUpscaleCases -ne 180 -or
            $storage.cpuUploadUpscaleCases -ne 180 -or $storage.cpuDownloadUpscaleCases -ne 180 -or
            $storage.nativeUpscalePixelTransfers -ne 0 -or -not $storage.upscalePadding) {
            throw "Invalid Engine image storage proof GPU $index."
        }
        Copy-Item -LiteralPath build/image-storage-smoke/result.json -Destination (Join-Path $directory 'storage-result.json')
        & tools/run-windows-qt6.ps1 -Program ClipBorrowSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'borrow-run.log')
        if ($LASTEXITCODE -ne 0) { throw "Native clip borrowing GPU $index failed." }
        $borrow = Get-Content -LiteralPath build/clip-borrow-smoke/result.json -Raw | ConvertFrom-Json
        if (-not $borrow.passed -or $borrow.device -ne $operations.device -or $borrow.validationErrors -ne 0 -or
            $borrow.nativePixelTransfers -ne 0 -or $borrow.referenceCases -lt 144 -or -not $borrow.borrowLifetime -or -not $borrow.roiPermissions -or
            -not $borrow.synchronizedCacheAllocation -or -not $borrow.normalizedDeviceSelection -or -not $borrow.sharedInstance -or -not $borrow.viewerSnapshot) {
            throw "Invalid native clip borrowing proof GPU $index."
        }
        Copy-Item -LiteralPath build/clip-borrow-smoke/result.json -Destination (Join-Path $directory 'borrow-result.json')
        & tools/run-windows-qt6.ps1 -Program EngineVulkanSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
        if ($LASTEXITCODE -ne 0) { throw "Engine Vulkan smoke GPU $index failed." }
        $result = Get-Content -LiteralPath build/engine-vulkan-smoke/result.json -Raw | ConvertFrom-Json
        if (-not $result.ok -or $result.gpu_index -ne [string]$index -or $result.device -ne $operations.device -or
            $result.validation_errors -ne 0 -or -not $result.validation_enabled -or $result.ofx_renders -lt 2 -or
            -not $result.native_clip_transport -or $result.native_chain_length -ne 2 -or $result.native_renders -lt 6 -or
            $result.native_pixel_transfers -ne 0 -or -not $result.native_cache_reuse -or -not $result.cpu_boundary_reference -or $result.max_byte_error -gt 1 -or
            $result.native_generator_pixels -ne 12467 -or $result.native_generator_renders -lt 1) {
            throw "Invalid Engine Vulkan proof GPU $index."
        }
        if ($result.boundary_color_management -ne 'linear_no_op' -or $result.graph_boundary_transfers.Count -ne 3) {
            throw "Missing graph boundary transfer proof GPU $index."
        }
        foreach ($transfers in $result.graph_boundary_transfers) {
            if ($transfers.Count -ne 2 -or $transfers[0] -ne 1 -or $transfers[1] -ne 1) { throw "Unexpected graph CPU transfer GPU $index." }
        }
        $stdout = Get-Content -LiteralPath (Join-Path $result.profile 'stdout.log') -Raw
        if (-not $stdout.Contains('NATRON_INTERPRETER_EOF_OK')) { throw 'Interpreter input/EOF test did not complete.' }
        $artifactProof = Get-Content -LiteralPath build/engine-vulkan-smoke/result.json -Raw | ConvertFrom-Json -AsHashtable
        foreach ($file in $artifactProof.artifact_sha256.Keys) {
            if ((Get-FileHash -LiteralPath (Join-Path 'build/engine-vulkan-smoke' $file)).Hash.ToLowerInvariant() -ne $artifactProof.artifact_sha256[$file]) {
                throw "Renderer modified a verified fixture after the test: $file"
            }
        }
        $artifactFiles = @('result.json') + @($artifactProof.artifact_sha256.Keys)
        foreach ($file in $artifactFiles) {
            Copy-Item -LiteralPath (Join-Path 'build/engine-vulkan-smoke' $file) -Destination (Join-Path $directory $file)
        }
        foreach ($file in @('stdout.log', 'stderr.log', 'stdin.txt')) {
            Copy-Item -LiteralPath (Join-Path $result.profile $file) -Destination (Join-Path $directory $file)
        }
        $artifacts = @{}
        Get-ChildItem -LiteralPath $directory -File | ForEach-Object { $artifacts[$_.Name] = (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant() }
        $cases += @{gpu=$index;device=$result.device;passed=$true;referenceCases=$operations.referenceCases;
            engineImageCases=$storage.referenceCases;engineImageMaxError=$storage.maxAbsoluteError;cacheLifetime=$storage.cacheLifetime;
            nanCases=$storage.nanCases;nanPixelTransfers=$storage.nanPixelTransfers;nanDiagnosticBytes=$storage.nanDiagnosticBytes;nanBitExact=$storage.nanBitExact;
            sharedRuntimeCases=$storage.sharedRuntimeCases;runtimeWorkers=$storage.runtimeWorkers;
            nativeMipmapCases=$storage.mipmapCases;nativeMipmapPixelTransfers=$storage.mipmapPixelTransfers;
            nativeMipmapMaxLevel=$storage.mipmapMaxLevel;nativeMipmapBitmaps=$storage.mipmapBitmaps;
            upscaleCases=$storage.upscaleCases;nativeUpscaleCases=$storage.nativeUpscaleCases;
            cpuUploadUpscaleCases=$storage.cpuUploadUpscaleCases;cpuDownloadUpscaleCases=$storage.cpuDownloadUpscaleCases;
            nativeUpscalePixelTransfers=$storage.nativeUpscalePixelTransfers;upscalePadding=$storage.upscalePadding;
            clipBorrowCases=$borrow.referenceCases;clipBorrowPixelTransfers=$borrow.nativePixelTransfers;
            nativeClipChainLength=$result.native_chain_length;nativeClipRenders=$result.native_renders;
            nativeClipPixelTransfers=$result.native_pixel_transfers;nativeClipCacheReuse=$result.native_cache_reuse;
            cpuBoundaryReference=$result.cpu_boundary_reference;maxByteError=$result.max_byte_error;
            graphBoundaryTransfers=$result.graph_boundary_transfers;boundaryColorManagement=$result.boundary_color_management;
            nativeMaxError=$operations.maxAbsoluteError;ofxMaxError=$result.max_absolute_error;ofxRenders=$result.ofx_renders;
            interpreterInputAndEOF=$true;
            directory=$directory;fileSha256=$artifacts;operationsLogSha256=(Get-FileHash -LiteralPath $operationsPath).Hash.ToLowerInvariant()}
        Save-Proof $false
        Write-Output "Engine Vulkan GPU $index passed: $($result.device)."
    }
    foreach ($file in $proofFiles) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $fileHashes[$file]) { throw "Build or test source changed: $file" }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferencesBefore) { throw 'User preferences changed during Engine Vulkan tests.' }
    Save-Proof $true
    Write-Output "Engine Vulkan proof: $reportPath"
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
    $env:NATRON_VULKAN_VALIDATION = $previousValidation
}
