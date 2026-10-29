[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build/vulkan-pressure-tests',
    [string]$IcdPath,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }
if (Test-Path -LiteralPath (Join-Path $destination 'report.json')) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'NATRON_VULKAN_MEMORY_LIMIT_MB', 'NATRON_VULKAN_VALIDATION',
               'VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'VK_LOADER_DRIVERS_DISABLE')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$proofFiles = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Gpu/NatronGpu.lib',
    'build/ofx-tests/VulkanPressureSmoke.ofx.bundle/Contents/Win64/VulkanPressureSmoke.ofx',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'Tests/VulkanPressurePlugin.cpp', 'Tests/CMakeLists.txt',
    'Engine/EffectInstance.cpp', 'Engine/EffectInstanceRenderRoI.cpp', 'Engine/VulkanImageStorage.cpp',
    'Engine/ImageVulkan.cpp', 'Engine/OfxVulkanImageSuite.cpp', 'Gpu/VulkanDevice.cpp',
    'tools/vulkan-memory-pressure-smoke.py', 'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan-pressure.ps1')
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
    $env:NATRON_VULKAN_MEMORY_LIMIT_MB = '32'
    foreach ($gpu in $GpuIndices) {
        $result = $null;
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        $capture = Join-Path $destination "gpu$gpu"
        New-Item -ItemType Directory -Force -Path $capture | Out-Null
        & ./tools/run-windows-qt6.ps1 -Program MemoryPressureSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
        $runExit = $LASTEXITCODE
        $resultPath = Join-Path $repoRoot 'build/vulkan-memory-pressure-smoke/result.json'
        if (Test-Path -LiteralPath $resultPath) {
            $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            Copy-Item -LiteralPath $resultPath -Destination (Join-Path $capture 'result.json')
            foreach ($file in $result.artifact_sha256.PSObject.Properties.Name) {
                $source = Join-Path $repoRoot "build/vulkan-memory-pressure-smoke/$file"
                if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $result.artifact_sha256.$file) { throw "Artifact changed: $file" }
                Copy-Item -LiteralPath $source -Destination (Join-Path $capture $file)
            }
            $report.cases += @{ gpu = $gpu; result = $result }
        }
        if ($runExit -ne 0 -or -not $result.ok) { throw "Cached input memory test failed on GPU $gpu" }
        if (-not $result.cached_native_input -or -not $result.cpu_retry -or -not $result.native_recovered -or
            $result.pressure_status -ne 8 -or $result.validation_errors -ne 0 -or $result.opengl_loaded) { throw "Invalid result on GPU $gpu" }
        Write-Output "GPU $gpu passed: $($result.device), $($result.pixels_compared) compared pixels."
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
