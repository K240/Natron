[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('8u', '32f')][string[]]$TextureDepths = @('8u', '32f'),
    [string]$ReportDirectory = 'build\reader-vulkan-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
& uv run --frozen python tools/reader-vulkan-fixtures.py
if ($LASTEXITCODE -ne 0) { throw 'Could not prepare Reader fixtures.' }
$proofFiles = @(
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'build/ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64/OcioVulkanSuiteSmoke.ofx',
    'Engine/ReadNode.cpp', 'Engine/ReadNode.h', 'Engine/EffectInstance.cpp', 'Engine/EffectInstanceRenderRoI.cpp',
    'Engine/ImageVulkan.cpp', 'Engine/OfxVulkanImageSuite.cpp', 'Global/OpenFX/NatronVulkanRenderSuite.h',
    'Engine/ViewerInstance.cpp', 'Engine/UpdateViewerParams.h', 'Engine/OpenGLViewerI.h', 'Engine/OutputSchedulerThread.cpp',
    'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h', 'Engine/VulkanImageContextPool.cpp',
    'Gpu/VulkanDevice.cpp', 'Gpu/VulkanDevice.h', 'Gpu/Qt/VulkanCanvas.cpp', 'Gpu/Qt/VulkanCanvas.h', 'Gpu/shaders/viewer.comp',
    'Gpu/ImageOperations.cpp', 'Gpu/ImageOperations.h', 'Gpu/shaders/image-operations.comp',
    'Gpu/ViewerDisplayConversion.cpp', 'Gpu/ViewerDisplayConversion.h', 'Gpu/shaders/viewer-display.comp',
    'Gpu/ViewerRenderer.cpp', 'Gpu/ViewerRenderer.h',
    'Gpu/CMakeLists.txt', 'Engine/Lut.cpp', 'Engine/Lut.h',
    'Gui/ViewerVulkan.cpp', 'Gui/ViewerGL.cpp', 'Gui/ViewerGL.h', 'Gui/ViewerGLPrivate.h',
    'build-configs/windows-ofx/native/OcioVulkan.cpp', 'build-configs/windows-ofx/native/OcioVulkan.h',
    'build-configs/windows-ofx/native/OcioVulkanAdapter.cpp', 'build-configs/windows-ofx/native/OcioVulkanAdapter.h',
    'build-configs/windows-ofx/native/IOVulkan.def', 'build-configs/windows-ofx/openfx-io.patch',
    'build-configs/windows-ofx/native/NatronOverlayInteract.h', 'build-configs/windows-ofx/openfx-io-supportext-vulkan.patch',
    'Gui/Gui15.cpp', 'Gui/GuiPrivate.h',
    'Tests/OcioVulkanSuitePlugin.cpp', 'tools/run-windows-qt6.ps1', 'tools/reader-vulkan-smoke.py',
    'tools/reader-vulkan-gui-smoke.py', 'tools/reader-vulkan-fixtures.py', 'tools/test-windows-vulkan-reader.ps1',
    'tools/test-settings-fingerprint.py'
)
$hashes = @{}
foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferences = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$cases = @()
function Save-Artifacts($result, [string]$sourceDirectory, [string]$directory) {
    foreach ($name in $result.artifact_sha256.Keys) {
        if ([IO.Path]::GetFileName($name) -ne $name) { throw 'Artifact names must be local filenames.' }
        $source = Join-Path $sourceDirectory $name
        $destination = Join-Path $directory $name
        if ((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$name]) {
            throw "Reader artifact changed after exit: $name"
        }
        Copy-Item -LiteralPath $source -Destination $destination
        if ((Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$name]) {
            throw "Reader artifact copy differs: $name"
        }
    }
    foreach ($name in @('stdout.log', 'stderr.log', 'stdin.txt')) {
        $source = Join-Path $result.profile $name
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $directory $name) }
    }
    $files = @{}
    foreach ($file in Get-ChildItem -LiteralPath $directory -File) { $files[$file.Name] = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant() }
    return $files
}
function Save-Proof([bool]$complete) {
    $name = if ($complete) { 'report.json' } else { 'report.partial.json' }
    @{passed=$complete;fileSha256=$hashes;preferencesSha256=$preferences;cases=$cases;finishedAt=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $ReportDirectory $name) -Encoding utf8
}
try {
    Save-Proof $false
    foreach ($gpu in $GpuIndices) {
        if ($gpu -lt 0) { throw 'GPU index must be nonnegative.' }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        $directory = Join-Path $ReportDirectory "gpu$gpu/engine"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        & tools/run-windows-qt6.ps1 -Program ReaderEngineVulkanSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
        if ($LASTEXITCODE -ne 0) { throw "Reader engine GPU $gpu failed. Logs: $directory" }
        $result = Get-Content -LiteralPath 'build/reader-vulkan-smoke/result.json' -Raw | ConvertFrom-Json -AsHashtable
        if (-not $result.ok -or $result.cases.Count -ne 35 -or -not $result.project_roundtrip -or $result.validation_errors -ne 0 -or
            -not $result.algorithm.passed -or $result.algorithm.cases -ne 216 -or $result.algorithm.processingDownloads -ne 0 -or
            $result.algorithm.uploadsPerCase -ne 1 -or -not $result.algorithm.signedPaddedRows -or -not $result.algorithm.roiGuards -or
            $result.algorithm.validationErrors -ne 0 -or ($result.roundtrip_transfers -join ',') -ne '1,1' -or
            $result.native_chain.ocio_nodes -ne 2 -or ($result.native_chain.native_transfers -join ',') -ne '1,1' -or
            ($result.native_chain.cpu_transfers -join ',') -ne '0,0') { throw 'Incomplete Reader engine proof.' }
        foreach ($case in $result.cases) {
            if (($case.native_transfers -join ',') -ne '1,1' -or ($case.cpu_transfers -join ',') -ne '0,0' -or $case.samples_compared -lt 1) {
                throw 'Reader engine performed redundant transfers.'
            }
        }
        Copy-Item -LiteralPath 'build/reader-vulkan-smoke/result.json' -Destination (Join-Path $directory 'result.json')
        $artifacts = Save-Artifacts $result 'build/reader-vulkan-smoke' $directory
        $cases += @{gpu=$gpu;kind='engine';result=$result;directory=$directory;fileSha256=$artifacts}
        Save-Proof $false
        foreach ($depth in $TextureDepths) {
            $directory = Join-Path $ReportDirectory "gpu$gpu/$depth"
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
            & tools/run-windows-qt6.ps1 -Program ReaderVulkanSmoke -GuiTextureDepth $depth -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
            if ($LASTEXITCODE -ne 0) { throw "Reader Viewer GPU $gpu $depth failed. Logs: $directory" }
            $result = Get-Content -LiteralPath 'build/reader-vulkan-gui-smoke/result.json' -Raw | ConvertFrom-Json -AsHashtable
            if (-not $result.ok -or !$result.cpu_display_reference -or $result.display_matrix -or $result.cases.Count -ne 42 -or $result.unexpected_dialogs.Count -ne 0 -or
                ($result.validation_errors | Where-Object { $_ -ne 0 }) -or ($result.graphics_errors | Where-Object { $_ }) -or
                $result.native_scale_renders[0] -lt 42 -or ($result.native_scale_renders[1..5] | Where-Object { $_ -ne 0 })) {
                throw 'Incomplete Reader Viewer proof.'
            }
            if (($result.shared_device_ids -join ',') -ne ($result.engine_device_ids -join ',') -or
                ($result.native_viewer_copies | Measure-Object -Sum).Sum -lt 84) {
                throw 'Reader Viewer device sharing or native display proof is incomplete.'
            }
            foreach ($name in @('identity', 'identity-premult', 'identity-associated', 'color', 'color-premult', 'color-unpremult', 'color-associated')) {
                $variants = @($result.cases | Where-Object { $_.name -eq $name })
                if ($variants.Count -ne 6 -or (($variants.level | Sort-Object) -join ',') -ne '0,1,2,3,4,5') { throw 'Incomplete Reader scale matrix.' }
            }
            foreach ($case in $result.cases) {
                if (!$case.cpu_display_reference -or $case.maximum_byte_error -gt 2 -or $case.cache_maximum_byte_error -gt 1 -or $case.pixels_compared -ne 425 -or
                    $case.native_transfers[0] -ne $case.native_renders -or $case.cache_transfers[0] -ne 0 -or
                    $case.native_transfers[1] -ne 0 -or $case.cache_transfers[1] -ne 0) { throw 'Invalid Reader cache/Viewer comparison.' }
            }
            Copy-Item -LiteralPath 'build/reader-vulkan-gui-smoke/result.json' -Destination (Join-Path $directory 'result.json')
            $artifacts = Save-Artifacts $result 'build/reader-vulkan-gui-smoke' $directory
            $cases += @{gpu=$gpu;kind='gui';textureDepth=$depth;result=$result;directory=$directory;fileSha256=$artifacts}
            Save-Proof $false
        }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferences) { throw 'User preferences changed during Reader tests.' }
    foreach ($file in $proofFiles) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Proof input changed: $file" }
    }
    Save-Proof $true
    Write-Output "Reader Vulkan proof: $(Join-Path $ReportDirectory 'report.json')"
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
}
