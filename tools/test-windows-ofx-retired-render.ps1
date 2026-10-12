[CmdletBinding()]
param([string]$ReportDirectory = 'build/windows-ofx-retired-render-tests')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }))
if (Test-Path -LiteralPath $destination) { throw 'Use a fresh report directory.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'QT_SCALE_FACTOR', 'NATRON_TEST_VIEWER_BACKEND', 'NATRON_VULKAN_DISABLED', 'NATRON_VULKAN_MEMORY_LIMIT_MB')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$source = Get-Content -LiteralPath 'build/retired-render-source-audit.json' -Raw | ConvertFrom-Json -AsHashtable
$runtime = Get-Content -LiteralPath 'build/retired-render-runtime-audit.json' -Raw | ConvertFrom-Json
if (-not $source.passed -or -not $runtime.passed) { throw 'Current source/runtime audits must pass first.' }
$files = @('tools/test-windows-ofx-retired-render.ps1', 'tools/ofx-retired-render-gui-smoke.py',
    'tools/run-windows-owned-gui-test.ps1', 'tools/verify-ofx-retired-render.py', 'tools/test-settings-fingerprint.py',
    'tools/ofx-interact-inventory.cpp', 'tools/summarize-ofx-interacts.py', 'tools/audit-windows-ofx-interacts.ps1',
    'build-configs/ofx-interact-inventory/CMakeLists.txt', 'build/ofx-interact-inventory/OfxInteractInventory.exe',
    'build/retired-render-source-audit.json', 'build/retired-render-runtime-audit.json',
    'build/retired-render-native-shipped-inventory/report.json', 'build/retired-render-native-shipped-inventory/native.json',
    'build/retired-render-native-shipped-inventory/legacy-discovery.json',
    'build/retired-render-native-shipped-inventory/plugins.csv',
    'build/ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/Win64/RetiredRenderSmoke.ofx',
    'build/windows-qt6/build.ninja', 'Tests/CMakeLists.txt')
$files += $source.fileSha256.Keys
$files += $runtime.files.file
function Get-InputHashes {
    $hashes = @{}
    foreach ($file in ($files | Select-Object -Unique)) { $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$before = Get-InputHashes
$settings = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE) { throw 'Could not fingerprint user settings.' }
$report = [ordered]@{passed=$false;completionPlatform='windows';cases=@();inputSha256=$before;settingsBefore=$settings}
function Run-Case([string]$name, [int]$gpu, [int]$scale, [string]$depth, [string]$backend, [bool]$disabled, [string]$cacheSource = '') {
    $env:NATRON_VULKAN_DEVICE = [string]$gpu
    $env:QT_SCALE_FACTOR = [string]$scale
    $env:NATRON_TEST_VIEWER_BACKEND = $backend
    if ($disabled) { $env:NATRON_VULKAN_DISABLED = '1' } else { Remove-Item -LiteralPath Env:NATRON_VULKAN_DISABLED -ErrorAction SilentlyContinue }
    $result = Join-Path $destination "$name/result.json"
    $profile = 'build/test-profiles/RetiredRender-' + [Guid]::NewGuid().ToString('N')
    $failure = $null
    $arguments = @{Script='tools/ofx-retired-render-gui-smoke.py';Profile=$profile;Result=$result;TextureDepth=$depth;TimeoutSeconds=120}
    if ($cacheSource) { $arguments.OfxCacheSource = $cacheSource }
    try { & tools/run-windows-owned-gui-test.ps1 @arguments *> (Join-Path $destination "$name.log") }
    catch { $failure = $_.Exception.Message }
    $case = @{gpu=$gpu;scale=$scale;depth=$depth;backend=$backend;disabled=$disabled;name=$name;result=$result;
        profile=[IO.Path]::GetFullPath((Join-Path $repoRoot $profile));passed=($null -eq $failure);failure=$failure;
        cacheSource=$cacheSource;cacheSourceSha256=$(if ($cacheSource) { (Get-FileHash -LiteralPath $cacheSource -Algorithm SHA256).Hash.ToLowerInvariant() } else { $null })}
    $report.cases += $case
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
    if ($failure) { throw $failure }
    Write-Output "$name passed."
}
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($gpu in @(0, 1)) {
        foreach ($scale in @(1, 2)) {
            foreach ($depth in @('8u', '32f')) {
                foreach ($backend in @('Vulkan', 'Raster')) {
                    Run-Case "gpu$gpu-scale$scale-$depth-$backend" $gpu $scale $depth $backend $false
                }
            }
        }
    }
    foreach ($scale in @(1, 2)) {
        foreach ($depth in @('8u', '32f')) {
            Run-Case "disabled-scale$scale-$depth" 0 $scale $depth 'Raster' $true
        }
    }
    $cache = @(Get-ChildItem -LiteralPath (Join-Path $report.cases[0].profile 'cache/OFXLoadCache') -File -Filter 'OFXCache_*.xml')
    if ($cache.Count -ne 1) { throw 'The cold run did not preserve its OFX cache.' }
    Run-Case 'cache-v2' 0 1 '8u' 'Vulkan' $false $cache[0].FullName
    $seedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot ('build/test-profiles/RetiredCache-' + [Guid]::NewGuid().ToString('N'))))
    $ownedRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build/test-profiles'))
    if (-not $seedRoot.StartsWith($ownedRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid owned cache seed directory.' }
    New-Item -ItemType Directory -Path $seedRoot | Out-Null
    $oldCache = Join-Path $seedRoot $cache[0].Name
    $xml = [xml](Get-Content -LiteralPath $cache[0].FullName -Raw)
    if ($xml.cache.version -ne 'NatronOFXCachev2') { throw 'Unexpected current cache version.' }
    $xml.cache.version = 'NatronOFXCachev1'
    foreach ($property in @($xml.SelectNodes("//property[@name='OfxImageEffectPropOpenGLRenderSupported']"))) {
        $property.ParentNode.RemoveChild($property) | Out-Null
    }
    $xml.Save($oldCache)
    Run-Case 'cache-v1-rebuild' 0 1 '8u' 'Vulkan' $false $oldCache
    $after = Get-InputHashes
    foreach ($name in $before.Keys) { if ($before[$name] -ne $after[$name]) { throw "Input changed during tests: $name" } }
    $report.settingsAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -or $report.settingsBefore -ne $report.settingsAfter) { throw 'User preferences changed.' }
    $report.passed = $true
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
} finally {
    foreach ($name in $variables) {
        if ($null -eq $previous[$name]) { Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    }
}
