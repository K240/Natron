[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CacheFile,
    [string]$PluginDirectory = 'build\Plugins',
    [string]$ReportDirectory = 'build\ofx-vulkan-audit',
    [string]$Dumpbin
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
foreach ($variable in @('CacheFile', 'PluginDirectory', 'ReportDirectory')) {
    $value = Get-Variable -Name $variable -ValueOnly
    if (-not [IO.Path]::IsPathRooted($value)) { Set-Variable -Name $variable -Value (Join-Path $repoRoot $value) }
}
$pluginRoot = (Get-Item -LiteralPath $PluginDirectory).FullName.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $Dumpbin) {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($command) { $Dumpbin = $command.Source }
    else {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Specify -Dumpbin or run from a Visual Studio developer shell.' }
        $Dumpbin = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' |
            Select-Object -First 1
    }
}
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin)) { throw 'Could not locate dumpbin.exe.' }
[xml]$cache = Get-Content -LiteralPath $CacheFile -Raw
if ($cache.cache.version -ne 'NatronOFXCachev1') { throw 'Unsupported OFX cache format.' }

function Get-CachedProperty {
    param($Plugin, [string]$Name)
    $property = @($Plugin.apiproperties.property) | Where-Object { $_.name -eq $Name } | Select-Object -First 1
    if ($property -and $property.value) { return [string](@($property.value)[0].value) }
    return $null
}

$modules = @()
$plugins = @()
foreach ($bundle in $cache.cache.bundle) {
    $path = (Get-Item -LiteralPath $bundle.binary.path).FullName
    if (-not $path.StartsWith($pluginRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Cache module is outside PluginDirectory: $path"
    }
    $dependencies = & $Dumpbin /nologo /dependents $path
    $inspector = 'dumpbin'
    $primaryDiagnostic = $null
    $pattern = '(?im)^\s+([A-Za-z0-9_.-]+\.dll)\s*$'
    if ($LASTEXITCODE -ne 0) {
        $primaryDiagnostic = $dependencies -join "`n"
        # Older stripped MinGW binaries can have stale COFF symbol pointers
        # that native inspectors reject. Inspect only the import directory with
        # pefile, using uv to manage and run the pinned Python dependency.
        $dependencies = & uv run --no-project --python (Join-Path $repoRoot '.venv\Scripts\python.exe') --with pefile==2024.8.26 python (Join-Path $PSScriptRoot 'ofx-pe-imports.py') $path
        if ($LASTEXITCODE -ne 0) { throw "Both dependency inspectors failed for OFX module: $path" }
        $inspector = 'pefile'
        $imports = @($dependencies | ConvertFrom-Json | Sort-Object -Unique)
    } else {
        $imports = @([regex]::Matches(($dependencies -join "`n"), $pattern) |
            ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    }
    if (-not $imports.Count) { throw "No PE imports were read from OFX module: $path" }
    $openGL = $imports -contains 'OPENGL32.dll'
    $moduleName = Split-Path -Leaf $bundle.binary.bundle_path
    $module = [ordered]@{
        bundle = $moduleName; path = $path
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        inspector = $inspector; primaryDiagnostic = $primaryDiagnostic
        cachedPlugins = @($bundle.plugin).Count; directOpenGLImport = $openGL; directImports = $imports
    }
    $modules += [pscustomobject]$module
    foreach ($plugin in $bundle.plugin) {
        $plugins += [pscustomobject][ordered]@{
            id = [string]$plugin.name; major = [int]$plugin.major_version; minor = [int]$plugin.minor_version
            bundle = $moduleName; directOpenGLImport = $openGL
            declaredOpenGLRender = Get-CachedProperty $plugin 'OfxImageEffectPropOpenGLRenderSupported'
            grouping = Get-CachedProperty $plugin 'OfxImageEffectPluginPropGrouping'
            overlayDrawSuite = 'unverified'; cpuRender = 'unverified'; vulkanMigration = 'pending'
        }
    }
}
if (-not $plugins.Count) { throw 'The OFX cache has no plugins.' }
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$plugins | Sort-Object bundle, id, major, minor | Export-Csv -LiteralPath (Join-Path $ReportDirectory 'plugins.csv') -NoTypeInformation -Encoding utf8
$report = [ordered]@{
    cacheFile = (Get-Item -LiteralPath $CacheFile).FullName
    cacheSha256 = (Get-FileHash -LiteralPath $CacheFile -Algorithm SHA256).Hash
    moduleCount = $modules.Count; cachedPluginVersions = $plugins.Count
    modulesWithDirectOpenGLImport = @($modules | Where-Object directOpenGLImport).Count
    scope = 'Static direct imports and cached metadata. CPU rendering, overlays, transitive dependencies and runtime loads require separate verification.'
    modules = $modules
}
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ReportDirectory 'report.json') -Encoding utf8
$modules | Select-Object bundle, cachedPlugins, directOpenGLImport | Format-Table | Out-String | Write-Output
Write-Output "Inventoried $($plugins.Count) cached plugin versions. Reports: $ReportDirectory"
