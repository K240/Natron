[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('4K', '8K')][string[]]$Resolutions = @('4K', '8K'),
    [ValidateSet(0, 1)][int[]]$ValidationModes = @(1, 0),
    [ValidateRange(5, 40)][int]$Samples = 20,
    [string]$ReportDirectory = 'build/vulkan-application-performance-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }
if (Test-Path -LiteralPath (Join-Path $destination 'report.json')) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'NATRON_VULKAN_MEMORY_LIMIT_MB', 'NATRON_VULKAN_VALIDATION', 'NATRON_TEST_VULKAN_VALIDATION',
               'NATRON_BENCHMARK_WIDTH', 'NATRON_BENCHMARK_HEIGHT', 'NATRON_BENCHMARK_SAMPLES', 'NATRON_BENCHMARK_VALIDATION')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$proofFiles = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Gpu/NatronGpu.lib',
    'build/ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/Win64/VulkanBenchmarkSmoke.ofx',
    'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'tools/vulkan-device-loss-abi.py', 'tools/vulkan-performance-abi.py', 'tools/vulkan-performance-render.py',
    'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan-performance.ps1')
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
    viewerWsiTested = $false; parsecTested = $false; glBaselineTested = $false; fullVulkanMigrationComplete = $false }
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($gpu in $GpuIndices) {
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($resolution in $Resolutions) {
            $env:NATRON_BENCHMARK_WIDTH = if ($resolution -eq '4K') { '3840' } else { '7680' }
            $env:NATRON_BENCHMARK_HEIGHT = if ($resolution -eq '4K') { '2160' } else { '4320' }
            $env:NATRON_BENCHMARK_SAMPLES = [string]$Samples
            foreach ($validation in $ValidationModes) {
                $env:NATRON_BENCHMARK_VALIDATION = [string]$validation
                $capture = Join-Path $destination "gpu$gpu-$resolution-validation$validation"
                New-Item -ItemType Directory -Force -Path $capture | Out-Null
                & ./tools/run-windows-qt6.ps1 -Program PerformanceRender -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
                $runExit = $LASTEXITCODE
                $source = Join-Path $repoRoot 'build/vulkan-performance-render'
                $result = Get-Content -LiteralPath (Join-Path $source 'result.json') -Raw | ConvertFrom-Json
                Copy-Item -LiteralPath (Join-Path $source 'result.json') -Destination (Join-Path $capture 'result.json')
                foreach ($file in $result.artifact_sha256.PSObject.Properties.Name) {
                    $artifact = Join-Path $source $file
                    if ((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant() -ne $result.artifact_sha256.$file) { throw "Artifact changed: $file" }
                    Copy-Item -LiteralPath $artifact -Destination (Join-Path $capture $file)
                }
                $report.cases += @{ gpu = $gpu; resolution = $resolution; validation = $validation; artifacts = $capture; result = $result }
                if ($runExit -ne 0 -or -not $result.ok -or $result.samples -ne $Samples -or
                    $result.validation_errors -ne 0 -or $result.unexpected_dialogs -or
                    $result.full_reference_pixels -ne ([long]$env:NATRON_BENCHMARK_WIDTH * [long]$env:NATRON_BENCHMARK_HEIGHT)) { throw "Application performance test failed: $capture" }
                Write-Output "GPU $gpu $resolution validation $validation passed: $($result.device)."
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
    foreach ($name in $variables) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
    $report.environmentRestored = $true
    foreach ($name in $variables) {
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) { $report.environmentRestored = $false; $report.passed = $false }
    }
    $report | ConvertTo-Json -Depth 22 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
}
if (-not $report.environmentRestored) { throw 'Performance test environment was not restored.' }
