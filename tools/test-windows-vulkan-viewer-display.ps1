[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build\viewer-display-vulkan-tests',
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
& uv run --frozen python tools/reader-vulkan-fixtures.py *> (Join-Path $ReportDirectory 'fixtures.log')
if ($LASTEXITCODE -ne 0) { throw 'Viewer display fixture preparation failed.' }
$proofFiles = @(
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-vulkan/GpuTests/NatronVulkanViewerDisplay.exe',
    'build/windows-vulkan/GpuTests/NatronVulkanViewerGrid.exe',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'Gpu/ViewerDisplayConversion.cpp', 'Gpu/ViewerDisplayConversion.h', 'Gpu/shaders/viewer-display.comp',
    'Gpu/ViewerRenderer.cpp', 'Gpu/ViewerRenderer.h', 'Gpu/shaders/viewer.comp',
    'Gpu/CMakeLists.txt', 'Gpu/tests/CMakeLists.txt', 'Gpu/tests/ViewerDisplay.cpp',
    'Gpu/tests/ViewerGrid.cpp',
    'Engine/ViewerInstance.cpp', 'Engine/ViewerInstance.h', 'Engine/ViewerInstancePrivate.h',
    'Engine/UpdateViewerParams.h', 'Engine/OpenGLViewerI.h', 'Engine/Lut.cpp', 'Engine/Lut.h',
    'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h',
    'Engine/VulkanImageContextPool.cpp', 'Engine/VulkanImageContextPool.h',
    'Gui/ViewerGL.cpp', 'Gui/ViewerGL.h', 'Gui/ViewerGLPrivate.h', 'Gui/ViewerVulkan.cpp',
    'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan.ps1', 'tools/test-windows-vulkan-viewer-display.ps1',
    'tools/reader-vulkan-gui-smoke.py', 'tools/reader-vulkan-fixtures.py', 'tools/test-settings-fingerprint.py'
)
$hashes = @{}
foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferences = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint user preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$previousMatrix = $env:NATRON_TEST_VIEWER_DISPLAY_MATRIX
$previousReference = $env:NATRON_TEST_VIEWER_CPU_REFERENCE
$cases = @()
function Save-Proof([bool]$complete) {
    $name = if ($complete) { 'report.json' } else { 'report.partial.json' }
    @{passed=$complete;fileSha256=$hashes;preferencesSha256=$preferences;cases=$cases;finishedAt=[DateTime]::UtcNow.ToString('o')} |
        ConvertTo-Json -Depth 14 | Set-Content -LiteralPath (Join-Path $ReportDirectory $name) -Encoding utf8
}
try {
    Save-Proof $false
    & tools/test-windows-vulkan.ps1 -Tests ViewerDisplay,ViewerGrid -GpuIndices $GpuIndices -ReportDirectory (Join-Path $ReportDirectory 'backend') -TimeoutSeconds 180
    if ($LASTEXITCODE -ne 0) { throw 'Viewer byte conversion backend failed.' }
    $env:NATRON_TEST_VIEWER_DISPLAY_MATRIX = '1'
    $env:NATRON_TEST_VIEWER_CPU_REFERENCE = $null
    foreach ($gpu in $GpuIndices) {
        if ($gpu -lt 0) { throw 'GPU index must be nonnegative.' }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        $directory = Join-Path $ReportDirectory "gpu$gpu"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $backendDirectory = Join-Path $ReportDirectory "backend/gpu$gpu"
        $backend = Get-Content -LiteralPath (Join-Path $backendDirectory 'ViewerDisplay.stdout.log') -Raw | ConvertFrom-Json -AsHashtable
        $grid = Get-Content -LiteralPath (Join-Path $backendDirectory 'ViewerGrid.stdout.log') -Raw | ConvertFrom-Json -AsHashtable
        if (!$grid.passed -or $grid.frames -ne 8 -or $grid.channelsCompared -ne 26880000 -or $grid.maxAbsoluteError -gt .00002 -or $grid.validationErrors -ne 0) {
            throw 'Viewer cropped/padded grid comparison failed.'
        }
        if (!$backend.passed -or $backend.cases -ne 432 -or $backend.channelsCompared -ne 580032 -or
            $backend.maximumByteError -gt 1 -or $backend.invalidCases -ne 6 -or !$backend.stableDithering -or
            !$backend.sourceUnchanged -or $backend.validationErrors -ne 0 -or $backend.openGLLoaded) {
            throw 'Viewer display backend proof is incomplete.'
        }
        & tools/run-windows-qt6.ps1 -Program ReaderVulkanSmoke -GuiTextureDepth 8u -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
        if ($LASTEXITCODE -ne 0) { throw "Viewer byte display GPU $gpu failed: $directory" }
        $result = Get-Content -LiteralPath 'build/reader-vulkan-gui-smoke/result.json' -Raw | ConvertFrom-Json -AsHashtable
        if (!$result.ok -or !$result.display_matrix -or !$result.cpu_display_reference -or $result.cases.Count -ne 32 -or
            $result.node_previews_enabled -or $result.unexpected_dialogs.Count -ne 0 -or
            ($result.validation_errors | Where-Object { $_ -ne 0 }) -or ($result.graphics_errors | Where-Object { $_ }) -or
            ($result.shared_device_ids -join ',') -ne ($result.engine_device_ids -join ',') -or
            ($result.native_viewer_copies | Measure-Object -Sum).Sum -lt 64) {
            throw 'Incomplete native byte Viewer GUI proof.'
        }
        foreach ($lut in 0..3) {
            $variants = @($result.cases | Where-Object { $_.display_correction.lut -eq $lut })
            if ($variants.Count -ne 8) { throw 'Incomplete display LUT matrix.' }
            foreach ($gamma in @(0., .45, 1., 2.2)) {
                if (@($variants | Where-Object { $_.display_correction.gamma -eq $gamma }).Count -ne 2) { throw 'Incomplete display gamma matrix.' }
            }
        }
        foreach ($case in $result.cases) {
            if (!$case.cpu_display_reference -or $case.maximum_byte_error -gt 2 -or $case.cache_maximum_byte_error -gt 1 -or
                $case.pixels_compared -ne 425 -or $case.native_transfers[0] -ne $case.native_renders -or
                $case.native_transfers[1] -ne 0 -or $case.cache_transfers[0] -ne 0 -or $case.cache_transfers[1] -ne 0) {
                throw 'Viewer display/cache CPU comparison failed.'
            }
        }
        Copy-Item -LiteralPath 'build/reader-vulkan-gui-smoke/result.json' -Destination (Join-Path $directory 'result.json')
        foreach ($name in $result.artifact_sha256.Keys) {
            if ([IO.Path]::GetFileName($name) -ne $name) { throw 'Artifact must be a local filename.' }
            $source = Join-Path 'build/reader-vulkan-gui-smoke' $name
            if ((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$name]) { throw "Artifact changed: $name" }
            Copy-Item -LiteralPath $source -Destination (Join-Path $directory $name)
            if ((Get-FileHash -LiteralPath (Join-Path $directory $name)).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$name]) { throw "Artifact copy changed: $name" }
        }
        foreach ($name in @('stdout.log', 'stderr.log', 'started.txt')) {
            Copy-Item -LiteralPath (Join-Path $result.profile $name) -Destination (Join-Path $directory $name)
        }
        if (Test-Path -LiteralPath (Join-Path $result.profile 'timeout.json')) { throw 'Viewer display watchdog timed out.' }
        $artifacts = @{}
        foreach ($file in Get-ChildItem -LiteralPath $directory -File) { $artifacts[$file.Name] = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant() }
        $backendHashes = @{}
        foreach ($file in Get-ChildItem -LiteralPath $backendDirectory -File) { $backendHashes[$file.Name] = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant() }
        $cases += @{gpu=$gpu;result=$result;backend=$backend;grid=$grid;directory=$directory;fileSha256=$artifacts;backendDirectory=$backendDirectory;backendSha256=$backendHashes}
        Save-Proof $false
        Write-Output "Viewer byte LUT/gamma GUI GPU $gpu passed: $($result.devices -join ', ')."
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferences) { throw 'User preferences changed.' }
    foreach ($file in $proofFiles) { if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Proof input changed: $file" } }
    Save-Proof $true
    Write-Output "Viewer display Vulkan proof: $(Join-Path $ReportDirectory 'report.json')"
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
    $env:NATRON_TEST_VIEWER_DISPLAY_MATRIX = $previousMatrix
    $env:NATRON_TEST_VIEWER_CPU_REFERENCE = $previousReference
}
