[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('4K', '8K')][string[]]$Resolutions = @('4K', '8K'),
    [ValidateSet('native', 'raster')][string[]]$Modes = @('native', 'raster'),
    [ValidateSet('8u', '32f')][string[]]$TextureDepths = @('8u', '32f'),
    [string]$ReportDirectory = 'build/windows-vulkan-playback-tests'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }))
if (Test-Path -LiteralPath $destination) { throw 'Use a fresh report directory.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'NATRON_BENCHMARK_WIDTH', 'NATRON_BENCHMARK_GUI_MODE',
    'NATRON_TEST_VIEWER_BACKEND', 'NATRON_VULKAN_MEMORY_LIMIT_MB')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$files = @('tools/vulkan-playback-gui-smoke.py', 'tools/vulkan-viewer-reference.py',
    'tools/verify-vulkan-playback.py',
    'tools/vulkan-performance-abi.py', 'tools/vulkan-device-loss-abi.py',
    'tools/test-windows-vulkan-playback.ps1', 'tools/run-windows-owned-gui-test.ps1',
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/Win64/VulkanBenchmarkSmoke.ofx',
    'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx',
    'build/viewer-presentation-source-audit.json', 'build/viewer-presentation-runtime-audit.json')
$audit = Get-Content -LiteralPath 'build/viewer-presentation-source-audit.json' -Raw | ConvertFrom-Json -AsHashtable
$runtime = Get-Content -LiteralPath 'build/viewer-presentation-runtime-audit.json' -Raw | ConvertFrom-Json
$files += $audit.fileSha256.Keys
$files += $runtime.files.file
function Get-InputHashes {
    $hashes = @{}
    foreach ($file in ($files | Select-Object -Unique)) { $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$before = Get-InputHashes
$settings = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint user settings.' }
$report = [ordered]@{ passed = $false; completionPlatform = 'windows'; cases = @(); inputSha256 = $before;
    settingsBefore = $settings; continuousPlaybackTested = $true; parsecTested = $false; physicalScanoutMeasured = $false }
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($gpu in $GpuIndices) {
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($resolution in $Resolutions) {
            $env:NATRON_BENCHMARK_WIDTH = if ($resolution -eq '4K') { '3840' } else { '7680' }
            foreach ($depth in $TextureDepths) {
                foreach ($mode in $Modes) {
                    $env:NATRON_BENCHMARK_GUI_MODE = $mode
                    if ($mode -eq 'raster') { $env:NATRON_TEST_VIEWER_BACKEND = 'Raster' }
                    else { Remove-Item -LiteralPath Env:NATRON_TEST_VIEWER_BACKEND -ErrorAction SilentlyContinue }
                    $name = "gpu$gpu-$resolution-$depth-$mode"
                    $result = Join-Path $destination "$name/result.json"
                    $profile = 'build/test-profiles/Playback-' + [Guid]::NewGuid().ToString('N')
                    $failure = $null
                    try {
                        & ./tools/run-windows-owned-gui-test.ps1 -Script tools/vulkan-playback-gui-smoke.py -Profile $profile `
                            -Result $result -TextureDepth $depth *> (Join-Path $destination "$name.log")
                    } catch { $failure = $_.Exception.Message }
                    $record = if (Test-Path -LiteralPath $result) { Get-Content -LiteralPath $result -Raw | ConvertFrom-Json } else { $null }
                    $report.cases += @{ gpu = $gpu; resolution = $resolution; depth = $depth; mode = $mode;
                        result = $result; profile = $profile; failure = $failure }
                    if ($failure -or -not $record.ok -or $record.stages.Count -ne 7 -or $record.captures.Count -ne 14 -or
                        $record.final_statistics.validation_errors -ne 0) { throw "Playback case failed: $name. $failure" }
                    Write-Output "Playback $name passed."
                }
            }
        }
    }
    $after = Get-InputHashes
    foreach ($name in $before.Keys) { if ($after[$name] -ne $before[$name]) { throw "Input changed during playback: $name" } }
    $report.settingsAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $report.settingsAfter -ne $settings) { throw 'User settings changed.' }
    $report.passed = $true
} catch {
    $report.failure = $_.Exception.Message
    throw
} finally {
    foreach ($name in $variables) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
    $report.environmentRestored = $true
    foreach ($name in $variables) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) { $report.environmentRestored = $false; $report.passed = $false }
    }
    $report | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
}
if (-not $report.environmentRestored) { throw 'Playback environment restoration failed.' }
