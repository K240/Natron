[CmdletBinding()]
param(
    [string]$SdkRoot = 'C:\VulkanSDK\1.4.363.0',
    [string]$QtRoot = 'D:\Qt\6.11.2\msvc2022_64',
    [string]$BuildDirectory = 'build\windows-vulkan',
    [string]$ReportDirectory = 'build\vulkan-tests',
    [int[]]$GpuIndices = @(0, 1),
    [ValidateSet('Compute', 'DeviceLoss', 'MemoryBudget', 'Renderer', 'Viewer', 'ViewerDisplay', 'ViewerGrid', 'ImageOperations', 'FragmentProgram', 'FragmentLut')]
    [string[]]$Tests = @('Compute', 'DeviceLoss', 'MemoryBudget', 'Renderer', 'Viewer', 'ViewerDisplay', 'ViewerGrid', 'ImageOperations', 'FragmentProgram', 'FragmentLut'),
    [ValidateRange(1, 600)]
    [int]$TimeoutSeconds = 90
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $repoRoot $BuildDirectory }
if (-not [IO.Path]::IsPathRooted($ReportDirectory)) { $ReportDirectory = Join-Path $repoRoot $ReportDirectory }
$previousPath = $env:PATH
$previousPlugins = $env:QT_PLUGIN_PATH
$previousLayerPath = $env:VK_LAYER_PATH
try {
    $env:PATH = "$QtRoot\bin;$SdkRoot\Bin;$previousPath"
    $env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
    $env:VK_LAYER_PATH = Join-Path $SdkRoot 'Bin'
    foreach ($gpu in $GpuIndices) {
        $output = Join-Path $ReportDirectory "gpu$gpu"
        New-Item -ItemType Directory -Force -Path $output | Out-Null
        foreach ($test in $Tests) {
            $executable = Join-Path $BuildDirectory "GpuTests\NatronVulkan$test.exe"
            $arguments = "--gpu $gpu"
            if ($test -eq 'Viewer') { $arguments += ' --output "' + $output + '"' }
            $stdout = Join-Path $output "$test.stdout.log"
            $stderr = Join-Path $output "$test.stderr.log"
            $launch = @{
                FilePath = $executable
                ArgumentList = $arguments
                PassThru = $true
                WindowStyle = 'Hidden'
                RedirectStandardOutput = $stdout
                RedirectStandardError = $stderr
            }
            $process = Start-Process @launch
            if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
                # Only this test's process is terminated. Existing Natron sessions
                # and user projects are never closed by the watchdog.
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
                if (-not $process.WaitForExit(5000)) { throw "Could not terminate Vulkan test PID $($process.Id). Logs: $output" }
                throw "$test GPU $gpu timed out after $TimeoutSeconds seconds. Logs: $output"
            }
            $process.Refresh()
            if ($process.ExitCode -ne 0) {
                Get-Content -LiteralPath $stderr
                throw "$test GPU $gpu failed with exit code $($process.ExitCode). Logs: $output"
            }
            Write-Output "$test GPU $gpu passed. Logs: $output"
        }
    }
} finally {
    $env:PATH = $previousPath
    $env:QT_PLUGIN_PATH = $previousPlugins
    $env:VK_LAYER_PATH = $previousLayerPath
}
exit 0
