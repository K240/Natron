[CmdletBinding()]
param([string]$Destination = 'C:\VulkanSDK\1.4.363.0')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath "$repoRoot/build-configs/vulkan/windows-sdk.json" -Raw | ConvertFrom-Json
if (-not (Test-Path -LiteralPath "$Destination/Include/vulkan/vulkan_core.h")) {
    if (Test-Path -LiteralPath $Destination) { throw 'SDK destination already exists but is incomplete.' }
    $installer = Join-Path $repoRoot "build/vulkansdk-windows-X64-$($lock.version).exe"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $installer) | Out-Null
    Invoke-WebRequest -Uri $lock.url -OutFile $installer
    if ((Get-FileHash -LiteralPath $installer).Hash.ToLowerInvariant() -ne $lock.sha256) { throw 'Vulkan SDK installer hash mismatch.' }
    # LunarG's supported unattended copy-only mode leaves global registration alone.
    # https://vulkan.lunarg.com/doc/view/1.4.304.1/windows/getting_started.html
    $process = Start-Process -FilePath $installer -ArgumentList @('--root',"`"$Destination`"",'--accept-licenses','--default-answer',
        '--confirm-command','install','com.lunarg.vulkan.volk','com.lunarg.vulkan.vma','copy_only=1') -WindowStyle Hidden -PassThru
    try {
        while (-not $process.WaitForExit(1000)) { }
        if ($process.ExitCode -ne 0) { throw "SDK installer failed: $($process.ExitCode)" }
    } finally { $process.Dispose() }
}
foreach ($required in @('Include/vulkan/vulkan_core.h','Include/Volk/volk.h','Include/vma/vk_mem_alloc.h',
    'Lib/shaderc_combined.lib','Bin/glslangValidator.exe','Bin/spirv-val.exe','Bin/VkLayer_khronos_validation.json')) {
    if (-not (Test-Path -LiteralPath "$Destination/$required")) { throw "SDK file missing: $required" }
}
$header = Get-Content -LiteralPath "$Destination/Include/vulkan/vulkan_core.h" -Raw
if ($header -notmatch '(?m)^#define VK_HEADER_VERSION\s+363\s*$') { throw 'Unexpected SDK header version.' }
$env:VULKAN_SDK = (Resolve-Path -LiteralPath $Destination).Path
$env:VK_LAYER_PATH = Join-Path $env:VULKAN_SDK 'Bin'
Write-Output "Vulkan SDK $($lock.version) ready: $env:VULKAN_SDK"
