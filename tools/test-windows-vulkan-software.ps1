[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$IcdPath,
    [string]$SdkRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$BuildDirectory = 'build/windows-vulkan',
    [string]$ReportDirectory = 'build/software-vulkan-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$IcdPath = (Resolve-Path -LiteralPath $IcdPath).Path
$manifest = Get-Content -LiteralPath $IcdPath -Raw | ConvertFrom-Json
$library = $manifest.ICD.library_path
if (-not $library) { throw 'The ICD manifest has no library_path.' }
if (-not [IO.Path]::IsPathRooted($library)) { $library = Join-Path (Split-Path -Parent $IcdPath) $library }
$library = (Resolve-Path -LiteralPath $library).Path
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot $BuildDirectory }
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$tests = @('Compute', 'DeviceLoss', 'MemoryBudget', 'Renderer', 'ViewerDisplay', 'ViewerGrid', 'ImageOperations', 'FragmentProgram', 'FragmentLut')
$proofFiles = @($IcdPath, $library, (Join-Path $BuildDirectory 'Gpu/NatronGpu.lib'),
    (Join-Path $PSScriptRoot 'test-windows-vulkan-software.ps1'),
    (Join-Path $PSScriptRoot 'test-windows-vulkan.ps1'),
    (Join-Path $PSScriptRoot 'probe-windows-vulkan.ps1'))
$proofFiles += @($tests | ForEach-Object { Join-Path $BuildDirectory "GpuTests/NatronVulkan$_.exe" })
$proofFiles += @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'Gpu') -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.h', '.comp', '.vert', '.frag', '.cmake') -or $_.Name -eq 'CMakeLists.txt' } |
    ForEach-Object { $_.FullName })
function Get-ProofHashes {
    $values = @{}
    foreach ($file in $proofFiles) { $values[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $values
}
$before = Get-ProofHashes
$variables = @('VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'NATRON_VULKAN_DEVICE',
    'NATRON_VULKAN_DISABLED', 'NATRON_VULKAN_MEMORY_LIMIT_MB', 'NATRON_VULKAN_VALIDATION', 'DISABLE_VULKAN_OBS_CAPTURE')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$report = [ordered]@{ passed = $false; scope = 'Windows software Vulkan headless rendering core';
    fullVulkanMigrationComplete = $false; icd = $IcdPath; library = $library; inputSha256 = $before; cases = @() }
try {
    $env:VK_DRIVER_FILES = $IcdPath
    $env:VK_ICD_FILENAMES = $IcdPath
    Remove-Item -LiteralPath Env:VK_ADD_DRIVER_FILES,Env:NATRON_VULKAN_DISABLED,Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    $env:NATRON_VULKAN_DEVICE = '0'
    $env:NATRON_VULKAN_VALIDATION = '1'
    $env:DISABLE_VULKAN_OBS_CAPTURE = '1'
    $probeDirectory = Join-Path $ReportDirectory 'probe'
    & ./tools/probe-windows-vulkan.ps1 -SdkRoot $SdkRoot -OutputDirectory $probeDirectory *> (Join-Path $ReportDirectory 'probe.log')
    if ($LASTEXITCODE -ne 0) { throw 'Software Vulkan capability probe failed.' }
    $probe = Get-Content -LiteralPath (Join-Path $probeDirectory 'report.json') -Raw | ConvertFrom-Json
    if ($probe.devices.Count -ne 1 -or $probe.devices[0].type -ne 'VK_PHYSICAL_DEVICE_TYPE_CPU' -or
        [version]$probe.devices[0].apiVersion -lt [version]'1.3.0' -or -not $probe.validationLayerAvailable) {
        throw 'The selected ICD must expose exactly one CPU Vulkan 1.3 device and the validation layer.'
    }
    $report.device = $probe.devices[0]
    $coreDirectory = Join-Path $ReportDirectory 'core'
    & ./tools/test-windows-vulkan.ps1 -SdkRoot $SdkRoot -QtRoot $QtRoot -BuildDirectory $BuildDirectory `
        -Tests $tests -GpuIndices 0 -ReportDirectory $coreDirectory -TimeoutSeconds $TimeoutSeconds *> (Join-Path $ReportDirectory 'core.log')
    if ($LASTEXITCODE -ne 0) { throw 'Software Vulkan rendering tests failed.' }
    foreach ($test in $tests) {
        $stdout = Join-Path $coreDirectory "gpu0/$test.stdout.log"
        $stderr = Join-Path $coreDirectory "gpu0/$test.stderr.log"
        $result = Get-Content -LiteralPath $stdout -Raw | ConvertFrom-Json
        if (-not $result.passed -or $result.device -ne $report.device.name -or
            $result.validationErrors -or $result.validation_errors -or $result.openGLLoaded -or
            (Get-Content -LiteralPath $stderr -Raw) -match 'VUID-|Vulkan validation error:|Validation Error') {
            throw "Invalid software Vulkan result: $test"
        }
        $report.cases += @{ test = $test; result = $result;
            stdoutSha256 = (Get-FileHash -LiteralPath $stdout).Hash.ToLowerInvariant();
            stderrSha256 = (Get-FileHash -LiteralPath $stderr).Hash.ToLowerInvariant() }
    }
    $after = Get-ProofHashes
    foreach ($file in $proofFiles) { if ($before[$file] -ne $after[$file]) { throw "Software Vulkan input changed: $file" } }
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
        if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) {
            $report.environmentRestored = $false
            $report.passed = $false
        }
    }
    $report | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $reportPath -Encoding utf8
}
if (-not $report.environmentRestored) { throw 'Software Vulkan environment variables were not restored.' }
Write-Output "Software Vulkan passed $($tests.Count) core tests: $($report.device.name). Proof: $reportPath"
