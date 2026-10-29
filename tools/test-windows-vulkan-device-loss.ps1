[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('8u', '32f')][string[]]$TextureDepths = @('8u', '32f'),
    [ValidateSet(1, 2)][int[]]$DisplayScales = @(1, 2),
    [string]$ReportDirectory = 'build/vulkan-device-loss-tests',
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
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx', 'build/Plugins/Misc.ofx.bundle/Contents/Win64/Misc.ofx',
    'tools/vulkan-device-loss-abi.py', 'tools/vulkan-device-loss-smoke.py', 'tools/vulkan-device-loss-gui-smoke.py',
    'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan-device-loss.ps1')
foreach ($directory in @('Gpu', 'Engine', 'Gui', 'Global', 'Tests', 'build-configs/windows-ofx/native')) {
    $proofFiles += @(Get-ChildItem -LiteralPath (Join-Path $repoRoot $directory) -File -Recurse |
        Where-Object { $_.Extension -in @('.h', '.cpp', '.comp', '.frag', '.vert', '.cmake') -or $_.Name -eq 'CMakeLists.txt' } |
        ForEach-Object { $_.FullName })
}
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
$report = [ordered]@{ passed = $false; cases = @(); proofHashes = $proofBefore; settingsBefore = $settingsBefore;
    icd = $IcdPath; physicalDriverReset = $false; fullVulkanMigrationComplete = $false }
try {
    if ($IcdPath) {
        $env:VK_DRIVER_FILES = $resolvedIcd
        $env:VK_ICD_FILENAMES = $resolvedIcd
        Remove-Item -LiteralPath Env:VK_ADD_DRIVER_FILES, Env:VK_LOADER_DRIVERS_DISABLE -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($gpu in $GpuIndices) {
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        $requests = @(@{ program = 'DeviceLossSmoke'; mode = 'render'; depth = '8u'; scale = 1 })
        foreach ($depth in $TextureDepths) {
            foreach ($scale in $DisplayScales) {
                $requests += @{ program = 'DeviceLossGuiSmoke'; mode = 'viewer'; depth = $depth; scale = $scale }
            }
        }
        foreach ($request in $requests) {
            $env:QT_SCALE_FACTOR = [string]$request.scale
            $capture = Join-Path $destination "gpu$gpu-$($request.mode)-$($request.depth)-scale$($request.scale)"
            New-Item -ItemType Directory -Force -Path $capture | Out-Null
            & ./tools/run-windows-qt6.ps1 -Program $request.program -GuiTextureDepth $request.depth `
                -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
            $runExit = $LASTEXITCODE
            $source = Join-Path $repoRoot $(if ($request.mode -eq 'render') { 'build/vulkan-device-loss-smoke' } else { 'build/vulkan-device-loss-gui-smoke' })
            $result = $null
            if (Test-Path -LiteralPath (Join-Path $source 'result.json')) {
                $result = Get-Content -LiteralPath (Join-Path $source 'result.json') -Raw | ConvertFrom-Json
                Copy-Item -LiteralPath (Join-Path $source 'result.json') -Destination (Join-Path $capture 'result.json')
                foreach ($file in $result.artifact_sha256.PSObject.Properties.Name) {
                    $artifact = Join-Path $source $file
                    if ((Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant() -ne $result.artifact_sha256.$file) { throw "Artifact changed: $file" }
                    Copy-Item -LiteralPath $artifact -Destination (Join-Path $capture $file)
                }
                $report.cases += @{ gpu = $gpu; mode = $request.mode; depth = $request.depth; scale = $request.scale;
                                    artifacts = $capture; result = $result }
            }
            if ($runExit -ne 0 -or -not $result.ok -or $result.validation_errors -ne 0 -or
                $result.physical_driver_reset -or $result.unexpected_dialogs) { throw "Device-loss test failed: $capture" }
            Write-Output "GPU $gpu $($request.mode) $($request.depth) scale $($request.scale) passed: $($result.device)."
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
    $report | ConvertTo-Json -Depth 18 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
}
if (-not $report.environmentRestored) { throw 'Device-loss test environment was not restored.' }
