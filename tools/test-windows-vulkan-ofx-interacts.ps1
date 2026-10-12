[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [int[]]$ScaleFactors = @(1, 2),
    [ValidateSet('8u', '32f')][string[]]$TextureDepths = @('8u', '32f'),
    [ValidateSet('Vulkan', 'Raster')][string[]]$Backends = @('Vulkan', 'Raster'),
    [string]$ReportDirectory = 'build/windows-vulkan-ofx-interact-tests'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }))
if (Test-Path -LiteralPath $destination) { throw 'Use a fresh report directory.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$variables = @('NATRON_VULKAN_DEVICE', 'QT_SCALE_FACTOR', 'NATRON_TEST_VIEWER_BACKEND', 'NATRON_VULKAN_MEMORY_LIMIT_MB')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
$files = @('tools/test-windows-vulkan-ofx-interacts.ps1', 'tools/verify-ofx-interact-migration.py',
    'tools/run-windows-owned-gui-test.ps1', 'tools/generator-overlay-gui-smoke.py',
    'tools/io-overlay-gui-smoke.py', 'tools/ofx-legacy-interact-gui-smoke.py', 'tools/windows-module-audit.py',
    'tools/verify-native-ofx-source.py', 'tools/test-settings-fingerprint.py',
    'build-configs/windows-ofx/openfx-misc-supportext-vulkan.patch',
    'build-configs/windows-ofx/openfx-arena-supportext-vulkan.patch',
    'build-configs/windows-ofx/openfx-io-supportext-vulkan.patch',
    'build/generator-drawsuite-source-audit.json', 'build/generator-drawsuite-runtime-audit.json',
    'build/drawsuite-inventory/after-native.json', 'build/drawsuite-inventory/report.json',
    'build/drawsuite-inventory/inspect.cpp', 'build/drawsuite-inventory/inspect.exe',
    'build/ofx-tests/LegacyInteractSmoke.ofx.bundle/Contents/Win64/LegacyInteractSmoke.ofx')
$sourceAudit = Get-Content -LiteralPath 'build/generator-drawsuite-source-audit.json' -Raw | ConvertFrom-Json -AsHashtable
$runtimeAudit = Get-Content -LiteralPath 'build/generator-drawsuite-runtime-audit.json' -Raw | ConvertFrom-Json
if (-not $sourceAudit.passed -or -not $runtimeAudit.passed) { throw 'Source/runtime audit is incomplete.' }
$files += $sourceAudit.fileSha256.Keys
$files += $runtimeAudit.files.file
function Get-InputHashes {
    $hashes = @{}
    foreach ($file in ($files | Select-Object -Unique)) { $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$before = Get-InputHashes
$settings = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE) { throw 'Could not fingerprint user settings.' }
$report = [ordered]@{passed=$false;completionPlatform='windows';cases=@();inputSha256=$before;settingsBefore=$settings}
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($gpu in $GpuIndices) {
        if ($gpu -lt 0) { throw 'Use an installed GPU index.' }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($scale in $ScaleFactors) {
            if ($scale -notin @(1, 2)) { throw 'Use 100% or 200% scale.' }
            $env:QT_SCALE_FACTOR = [string]$scale
            foreach ($depth in $TextureDepths) {
                foreach ($backend in $Backends) {
                    $env:NATRON_TEST_VIEWER_BACKEND = $backend
                    foreach ($test in @('generator', 'legacy')) {
                        $name = "gpu$gpu-scale$scale-$depth-$backend-$test"
                        $result = Join-Path $destination "$name/result.json"
                        $profile = 'build/test-profiles/OfxInteract-' + [Guid]::NewGuid().ToString('N')
                        $script = if ($test -eq 'generator') { 'tools/generator-overlay-gui-smoke.py' } else { 'tools/ofx-legacy-interact-gui-smoke.py' }
                        $failure = $null
                        try {
                            & tools/run-windows-owned-gui-test.ps1 -Script $script -Profile $profile -Result $result -TextureDepth $depth -TimeoutSeconds 180 *> (Join-Path $destination "$name.log")
                        } catch { $failure = $_.Exception.Message }
                        $case = @{gpu=$gpu;scale=$scale;depth=$depth;backend=$backend;test=$test;result=$result;profile=[IO.Path]::GetFullPath((Join-Path $repoRoot $profile));passed=($null -eq $failure);failure=$failure}
                        $report.cases += $case
                        $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
                        if ($failure) { throw $failure }
                        Write-Output "$name passed."
                    }
                }
            }
        }
    }
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
