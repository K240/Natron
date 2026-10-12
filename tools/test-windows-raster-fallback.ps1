[CmdletBinding()]
param([string]$ReportDirectory = 'build/raster-fallback-tests')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = [IO.Path]::GetFullPath((Join-Path $repoRoot $ReportDirectory))
if (Test-Path -LiteralPath (Join-Path $destination 'report.json')) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$variables = @('NATRON_VULKAN_DISABLED', 'VK_DRIVER_FILES', 'VK_ICD_FILENAMES', 'VK_ADD_DRIVER_FILES', 'QT_SCALE_FACTOR')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$proofFiles = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
                'build/windows-qt6/Tests/NatronRasterViewer.exe', 'Gpu/VulkanDevice.cpp',
                'tools/run-windows-qt6.ps1', 'tools/headless-cpu-smoke.py', 'tools/raster-gui-smoke.py',
                'tools/ofx-smoke.py', 'tools/io-overlay-gui-smoke.py', 'tools/test-windows-raster-fallback.ps1')
$proofFiles += @('Engine/EffectInstanceRenderRoI.cpp', 'Engine/OfxHost.cpp', 'Engine/OpenGLViewerI.h',
                 'Engine/OutputSchedulerThread.cpp', 'Engine/ViewerInstance.cpp',
                 'Engine/VulkanImageContextPool.cpp', 'Engine/VulkanImageContextPool.h',
                 'Gui/AboutWindow.cpp', 'Gui/ViewerGL.cpp', 'Gui/ViewerGL.h', 'Gui/ViewerGLPrivate.h', 'Gui/ViewerVulkan.cpp',
                 'Gpu/CMakeLists.txt', 'Gpu/RasterViewerRenderer.cpp', 'Gpu/RasterViewerRenderer.h', 'Gpu/ViewerFrame.cpp', 'Gpu/ViewerFrame.h',
                 'Tests/CMakeLists.txt', 'Gpu/tests/RasterViewer.cpp', 'Gpu/tests/RasterGpuCompare.cpp',
                 'build/windows-qt6/Tests/NatronRasterGpuCompare.exe',
                 'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
                 'build-configs/windows-ofx/native/OcioVulkanAdapter.cpp')
function Get-ProofHashes {
    $hashes = @{}
    foreach ($file in $proofFiles | Sort-Object -Unique) {
        $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $repoRoot $file) -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    return $hashes
}
$hashesBefore = Get-ProofHashes
$preferencesBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint the user settings.' }
$cases = @()
function Invoke-CpuCase([string]$Mode, [string]$Program, [string]$Name, [string]$Depth = '8u', [string]$Scale = '1') {
    $env:QT_SCALE_FACTOR = $Scale
    $caseDirectory = Join-Path $destination $Name
    New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
    & (Join-Path $PSScriptRoot 'run-windows-qt6.ps1') -Program $Program -GuiTextureDepth $Depth -GuiTimeoutSeconds 120 *> (Join-Path $caseDirectory 'launcher.log')
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath (Join-Path $caseDirectory 'launcher.log') -Tail 25; throw "$Name failed." }
    $source = Join-Path $repoRoot $(if ($Program -eq 'RasterGuiSmoke') { 'build/raster-gui-smoke' }
                                  elseif ($Program -eq 'RasterIOOverlaySmoke') { 'build/raster-io-overlay-gui-smoke' }
                                  else { 'build/headless-cpu-smoke' })
    foreach ($item in Get-ChildItem -LiteralPath $source) { Copy-Item -LiteralPath $item.FullName -Destination $caseDirectory -Recurse }
    $result = Get-Content -LiteralPath (Join-Path $caseDirectory 'result.json') -Raw | ConvertFrom-Json
    if (-not $result.ok) { throw "$Name did not pass." }
    $profile = $result.profile
    if (-not (Test-Path -LiteralPath (Join-Path $profile 'started.txt')) -or (Test-Path -LiteralPath (Join-Path $profile 'timeout.json'))) { throw "$Name did not start and close normally." }
    $stderr = Get-Content -LiteralPath (Join-Path $profile 'stderr.log') -Raw
    if ($stderr -match 'VUID-|Validation Error|Traceback') { throw "$Name reported validation or Python errors." }
    $hashes = @{}
    foreach ($file in Get-ChildItem -LiteralPath $caseDirectory -File -Recurse) {
        $hashes[[IO.Path]::GetRelativePath($caseDirectory, $file.FullName)] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    Write-Host "CPU fallback $Name passed."
    return @{ mode = $Mode; program = $Program; depth = $Depth; scale = [int]$Scale; directory = $caseDirectory; result = $result; fileSha256 = $hashes }
}
try {
    foreach ($mode in @('disabled', 'missing-icd')) {
        $env:NATRON_VULKAN_DISABLED = if ($mode -eq 'disabled') { '1' } else { $null }
        $env:VK_DRIVER_FILES = if ($mode -eq 'missing-icd') { Join-Path $destination 'intentionally-missing-vulkan-driver.json' } else { $null }
        $env:VK_ICD_FILENAMES = $null
        $env:VK_ADD_DRIVER_FILES = $null
        $cases += Invoke-CpuCase $mode 'HeadlessCpuSmoke' "$mode-headless"
        foreach ($depth in @('8u', '32f')) {
            foreach ($scale in @('1', '2')) {
                $cases += Invoke-CpuCase $mode 'RasterGuiSmoke' "$mode-$depth-scale$scale" $depth $scale
            }
        }
        foreach ($scale in @('1', '2')) {
            $cases += Invoke-CpuCase $mode 'RasterIOOverlaySmoke' "$mode-io-overlay-scale$scale" '32f' $scale
        }
    }
    $hashes = Get-ProofHashes
    foreach ($file in $hashes.Keys) {
        if ($hashes[$file] -ne $hashesBefore[$file]) { throw "CPU fallback input changed during validation: $file" }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesBefore -ne $preferencesAfter) { throw 'User settings changed during validation.' }
    $completedReport = @{ passed = $true; cases = $cases; fileSha256 = $hashes; preferencesSha256 = $preferencesAfter; fullVulkanMigrationComplete = $false }
} finally {
    foreach ($name in $variables) {
        # On current PowerShell/.NET, an empty string is a defined environment
        # value. A previously absent driver override must be removed explicitly.
        if ($null -eq $previous[$name]) {
            Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        } else {
            [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process')
        }
    }
}
foreach ($name in $variables) {
    if ([Environment]::GetEnvironmentVariable($name, 'Process') -cne $previous[$name]) { throw "Test environment was not restored: $name" }
}
$completedReport.environmentRestored = $true
$completedReport | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
