[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][string]$ReportDirectory,
    [string]$SoftwareDriverDirectory,
    [ValidateRange(1,600)][int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$archivePath = (Resolve-Path -LiteralPath $Archive).Path
$reportPath = [IO.Path]::GetFullPath($ReportDirectory)
if (Test-Path -LiteralPath $reportPath) { throw 'Use a fresh report directory.' }
New-Item -ItemType Directory -Path $reportPath | Out-Null
# A relocated path containing spaces and Unicode catches installation assumptions.
$packagePath = Join-Path $env:TEMP ('Natron Portable 配布 ' + [Guid]::NewGuid().ToString('N'))
[IO.Compression.ZipFile]::ExtractToDirectory($archivePath,$packagePath)
$profilePath = Join-Path $repoRoot ('build/test-profiles/Portable-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$profilePath/settings/INRIA" -Force | Out-Null
foreach ($name in @('data','cache','temp')) { New-Item -ItemType Directory -Path (Join-Path $profilePath $name) | Out-Null }
# Renderer saves existingSettings before the GUI starts. Seed the current GUI
# versions so this owned profile does not ask to migrate absent shortcuts.
@('[General]','checkForUpdates=false','startupCheckOCIO=false','autoTurbo=false','texturesBitDepth=8u',
  'appearanceVersion=1','NATRON_SHORTCUTS_DEFAULT_VERSION=8') |
    Set-Content -LiteralPath "$profilePath/settings/INRIA/Natron.ini" -Encoding utf8
$settings = & uv run --frozen python "$PSScriptRoot/test-settings-fingerprint.py"
if ($LASTEXITCODE) { throw 'Could not fingerprint settings.' }
$proof = [ordered]@{passed=$false;archiveSha256=(Get-FileHash -LiteralPath $archivePath).Hash.ToLowerInvariant();
    package=$packagePath;profile=$profilePath;settingsBefore=$settings;cases=@();excludedTests=@('parsec')}
$proof.testDefinitions = @{}
foreach ($name in @('test-windows-portable.ps1','portable-package-smoke.py','ofx-smoke.py','qt6-gui-smoke.py','test-settings-fingerprint.py','windows-module-audit.py')) {
    $proof.testDefinitions[$name] = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash.ToLowerInvariant()
}
function Start-PackageProcess([string]$Executable,[string[]]$Arguments,[string]$Name,[bool]$Interactive) {
    $binary = Join-Path $packagePath "bin/$Executable"
    $start = [Diagnostics.ProcessStartInfo]::new($(if ($Name -eq 'python') { (Get-Command uv).Source } else { $binary }))
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    # Presentation acceptance requires the owned GUI to be exposed. Keep
    # command-line helpers hidden, but do not suppress this test window.
    $start.WindowStyle = if ($Name -eq 'gui') { 'Normal' } else { 'Hidden' }
    $start.WorkingDirectory = $reportPath
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.RedirectStandardInput = $Interactive
    if ($Name -eq 'python') {
        foreach ($argument in @('run','--no-project','--no-python-downloads',$binary)) { $start.ArgumentList.Add($argument) }
    }
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    foreach ($key in @($start.Environment.Keys)) {
        if ($key -match '^(PYTHON|QT_|OFX_|OCIO|FONTCONFIG|MAGICK_|VK_|VULKAN_|NATRON_)') { $start.Environment.Remove($key) | Out-Null }
    }
    $environment = @{
        PATH="$packagePath/bin;$packagePath/ofx-runtime/bin;$packagePath/Lib/site-packages/PySide6;$packagePath/Lib/site-packages/shiboken6;$env:WINDIR/System32;$env:WINDIR"
        PYTHONHOME=$packagePath;PYTHONPATH="$packagePath/Lib/site-packages";PYTHONNOUSERSITE='1'
        QT_PLUGIN_PATH="$packagePath/qtplugins";OCIO="$packagePath/Resources/OpenColorIO-Configs/blender/config.ocio"
        OFX_PLUGIN_PATH="$packagePath/Plugins/OFX/Natron";FONTCONFIG_PATH="$packagePath/Resources/etc/fonts"
        FONTCONFIG_FILE="$packagePath/Resources/etc/fonts/fonts.conf";MAGICK_CONFIGURE_PATH="$packagePath/ofx-runtime/etc/ImageMagick-7"
        NATRON_PORTABLE_ROOT=$packagePath;NATRON_TEST_PROFILE=$profilePath
        NATRON_TEST_START_MARKER="$profilePath/started-$Name.txt"
        NATRON_GUI_TEST_RESULT="$reportPath/$Name/result.json";NATRON_PORTABLE_OFX_TEST="$PSScriptRoot/ofx-smoke.py"
        NATRON_PORTABLE_MODULE_TEST="$PSScriptRoot/windows-module-audit.py"
        DISABLE_VULKAN_OBS_CAPTURE='1';NATRON_VULKAN_DEVICE='0';NATRON_VULKAN_VALIDATION='0';NATRON_TEST_VULKAN_VALIDATION='0'
    }
    if ($SoftwareDriverDirectory) {
        $driver = (Resolve-Path -LiteralPath $SoftwareDriverDirectory).Path
        $environment.VK_DRIVER_FILES = (Resolve-Path -LiteralPath "$driver/vk_swiftshader_icd.json").Path
        $environment.VK_ICD_FILENAMES = $environment.VK_DRIVER_FILES
        $environment.PATH = "$driver/bin;$($environment.PATH)"
        $environment.NATRON_PORTABLE_SOFTWARE_DRIVER = $driver
    }
    $magick = Get-ChildItem -LiteralPath "$packagePath/ofx-runtime/lib" -Directory -Filter 'ImageMagick-*' | Select-Object -First 1
    $environment.MAGICK_CODER_MODULE_PATH = "$($magick.FullName)/modules-Q16HDRI/coders"
    $environment.MAGICK_FILTER_MODULE_PATH = "$($magick.FullName)/modules-Q16HDRI/filters"
    foreach ($key in $environment.Keys) { $start.Environment[$key]=$environment[$key] }
    $process = [Diagnostics.Process]::Start($start)
    $stdout=$process.StandardOutput.ReadToEndAsync()
    $stderr=$process.StandardError.ReadToEndAsync()
    try {
        if ($Interactive) { $process.StandardInput.WriteLine("print('NATRON_PORTABLE_EOF_OK')");$process.StandardInput.Close() }
        $deadline=[DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        while (-not $process.WaitForExit(500)) {
            if ([DateTime]::UtcNow -ge $deadline) {
                $process.Kill($true);$process.WaitForExit(5000) | Out-Null
                throw "Portable process timed out: $Name; owned PID $($process.Id)"
            }
        }
        $out=$stdout.GetAwaiter().GetResult();$err=$stderr.GetAwaiter().GetResult()
        $out | Set-Content -LiteralPath "$reportPath/$Name.stdout.log" -Encoding utf8
        $err | Set-Content -LiteralPath "$reportPath/$Name.stderr.log" -Encoding utf8
        if ($process.ExitCode -ne 0 -or $err -match 'Traceback|VUID-|Vulkan runtime error:') { throw "Portable $Name failed with exit $($process.ExitCode)." }
        if ($Interactive -and -not $out.Contains('NATRON_PORTABLE_EOF_OK')) { throw 'Interpreter did not finish.' }
        $proof.cases += @{name=$Name;exitCode=$process.ExitCode;pid=$process.Id;stdoutSha256=(Get-FileHash "$reportPath/$Name.stdout.log").Hash.ToLowerInvariant();
            stderrSha256=(Get-FileHash "$reportPath/$Name.stderr.log").Hash.ToLowerInvariant()}
    } finally {
        if (-not $process.HasExited) { $process.Kill($true);$process.WaitForExit(5000) | Out-Null }
        $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath "$reportPath/$Name.stdout.log" -Encoding utf8
        $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath "$reportPath/$Name.stderr.log" -Encoding utf8
        $process.Dispose()
    }
}
try {
    $manifest = Get-Content -LiteralPath "$packagePath/manifest.json" -Raw | ConvertFrom-Json -AsHashtable
    foreach ($name in $manifest.files.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $packagePath $name)).Hash.ToLowerInvariant() -ne $manifest.files[$name]) { throw "Package hash mismatch: $name" }
    }
    Start-PackageProcess 'natron-python.exe' @('-c','import sys,qtpy; from PySide6.QtGui import QImage; print(sys.prefix,qtpy.API_NAME)') 'python' $false
    Start-PackageProcess 'NatronRenderer.exe' @('--clear-openfx-cache','--interpreter',"$PSScriptRoot/portable-package-smoke.py") 'renderer' $true
    $renderer = Get-Content -LiteralPath "$reportPath/renderer/result.json" -Raw | ConvertFrom-Json
    if (-not $renderer.ok -or $renderer.nativePixelsVerified -ne 3072 -or $renderer.unexpectedModules.Count) { throw 'Portable renderer assertions failed.' }
    Start-PackageProcess 'Natron.exe' @('--clear-openfx-cache',"$PSScriptRoot/qt6-gui-smoke.py") 'gui' $false
    $gui = Get-Content -LiteralPath "$reportPath/build/gui-qt6-result.json" -Raw | ConvertFrom-Json
    if (-not $gui.ok -or $gui.unexpected_dialogs.Count -or $gui.qt_gl_modules.Count -or $gui.unexpected_modules.Count) { throw 'Portable GUI assertions failed.' }
    $proof.renderer=$renderer
    $proof.gui=$gui
    $proof.settingsAfter=& uv run --frozen python "$PSScriptRoot/test-settings-fingerprint.py"
    if ($LASTEXITCODE -or $proof.settingsAfter -ne $settings) { throw 'User settings changed.' }
    foreach ($name in $proof.testDefinitions.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name)).Hash.ToLowerInvariant() -ne $proof.testDefinitions[$name]) { throw "Test definition changed: $name" }
    }
    $proof.passed=$true
} catch { $proof.failure=$_.Exception.Message;throw }
finally {
    $proof.settingsAfter=& uv run --frozen python "$PSScriptRoot/test-settings-fingerprint.py"
    if ($LASTEXITCODE -or $proof.settingsAfter -ne $settings) { $proof.passed=$false;$proof.failure='User settings changed or could not be fingerprinted.' }
    foreach ($log in Get-ChildItem -LiteralPath $reportPath -File -Filter '*.log') {
        $proof[$log.Name + 'Sha256']=(Get-FileHash -LiteralPath $log.FullName).Hash.ToLowerInvariant()
    }
    $cache=Join-Path $profilePath 'cache/ViewerCache'
    if (Test-Path -LiteralPath $cache) {
        foreach ($file in Get-ChildItem -LiteralPath $cache -File -Filter 'CachePart*') {
            $full=[IO.Path]::GetFullPath($file.FullName)
            if ($file.Name -match '^CachePart[0-9]+$' -and $full.StartsWith($profilePath + '\', [StringComparison]::OrdinalIgnoreCase)) {
                Remove-Item -LiteralPath $full
            }
        }
    }
    $proof | ConvertTo-Json -Depth 14 | Set-Content -LiteralPath "$reportPath/report.json" -Encoding utf8
}
Write-Output "Portable Python, OFX/native Renderer and GUI passed: $reportPath/report.json"
