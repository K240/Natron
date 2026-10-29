[CmdletBinding()]
param(
    [string]$SdkRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$OutputDirectory = 'build\vulkan-probe'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $repoRoot $OutputDirectory
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$SdkRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$vulkanInfo = Join-Path $SdkRoot 'Bin\vulkaninfoSDK.exe'
if (-not (Test-Path -LiteralPath $vulkanInfo)) {
    $vulkanInfo = Join-Path $SdkRoot 'Bin\vulkaninfo.exe'
}
$glslang = Join-Path $SdkRoot 'Bin\glslangValidator.exe'
$spirvVal = Join-Path $SdkRoot 'Bin\spirv-val.exe'
$headerPath = Join-Path $SdkRoot 'Include\vulkan\vulkan_core.h'
foreach ($requiredFile in @($vulkanInfo, $glslang, $spirvVal, $headerPath)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "Required Vulkan SDK file not found: $requiredFile"
    }
}

function Invoke-ProbeTool {
    param([string]$Command, [string[]]$Arguments, [string]$LogName)
    $outputLines = @(& $Command @Arguments 2> (Join-Path $OutputDirectory "$LogName.stderr.log"))
    if ($LASTEXITCODE -ne 0) {
        throw "$Command failed with exit code $LASTEXITCODE. See $LogName.stderr.log."
    }
    $outputText = $outputLines -join "`n"
    Set-Content -LiteralPath (Join-Path $OutputDirectory "$LogName.stdout.log") -Value $outputText -Encoding utf8
    return $outputText
}

function Get-VulkanVersion {
    param([uint32]$PackedVersion)
    return '{0}.{1}.{2}' -f (($PackedVersion -shr 22) -band 127), (($PackedVersion -shr 12) -band 1023), ($PackedVersion -band 4095)
}

$previousSdk = $env:VULKAN_SDK
$previousPath = $env:PATH
try {
    # The desktop process may still inherit an older SDK after a new installation.
    $env:VULKAN_SDK = $SdkRoot
    $env:PATH = "$(Join-Path $SdkRoot 'Bin');$previousPath"
    $headerText = Get-Content -LiteralPath $headerPath -Raw
    $headerVersion = [regex]::Match($headerText, '(?m)^#define VK_HEADER_VERSION\s+(\d+)\s*$')
    if (-not $headerVersion.Success) { throw 'Cannot identify the Vulkan SDK header version.' }

    $summary = Invoke-ProbeTool $vulkanInfo @('--summary') 'summary'
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'summary.txt') -Value $summary -Encoding utf8
    $instanceVersion = [regex]::Match($summary, 'Vulkan Instance Version:\s*(\S+)')
    $gpuHeaders = [regex]::Matches($summary, '(?m)^GPU(?<index>\d+):\s*$')
    if (-not $instanceVersion.Success -or $gpuHeaders.Count -eq 0) {
        throw 'vulkaninfo did not report a Vulkan instance and physical devices.'
    }

    $textPath = Join-Path $OutputDirectory 'vulkaninfo.txt'
    $null = Invoke-ProbeTool $vulkanInfo @('--text', '--show-formats', '--output', $textPath) 'vulkaninfo'
    $fullText = Get-Content -LiteralPath $textPath -Raw
    $deviceText = @{}
    foreach ($block in [regex]::Matches($fullText, '(?ms)^GPU(?<index>\d+):\r?\n(?<body>.*?)(?=^GPU\d+:\r?\n|\z)')) {
        $deviceText[[int]$block.Groups['index'].Value] = $block.Groups['body'].Value
    }

    $formatNames = @('VK_FORMAT_R8G8B8A8_UNORM', 'VK_FORMAT_R16G16B16A16_UNORM',
        'VK_FORMAT_R16G16B16A16_SFLOAT', 'VK_FORMAT_R32G32B32A32_SFLOAT')
    $formatRequirements = @('VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT', 'VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT',
        'VK_FORMAT_FEATURE_TRANSFER_SRC_BIT', 'VK_FORMAT_FEATURE_TRANSFER_DST_BIT')
    $devices = @()
    foreach ($gpuHeader in $gpuHeaders) {
        $gpuIndex = [int]$gpuHeader.Groups['index'].Value
        $profilePath = Join-Path $OutputDirectory "gpu-$gpuIndex.json"
        $null = Invoke-ProbeTool $vulkanInfo @("--json=$gpuIndex", '--output', $profilePath) "gpu-$gpuIndex"
        $caps = (Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json).capabilities.device
        if (-not $caps) { throw "GPU $gpuIndex profile has no device capabilities." }
        $properties = $caps.properties.VkPhysicalDeviceProperties
        $apiVersion = Get-VulkanVersion $properties.apiVersion
        $features = [ordered]@{
            timelineSemaphore = $caps.features.VkPhysicalDeviceVulkan12Features.timelineSemaphore -eq $true
            synchronization2 = $caps.features.VkPhysicalDeviceVulkan13Features.synchronization2 -eq $true
            dynamicRendering = $caps.features.VkPhysicalDeviceVulkan13Features.dynamicRendering -eq $true
        }
        $formats = @()
        foreach ($formatName in $formatNames) {
            $flags = @($caps.formats.$formatName.VkFormatProperties.optimalTilingFeatures)
            $missingFlags = @($formatRequirements | Where-Object { $_ -notin $flags })
            $formats += [pscustomobject]@{
                format = $formatName
                sampledStorageTransferSupported = $missingFlags.Count -eq 0
                missingRequiredFeatures = $missingFlags
                optimalTilingFeatures = $flags
            }
        }

        $presentByFamily = @{}
        foreach ($queueMatch in [regex]::Matches([string]$deviceText[$gpuIndex],
            '(?ms)^\tqueueProperties\[(?<index>\d+)\]:.*?^\t\tpresent support\s*=\s*(?<supported>true|false)')) {
            $presentByFamily[[int]$queueMatch.Groups['index'].Value] = $queueMatch.Groups['supported'].Value -eq 'true'
        }
        $queues = @()
        $queueIndex = 0
        foreach ($queueEntry in $caps.queueFamiliesProperties) {
            $queue = $queueEntry.VkQueueFamilyProperties
            $queues += [pscustomobject]@{
                index = $queueIndex
                count = $queue.queueCount
                flags = @($queue.queueFlags)
                win32PresentSupported = $presentByFamily[$queueIndex]
            }
            $queueIndex++
        }
        $graphicsComputePresent = @($queues | Where-Object {
            'VK_QUEUE_GRAPHICS_BIT' -in $_.flags -and 'VK_QUEUE_COMPUTE_BIT' -in $_.flags -and $_.win32PresentSupported -eq $true
        })
        $allFormatsSupported = @($formats | Where-Object { -not $_.sampledStorageTransferSupported }).Count -eq 0
        $allFeaturesSupported = @($features.Values | Where-Object { -not $_ }).Count -eq 0
        $swapchainSupported = $null -ne $caps.extensions.PSObject.Properties['VK_KHR_swapchain']
        $devices += [pscustomobject]@{
            index = $gpuIndex
            name = $properties.deviceName
            type = $properties.deviceType
            vendorId = $properties.vendorID
            deviceId = $properties.deviceID
            apiVersion = $apiVersion
            maxImageDimension2D = $properties.limits.maxImageDimension2D
            features = $features
            formats = $formats
            queues = $queues
            swapchainSupported = $swapchainSupported
            meetsProposedBaseline = ([version]$apiVersion -ge [version]'1.3.0') -and $allFeaturesSupported -and $allFormatsSupported -and $swapchainSupported -and ($graphicsComputePresent.Count -gt 0)
        }
    }

    $glslangVersion = Invoke-ProbeTool $glslang @('--version') 'glslang-version'
    $spirvVersion = Invoke-ProbeTool $spirvVal @('--version') 'spirv-version'
    $shaderPath = Join-Path $OutputDirectory 'toolchain-smoke.comp'
    $spirvPath = Join-Path $OutputDirectory 'toolchain-smoke.comp.spv'
    Set-Content -LiteralPath $shaderPath -Encoding ascii -Value @'
#version 450
layout(local_size_x = 16) in;
layout(set = 0, binding = 0, std430) buffer Output { float values[]; } outputBuffer;
void main() { outputBuffer.values[gl_GlobalInvocationID.x] = 0.5; }
'@
    $null = Invoke-ProbeTool $glslang @('-V', '--target-env', 'vulkan1.3', '-S', 'comp', '-o', $spirvPath, $shaderPath) 'shader-compile'
    $null = Invoke-ProbeTool $spirvVal @('--target-env', 'vulkan1.3', $spirvPath) 'shader-validate'

    $warnings = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.stderr.log' | ForEach-Object {
        Get-Content -LiteralPath $_.FullName | Where-Object { $_ -match 'WARNING:|ERROR:' }
    } | Sort-Object -Unique)
    $report = [ordered]@{
        generatedAt = [DateTimeOffset]::Now.ToString('o')
        sdkRoot = $SdkRoot
        sdkHeaderVersion = [int]$headerVersion.Groups[1].Value
        inheritedSdkRoot = $previousSdk
        instanceVersion = $instanceVersion.Groups[1].Value
        minimumRuntimeApiVersion = '1.3.0'
        validationLayerAvailable = $summary.Contains('VK_LAYER_KHRONOS_validation')
        shaderCompilationAndValidationPassed = $true
        glslangVersion = $glslangVersion
        spirvToolsVersion = $spirvVersion
        devices = $devices
        warnings = $warnings
        runtimeComputeTested = $false
        qtViewerPresentationTested = $false
        recommendedDeviceIndex = $null
    }
    $preferredDevices = @($devices | Where-Object { $_.meetsProposedBaseline } | Sort-Object { $_.type -ne 'VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU' })
    if ($preferredDevices.Count -gt 0) { $report.recommendedDeviceIndex = $preferredDevices[0].index }
    $reportPath = Join-Path $OutputDirectory 'report.json'
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding utf8
    $devices | Select-Object index, name, apiVersion, meetsProposedBaseline | Format-Table -AutoSize
    Write-Host "Vulkan capability report: $reportPath"
    if ($preferredDevices.Count -eq 0) { throw 'No device meets the proposed Vulkan capability baseline. See report.json.' }
} finally {
    $env:VULKAN_SDK = $previousSdk
    $env:PATH = $previousPath
}
