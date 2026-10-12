[CmdletBinding()]
param(
    [int[]]$GpuIndices = @(0, 1),
    [int[]]$ScaleFactors = @(1, 2),
    [string]$ReportDirectory = 'build\io-overlay-vulkan-tests',
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 180
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
New-Item -ItemType Directory -Force -Path $ReportDirectory | Out-Null
$reportPath = Join-Path $ReportDirectory 'report.json'
if (Test-Path -LiteralPath $reportPath) { Remove-Item -LiteralPath $reportPath }
$files = @('build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx', 'build/windows-ofx/io/IOOverlayDrawingTest.exe',
    'build-configs/windows-ofx/native/NatronOverlayInteract.h', 'build-configs/windows-ofx/native/tests/OverlayDrawing.cpp',
    'build-configs/windows-ofx/CMakeLists.txt', 'build-configs/windows-ofx/openfx-io.patch',
    'build-configs/windows-ofx/openfx-io-supportext-vulkan.patch', 'tools/build-windows-ofx.ps1',
    'Gui/Gui15.cpp', 'Gui/GuiPrivate.h', 'Gui/ViewerGL.cpp', 'Gui/ViewerVulkan.cpp', 'Engine/OfxOverlayInteract.cpp', 'Engine/OfxDrawSuite.cpp',
    'tools/run-windows-qt6.ps1', 'tools/io-overlay-gui-smoke.py', 'tools/test-windows-vulkan-io-overlay.ps1',
    'tools/ofx-pe-imports.py', 'tools/verify-io-vulkan-source.py', 'tools/test-settings-fingerprint.py')
$files += Get-ChildItem -LiteralPath 'build/ofx-runtime/bin' -File -Filter '*.dll' |
    ForEach-Object { [IO.Path]::GetRelativePath($repoRoot, $_.FullName).Replace('\', '/') }
$hashes = @{}
foreach ($file in $files) { $hashes[$file] = (Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() }
$sourceProof = & uv run --frozen python tools/verify-io-vulkan-source.py
if ($LASTEXITCODE -ne 0) { throw 'IO sources differ from saved patches.' }
$sourceProof | Set-Content -LiteralPath (Join-Path $ReportDirectory 'source.json') -Encoding utf8
$sourceProof = $sourceProof | ConvertFrom-Json
$preferencesBefore = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Preference fingerprint failed.' }
$imports = & uv run --no-project --python .venv/Scripts/python.exe --with pefile==2024.8.26 python tools/ofx-pe-imports.py build/Plugins/IO.ofx.bundle/Contents/Win64/IO.ofx
if ($LASTEXITCODE -ne 0) { throw 'IO PE import inspection failed.' }
$imports = $imports | ConvertFrom-Json
if ($imports -contains 'OPENGL32.dll') { throw 'IO still imports OpenGL.' }
$previousPath = $env:PATH
try {
    $env:PATH = (Join-Path $repoRoot 'build\ofx-runtime\bin') + ';' + $env:PATH
    $native = & build/windows-ofx/io/IOOverlayDrawingTest.exe
    if ($LASTEXITCODE -ne 0) { throw 'Overlay drawing adapter test failed.' }
} finally {
    $env:PATH = $previousPath
}
$native | Set-Content -LiteralPath (Join-Path $ReportDirectory 'adapter.json') -Encoding utf8
$native = $native | ConvertFrom-Json
if (-not $native.passed -or $native.checks -lt 26) { throw 'Incomplete adapter checks.' }
$previousGpu = $env:NATRON_VULKAN_DEVICE
$previousScale = $env:QT_SCALE_FACTOR
$cases = @()
try {
    foreach ($gpu in $GpuIndices) {
        foreach ($scale in $ScaleFactors) {
            if ($gpu -lt 0 -or $scale -notin @(1, 2)) { throw 'Unsupported GPU or screen scale.' }
            $env:NATRON_VULKAN_DEVICE = "$gpu"
            $env:QT_SCALE_FACTOR = "$scale"
            $cell = Join-Path $ReportDirectory "gpu$gpu-scale$scale"
            New-Item -ItemType Directory -Force -Path $cell | Out-Null
            & tools/run-windows-qt6.ps1 -Program IOOverlaySmoke -GuiTimeoutSeconds $TimeoutSeconds *> (Join-Path $cell 'launcher.log')
            if ($LASTEXITCODE -ne 0) { throw "IO overlay GUI failed on GPU $gpu at scale $scale." }
            $result = Get-Content build/io-overlay-gui-smoke/result.json -Raw | ConvertFrom-Json
            if (-not $result.ok -or $result.cases.Count -ne 4) { throw 'Incomplete IO overlay GUI checks.' }
            $profile = Get-ChildItem build/test-profiles -Directory -Filter 'IOOverlaySmoke-*' |
                Sort-Object LastWriteTime -Descending | Select-Object -First 1
            if (-not $profile -or -not (Test-Path -LiteralPath (Join-Path $profile.FullName 'started.txt')) -or
                (Test-Path -LiteralPath (Join-Path $profile.FullName 'timeout.json'))) { throw 'Invalid GUI test profile.' }
            Copy-Item -LiteralPath (Join-Path $profile.FullName 'stdout.log'), (Join-Path $profile.FullName 'stderr.log'),
                (Join-Path $profile.FullName 'started.txt') -Destination $cell
            Copy-Item -LiteralPath 'build/io-overlay-gui-smoke/result.json' -Destination $cell
            $captures = @()
            foreach ($case in $result.cases) {
                foreach ($label in @('before', 'after', 'focus') + $(if ($case.kind -eq 'rectangle') { @('outline') } else { @() })) {
                    $capture = $case.$label
                    if ([Math]::Abs($capture.device_pixel_ratio - $scale) -gt .01) { throw 'Requested screen density was not used.' }
                    $source = Join-Path 'build/io-overlay-gui-smoke' $capture.artifact
                    Copy-Item -LiteralPath $source -Destination $cell
                    $captures += @{file=$capture.artifact;sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()}
                }
            }
            $cases += @{gpu=$gpu;scale=$scale;result=$result;profile=$profile.FullName;captures=$captures}
            Write-Output "IO overlay GPU $gpu scale $scale passed."
        }
    }
} finally {
    $env:NATRON_VULKAN_DEVICE = $previousGpu
    $env:QT_SCALE_FACTOR = $previousScale
}
foreach ($file in $files) {
    if ((Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant() -ne $hashes[$file]) { throw "Test input changed: $file" }
}
$preferencesAfter = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0 -or $preferencesAfter -ne $preferencesBefore) { throw 'User preferences changed.' }
$sourceAfter = & uv run --frozen python tools/verify-io-vulkan-source.py
if ($LASTEXITCODE -ne 0) { throw 'IO source verification failed after tests.' }
@{passed=$true;fileSha256=$hashes;preferencesSha256=$preferencesBefore;ioImports=$imports;
  source=$sourceProof;adapter=$native;cases=$cases;finishedAt=[DateTime]::UtcNow.ToString('o')} |
    ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "IO overlay proof: $reportPath"
