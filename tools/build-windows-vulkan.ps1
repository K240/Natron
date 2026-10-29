[CmdletBinding()]
param(
    [string]$SdkRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$BuildDirectory = 'build\windows-vulkan',
    [int]$Jobs = 16
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio x64 C++ tools are required.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot $BuildDirectory }
$ninja = Join-Path (Split-Path (Split-Path $QtRoot)) 'Tools\Ninja\ninja.exe'
$previousSdk = $env:VULKAN_SDK
try {
    $env:VULKAN_SDK = $SdkRoot
    $configure = @('-S', (Join-Path $repoRoot 'build-configs\vulkan'), '-B', $BuildDirectory, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_PREFIX_PATH=$QtRoot",
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', "-DNATRON_VULKAN_SDK_ROOT=$SdkRoot",
        "-DVulkan_INCLUDE_DIR=$SdkRoot\Include", "-DVulkan_LIBRARY=$SdkRoot\Lib\vulkan-1.lib",
        "-DVulkan_GLSLANG_VALIDATOR_EXECUTABLE=$SdkRoot\Bin\glslangValidator.exe")
    & cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan CMake configuration failed.' }
    & cmake --build $BuildDirectory --parallel $Jobs
    if ($LASTEXITCODE -ne 0) { throw 'Vulkan build failed.' }
} finally {
    $env:VULKAN_SDK = $previousSdk
}
