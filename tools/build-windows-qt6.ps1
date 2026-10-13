[CmdletBinding()]
param(
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$VulkanSdkRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$VcpkgRoot = 'D:\work\apps\vcpkg',
    [string]$BuildDirectory = 'build\windows-qt6',
    [int]$Jobs = 16,
    [switch]$ConfigureOnly,
    [switch]$SkipDependencies
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio with the x64 C++ tools is required.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$llvmRoot = Join-Path $repoRoot 'build\llvm20'
if (-not (Test-Path -LiteralPath (Join-Path $llvmRoot 'lib\clang\20\include'))) {
    New-Item -Path (Join-Path $repoRoot 'build') -ItemType Directory -Force | Out-Null
    $clangArchive = Join-Path $repoRoot 'build\clang-20.1.8.src.tar.xz'
    Invoke-WebRequest -Uri 'https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/clang-20.1.8.src.tar.xz' -OutFile $clangArchive
    Invoke-Checked -Command tar -Arguments @('-xf', $clangArchive, '-C', (Join-Path $repoRoot 'build'), 'clang-20.1.8.src/lib/Headers')
    New-Item -Path (Join-Path $llvmRoot 'lib\clang\20') -ItemType Directory -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot 'build\clang-20.1.8.src\lib\Headers') -Destination (Join-Path $llvmRoot 'lib\clang\20\include') -Recurse
}
$env:LLVM_INSTALL_DIR = $llvmRoot

Invoke-Checked -Command uv -Arguments @('sync', '--frozen')
$qtVersion = & (Join-Path $QtRoot 'bin\qtpaths.exe') --qt-version
if ($LASTEXITCODE -ne 0) { throw 'Could not read the Qt SDK version.' }
$pysideVersion = & uv run --frozen python -c 'import PySide6; print(PySide6.__version__)'
if ($LASTEXITCODE -ne 0) { throw 'Could not read the PySide6 version.' }
if ($qtVersion -ne $pysideVersion) {
    throw "Qt SDK $qtVersion and PySide6 $pysideVersion must match. Update pyproject.toml and uv.lock together when changing Qt versions."
}
Invoke-Checked -Command git -Arguments @('submodule', 'update', '--init', '--recursive', '--jobs', '8')

if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'OpenColorIO-Configs'))) {
    $archive = Join-Path $repoRoot 'build\Natron-v2.5.tar.gz'
    New-Item -ItemType Directory -Force -Path (Split-Path $archive) | Out-Null
    Invoke-WebRequest -Uri 'https://github.com/NatronGitHub/OpenColorIO-Configs/archive/refs/tags/Natron-v2.5.tar.gz' -OutFile $archive
    Invoke-Checked -Command tar -Arguments @('-xzf', $archive)
    Move-Item -LiteralPath (Join-Path $repoRoot 'OpenColorIO-Configs-Natron-v2.5') -Destination (Join-Path $repoRoot 'OpenColorIO-Configs')
}

$depsRoot = Join-Path $repoRoot 'build\vcpkg_installed'
$env:VCPKG_ROOT = $VcpkgRoot
if (-not $SkipDependencies) {
    Invoke-Checked -Command (Join-Path $VcpkgRoot 'vcpkg.exe') -Arguments @(
        'install', "--x-manifest-root=$repoRoot\build-configs\windows-qt6",
        "--x-install-root=$depsRoot", '--triplet=x64-windows', '--host-triplet=x64-windows'
    )
}

$sitePackages = Join-Path $repoRoot '.venv\Lib\site-packages'
$pythonExe = Join-Path $repoRoot '.venv\Scripts\python.exe'
$env:PKG_CONFIG_PATH = "$depsRoot\x64-windows\lib\pkgconfig;$depsRoot\x64-windows\share\pkgconfig"
$env:PATH = "$QtRoot\bin;$depsRoot\x64-windows\bin;$env:PATH"
$prefixes = @($QtRoot, "$sitePackages\PySide6", "$sitePackages\shiboken6", "$sitePackages\shiboken6_generator") -join ';'
$ninja = Join-Path (Split-Path (Split-Path $QtRoot)) 'Tools\Ninja\ninja.exe'
if (-not (Test-Path -LiteralPath $ninja)) {
    $ninja = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
}
if (-not (Test-Path -LiteralPath $ninja)) {
    $ninja = (Get-Command ninja -ErrorAction Stop).Source
}
Invoke-Checked -Command cmake -Arguments @(
    '-S', $repoRoot, '-B', $BuildDirectory, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_PREFIX_PATH=$prefixes", "-DCMAKE_TOOLCHAIN_FILE=$VcpkgRoot\scripts\buildsystems\vcpkg.cmake",
    "-DVCPKG_INSTALLED_DIR=$depsRoot", '-DVCPKG_MANIFEST_MODE=OFF', '-DVCPKG_TARGET_TRIPLET=x64-windows',
    "-DPython3_EXECUTABLE=$pythonExe", "-DPython_EXECUTABLE=$pythonExe",
    "-DPKG_CONFIG_EXECUTABLE=$depsRoot\x64-windows\tools\pkgconf\pkgconf.exe",
    '-DSHIBOKEN6TOOLS_SKIP_FIND_DEPENDENCIES=ON',
    "-DNATRON_VULKAN_SDK_ROOT=$VulkanSdkRoot",
    '-DNATRON_ENABLE_USD=ON',
    "-DVulkan_INCLUDE_DIR=$VulkanSdkRoot\Include",
    "-DVulkan_LIBRARY=$VulkanSdkRoot\Lib\vulkan-1.lib",
    "-DVulkan_GLSLANG_VALIDATOR_EXECUTABLE=$VulkanSdkRoot\Bin\glslangValidator.exe"
)
if (-not $ConfigureOnly) {
    Invoke-Checked -Command cmake -Arguments @('--build', $BuildDirectory, '--parallel', "$Jobs", '--', '-k', '0')
}
