[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('8u', '32f')]
    [string[]]$TextureDepths = @('8u', '32f'),
    [ValidateSet('GuiSmoke', 'OfxGuiSmoke', 'OverlaySmoke', 'DrawSuiteSmoke', 'ParamDrawSuiteSmoke', 'CurveSmoke', 'DopeSheetSmoke', 'TimelineSmoke', 'HistogramSmoke')]
    [string[]]$Tests = @('GuiSmoke', 'OfxGuiSmoke', 'OverlaySmoke', 'DrawSuiteSmoke', 'ParamDrawSuiteSmoke', 'CurveSmoke', 'DopeSheetSmoke', 'TimelineSmoke', 'HistogramSmoke'),
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 60,
    [string]$ReportDirectory = 'build\vulkan-natron-tests',
    [switch]$Resume
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$summaryPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $summaryPath) { Remove-Item -LiteralPath $summaryPath }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$reports = @()
$binaryPath = Join-Path $repoRoot 'build\windows-qt6\App\Natron.exe'
$binaryBefore = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
$preferencesBefore = & uv run --frozen python (Join-Path $PSScriptRoot 'test-settings-fingerprint.py')
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint Natron user settings.' }
$definitionFiles = @((Join-Path $PSScriptRoot 'run-windows-qt6.ps1'), $PSCommandPath)
$definitionFiles += @(Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*smoke.py' -File | Sort-Object Name | ForEach-Object { $_.FullName })
$definitionDigest = ($definitionFiles | ForEach-Object { (Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }) -join ':'
$hasher = [System.Security.Cryptography.SHA256]::Create()
try { $definitionHash = ([BitConverter]::ToString($hasher.ComputeHash([Text.Encoding]::UTF8.GetBytes($definitionDigest)))).Replace('-', '').ToLowerInvariant() }
finally { $hasher.Dispose() }
$displayScale = if ($env:QT_SCALE_FACTOR) { $env:QT_SCALE_FACTOR } else { '1' }

function Save-PartialReport {
    $partialPath = Join-Path $ReportDirectory 'report.partial.json'
    @{ passed = $false; binarySha256 = $binaryBefore; preferencesSha256 = $preferencesBefore; testDefinitionSha256 = $definitionHash;
        displayScale = $displayScale; cases = $reports } | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath "$partialPath.tmp" -Encoding utf8
    Move-Item -LiteralPath "$partialPath.tmp" -Destination $partialPath -Force
}
try {
    foreach ($gpu in $GpuIndices) {
        if ($gpu -lt 0) { throw 'GPU indices must be nonnegative.' }
        $env:NATRON_VULKAN_DEVICE = [string]$gpu
        foreach ($depth in $TextureDepths) {
            foreach ($test in $Tests) {
                $caseDirectory = Join-Path $ReportDirectory "gpu$gpu\$depth\$test"
                New-Item -ItemType Directory -Path $caseDirectory -Force | Out-Null
                $caseProofPath = Join-Path $caseDirectory 'case.json'
                if ($Resume -and (Test-Path -LiteralPath $caseProofPath)) {
                    try { $proof = Get-Content -LiteralPath $caseProofPath -Raw | ConvertFrom-Json -AsHashtable }
                    catch { $proof = @{} }
                    $reusable = $proof.passed -and $proof.binarySha256 -eq $binaryBefore -and $proof.preferencesSha256 -eq $preferencesBefore -and
                        $proof.testDefinitionSha256 -eq $definitionHash -and $proof.displayScale -eq $displayScale -and
                        $proof.gpu -eq $gpu -and $proof.depth -eq $depth -and $proof.test -eq $test -and $proof.fileSha256.Count -gt 0
                    if ($reusable) {
                        foreach ($file in $proof.fileSha256.Keys) {
                            $path = Join-Path $caseDirectory $file
                            if (-not (Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $proof.fileSha256[$file]) {
                                $reusable = $false
                                break
                            }
                        }
                    }
                    if ($reusable) {
                        $reports += @{ gpu = $gpu; depth = $depth; test = $test; passed = $true; directory = $caseDirectory }
                        Save-PartialReport
                        Write-Output "$test GPU $gpu $depth verified from checkpoint. Logs: $caseDirectory"
                        continue
                    }
                }
                $log = Join-Path $caseDirectory 'run.log'
                & (Join-Path $PSScriptRoot 'run-windows-qt6.ps1') -Program $test -GuiTextureDepth $depth -GuiTimeoutSeconds $TimeoutSeconds *> $log
                if ($LASTEXITCODE -ne 0) {
                    Get-Content -LiteralPath $log -Tail 40
                    throw "$test GPU $gpu depth $depth failed. Logs: $caseDirectory"
                }
                $resultFile = @{ GuiSmoke = 'build\gui-qt6-result.json'; OfxGuiSmoke = 'build\ofx-gui-result.json'; OverlaySmoke = 'build\overlay-gui-result.json'; DrawSuiteSmoke = 'build\ofx-drawsuite-gui-result.json'; ParamDrawSuiteSmoke = 'build\ofx-param-drawsuite-gui-result.json'; CurveSmoke = 'build\curve-gui-result.json'; DopeSheetSmoke = 'build\dope-sheet-gui-result.json'; TimelineSmoke = 'build\timeline-gui-result.json'; HistogramSmoke = 'build\histogram-gui-result.json' }[$test]
                $result = Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json
                if (-not $result.ok) { throw "$test GPU $gpu depth $depth failed its assertions." }
                Copy-Item -LiteralPath $resultFile -Destination (Join-Path $caseDirectory 'result.json')
                $frameFile = @{ GuiSmoke = 'build\gui-qt6-frame.png'; OfxGuiSmoke = 'build\ofx-gui-frame.png'; OverlaySmoke = 'build\overlay-gui-frame.png'; DrawSuiteSmoke = 'build\ofx-drawsuite-gui-frame.png'; ParamDrawSuiteSmoke = 'build\ofx-param-drawsuite-gui-frame.png'; CurveSmoke = 'build\curve-gui-frame.png'; DopeSheetSmoke = 'build\dope-sheet-gui-frame.png'; TimelineSmoke = 'build\timeline-gui-frame.png'; HistogramSmoke = 'build\histogram-gui-frame.png' }[$test]
                Copy-Item -LiteralPath $frameFile -Destination (Join-Path $caseDirectory 'frame.png')
                if ($test -eq 'OverlaySmoke') {
                    Copy-Item -LiteralPath 'build\overlay-roto-frame.png' -Destination (Join-Path $caseDirectory 'roto.png')
                    Copy-Item -LiteralPath 'build\overlay-transform-before.png' -Destination (Join-Path $caseDirectory 'transform.png')
                    Copy-Item -LiteralPath 'build\overlay-tracker-before.png' -Destination (Join-Path $caseDirectory 'tracker.png')
                }
                if ($test -eq 'DrawSuiteSmoke') {
                    Copy-Item -LiteralPath 'build\ofx-drawsuite-before.png' -Destination (Join-Path $caseDirectory 'before.png')
                }
                if ($test -eq 'TimelineSmoke') {
                    Copy-Item -LiteralPath 'build\timeline-before.png' -Destination (Join-Path $caseDirectory 'before.png')
                    Copy-Item -LiteralPath 'build\timeline-timecode.png' -Destination (Join-Path $caseDirectory 'timecode.png')
                }
                if ($test -eq 'ParamDrawSuiteSmoke') {
                    Copy-Item -LiteralPath 'build\ofx-param-drawsuite-before.png' -Destination (Join-Path $caseDirectory 'before.png')
                    Copy-Item -LiteralPath 'build\ofx-param-drawsuite-resize.png' -Destination (Join-Path $caseDirectory 'resize.png')
                }
                if ($test -eq 'HistogramSmoke') {
                    Copy-Item -LiteralPath 'build\histogram-before.png' -Destination (Join-Path $caseDirectory 'before.png')
                    Copy-Item -LiteralPath 'build\histogram-picker.png' -Destination (Join-Path $caseDirectory 'picker.png')
                    Copy-Item -LiteralPath 'build\histogram-missing.png' -Destination (Join-Path $caseDirectory 'missing.png')
                }
                if ($test -eq 'CurveSmoke') {
                    foreach ($capture in @('before', 'tangents', 'color_lookup', 'lookup_histogram', 'hue_correct', 'hue_correct_v1', 'hue_keyer', 'hue_sheared', 'ordinary')) {
                        Copy-Item -LiteralPath "build\curve-gui-$capture.png" -Destination (Join-Path $caseDirectory "$capture.png")
                    }
                }
                if ($test -eq 'DopeSheetSmoke') {
                    foreach ($capture in @('before', 'selected', 'moved', 'scrub', 'rectangle', 'interpolation', 'navigation', 'clips',
                            'reader_left', 'reader_right', 'reader_slip', 'reader_move', 'offset_move', 'range_move', 'group_move',
                            'collapsed_color', 'collapsed_node', 'selected_master', 'hidden_node', 'resize', 'scroll', 'modal_interruption')) {
                        Copy-Item -LiteralPath "build\dope-sheet-gui-$capture.png" -Destination (Join-Path $caseDirectory "$capture.png")
                    }
                }
                $reports += @{ gpu = $gpu; depth = $depth; test = $test; passed = $true; directory = $caseDirectory }
                $fileHashes = @{}
                foreach ($file in Get-ChildItem -LiteralPath $caseDirectory -File | Where-Object { $_.Extension -eq '.png' -or $_.Name -in @('result.json', 'run.log') }) {
                    $fileHashes[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                }
                @{ passed = $true; gpu = $gpu; depth = $depth; test = $test; binarySha256 = $binaryBefore; preferencesSha256 = $preferencesBefore;
                    testDefinitionSha256 = $definitionHash; displayScale = $displayScale; fileSha256 = $fileHashes } |
                    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$caseProofPath.tmp" -Encoding utf8
                Move-Item -LiteralPath "$caseProofPath.tmp" -Destination $caseProofPath -Force
                Save-PartialReport
                Write-Output "$test GPU $gpu $depth passed. Logs: $caseDirectory"
            }
        }
    }
    $preferencesAfter = & uv run --frozen python (Join-Path $PSScriptRoot 'test-settings-fingerprint.py')
    if ($LASTEXITCODE -ne 0 -or $preferencesBefore -ne $preferencesAfter) {
        throw 'Natron user preferences changed during the GUI test matrix.'
    }
    $binaryAfter = (Get-FileHash -LiteralPath $binaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($binaryAfter -ne $binaryBefore) { throw 'Natron executable changed during the GUI test matrix.' }
    @{ passed = $true; userPreferencesUnchanged = $true; binarySha256 = $binaryBefore; preferencesSha256 = $preferencesBefore; testDefinitionSha256 = $definitionHash;
        displayScale = $displayScale; cases = $reports } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$summaryPath.tmp" -Encoding utf8
    Move-Item -LiteralPath "$summaryPath.tmp" -Destination $summaryPath -Force
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
}
