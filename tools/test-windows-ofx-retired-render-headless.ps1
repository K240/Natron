[CmdletBinding()]
param([string]$ReportDirectory = 'build/windows-ofx-retired-render-headless-tests',
      [string]$SourceAudit = 'build/retired-render-source-audit.json',
      [string]$RuntimeAudit = 'build/retired-render-runtime-audit.json')
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$destination = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory } else { Join-Path $repoRoot $ReportDirectory }))
if (Test-Path -LiteralPath $destination) { throw 'Use a fresh report directory.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$previousGpu = $env:NATRON_VULKAN_DEVICE
$previousDisabled = $env:NATRON_VULKAN_DISABLED
$previousLimit = $env:NATRON_VULKAN_MEMORY_LIMIT_MB
$definitions = @('tools/test-windows-ofx-retired-render-headless.ps1','tools/ofx-retired-render-headless-smoke.py',
    'tools/run-windows-owned-gui-test.ps1','tools/verify-ofx-retired-render-headless.py',
    $SourceAudit,$RuntimeAudit,
    'build/ofx-tests/RetiredRenderSmoke.ofx.bundle/Contents/Win64/RetiredRenderSmoke.ofx')
$source = Get-Content -LiteralPath $SourceAudit -Raw | ConvertFrom-Json -AsHashtable
$runtime = Get-Content -LiteralPath $RuntimeAudit -Raw | ConvertFrom-Json
if (-not $source.passed -or -not $runtime.passed) { throw 'Source/runtime audit is incomplete.' }
$definitions += $source.fileSha256.Keys
$definitions += $runtime.files.file
function Hash-Inputs {
    $hashes = @{}
    foreach ($file in ($definitions | Select-Object -Unique)) { $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$before = Hash-Inputs
$settings = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE) { throw 'Could not fingerprint user settings.' }
$report = [ordered]@{passed=$false;cases=@();inputSha256=$before;settingsBefore=$settings;completionPlatform='windows';
    sourceAudit=$SourceAudit;runtimeAudit=$RuntimeAudit}
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_MEMORY_LIMIT_MB -ErrorAction SilentlyContinue
    foreach ($name in @('gpu0','gpu1','disabled')) {
        $env:NATRON_VULKAN_DEVICE = if ($name -eq 'gpu1') { '1' } else { '0' }
        if ($name -eq 'disabled') { $env:NATRON_VULKAN_DISABLED = '1' }
        else { Remove-Item -LiteralPath Env:NATRON_VULKAN_DISABLED -ErrorAction SilentlyContinue }
        $result = Join-Path $destination "$name/result.json"
        $profile = 'build/test-profiles/RetiredHeadless-' + [Guid]::NewGuid().ToString('N')
        $failure = $null
        try { & tools/run-windows-owned-gui-test.ps1 -Program NatronRenderer -Script tools/ofx-retired-render-headless-smoke.py -Profile $profile -Result $result -TimeoutSeconds 60 *> (Join-Path $destination "$name.log") }
        catch { $failure = $_.Exception.Message }
        $report.cases += @{name=$name;gpu=$env:NATRON_VULKAN_DEVICE;disabled=($name -eq 'disabled');result=$result;
            profile=[IO.Path]::GetFullPath((Join-Path $repoRoot $profile));passed=($null -eq $failure);failure=$failure}
        $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
        if ($failure) { throw $failure }
        Write-Output "$name headless passed."
    }
    $after = Hash-Inputs
    foreach ($file in $before.Keys) { if ($before[$file] -ne $after[$file]) { throw "Input changed during tests: $file" } }
    $report.settingsAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -or $report.settingsBefore -ne $report.settingsAfter) { throw 'User preferences changed.' }
    $report.passed = $true
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
} finally {
    foreach ($pair in @(@('NATRON_VULKAN_DEVICE',$previousGpu),@('NATRON_VULKAN_DISABLED',$previousDisabled),@('NATRON_VULKAN_MEMORY_LIMIT_MB',$previousLimit))) {
        if ($null -eq $pair[1]) { Remove-Item -LiteralPath "Env:$($pair[0])" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($pair[0],$pair[1],'Process') }
    }
}
