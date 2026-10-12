[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build\writer-vulkan-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 300
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$files = @(
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx',
    'build/ofx-tests/OcioVulkanSuiteSmoke.ofx.bundle/Contents/Win64/OcioVulkanSuiteSmoke.ofx',
    'Engine/WriteNode.cpp', 'Engine/WriteNode.h', 'Engine/EffectInstanceRenderRoI.cpp', 'Engine/OutputSchedulerThread.cpp',
    'build-configs/windows-ofx/native/OcioVulkan.cpp', 'build-configs/windows-ofx/native/OcioVulkan.h',
    'build-configs/windows-ofx/native/OcioVulkanAdapter.cpp', 'build-configs/windows-ofx/native/OcioVulkanAdapter.h',
    'build-configs/windows-ofx/native/IOVulkan.def', 'build-configs/windows-ofx/openfx-io.patch',
    'build-configs/windows-ofx/native/NatronOverlayInteract.h', 'build-configs/windows-ofx/openfx-io-supportext-vulkan.patch',
    'Gui/Gui15.cpp', 'Gui/GuiPrivate.h',
    'Tests/OcioVulkanSuitePlugin.cpp', 'tools/run-windows-qt6.ps1', 'tools/writer-vulkan-smoke.py',
    'tools/compare-writer-vulkan.py', 'tools/test-windows-vulkan-writer.ps1', 'tools/test-settings-fingerprint.py'
)
$hashes = @{}
foreach ($file in $files) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferences = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Cannot fingerprint preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$previousChain = $env:NATRON_TEST_WRITER_CHAIN_ONLY
$cases = @()
function Save-Proof([bool]$completed) {
    @{completed=$completed;inputs=$hashes;preferencesBefore=$preferences;cases=$cases} |
        ConvertTo-Json -Depth 30 | Set-Content -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
}
try {
    $env:NATRON_TEST_WRITER_CHAIN_ONLY = $null
    Save-Proof $false
    foreach ($gpu in $GpuIndices) {
        if ($gpu -lt 0) { throw 'GPU index must be nonnegative.' }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        $directory = Join-Path $ReportDirectory "gpu$gpu"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        & tools/run-windows-qt6.ps1 -Program WriterEngineVulkanSmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
        if ($LASTEXITCODE -ne 0) { throw "Writer GPU $gpu failed: $directory" }
        & uv run --frozen python tools/compare-writer-vulkan.py *> (Join-Path $directory 'comparison.log')
        if ($LASTEXITCODE -ne 0) { throw "Writer pixel comparison GPU $gpu failed: $directory" }
        $result = Get-Content -LiteralPath 'build/writer-vulkan-smoke/comparison.json' -Raw | ConvertFrom-Json -AsHashtable
        if (-not $result.ok -or $result.cases.Count -ne 154 -or -not $result.project_roundtrip -or
            -not $result.algorithm.passed -or $result.algorithm.cases -ne 240 -or $result.validation_errors -ne 0 -or
            -not $result.comparison.passed -or $result.comparison.source_channel_cases -ne 50 -or
            $result.comparison.samples -ne 730742 -or $result.native_chain.transfers[0] -ne 1 -or
            $result.native_chain.transfers[1] -ne 2 -or -not $result.native_chain.pass_through_original) {
            throw 'Incomplete Writer proof.'
        }
        Copy-Item -LiteralPath 'build/writer-vulkan-smoke/comparison.json' -Destination (Join-Path $directory 'result.json')
        $artifacts = @{}
        foreach ($name in $result.artifact_sha256.Keys) {
            if ([IO.Path]::GetFileName($name) -ne $name) { throw 'Artifact name must be local.' }
            $source = Join-Path $repoRoot "build/writer-vulkan-smoke/$name"
            if ((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$name]) { throw "Changed artifact $name" }
            $destination = Join-Path $directory $name
            Copy-Item -LiteralPath $source -Destination $destination
            $artifacts[$name] = (Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant()
            if ($artifacts[$name] -ne $result.artifact_sha256[$name]) { throw "Artifact copy mismatch $name" }
        }
        foreach ($name in @('stdout.log', 'stderr.log', 'stdin.txt', 'started.txt')) {
            $source = Join-Path $result.profile $name
            Copy-Item -LiteralPath $source -Destination (Join-Path $directory $name)
            $artifacts[$name] = (Get-FileHash -LiteralPath (Join-Path $directory $name)).Hash.ToLowerInvariant()
        }
        if (-not (Select-String -LiteralPath (Join-Path $directory 'stdout.log') -Pattern 'NATRON_INTERPRETER_EOF_OK' -Quiet) -or
            (Test-Path -LiteralPath (Join-Path $result.profile 'timeout.json'))) { throw 'Process did not close normally.' }
        $cases += @{gpu=$gpu;directory=$directory;result=$result;fileSha256=$artifacts}
        Save-Proof $false
    }
    foreach ($file in $files) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Proof input changed: $file" }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferences) { throw 'User preferences changed.' }
    Save-Proof $true
    Write-Output "Writer Vulkan proof: $(Join-Path $ReportDirectory 'report.json')"
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
    $env:NATRON_TEST_WRITER_CHAIN_ONLY = $previousChain
}
