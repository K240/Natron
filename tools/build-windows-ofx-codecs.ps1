[CmdletBinding()]
param([string]$MsysRoot, [int]$Jobs = 16)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $MsysRoot) { $MsysRoot = Join-Path $repoRoot 'build/msys64' }
$ucrtRoot = Join-Path ([IO.Path]::GetFullPath($MsysRoot)) 'ucrt64'
$ucrtBin = Join-Path $ucrtRoot 'bin'
$previousPath = $env:PATH
function Invoke-CodecCommand([string]$command, [string[]]$arguments) {
    & $command @arguments
    if ($LASTEXITCODE -ne 0) { throw "Codec command failed: $command" }
}
try {
    $env:PATH = "$ucrtBin;$env:PATH"
    $revision = (Get-Content (Join-Path $repoRoot 'build-configs/windows-ofx/sources.json') -Raw |
        ConvertFrom-Json).codecs.libultrahdr
    $source = Join-Path $repoRoot 'build/ofx-src/libultrahdr'
    $binary = Join-Path $repoRoot 'build/windows-ofx/codecs/libultrahdr'
    $prefix = Join-Path $repoRoot 'build/ofx-codecs'
    if (-not (Test-Path -LiteralPath (Join-Path $source '.git'))) {
        Invoke-CodecCommand git @('clone', '--no-checkout', 'https://github.com/google/libultrahdr.git', $source)
        Invoke-CodecCommand git @('-C', $source, 'checkout', '--detach', $revision)
    }
    $changes = & git -C $source status --porcelain
    if ($LASTEXITCODE -ne 0 -or $changes) { throw 'libultrahdr source must be clean.' }
    $head = & git -C $source rev-parse HEAD
    if ($head -ne $revision) {
        Invoke-CodecCommand git @('-C', $source, 'fetch', 'origin', $revision)
        Invoke-CodecCommand git @('-C', $source, 'checkout', '--detach', $revision)
    }
    $cmake = Join-Path $ucrtBin 'cmake.exe'
    Invoke-CodecCommand $cmake @('-S', $source, '-B', $binary, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_C_COMPILER=$ucrtBin/gcc.exe", "-DCMAKE_CXX_COMPILER=$ucrtBin/g++.exe",
        "-DCMAKE_MAKE_PROGRAM=$ucrtBin/ninja.exe", "-DCMAKE_PREFIX_PATH=$ucrtRoot", "-DCMAKE_INSTALL_PREFIX=$prefix",
        '-DBUILD_SHARED_LIBS=ON', '-DCMAKE_DLL_NAME_WITH_SOVERSION=ON', '-DUHDR_ENABLE_GLES=OFF', '-DUHDR_BUILD_DEPS=OFF', '-DUHDR_BUILD_EXAMPLES=ON',
        '-DUHDR_BUILD_TESTS=OFF', '-DUHDR_BUILD_JAVA=OFF')
    Invoke-CodecCommand $cmake @('--build', $binary, '--parallel', "$Jobs")
    Invoke-CodecCommand $cmake @('--install', $binary)
    $dll = Join-Path $prefix 'bin/libuhdr-1.dll'
    if (-not (Test-Path -LiteralPath $dll)) { throw 'Expected ABI-compatible libuhdr-1.dll was not built.' }
    $headers = & (Join-Path $ucrtBin 'objdump.exe') -p $dll
    if ($LASTEXITCODE -ne 0 -or ($headers -join "`n") -match 'DLL Name:\s+(OPENGL32|libEGL|libGLES\w*)\.dll') {
        throw 'The codec still imports an OpenGL library.'
    }
    @{revision=$revision;gles=$false;dllSha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
      cmakeCacheSha256=(Get-FileHash (Join-Path $binary 'CMakeCache.txt')).Hash.ToLowerInvariant()} |
        ConvertTo-Json | Set-Content (Join-Path $prefix 'build-proof.json') -Encoding utf8
} finally { $env:PATH = $previousPath }
