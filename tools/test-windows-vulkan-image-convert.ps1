[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [string]$ReportDirectory = 'build\vulkan-image-convert-tests',
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
if ($GpuIndices.Count -eq 0 -or @($GpuIndices | Select-Object -Unique).Count -ne $GpuIndices.Count -or
    @($GpuIndices | Where-Object { $_ -lt 0 }).Count -gt 0) { throw 'Specify distinct nonnegative GPU indices.' }
$proofFiles = @(
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Tests/NatronVulkanImageConvert.exe',
    'Engine/Image.cpp', 'Engine/Image.h', 'Engine/ImageVulkan.cpp', 'Engine/ImageConvert.cpp',
    'Engine/Lut.cpp', 'Engine/Lut.h', 'Engine/ImageParams.h',
    'Engine/CacheEntry.h', 'Engine/NonKeyParams.h', 'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h',
    'Gpu/ImageOperations.cpp', 'Gpu/ImageOperations.h', 'Gpu/shaders/image-operations.comp',
    'Gpu/VulkanDevice.cpp', 'Gpu/VulkanDevice.h',
    'Gpu/CMakeLists.txt', 'Engine/CMakeLists.txt', 'Tests/CMakeLists.txt', 'Global/Enums.h',
    'Tests/VulkanImageConvert.cpp', 'tools/run-windows-qt6.ps1',
    'tools/test-windows-vulkan-image-convert.ps1', 'tools/test-settings-fingerprint.py'
)
$hashes = @{}
foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferences = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint user preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$cases = @()
function Save-Proof([bool]$complete) {
    $path = if ($complete) { $reportPath } else { Join-Path $ReportDirectory 'report.partial.json' }
    @{passed=$complete;fileSha256=$hashes;preferencesSha256=$preferences;cases=$cases;
      finishedAt=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath "$path.tmp" -Encoding utf8
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
}
try {
    Save-Proof $false
    foreach ($index in $GpuIndices) {
        $env:NATRON_VULKAN_DEVICE = [string]$index
        $directory = Join-Path $ReportDirectory "gpu$index"
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
        $runLog = Join-Path $directory 'run.log'
        & tools/run-windows-qt6.ps1 -Program ImageConvertSmoke -GuiTimeoutSeconds $TimeoutSeconds *> $runLog
        if ($LASTEXITCODE -ne 0) { throw "Image conversion failed on GPU $index. See $runLog" }
        $resultPath = 'build/image-convert-smoke/result.json'
        $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        if (-not $result.ok -or -not $result.passed -or $result.validationErrors -ne 0 -or
            $result.referenceCases -ne 1218 -or $result.cpuToVulkanCases -ne 384 -or
            $result.vulkanToCpuCases -ne 768 -or $result.nativeCases -ne 64 -or
            $result.crossContextCases -ne 2 -or $result.nativeTransfers -ne 0 -or -not $result.bitmapAndRoi) {
            throw "Incomplete image conversion proof on GPU $index."
        }
        $profileMatch = Select-String -LiteralPath $runLog -Pattern '^GUI smoke profile and logs: (.+)$' | Select-Object -Last 1
        if (-not $profileMatch) { throw "No completed process/profile proof on GPU $index." }
        $profile = $profileMatch.Matches[0].Groups[1].Value.Trim()
        if (Test-Path -LiteralPath (Join-Path $profile 'timeout.json')) { throw 'Test process timed out.' }
        Copy-Item -LiteralPath $resultPath -Destination (Join-Path $directory 'result.json')
        foreach ($file in @('stdout.log', 'stderr.log')) {
            Copy-Item -LiteralPath (Join-Path $profile $file) -Destination (Join-Path $directory $file)
        }
        $artifacts = @{}
        foreach ($file in @('result.json', 'run.log', 'stdout.log', 'stderr.log')) {
            $artifacts[$file] = (Get-FileHash -LiteralPath (Join-Path $directory $file)).Hash.ToLowerInvariant()
        }
        $cases += @{passed=$true;gpu=$index;device=$result.device;referenceCases=$result.referenceCases;
            nativeCases=$result.nativeCases;nativeTransfers=$result.nativeTransfers;maxAbsoluteError=$result.maxAbsoluteError;
            validationErrors=$result.validationErrors;bitmapAndRoi=$result.bitmapAndRoi;processExited=$true;
            profile=$profile;directory=$directory;fileSha256=$artifacts}
        Save-Proof $false
        Write-Output "Image conversion GPU $index passed: $($result.device), 1218 references, no CPU transfers for native conversions."
    }
    foreach ($file in $proofFiles) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Build or test source changed: $file" }
    }
    $preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferences) { throw 'User preferences changed during conversion tests.' }
    Save-Proof $true
    Write-Output "Image conversion proof: $reportPath"
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
}
exit 0
