[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Script,
    [Parameter(Mandatory)][string]$Profile,
    [Parameter(Mandatory)][string]$Result,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 150,
    [ValidateSet('8u', '32f')][string]$TextureDepth = '32f',
    [ValidateSet('Natron', 'NatronRenderer')][string]$Program = 'Natron',
    [string]$OfxCacheSource,
    [string]$BuildDirectory = 'build/windows-qt6',
    [string]$OfxPluginDirectory = 'build/Plugins',
    [string]$OfxRuntimeDirectory = 'build/ofx-runtime',
    [string]$OfxTestPluginDirectory = 'build/ofx-tests',
    [string]$GpuMemoryReport,
    [ValidateSet(0, 1)][int]$Validation = 1,
    [hashtable]$ProfileSettings = @{},
    [string[]]$ProgramArguments = @(),
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64'
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-TaskPath([string]$value) {
    return [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($value)) { $value } else { Join-Path $repoRoot $value }))
}
$profilePath = Resolve-TaskPath $Profile
$profileRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot 'build/test-profiles'))
if (-not $profilePath.StartsWith($profileRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $profilePath)) {
    throw 'Use a new owned profile under build/test-profiles.'
}
New-Item -ItemType Directory -Path (Join-Path $profilePath 'settings/INRIA') -Force | Out-Null
@('[General]', 'checkForUpdates=false', 'startupCheckOCIO=false', 'autoTurbo=false', "texturesBitDepth=$TextureDepth") |
    Set-Content -LiteralPath (Join-Path $profilePath 'settings/INRIA/Natron.ini') -Encoding utf8
foreach ($name in $ProfileSettings.Keys) {
    if ($name -notmatch '^[A-Za-z][A-Za-z0-9_]*$' -or [string]$ProfileSettings[$name] -match '[\r\n]') { throw 'Invalid owned profile setting.' }
    Add-Content -LiteralPath (Join-Path $profilePath 'settings/INRIA/Natron.ini') -Value "$name=$($ProfileSettings[$name])" -Encoding utf8
}
foreach ($directory in @('data', 'cache', 'temp')) { New-Item -ItemType Directory -Path (Join-Path $profilePath $directory) | Out-Null }
if ($OfxCacheSource) {
    $cacheSource = Resolve-TaskPath $OfxCacheSource
    if (-not $cacheSource.StartsWith($profileRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or
        -not (Test-Path -LiteralPath $cacheSource -PathType Leaf) -or
        [IO.Path]::GetFileName($cacheSource) -notmatch '^OFXCache_[A-Za-z0-9_.-]+\.xml$') {
        throw 'Seed only an OFX cache XML from an owned test profile.'
    }
    $cacheDestination = Join-Path $profilePath 'cache/OFXLoadCache'
    New-Item -ItemType Directory -Path $cacheDestination | Out-Null
    Copy-Item -LiteralPath $cacheSource -Destination (Join-Path $cacheDestination ([IO.Path]::GetFileName($cacheSource)))
}
$pythonRoot = & uv run --frozen python -c 'import sys; print(sys.base_prefix)'
if ($LASTEXITCODE -ne 0) { throw 'Could not locate the uv Python runtime.' }
$sitePackages = Join-Path $repoRoot '.venv/Lib/site-packages'
$ofxRuntime = Resolve-TaskPath $OfxRuntimeDirectory
$programPath = if ($Program -eq 'Natron') { 'App/Natron.exe' } else { 'Renderer/NatronRenderer.exe' }
$start = [Diagnostics.ProcessStartInfo]::new((Join-Path (Resolve-TaskPath $BuildDirectory) $programPath))
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.WindowStyle = 'Hidden'
$start.WorkingDirectory = $repoRoot
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
if ($Program -eq 'NatronRenderer') {
    $start.RedirectStandardInput = $true
    $start.ArgumentList.Add('--interpreter')
}
if (-not $OfxCacheSource) { $start.ArgumentList.Add('--clear-openfx-cache') }
foreach ($argument in $ProgramArguments) { $start.ArgumentList.Add($argument) }
$start.ArgumentList.Add((Resolve-TaskPath $Script))
$childEnvironment = @{
    PATH = "$QtRoot\bin;$sitePackages\PySide6;$sitePackages\shiboken6;$pythonRoot;$repoRoot\build\vcpkg_installed\x64-windows\bin;$ofxRuntime\bin;$env:PATH"
    PYTHONHOME = $pythonRoot; PYTHONPATH = $sitePackages; QT_PLUGIN_PATH = "$QtRoot\plugins"
    OCIO = "$repoRoot\OpenColorIO-Configs\blender\config.ocio"
    OFX_PLUGIN_PATH = "$(Resolve-TaskPath $OfxPluginDirectory);$(Resolve-TaskPath $OfxTestPluginDirectory)"
    FONTCONFIG_PATH = "$repoRoot\build\vcpkg_installed\x64-windows\etc\fonts"
    FONTCONFIG_FILE = "$repoRoot\build\vcpkg_installed\x64-windows\etc\fonts\fonts.conf"
    MAGICK_CONFIGURE_PATH = "$ofxRuntime\etc\ImageMagick-7"
    NATRON_TEST_PROFILE = $profilePath; NATRON_TEST_START_MARKER = "$profilePath\started.txt"
    VK_LAYER_PATH = 'C:\VulkanSDK\1.4.363.0\Bin'; DISABLE_VULKAN_OBS_CAPTURE = '1'
    NATRON_VULKAN_VALIDATION = [string]$Validation; NATRON_TEST_VULKAN_VALIDATION = [string]$Validation
    NATRON_GUI_TEST_RESULT = (Resolve-TaskPath $Result)
}
$magick = Get-ChildItem -LiteralPath "$ofxRuntime\lib" -Directory -Filter 'ImageMagick-*' | Select-Object -First 1
if ($magick) {
    $childEnvironment.MAGICK_CODER_MODULE_PATH = "$($magick.FullName)\modules-Q16HDRI\coders"
    $childEnvironment.MAGICK_FILTER_MODULE_PATH = "$($magick.FullName)\modules-Q16HDRI\filters"
}
foreach ($name in $childEnvironment.Keys) { $start.Environment[$name] = $childEnvironment[$name] }
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
$hasStarted = $false
$memoryProcess = $null
try {
    if (-not $process.Start()) { throw 'Natron test process did not start.' }
    $hasStarted = $true
    if ($GpuMemoryReport) {
        $memoryPath = Resolve-TaskPath $GpuMemoryReport
        if (Test-Path -LiteralPath $memoryPath) { throw 'Use a fresh memory report.' }
        $memoryStart = [Diagnostics.ProcessStartInfo]::new((Get-Command uv -ErrorAction Stop).Source)
        $memoryStart.UseShellExecute = $false
        $memoryStart.CreateNoWindow = $true
        $memoryStart.WindowStyle = 'Hidden'
        $memoryStart.WorkingDirectory = $repoRoot
        foreach ($argument in @('run', '--frozen', 'python', "$repoRoot/tools/windows-gpu-memory.py", '--pid', [string]$process.Id,
                                 '--output', $memoryPath, '--seconds', [string]($TimeoutSeconds + 10))) { $memoryStart.ArgumentList.Add($argument) }
        $memoryStart.RedirectStandardOutput = $true
        $memoryStart.RedirectStandardError = $true
        $memoryProcess = [Diagnostics.Process]::Start($memoryStart)
        $memoryStdout = $memoryProcess.StandardOutput.ReadToEndAsync()
        $memoryStderr = $memoryProcess.StandardError.ReadToEndAsync()
        Set-Content -LiteralPath "$profilePath/process.json" -Value (@{ pid = $process.Id; memoryPid = $memoryProcess.Id } | ConvertTo-Json) -Encoding utf8
    }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    if ($Program -eq 'NatronRenderer') {
        $process.StandardInput.WriteLine("assert app is app1`nprint('NATRON_OWNED_INTERPRETER_EOF_OK')")
        $process.StandardInput.Close()
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while (-not $process.WaitForExit(500)) {
        if ([DateTime]::UtcNow -ge $deadline) {
            @{ pid = $process.Id; script = $Script; timeoutSeconds = $TimeoutSeconds } | ConvertTo-Json |
                Set-Content -LiteralPath "$profilePath/timeout.json" -Encoding utf8
            $process.Kill()
            if (-not $process.WaitForExit(5000)) { throw 'Owned test process could not terminate.' }
            throw "GUI test timed out. Profile: $profilePath"
        }
    }
    if ($process.ExitCode -ne 0) { throw "GUI test exited with code $($process.ExitCode). Profile: $profilePath" }
} finally {
    if ($hasStarted -and -not $process.HasExited) {
        $process.Kill()
        $process.WaitForExit(5000) | Out-Null
    }
    if ($hasStarted -and $process.HasExited) {
        $stdout.GetAwaiter().GetResult() | Set-Content -LiteralPath "$profilePath/stdout.log" -Encoding utf8
        $stderr.GetAwaiter().GetResult() | Set-Content -LiteralPath "$profilePath/stderr.log" -Encoding utf8
        $cache = Join-Path $profilePath 'cache/ViewerCache'
        if (Test-Path -LiteralPath $cache) {
            foreach ($file in Get-ChildItem -LiteralPath $cache -File -Filter 'CachePart*') {
                $full = [IO.Path]::GetFullPath($file.FullName)
                if ($file.Name -match '^CachePart[0-9]+$' -and $full.StartsWith($profilePath + '\', [StringComparison]::OrdinalIgnoreCase)) {
                    Remove-Item -LiteralPath $full
                }
            }
        }
    }
    $process.Dispose()
    if ($memoryProcess) {
        if (-not $memoryProcess.WaitForExit(10000)) { $memoryProcess.Kill(); $memoryProcess.WaitForExit(5000) | Out-Null }
        $memoryStdout.GetAwaiter().GetResult() | Set-Content -LiteralPath "$profilePath/memory.stdout.log" -Encoding utf8
        $memoryStderr.GetAwaiter().GetResult() | Set-Content -LiteralPath "$profilePath/memory.stderr.log" -Encoding utf8
        $memoryExit = $memoryProcess.ExitCode
        $memoryProcess.Dispose()
        if ($memoryExit -ne 0) { throw "Driver memory measurement failed: $profilePath" }
    }
}
$resultPath = $childEnvironment.NATRON_GUI_TEST_RESULT
$report = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
if (-not $report.ok -or $report.profile -ne $profilePath) { throw "GUI assertions failed: $resultPath" }
if (Select-String -LiteralPath "$profilePath/stderr.log" -Pattern 'VUID-|Vulkan validation error:|Vulkan runtime error:|Vulkan Viewer:') {
    throw "GUI test reported Vulkan errors: $profilePath"
}
Write-Output "GUI test passed. Profile: $profilePath"
