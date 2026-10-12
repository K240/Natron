[CmdletBinding()]
param(
    [string]$PluginDirectory = 'build/Plugins',
    [string]$ReportDirectory = 'build/windows-ofx-interact-inventory',
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$BuildDirectory = 'build/ofx-interact-inventory',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $repoRoot
function Resolve-TaskPath([string]$value) {
    return [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($value)) { $value } else { Join-Path $repoRoot $value }))
}
$pluginPath = Resolve-TaskPath $PluginDirectory
$destination = Resolve-TaskPath $ReportDirectory
$buildPath = Resolve-TaskPath $BuildDirectory
if (Test-Path -LiteralPath $destination) { throw 'Use a fresh report directory.' }
if (-not (Test-Path -LiteralPath $pluginPath)) { throw "Plugin directory does not exist: $pluginPath" }
$pythonRoot = & uv run --frozen python -c 'import sys; print(sys.base_prefix)'
$pythonExecutable = & uv run --frozen python -c 'import sys; print(sys.executable)'
if ($LASTEXITCODE) { throw 'Could not locate the uv Python environment.' }
if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Visual Studio x64 C++ tools are required.' }
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
    $ninja = Join-Path (Split-Path (Split-Path $QtRoot)) 'Tools/Ninja/ninja.exe'
    & cmake -S build-configs/ofx-interact-inventory -B $buildPath -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=$ninja" "-DCMAKE_PREFIX_PATH=$QtRoot;$repoRoot/build/vcpkg_installed/x64-windows" "-DPython3_EXECUTABLE=$pythonExecutable"
    if ($LASTEXITCODE) { throw 'Descriptor inventory CMake configuration failed.' }
    & cmake --build $buildPath --parallel 4
    if ($LASTEXITCODE) { throw 'Descriptor inventory build failed.' }
}
New-Item -ItemType Directory -Path $destination | Out-Null
foreach ($mode in @('native', 'legacy-discovery')) {
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $buildPath 'OfxInteractInventory.exe'))
    $start.WorkingDirectory = $repoRoot
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = 'Hidden'
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($arg in @($pluginPath, (Join-Path $destination "$mode.json"), $mode)) { $start.ArgumentList.Add($arg) }
    $start.Environment['PATH'] = "$QtRoot\bin;$pythonRoot;$repoRoot\build\vcpkg_installed\x64-windows\bin;$repoRoot\build\ofx-runtime\bin;$env:PATH"
    $start.Environment['OFX_PLUGIN_PATH'] = ''
    $start.Environment['OCIO'] = "$repoRoot\OpenColorIO-Configs\blender\config.ocio"
    $start.Environment['FONTCONFIG_PATH'] = "$repoRoot\build\vcpkg_installed\x64-windows\etc\fonts"
    $start.Environment['FONTCONFIG_FILE'] = "$repoRoot\build\vcpkg_installed\x64-windows\etc\fonts\fonts.conf"
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    try {
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            throw "Descriptor inspector timed out (owned PID $($process.Id))."
        }
        $stdout.Result | Set-Content -LiteralPath (Join-Path $destination "$mode.stdout.log") -Encoding utf8
        $stderr.Result | Set-Content -LiteralPath (Join-Path $destination "$mode.stderr.log") -Encoding utf8
        if ($process.ExitCode) { throw "Descriptor inspection failed: $mode. See $destination." }
    } finally { $process.Dispose() }
}
& uv run --frozen python tools/summarize-ofx-interacts.py --directory $destination
if ($LASTEXITCODE) { throw 'Descriptor inventory verification failed.' }
