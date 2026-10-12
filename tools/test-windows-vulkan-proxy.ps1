[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('8u', '32f')]
    [string[]]$TextureDepths = @('8u', '32f'),
    [string]$ReportDirectory = 'build\vulkan-proxy-tests',
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$proofFiles = @(
    'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/Tests/Tests.exe',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx', 'build/Plugins/Misc.ofx.bundle/Contents/Win64/Misc.ofx',
    'build/ofx-tests/VulkanImageSuiteSmoke.ofx.bundle/Contents/Win64/VulkanImageSuiteSmoke.ofx',
    'Tests/VulkanImageSuitePlugin.cpp', 'Engine/EffectInstanceRenderRoI.cpp', 'Engine/EffectInstance.cpp',
    'Engine/ParallelRenderArgs.cpp', 'Engine/OfxClipInstance.cpp', 'Engine/OfxEffectInstance.cpp',
    'Global/OpenFX/NatronVulkanRenderSuite.h', 'Engine/OfxVulkanImageSuite.cpp',
    'Engine/Image.cpp', 'Engine/Image.h', 'Tests/Image_Test.cpp', 'Engine/ImageVulkan.cpp', 'Engine/VulkanImageContextPool.cpp',
    'Engine/VulkanImageStorage.cpp', 'Engine/VulkanImageStorage.h', 'Gpu/ImageOperations.cpp', 'Gpu/ImageOperations.h',
    'Engine/ImageKey.cpp', 'Engine/ImageKey.h', 'Engine/ImageSerialization.h',
    'Gpu/shaders/image-operations.comp',
    'build-configs/windows-ofx/native/OcioVulkan.cpp', 'build-configs/windows-ofx/native/OcioVulkanAdapter.cpp',
    'build-configs/windows-ofx/native/ShadertoyVulkan.cpp', 'Gui/ViewerGL.cpp', 'Engine/ViewerInstance.cpp',
    'tools/run-windows-qt6.ps1', 'tools/vulkan-proxy-gui-smoke.py', 'tools/test-windows-vulkan-proxy.ps1',
    'tools/test-settings-fingerprint.py'
)
$hashes = @{}
foreach ($file in $proofFiles) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$preferences = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint user preferences.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$cases = @()
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
function Save-Proof([bool]$complete) {
    $path = if ($complete) { $reportPath } else { Join-Path $ReportDirectory 'report.partial.json' }
    @{passed=$complete;fileSha256=$hashes;preferencesSha256=$preferences;cases=$cases;finishedAt=[DateTime]::UtcNow.ToString('o');
      mipmapVariants=162;upscaleVariants=48;cacheKeySerializationVersion=3;mipmapLogSha256=$mipmapLogHash} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$path.tmp" -Encoding utf8
    Move-Item -LiteralPath "$path.tmp" -Destination $path -Force
}
try {
    Save-Proof $false
    $mipmapLog = Join-Path $ReportDirectory 'mipmap-run.log'
    & tools/run-windows-qt6.ps1 -Program Tests -ProgramArguments '--gtest_filter=ImageMipmapTest.*:ImageUpscaleTest.*:ImageKeyTest.SerializedProxyInputQuality' *> $mipmapLog
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $mipmapLog -SimpleMatch '[  PASSED  ] 4 tests.')) {
        throw "CPU mipmap edge tests failed. Logs: $mipmapLog"
    }
    $mipmapLogHash = (Get-FileHash -LiteralPath $mipmapLog).Hash.ToLowerInvariant()
    foreach ($gpu in $GpuIndices) {
        if ($gpu -lt 0) { throw 'GPU index must be nonnegative.' }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($depth in $TextureDepths) {
            $directory = Join-Path $ReportDirectory "gpu$gpu/$depth"
            New-Item -ItemType Directory -Path $directory -Force | Out-Null
            & tools/run-windows-qt6.ps1 -Program VulkanProxySmoke -GuiTextureDepth $depth -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $directory 'run.log')
            if ($LASTEXITCODE -ne 0) { throw "Vulkan proxy GPU $gpu $depth failed. Logs: $directory" }
            $result = Get-Content -LiteralPath 'build/vulkan-proxy-gui-smoke/result.json' -Raw | ConvertFrom-Json -AsHashtable
            if (-not $result.ok -or $result.cases.Count -ne 60 -or $result.unexpected_dialogs.Count -ne 0 -or
                ($result.validation_errors | Where-Object { $_ -ne 0 }) -or ($result.graphics_errors | Where-Object { $_ }) -or
                $result.native_pixel_transfers -ne 0 -or $result.native_scale_renders.Count -ne 6 -or
                ($result.native_scale_renders | Where-Object { $_ -lt 2 })) { throw 'Invalid proxy render proof.' }
            foreach ($graph in @('native-chain', 'ocio-chain', 'shader-scale', 'cached-native-chain', 'cached-ocio-chain',
                                'fullscale-proxy-inputs', 'fullscale-highdef-inputs', 'fullscale-mask-mix',
                                'cached-fullscale-proxy-inputs', 'cached-fullscale-highdef-inputs')) {
                $graphCases = @($result.cases | Where-Object { $_.graph -eq $graph })
                if ($graphCases.Count -ne 6 -or (($graphCases.level | Sort-Object) -join ',') -ne '0,1,2,3,4,5') { throw 'Incomplete proxy scale matrix.' }
                foreach ($case in $graphCases) {
                    $cachedMipmap = $graph.StartsWith('cached-') -and $graph -ne 'cached-fullscale-proxy-inputs' -and $case.level -gt 0
                    if ($case.pixels_compared -ne 425 -or (-not $cachedMipmap -and $case.native_renders -lt 1)) { throw 'Missing native proxy render/pixel comparison.' }
                    if ($graph -eq 'shader-scale') {
                        if (-not $case.uniforms) { throw 'Missing scaled shader uniform comparison.' }
                    } elseif ((-not $cachedMipmap -and $case.native_renders -lt 2) -or $case.cache_renders -ne 0 -or $case.maximum_byte_error -gt 2 -or
                              $case.cache_maximum_byte_error -ne 0 -or -not $case.native_viewer_copy -or
                              ($case.cache_pixel_transfers -join ',') -ne '0,0') {
                        throw 'Proxy chain, cache or CPU reference failed.'
                    }
                    if ($cachedMipmap -and ($case.native_renders -ne 0 -or $case.mipmap_transfers.Count -ne 2 -or
                        $case.mipmap_transfers[0] -ne 0 -or $case.mipmap_transfers[1] -ne 0)) { throw 'Native cached mipmap rendered again or transferred CPU pixels.' }
                    if ($graph.Contains('fullscale-') -and (-not $case.fullscale_action -or
                        ($case.fullscale_transfers -join ',') -ne $(if ($cachedMipmap) { '0,0' } elseif ($graph -eq 'fullscale-mask-mix') { '3,0' } else { '1,0' }))) {
                        throw 'Full-scale render grid or CPU boundary transfers differ.'
                    }
                }
            }
            foreach ($file in $result.artifact_sha256.Keys) {
                $path = Join-Path 'build/vulkan-proxy-gui-smoke' $file
                if ((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -ne $result.artifact_sha256[$file]) { throw "Proxy artifact changed after exit: $file" }
                Copy-Item -LiteralPath $path -Destination (Join-Path $directory $file)
            }
            Copy-Item -LiteralPath 'build/vulkan-proxy-gui-smoke/result.json' -Destination (Join-Path $directory 'result.json')
            foreach ($file in @('stdout.log', 'stderr.log')) {
                Copy-Item -LiteralPath (Join-Path $result.profile $file) -Destination (Join-Path $directory $file)
            }
            $artifactHashes = @{}
            foreach ($file in Get-ChildItem -LiteralPath $directory -File) { $artifactHashes[$file.Name] = (Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant() }
            $cases += @{gpu=$gpu;depth=$depth;device=$result.devices[0];passed=$true;comparisons=60;scales=6;cachedMipmapComparisons=15;fullScaleComparisons=30;
                        nativeScaleRenders=$result.native_scale_renders;nativePixelTransfers=0;unexpectedDialogs=0;
                        directory=$directory;fileSha256=$artifactHashes}
            Save-Proof $false
            Write-Output "Vulkan proxy GPU $gpu $depth passed: 60 comparisons, 6 scales, 30 full-scale comparisons."
        }
    }
    foreach ($file in $proofFiles) {
        if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Build/test source changed: $file" }
    }
    $after = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $after -ne $preferences) { throw 'User preferences changed during proxy tests.' }
    Save-Proof $true
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
}
