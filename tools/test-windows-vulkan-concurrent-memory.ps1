[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('8u', '32f')][string[]]$TextureDepths = @('8u', '32f'),
    [ValidateSet(1, 2)][int[]]$DisplayScales = @(1, 2),
    [string]$ReportDirectory = 'build/vulkan-concurrent-memory-tests',
    [string]$IcdPath,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }
if (Test-Path -LiteralPath (Join-Path $destination 'report.json')) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'NATRON_VULKAN_MEMORY_LIMIT_MB', 'NATRON_VULKAN_VALIDATION', 'QT_SCALE_FACTOR',
               'VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'VK_LOADER_DRIVERS_DISABLE')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$proofFiles = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Gpu/NatronGpu.lib',
    'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx',
    'Tests/VulkanPressurePlugin.cpp', 'Tests/CMakeLists.txt',
    'Engine/EffectInstance.cpp', 'Engine/EffectInstanceRenderRoI.cpp', 'Engine/OfxVulkanImageSuite.cpp',
    'Engine/ViewerInstance.cpp', 'Gui/ViewerVulkan.cpp', 'Gui/ViewerGL.cpp', 'Gui/ViewerGLPrivate.h',
    'Gpu/VulkanDevice.cpp', 'Gpu/Qt/VulkanCanvas.cpp',
    'tools/vulkan-concurrent-memory-gui-smoke.py', 'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan-concurrent-memory.ps1')
if ($IcdPath) {
    $resolvedIcd = (Resolve-Path -LiteralPath $IcdPath).Path
    $icd = Get-Content -LiteralPath $resolvedIcd -Raw | ConvertFrom-Json
    $driver = if ([IO.Path]::IsPathRooted($icd.ICD.library_path)) { $icd.ICD.library_path } else { Join-Path (Split-Path -Parent $resolvedIcd) $icd.ICD.library_path }
    $proofFiles += @($resolvedIcd, (Resolve-Path -LiteralPath $driver).Path)
}
function Get-ProofHashes {
    $hashes = @{}
    foreach ($file in $proofFiles) {
        $path = if ([IO.Path]::IsPathRooted($file)) { $file } else { Join-Path $repoRoot $file }
        $hashes[$file] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $hashes
}
$proofBefore = Get-ProofHashes
$settingsBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint Natron user settings.' }
$report = [ordered]@{ passed = $false; cases = @(); proofHashes = $proofBefore; settingsBefore = $settingsBefore; icd = $IcdPath }
try {
    if ($IcdPath) {
        $env:VK_DRIVER_FILES = $resolvedIcd
        $env:VK_ICD_FILENAMES = $resolvedIcd
        Remove-Item -LiteralPath Env:VK_ADD_DRIVER_FILES, Env:VK_LOADER_DRIVERS_DISABLE -ErrorAction SilentlyContinue
    }
    $env:NATRON_VULKAN_MEMORY_LIMIT_MB = '64'
    foreach ($gpu in $GpuIndices) {
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($scale in $DisplayScales) {
            $env:QT_SCALE_FACTOR = [string]$scale
            foreach ($depth in $TextureDepths) {
                $capture = Join-Path $destination "gpu$gpu-scale$scale-$depth"
                New-Item -ItemType Directory -Force -Path $capture | Out-Null
                & ./tools/run-windows-qt6.ps1 -Program ConcurrentMemoryGuiSmoke -GuiTextureDepth $depth -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
                if ($LASTEXITCODE -ne 0) { throw "Concurrent Viewer test failed: GPU $gpu, scale $scale, $depth" }
                $result = Get-Content -LiteralPath 'build/vulkan-concurrent-memory-gui-smoke/result.json' -Raw | ConvertFrom-Json
                Copy-Item -LiteralPath 'build/vulkan-concurrent-memory-gui-smoke/result.json' -Destination (Join-Path $capture 'result.json')
                foreach ($file in $result.artifact_sha256.PSObject.Properties.Name) {
                    $source = Join-Path $repoRoot "build/vulkan-concurrent-memory-gui-smoke/$file"
                    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $result.artifact_sha256.$file) { throw "Artifact changed: $file" }
                    Copy-Item -LiteralPath $source -Destination (Join-Path $capture $file)
                }
                $report.cases += @{ gpu = $gpu; scale = $scale; depth = $depth; result = $result }
                if (-not $result.ok -or -not $result.native_recovered -or $result.overlapping_cpu_actions -lt 2 -or
                    $result.raster_count_under_pressure -lt 1 -or $result.validation_errors -or
                    $result.qt_opengl_loaded.Count -or $result.unexpected_dialogs.Count -or $result.display_scale -ne $scale -or
                    $result.pixels_compared -lt 1000000) { throw "Invalid concurrent Viewer result: GPU $gpu, scale $scale, $depth" }
                Write-Output "GPU $gpu scale $scale $depth passed: $($result.device), $($result.pixels_compared) compared pixels."
            }
        }
    }
    $proofAfter = Get-ProofHashes
    foreach ($file in $proofFiles) { if ($proofAfter[$file] -ne $proofBefore[$file]) { throw "Proof file changed during testing: $file" } }
    $report.settingsAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $report.settingsAfter -ne $settingsBefore) { throw 'User settings changed during testing.' }
    $report.passed = $true
} catch {
    $report.failure = $_.Exception.Message
    throw
} finally {
    $report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
    foreach ($name in $variables) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
