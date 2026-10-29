[CmdletBinding()]
param(
    [string]$HdNvpRoot = $env:NATRON_HDNVP_ROOT,
    [string]$BuildDirectory = 'build/hdnvp-host',
    [int]$Jobs = 8,
    [switch]$StandaloneChild
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $HdNvpRoot) { $HdNvpRoot = Join-Path (Split-Path -Parent $repoRoot) 'houdini/hdNVP' }
$HdNvpRoot = (Resolve-Path -LiteralPath $HdNvpRoot).Path
if (-not $StandaloneChild) {
    # hdNVP's pinned MSVC environment must not leak into a subsequent Natron
    # build using a newer toolset in the caller's PowerShell session.
    & (Get-Command pwsh -ErrorAction Stop).Source -NoLogo -NoProfile -File $PSCommandPath -HdNvpRoot $HdNvpRoot -BuildDirectory $BuildDirectory -Jobs $Jobs -StandaloneChild
    if ($LASTEXITCODE -ne 0) { throw 'Host build failed in the isolated toolchain process.' }
    return
}
$sdk = Join-Path $HdNvpRoot 'external/standalone/vcpkg_installed/x64-windows-ditto'
$runtimeSource = Join-Path $HdNvpRoot 'build/win-standalone-release/src/plugin'
if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource 'Ditto.dll'))) { throw 'Build the hdNVP standalone Release plugin first.' }
& (Join-Path $HdNvpRoot 'tools/devshell.ps1') -Standalone
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot $BuildDirectory }
& cmake -S (Join-Path $PSScriptRoot 'hdnvp-host') -B $BuildDirectory -G Ninja '-DCMAKE_BUILD_TYPE=Release' "-DCMAKE_PREFIX_PATH=$sdk" '-DCMAKE_CXX_COMPILER=cl'
if ($LASTEXITCODE -ne 0) { throw 'Host configure failed.' }
& cmake --build $BuildDirectory --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Host build failed.' }
$runtime = Join-Path $BuildDirectory 'runtime'
# Keep the independent SDK and plugin together, outside Natron's DLL search directory.
# Hard links avoid duplicating the renderer's runtime and shaders on this volume.
foreach ($file in Get-ChildItem -LiteralPath $runtimeSource -Recurse -File) {
    $relative = [IO.Path]::GetRelativePath($runtimeSource, $file.FullName)
    if ($file.Extension -in @('.pdb','.lib','.exp','.obj')) { continue }
    $destination = Join-Path $runtime $relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    if ($file.Extension -in @('.dll','.exe')) {
        # Separate DLL files keep the running host from locking hdNVP's build outputs.
        if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    } else {
        if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
        try { New-Item -ItemType HardLink -Path $destination -Target $file.FullName -ErrorAction Stop | Out-Null }
        catch { Copy-Item -LiteralPath $file.FullName -Destination $destination }
    }
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'Gpu/usd/natron') -Destination (Join-Path $runtime 'usd') -Recurse -Force
$materialXLibraries = Join-Path $sdk 'libraries'
if (Test-Path -LiteralPath $materialXLibraries) {
    Copy-Item -LiteralPath $materialXLibraries -Destination $runtime -Recurse -Force
}
Write-Host "hdNVP host: $(Join-Path $runtime 'NatronHdNvpHost.exe')"
