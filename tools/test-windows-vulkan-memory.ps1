[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build/vulkan-memory-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 150
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }
if (Test-Path -LiteralPath (Join-Path $destination 'report.json')) { throw 'Use a fresh report directory to preserve previous evidence.' }
New-Item -ItemType Directory -Force -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'NATRON_VULKAN_MEMORY_LIMIT_MB', 'NATRON_TEST_MEMORY_OVERSIZED',
               'NATRON_TEST_MEMORY_GUI_MODE', 'NATRON_VULKAN_VALIDATION', 'QT_SCALE_FACTOR')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$proofFiles = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Gpu/NatronGpu.lib', 'build/windows-vulkan/GpuTests/NatronVulkanMemoryBudget.exe',
    'build/ofx-tests/VulkanMemoryFailureSmoke.ofx.bundle/Contents/Win64/VulkanMemoryFailureSmoke.ofx',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx', 'build/Plugins/Misc.ofx.bundle/Contents/Win64/Misc.ofx',
    'Engine/EffectInstanceRenderRoI.cpp', 'Engine/EffectInstance.h', 'Engine/OfxEffectInstance.cpp',
    'Engine/OfxEffectInstance.h', 'Engine/OfxImageEffectInstance.cpp', 'Engine/OfxVulkanImageSuite.cpp',
    'Engine/ViewerInstance.cpp', 'Engine/OpenGLViewerI.h', 'Global/OpenFX/NatronVulkanRenderSuite.h',
    'Gui/ViewerGL.cpp', 'Gui/ViewerGL.h', 'Gui/ViewerGLPrivate.h', 'Gui/ViewerVulkan.cpp', 'Tests/VulkanMemoryFailurePlugin.cpp',
    'Gpu/VulkanDevice.h', 'Gpu/VulkanDevice.cpp', 'Gpu/FragmentProgram.cpp', 'Gpu/LutTexture.cpp',
    'Gpu/ViewerRenderer.cpp', 'Gpu/ViewerDisplayConversion.cpp', 'Gpu/ImageOperations.cpp',
    'Gpu/Qt/VulkanCanvas.h', 'Gpu/Qt/VulkanCanvas.cpp', 'Gpu/tests/MemoryBudget.cpp',
    'build-configs/windows-ofx/native/OcioVulkan.cpp', 'build-configs/windows-ofx/openfx-misc.patch',
    'tools/run-windows-qt6.ps1', 'tools/test-windows-vulkan.ps1', 'tools/test-windows-vulkan-memory.ps1',
    'tools/vulkan-memory-failure-smoke.py', 'tools/vulkan-memory-gui-smoke.py')
function Get-ProofHashes {
    $hashes = @{}
    foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath (Join-Path $repoRoot $file) -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$proofBefore = Get-ProofHashes
$settingsBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint Natron user settings.' }
$report = [ordered]@{ passed = $false; cases = @(); core = @(); proofHashes = $proofBefore; settingsBefore = $settingsBefore }
try {
    $env:QT_SCALE_FACTOR = '1'
    & ./tools/test-windows-vulkan.ps1 -Tests MemoryBudget -GpuIndices $GpuIndices -ReportDirectory (Join-Path $destination 'core') -TimeoutSeconds $TimeoutSeconds
    if ($LASTEXITCODE -ne 0) { throw 'Core memory tests failed.' }
    foreach ($gpu in $GpuIndices) {
        $core = Get-Content -LiteralPath (Join-Path $destination "core/gpu$gpu/MemoryBudget.stdout.log") -Raw | ConvertFrom-Json
        if (-not $core.passed -or $core.validation_errors -or $core.peak_block_bytes -gt $core.limit_bytes) { throw "Invalid core memory result for GPU $gpu" }
        $report.core += @{ gpu = $gpu; result = $core }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($oversized in @('0', '1')) {
            $name = "engine-gpu$gpu-oversized$oversized"
            $capture = Join-Path $destination $name
            New-Item -ItemType Directory -Force -Path $capture | Out-Null
            $env:NATRON_VULKAN_MEMORY_LIMIT_MB = '32'
            $env:NATRON_TEST_MEMORY_OVERSIZED = $oversized
            & ./tools/run-windows-qt6.ps1 -Program MemoryFailureSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
            if ($LASTEXITCODE -ne 0) { throw "$name failed." }
            foreach ($file in @('result.json', 'reference.png', 'fallback.png')) {
                Copy-Item -LiteralPath (Join-Path $repoRoot "build/vulkan-memory-failure-smoke/$file") -Destination (Join-Path $capture $file)
            }
            $result = Get-Content -LiteralPath (Join-Path $capture 'result.json') -Raw | ConvertFrom-Json
            if (-not $result.ok -or -not $result.cpu_retry -or -not $result.gpu_only_failed) { throw "Invalid $name result" }
            $report.cases += @{ name = $name; gpu = $gpu; kind = 'engine'; result = $result }
            Write-Output "$name passed."
        }
        $env:NATRON_VULKAN_MEMORY_LIMIT_MB = '64'
        foreach ($case in @(@{ mode = 'transfer'; depth = '32f' }, @{ mode = 'native-transfer'; depth = '32f' }, @{ mode = 'canvas'; depth = '8u' }, @{ mode = 'canvas'; depth = '32f' })) {
            $name = "viewer-gpu$gpu-$($case.mode)-$($case.depth)"
            $capture = Join-Path $destination $name
            New-Item -ItemType Directory -Force -Path $capture | Out-Null
            $env:NATRON_TEST_MEMORY_GUI_MODE = $case.mode
            & ./tools/run-windows-qt6.ps1 -Program MemoryGuiSmoke -GuiTextureDepth $case.depth -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $capture 'run.log')
            if ($LASTEXITCODE -ne 0) { throw "$name failed." }
            foreach ($file in @('result.json', 'recovered.png')) {
                Copy-Item -LiteralPath (Join-Path $repoRoot "build/vulkan-memory-gui-smoke/$file") -Destination (Join-Path $capture $file)
            }
            $result = Get-Content -LiteralPath (Join-Path $capture 'result.json') -Raw | ConvertFrom-Json
            if (-not $result.ok -or $result.validation_errors -or $result.unexpected_dialogs.Count) { throw "Invalid $name result" }
            $report.cases += @{ name = $name; gpu = $gpu; kind = 'viewer'; result = $result }
            Write-Output "$name passed."
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
