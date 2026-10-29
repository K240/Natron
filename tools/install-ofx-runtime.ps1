[CmdletBinding()]
param(
    [string]$MsysRoot,
    [string]$PluginDirectory,
    [string]$RuntimeDirectory,
    [string]$CodecDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/ofx-codecs')
)
$ErrorActionPreference = 'Stop'
$ucrtRoot = Join-Path $MsysRoot 'ucrt64'
$ucrtBin = Join-Path $ucrtRoot 'bin'
$runtimeBin = Join-Path $RuntimeDirectory 'bin'
New-Item -ItemType Directory -Force $runtimeBin | Out-Null
$queue = [Collections.Generic.Queue[string]]::new()
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($bundle in @('Misc', 'CImg', 'Arena', 'GMIC', 'IO')) {
    $binary = Join-Path $PluginDirectory "$bundle.ofx.bundle\Contents\Win64\$bundle.ofx"
    if (Test-Path -LiteralPath $binary) { $queue.Enqueue($binary) }
}

# ImageMagick loads its format coders at runtime, so include their dependencies too.
$magickModules = Get-ChildItem -LiteralPath (Join-Path $ucrtRoot 'lib') -Directory -Filter 'ImageMagick-*'
foreach ($module in $magickModules) {
    $destination = Join-Path $RuntimeDirectory 'lib'
    New-Item -ItemType Directory -Force $destination | Out-Null
    Copy-Item -LiteralPath $module.FullName -Destination $destination -Recurse -Force
    Get-ChildItem -LiteralPath $module.FullName -Recurse -Filter '*.dll' | ForEach-Object { $queue.Enqueue($_.FullName) }
}
while ($queue.Count) {
    $binary = $queue.Dequeue()
    $headers = & (Join-Path $ucrtBin 'objdump.exe') -p $binary
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect DLL dependencies: $binary" }
    foreach ($match in [regex]::Matches(($headers -join "`n"), 'DLL Name:\s+(\S+)')) {
        $name = $match.Groups[1].Value
        if (-not $seen.Add($name)) { continue }
        $dependency = Join-Path (Join-Path $CodecDirectory 'bin') $name
        if (-not (Test-Path -LiteralPath $dependency)) { $dependency = Join-Path $ucrtBin $name }
        if (Test-Path -LiteralPath $dependency) {
            Copy-Item -LiteralPath $dependency -Destination $runtimeBin -Force
            $queue.Enqueue($dependency)
        } elseif ($name -notmatch '^(api-ms-|ext-ms-)' -and -not (Test-Path -LiteralPath (Join-Path "$env:WINDIR\System32" $name))) {
            throw "Unresolved dependency $name required by $binary"
        }
    }
}
# Retire only the two known graphics DLLs after all plugin and coder imports
# have been traversed. Keep a recoverable copy outside the loader's bin path.
$runtimeBinFull = [IO.Path]::GetFullPath($runtimeBin)
$runtimeParent = Split-Path -Parent ([IO.Path]::GetFullPath($RuntimeDirectory))
$retiredDirectory = [IO.Path]::GetFullPath((Join-Path $runtimeParent 'ofx-legacy-runtime-backup/codecs'))
if (-not $retiredDirectory.StartsWith($runtimeParent + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Invalid retired codec directory.'
}
foreach ($name in @('libEGL.dll', 'libGLESv2.dll')) {
    $file = [IO.Path]::GetFullPath((Join-Path $runtimeBinFull $name))
    if (-not $file.StartsWith($runtimeBinFull + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Invalid graphics dependency path.'
    }
    if (-not $seen.Contains($name) -and (Test-Path -LiteralPath $file)) {
        New-Item -ItemType Directory -Force -Path $retiredDirectory | Out-Null
        Move-Item -LiteralPath $file -Destination (Join-Path $retiredDirectory $name) -Force
    }
}
foreach ($relativePath in @('etc\ImageMagick-7', 'share\poppler', 'share\ghostscript', 'share\xml\fontconfig', 'share\fontconfig')) {
    $source = Join-Path $ucrtRoot $relativePath
    if (Test-Path -LiteralPath $source) {
        $destination = Join-Path $RuntimeDirectory (Split-Path -Parent $relativePath)
        New-Item -ItemType Directory -Force $destination | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
    }
}
Write-Output "Installed $((Get-ChildItem -LiteralPath $runtimeBin -Filter '*.dll').Count) OpenFX runtime DLLs."
