[CmdletBinding()]
param(
    [string]$MsysRoot,
    [ValidateSet('misc', 'arena', 'gmic', 'io')]
    [string[]]$PluginSets = @('misc', 'arena', 'gmic', 'io'),
    [int]$Jobs = 16,
    [switch]$SkipDependencies
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$buildRoot = Join-Path $repoRoot 'build'
$sourceRoot = Join-Path $buildRoot 'ofx-src'
$configRoot = Join-Path $repoRoot 'build-configs\windows-ofx'
$sources = Get-Content -LiteralPath (Join-Path $configRoot 'sources.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Force $sourceRoot | Out-Null
if (-not $MsysRoot) { $MsysRoot = Join-Path $buildRoot 'msys64' }
$MsysRoot = [IO.Path]::GetFullPath($MsysRoot)

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}
function Apply-SourcePatch {
    param([string]$Directory, [string]$Patch)
    & git -C $Directory apply --reverse --check $Patch 2>$null
    if ($LASTEXITCODE -eq 0) { return }
    Invoke-Checked -Command git -Arguments @('-C', $Directory, 'apply', '--check', $Patch)
    Invoke-Checked -Command git -Arguments @('-C', $Directory, 'apply', $Patch)
}

if (-not (Test-Path -LiteralPath (Join-Path $MsysRoot 'usr\bin\bash.exe'))) {
    if ($SkipDependencies) { throw "MSYS2 is missing: $MsysRoot" }
    $archive = Join-Path $buildRoot 'msys2-base.sfx.exe'
    Invoke-WebRequest -Uri 'https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-base-x86_64-latest.sfx.exe' -OutFile $archive
    if ((Split-Path -Leaf $MsysRoot) -ne 'msys64') { throw 'An automatically provisioned MSYS2 directory must be named msys64.' }
    Invoke-Checked -Command $archive -Arguments @('-y', "-o$(Split-Path -Parent $MsysRoot)")
}
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
$env:MSYSTEM = 'UCRT64'
$env:CHERE_INVOKING = 'yes'
if (-not $SkipDependencies) {
    Invoke-Checked -Command $bash -Arguments @('-lc', ' ')
    # Core updates can end their own shell; the second invocation finishes the update.
    & $bash -lc 'pacman --noconfirm -Syuu'
    Invoke-Checked -Command $bash -Arguments @('-lc', 'pacman --noconfirm -Syuu')
    $packages = @('gcc', 'cmake', 'ninja', 'pkgconf', 'imagemagick', 'lcms2', 'fontconfig',
        'pango', 'libxml2', 'libzip', 'librsvg', 'poppler', 'libcdr', 'librevenge', 'sox',
        'fftw', 'curl', 'libpng', 'zlib', 'graphviz', 'openexr', 'libjxl', 'libultrahdr', 'opencolorio',
        'openimageio', 'ffmpeg', 'seexpr')
    $packageNames = ($packages | ForEach-Object { "mingw-w64-ucrt-x86_64-$_" }) -join ' '
    Invoke-Checked -Command $bash -Arguments @('-lc', "pacman --noconfirm -S --needed $packageNames")
}
$ucrtBin = Join-Path $MsysRoot 'ucrt64\bin'
$env:PATH = "$ucrtBin;$env:PATH"
$env:PKG_CONFIG_PATH = Join-Path $MsysRoot 'ucrt64\lib\pkgconfig'
$cmake = Join-Path $ucrtBin 'cmake.exe'
& (Join-Path $PSScriptRoot 'build-windows-ofx-codecs.ps1') -MsysRoot $MsysRoot -Jobs $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Could not build the codec runtime without OpenGL.' }

foreach ($set in $PluginSets) {
    $name = "openfx-$set"
    $directory = Join-Path $sourceRoot $name
    $commit = $sources.repositories.$name
    if (-not (Test-Path -LiteralPath (Join-Path $directory '.git'))) {
        Invoke-Checked -Command git -Arguments @('clone', "https://github.com/NatronGitHub/$name.git", $directory)
    }
    $head = & git -C $directory rev-parse HEAD
    if ($head -ne $commit) {
        $changes = & git -C $directory status --porcelain
        if ($changes) { throw "Source changes in $directory prevent switching to the pinned commit." }
        Invoke-Checked -Command git -Arguments @('-C', $directory, 'fetch', 'origin', $commit)
        Invoke-Checked -Command git -Arguments @('-C', $directory, 'checkout', '--detach', $commit)
    }
    Invoke-Checked -Command git -Arguments @('-C', $directory, 'submodule', 'update', '--init', '--recursive', '--jobs', '8')
    $patch = Join-Path $configRoot "$name.patch"
    if (Test-Path -LiteralPath $patch) { Apply-SourcePatch -Directory $directory -Patch $patch }

    if ($set -eq 'io') {
        Apply-SourcePatch -Directory (Join-Path $directory 'SupportExt') -Patch (Join-Path $configRoot 'openfx-io-supportext-vulkan.patch')
    }

    if ($set -eq 'misc') {
        Apply-SourcePatch -Directory (Join-Path $directory 'SupportExt') -Patch (Join-Path $configRoot 'openfx-misc-supportext-vulkan.patch')
        $cimgHeader = Join-Path $directory 'CImg\CImg.h'
        $inpaintHeader = Join-Path $directory 'CImg\Inpaint\inpaint.h'
        $cimgUrl = "https://raw.githubusercontent.com/dtschump/CImg/$($sources.cimgCommit)"
        if (-not (Test-Path -LiteralPath $cimgHeader)) { Invoke-WebRequest -Uri "$cimgUrl/CImg.h" -OutFile $cimgHeader }
        if (-not (Test-Path -LiteralPath $inpaintHeader)) { Invoke-WebRequest -Uri "$cimgUrl/plugins/inpaint.h" -OutFile $inpaintHeader }
        # Preserve host cancellation in the downloaded Inpaint implementation.
        Apply-SourcePatch -Directory $directory -Patch (Join-Path $configRoot 'cimg-inpaint.patch')
    } elseif ($set -eq 'arena') {
        Apply-SourcePatch -Directory (Join-Path $directory 'SupportExt') -Patch (Join-Path $configRoot 'openfx-arena-supportext-vulkan.patch')
        Apply-SourcePatch -Directory (Join-Path $directory 'OpenFX-IO') -Patch (Join-Path $configRoot 'openfx-arena-io-unicode.patch')
    } elseif ($set -eq 'gmic') {
        if (-not (Test-Path -LiteralPath (Join-Path $directory 'gmic.cpp'))) {
            $gmicArchive = Join-Path $buildRoot "gmic_$($sources.gmicVersion).tar.gz"
            Invoke-WebRequest -Uri "https://gmic.eu/files/source/gmic_$($sources.gmicVersion).tar.gz" -OutFile $gmicArchive
            if ((Get-FileHash -LiteralPath $gmicArchive -Algorithm SHA256).Hash -ne $sources.gmicSha256) { throw "G'MIC source archive checksum mismatch." }
            $members = @('CImg.h', 'gmic.cpp', 'gmic.h', 'gmic_stdlib.h') | ForEach-Object { "gmic-$($sources.gmicVersion)/src/$_" }
            Invoke-Checked -Command tar -Arguments (@('-xf', $gmicArchive, '--strip-components', '2', '-C', $directory) + $members)
        }
        foreach ($file in @('gmic_libc.cpp', 'gmic_libc.h', 'gmic_stdlib_gmic.h')) {
            $destination = Join-Path $directory $file
            if (-not (Test-Path -LiteralPath $destination)) {
                Invoke-WebRequest -Uri "https://raw.githubusercontent.com/GreycLab/gmic-community/$($sources.gmicCommunityCommit)/libcgmic/$file" -OutFile $destination
            }
        }
    }
    $binaryDirectory = Join-Path $buildRoot "windows-ofx\$set"
    Invoke-Checked -Command $cmake -Arguments @('-S', $configRoot, '-B', $binaryDirectory, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_CXX_COMPILER=$ucrtBin\g++.exe", "-DCMAKE_MAKE_PROGRAM=$ucrtBin\ninja.exe",
        "-DOFX_SOURCE_ROOT=$sourceRoot", "-DOFX_SET=$set", '-DOFX_ARCH_NAME=Win64',
        "-DCMAKE_INSTALL_PREFIX=$buildRoot\Plugins")
    Invoke-Checked -Command $cmake -Arguments @('--build', $binaryDirectory, '--parallel', "$Jobs")
    Invoke-Checked -Command $cmake -Arguments @('--install', $binaryDirectory)
    if ($set -eq 'io') {
        # Old downloaded IO bundles carry DLLs beside IO.ofx. They take priority
        # over the shared current runtime and must leave the loader search path.
        $ioBin = [IO.Path]::GetFullPath((Join-Path $buildRoot 'Plugins\IO.ofx.bundle\Contents\Win64'))
        $installedFiles = @(Get-Content -LiteralPath (Join-Path $binaryDirectory 'install_manifest.txt') |
            ForEach-Object { [IO.Path]::GetFullPath($_) })
        $legacyBackup = Join-Path $buildRoot 'ofx-legacy-runtime-backup\IO'
        foreach ($file in Get-ChildItem -LiteralPath $ioBin -File -Filter '*.dll') {
            $resolvedFile = [IO.Path]::GetFullPath($file.FullName)
            if (-not $resolvedFile.StartsWith($ioBin + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
                throw "Unexpected IO runtime path: $resolvedFile"
            }
            if ($installedFiles -contains $resolvedFile) { continue }
            New-Item -ItemType Directory -Path $legacyBackup -Force | Out-Null
            $backupFile = Join-Path $legacyBackup $file.Name
            if (-not (Test-Path -LiteralPath $backupFile)) { Copy-Item -LiteralPath $resolvedFile -Destination $backupFile }
            Remove-Item -LiteralPath $resolvedFile
        }
    }
}
& (Join-Path $PSScriptRoot 'install-ofx-runtime.ps1') -MsysRoot $MsysRoot -PluginDirectory (Join-Path $buildRoot 'Plugins') -RuntimeDirectory (Join-Path $buildRoot 'ofx-runtime')
& $bash -lc 'pacman -Q' | Set-Content -Encoding utf8 (Join-Path $buildRoot 'ofx-package-versions.txt')
Write-Output 'OFX bundles are ready. Run tools/run-windows-qt6.ps1 -Program OfxSmoke to verify rendering.'
