[CmdletBinding()]
param(
    [string]$ReportDirectory = 'build/windows-renderer-api-comparison-tests',
    [string]$LegacyBuildDirectory = 'build/legacy-gl-comparison',
    [string]$LegacySourceDirectory,
    [string]$QtRoot = 'D:/Qt/6.11.2/msvc2022_64',
    [ValidateRange(5, 40)][int]$Samples = 20
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $taskRoot
if (-not $LegacySourceDirectory) {
    $entry = Select-String -LiteralPath "$LegacyBuildDirectory/CMakeCache.txt" -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$'
    if (-not $entry) { throw 'Specify the legacy comparison source directory.' }
    $LegacySourceDirectory = $entry.Matches[0].Groups[1].Value
}
$destination = [IO.Path]::GetFullPath((Join-Path $taskRoot $ReportDirectory))
if (Test-Path -LiteralPath $destination) { throw 'Use a new report directory.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$files = @('tools/renderer-api-comparison.py', 'tools/windows-gpu-memory.py', 'tools/run-windows-owned-gui-test.ps1',
    'tools/test-windows-renderer-api-comparison.ps1', 'build/windows-qt6/App/Natron.exe', 'build/windows-qt6/Renderer/NatronRenderer.exe',
    'build/windows-qt6/build.ninja', 'Global/WindowsUtf8.manifest', 'build/ofx-tests/VulkanBenchmarkSmoke.ofx.bundle/Contents/Win64/VulkanBenchmarkSmoke.ofx',
    "$LegacyBuildDirectory/App/Natron.exe", "$LegacyBuildDirectory/Renderer/NatronRenderer.exe", "$LegacyBuildDirectory/build.ninja",
    'build/IO-before-vulkan/IO.ofx.bundle/Contents/Win64/IO.ofx', 'OpenColorIO-Configs/blender/config.ocio')
foreach ($directory in @('Engine', 'Gui', 'Global', 'Gpu', 'Tests', 'build-configs/windows-ofx/native')) {
    $files += @(Get-ChildItem -LiteralPath $directory -File -Recurse | Where-Object { $_.Extension -in @('.cpp','.h','.comp','.frag','.vert') -or $_.Name -eq 'CMakeLists.txt' } | ForEach-Object FullName)
}
foreach ($directory in @('build/Plugins','build/ofx-runtime','build/legacy-gl-comparison-runtime',"$QtRoot/bin")) {
    $files += @(Get-ChildItem -LiteralPath $directory -File -Recurse | Where-Object { $_.Extension -in @('.dll','.ofx') } | ForEach-Object FullName)
}
$files += @('Engine/AppManager.cpp','Engine/StandardPaths.cpp','Engine/OfxEffectInstance.cpp','Engine/EffectInstanceRenderRoI.cpp') | ForEach-Object { Join-Path $LegacySourceDirectory $_ }
function Get-Inputs {
    $hashes = @{}
    foreach ($file in $files) { $hashes[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() }
    return $hashes
}
$before = Get-Inputs
$settings = & uv run --frozen python tools/test-settings-fingerprint.py
if ($LASTEXITCODE -ne 0) { throw 'Could not fingerprint settings.' }
$variables = @('NATRON_BENCHMARK_API','NATRON_BENCHMARK_WIDTH','NATRON_BENCHMARK_HEIGHT','NATRON_BENCHMARK_SAMPLES',
    'NATRON_LEGACY_OPENGL_TRACE','NATRON_VULKAN_DEVICE','NATRON_VULKAN_DISABLED','NATRON_VULKAN_MEMORY_LIMIT_MB','VK_DRIVER_FILES','VK_ICD_FILENAMES')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
$report = [ordered]@{ passed=$false; inputSha256=$before; settingsBefore=$settings; cases=@(); samples=$Samples;
    completionPlatform='windows'; excludedTests=@('parsec'); fullVulkanMigrationComplete=$false;
    scope='Headless full-size CPU source / OCIO Linear-to-sRGB / dispersed CPU PNG probe; RTX GL/Vulkan comparison and AMD Vulkan';
    legacyDependencyDifference='Legacy IO uses OpenColorIO 2.1; current IO uses 2.6. These timings do not isolate API cost.' }
try {
    Remove-Item -LiteralPath Env:NATRON_VULKAN_DISABLED,Env:NATRON_VULKAN_MEMORY_LIMIT_MB,Env:VK_DRIVER_FILES,Env:VK_ICD_FILENAMES -ErrorAction SilentlyContinue
    foreach ($backend in @('opengl','vulkan-0','vulkan-1')) {
        foreach ($resolution in @('4K','8K')) {
            $name = "$backend-$resolution"
            $cell = Join-Path $destination $name
            $resultPath = Join-Path $cell 'result.json'
            $memoryPath = Join-Path $cell 'memory.json'
            $profile = Join-Path $taskRoot "build/test-profiles/$([IO.Path]::GetFileName($destination))-$name"
            $env:NATRON_BENCHMARK_API = if ($backend -eq 'opengl') { 'opengl' } else { 'vulkan' }
            $env:NATRON_VULKAN_DEVICE = if ($backend -eq 'vulkan-1') { '1' } else { '0' }
            $env:NATRON_BENCHMARK_WIDTH = if ($resolution -eq '4K') { '3840' } else { '7680' }
            $env:NATRON_BENCHMARK_HEIGHT = if ($resolution -eq '4K') { '2160' } else { '4320' }
            $env:NATRON_BENCHMARK_SAMPLES = [string]$Samples
            $env:NATRON_LEGACY_OPENGL_TRACE = if ($backend -eq 'opengl') { '1' } else { '0' }
            $parameters = @{Program='NatronRenderer'; Script='tools/renderer-api-comparison.py'; Profile=$profile; Result=$resultPath;
                GpuMemoryReport=$memoryPath; Validation=0; TimeoutSeconds=600; OfxTestPluginDirectory='build/api-comparison-fixtures'; QtRoot=$QtRoot}
            if ($backend -eq 'opengl') {
                $parameters.BuildDirectory = $LegacyBuildDirectory
                $parameters.OfxPluginDirectory = 'build/IO-before-vulkan'
                $parameters.OfxRuntimeDirectory = 'build/legacy-gl-comparison-runtime'
                $parameters.ProgramArguments = @('--opengl','enabled')
            }
            & ./tools/run-windows-owned-gui-test.ps1 @parameters *> (Join-Path $destination "$name.log")
            if ($LASTEXITCODE -ne 0) { throw "Comparison cell failed: $name" }
            $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            $memory = Get-Content -LiteralPath $memoryPath -Raw | ConvertFrom-Json
            if (-not $result.ok -or -not $memory.passed -or $result.samples -ne $Samples -or $result.cases.Count -ne 2) { throw "Incomplete cell: $name" }
            if ($backend -eq 'opengl') {
                $callbacks = @(Select-String -LiteralPath "$profile/stderr.log" -Pattern 'NATRON_LEGACY_GL_CALLBACK.*enabled=1')
                if ($callbacks.Count -ne $Samples+3 -or -not (Select-String -LiteralPath "$profile/stderr.log" -Pattern 'renderer=NVIDIA GeForce RTX 5080')) { throw 'The legacy benchmark did not use the expected real OpenGL GPU.' }
            }
            $report.cases += @{backend=$backend; resolution=$resolution; profile=$profile; result=$resultPath; memory=$memoryPath;
                resultSha256=(Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).Hash.ToLowerInvariant();
                memorySha256=(Get-FileHash -LiteralPath $memoryPath -Algorithm SHA256).Hash.ToLowerInvariant()}
            $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $destination 'report.partial.json') -Encoding utf8
            Write-Output "$name passed: $Samples CPU and $Samples GPU samples."
        }
    }
    $after = Get-Inputs
    foreach ($file in $files) { if ($after[$file] -ne $before[$file]) { throw "Input changed: $file" } }
    $report.settingsAfter = & uv run --frozen python tools/test-settings-fingerprint.py
    if ($LASTEXITCODE -ne 0 -or $report.settingsAfter -ne $settings) { throw 'User settings changed.' }
    $report.passed = $true
} catch {
    $report.failure = $_.Exception.Message
    throw
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
    $report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $destination 'report.json') -Encoding utf8
}
