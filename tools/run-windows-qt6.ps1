[CmdletBinding()]
param(
    [ValidateSet('Natron', 'NatronRenderer', 'NatronPython', 'Tests', 'CTest', 'Smoke', 'GuiSmoke', 'OfxSmoke', 'OfxGuiSmoke')]
    [string]$Program = 'Natron',
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$BuildDirectory = 'build\windows-qt6',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ProgramArguments
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
$pythonRoot = & uv run --frozen python -c 'import sys; print(sys.base_prefix)'
if ($LASTEXITCODE -ne 0) { throw 'Could not locate the uv Python environment.' }
$sitePackages = Join-Path $repoRoot '.venv\Lib\site-packages'
$depsBin = Join-Path $repoRoot 'build\vcpkg_installed\x64-windows\bin'
$ofxRuntime = Join-Path $repoRoot 'build\ofx-runtime'
$env:PATH = "$QtRoot\bin;$sitePackages\PySide6;$sitePackages\shiboken6;$pythonRoot;$depsBin;$ofxRuntime\bin;$env:PATH"
$env:PYTHONHOME = $pythonRoot
$env:PYTHONPATH = $sitePackages
$env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
$env:OCIO = Join-Path $repoRoot 'OpenColorIO-Configs\blender\config.ocio'
$env:OFX_PLUGIN_PATH = Join-Path $repoRoot 'build\Plugins'
$env:FONTCONFIG_PATH = Join-Path $repoRoot 'build\vcpkg_installed\x64-windows\etc\fonts'
$env:FONTCONFIG_FILE = Join-Path $env:FONTCONFIG_PATH 'fonts.conf'
if (Test-Path -LiteralPath $ofxRuntime) {
    $env:MAGICK_CONFIGURE_PATH = Join-Path $ofxRuntime 'etc\ImageMagick-7'
    $coderDirectory = Get-ChildItem -LiteralPath (Join-Path $ofxRuntime 'lib') -Directory -Filter 'ImageMagick-*' | Select-Object -First 1
    if ($coderDirectory) {
        $env:MAGICK_CODER_MODULE_PATH = Join-Path $coderDirectory.FullName 'modules-Q16HDRI\coders'
        $env:MAGICK_FILTER_MODULE_PATH = Join-Path $coderDirectory.FullName 'modules-Q16HDRI\filters'
    }
}
if ($Program -in @('Tests', 'CTest', 'Smoke', 'GuiSmoke', 'OfxSmoke', 'OfxGuiSmoke') -and -not (Test-Path -LiteralPath "$env:OFX_PLUGIN_PATH\IO.ofx.bundle")) {
    $pluginArchive = Join-Path $repoRoot 'build\openfx-io-testing.zip'
    Invoke-WebRequest -Uri 'https://github.com/NatronGitHub/openfx-io/releases/download/natron_testing/openfx-io-build-windows_latest-testing.zip' -OutFile $pluginArchive
    New-Item -Path $env:OFX_PLUGIN_PATH -ItemType Directory -Force | Out-Null
    & tar -xf $pluginArchive -C $env:OFX_PLUGIN_PATH
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the OFX test plugins.' }
}
$smokeResult = $null
if ($Program -eq 'Smoke') {
    $smokeResult = Join-Path $repoRoot 'build\qt6-smoke\result.json'
    $ProgramArguments = @('--no-settings', 'tools/qt6-smoke.py', '1')
} elseif ($Program -eq 'GuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\gui-qt6-result.json'
    $ProgramArguments = @('--no-settings', 'tools/qt6-gui-smoke.py')
} elseif ($Program -eq 'OfxSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ofx-smoke\result.json'
    $ProgramArguments = @('--no-settings', '--clear-openfx-cache', 'tools/ofx-smoke.py', '1')
} elseif ($Program -eq 'OfxGuiSmoke') {
    $smokeResult = Join-Path $repoRoot 'build\ofx-gui-result.json'
    $ProgramArguments = @('--no-settings', 'tools/ofx-gui-smoke.py')
}
if ($smokeResult -and (Test-Path -LiteralPath $smokeResult)) {
    Remove-Item -LiteralPath $smokeResult
}
if ($Program -eq 'CTest') {
    $executable = (Get-Command ctest -ErrorAction Stop).Source
    $ProgramArguments = @('--test-dir', $BuildDirectory, '--output-on-failure') + $ProgramArguments
} else {
    $binary = @{ Smoke = 'NatronRenderer'; GuiSmoke = 'Natron'; OfxSmoke = 'NatronRenderer'; OfxGuiSmoke = 'Natron' }[$Program]
    if (-not $binary) { $binary = $Program }
    $module = @{ Natron = 'App'; NatronRenderer = 'Renderer'; NatronPython = 'PythonBin'; Tests = 'Tests' }[$binary]
    $binaryName = if ($binary -eq 'NatronPython') { 'natron-python' } else { $binary }
    $executable = Join-Path $BuildDirectory "$module\$binaryName.exe"
}
if (-not (Test-Path -LiteralPath $executable)) { throw "Build executable not found: $executable" }
& uv run --frozen -- $executable @ProgramArguments
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
if ($smokeResult) {
    if (-not (Test-Path -LiteralPath $smokeResult)) { throw 'Smoke test did not finish successfully. See the preceding Python diagnostics.' }
    $report = Get-Content -LiteralPath $smokeResult -Raw | ConvertFrom-Json
    if ($Program -in @('GuiSmoke', 'OfxSmoke', 'OfxGuiSmoke') -and -not $report.ok) { throw "$Program failed." }
    Write-Output (Get-Content -LiteralPath $smokeResult -Raw)
}
exit $LASTEXITCODE
