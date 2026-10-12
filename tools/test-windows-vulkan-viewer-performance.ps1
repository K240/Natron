[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('4K', '8K')][string[]]$Resolutions = @('4K', '8K'),
    [ValidateSet('native', 'cpu', 'raster')][string[]]$Modes = @('native', 'cpu', 'raster'),
    [ValidateSet('8u', '32f')][string[]]$TextureDepths = @('8u', '32f'),
    [ValidateSet(1, 2)][int[]]$ViewerCounts = @(1, 2),
    [ValidateSet(0, 1)][int[]]$ValidationModes = @(0),
    [ValidateRange(5, 40)][int]$Samples = 20,
    [string]$ReportDirectory = 'build/vulkan-viewer-performance-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }
if (Test-Path -LiteralPath (Join-Path $destination 'report.json')) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$launchVariables = @('PATH', 'PYTHONHOME', 'PYTHONPATH', 'QT_PLUGIN_PATH', 'OCIO', 'OFX_PLUGIN_PATH', 'FONTCONFIG_PATH',
    'FONTCONFIG_FILE', 'MAGICK_CONFIGURE_PATH', 'MAGICK_CODER_MODULE_PATH', 'MAGICK_FILTER_MODULE_PATH',
    'NATRON_VULKAN_VALIDATION', 'NATRON_TEST_VULKAN_VALIDATION')
$variables = $launchVariables + @('NATRON_VULKAN_DEVICE', 'NATRON_VULKAN_MEMORY_LIMIT_MB', 'NATRON_BENCHMARK_WIDTH',
    'NATRON_BENCHMARK_HEIGHT', 'NATRON_BENCHMARK_SAMPLES', 'NATRON_BENCHMARK_VALIDATION',
    'NATRON_BENCHMARK_GUI_MODE', 'NATRON_BENCHMARK_VIEWERS')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
function Restore-Variables([string[]]$names) {
    foreach ($name in $names) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
$proofFiles = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Gpu/NatronGpu.lib',
    'build/ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/Win64/VulkanBenchmarkSmoke.ofx',
    'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'tools/vulkan-device-loss-abi.py', 'tools/vulkan-performance-abi.py', 'tools/vulkan-viewer-performance.py',
    'tools/vulkan-viewer-reference.py', 'tools/verify-vulkan-viewer-performance.py',
    'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan-viewer-performance.ps1')
foreach ($directory in @('Gpu', 'Engine', 'Gui', 'Global', 'Tests', 'build-configs/windows-ofx/native')) {
    $proofFiles += @(Get-ChildItem -LiteralPath (Join-Path $repoRoot $directory) -File -Recurse |
        Where-Object { $_.Extension -in @('.h', '.cpp', '.comp', '.frag', '.vert', '.cmake') -or $_.Name -eq 'CMakeLists.txt' } |
        ForEach-Object { $_.FullName })
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
$report = [ordered]@{ passed = $false; cases = @(); proofHashes = $proofBefore; settingsBefore = $settingsBefore;
    configuration = @{ gpus = $GpuIndices; resolutions = $Resolutions; modes = $Modes; textureDepths = $TextureDepths;
        viewerCounts = $ViewerCounts; validationModes = $ValidationModes; samples = $Samples };
    viewerWsiTested = $true; continuousPlaybackTested = $false; physicalScanoutMeasured = $false;
    parsecTested = $false; glBaselineTested = $false; fullVulkanMigrationComplete = $false }
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($gpu in $GpuIndices) {
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($resolution in $Resolutions) {
            $env:NATRON_BENCHMARK_WIDTH = if ($resolution -eq '4K') { '3840' } else { '7680' }
            $env:NATRON_BENCHMARK_HEIGHT = if ($resolution -eq '4K') { '2160' } else { '4320' }
            $env:NATRON_BENCHMARK_SAMPLES = [string]$Samples
            foreach ($depth in $TextureDepths) {
                foreach ($count in $ViewerCounts) {
                    $env:NATRON_BENCHMARK_VIEWERS = [string]$count
                    foreach ($validation in $ValidationModes) {
                        $env:NATRON_BENCHMARK_VALIDATION = [string]$validation
                        foreach ($mode in $Modes) {
                            $env:NATRON_BENCHMARK_GUI_MODE = $mode
                            $capture = Join-Path $destination "gpu$gpu-$resolution-$depth-viewers$count-validation$validation-$mode"
                            New-Item -ItemType Directory -Force -Path $capture | Out-Null
                            $failure = $null
                            try {
                                & ./tools/run-windows-qt6.ps1 -Program ViewerPerformance -GuiTextureDepth $depth -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
                            } catch { $failure = $_.Exception.Message }
                            finally { Restore-Variables $launchVariables }
                            $source = Join-Path $repoRoot 'build/vulkan-viewer-performance'
                            if (-not (Test-Path -LiteralPath (Join-Path $source 'result.json'))) { throw "Missing Viewer result: $capture. $failure" }
                            $result = Get-Content -LiteralPath (Join-Path $source 'result.json') -Raw | ConvertFrom-Json
                            Copy-Item -LiteralPath (Join-Path $source 'result.json') -Destination (Join-Path $capture 'result.json')
                            foreach ($file in $result.artifact_sha256.PSObject.Properties.Name) {
                                $artifact = Join-Path $source $file
                                if ((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant() -ne $result.artifact_sha256.$file) { throw "Artifact changed: $file" }
                                Copy-Item -LiteralPath $artifact -Destination (Join-Path $capture $file)
                            }
                            $report.cases += @{ gpu = $gpu; resolution = $resolution; depth = $depth; viewerCount = $count;
                                validation = $validation; mode = $mode; artifacts = $capture; result = $result; runFailure = $failure }
                            if ($failure -or -not $result.ok -or $result.samples -ne $Samples -or
                                $result.validation_errors -ne 0 -or $result.unexpected_dialogs) { throw "Viewer performance test failed: $capture. $failure" }
                            Write-Output "GPU $gpu $resolution $depth $count Viewer(s) validation $validation $mode passed."
                        }
                    }
                }
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
    Restore-Variables $variables
    $report.environmentRestored = $true
    foreach ($name in $variables) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) { $report.environmentRestored = $false; $report.passed = $false }
    }
    $report | ConvertTo-Json -Depth 25 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
}
if (-not $report.environmentRestored) { throw 'Viewer test environment was not restored.' }
