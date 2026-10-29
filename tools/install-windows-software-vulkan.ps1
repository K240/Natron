[CmdletBinding()]
param(
    [string]$Destination = 'build/software-vulkan-driver',
    [string]$ArchiveCache = 'build/msys64/var/cache/pacman/pkg'
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$destinationPath = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($Destination)) { $Destination } else { Join-Path $taskRoot $Destination }))
if (Test-Path -LiteralPath $destinationPath) { throw 'Use a fresh software-driver directory.' }
$lockPath = Join-Path $taskRoot 'build-configs/vulkan/software-driver.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $destinationPath | Out-Null
$downloadPath = Join-Path $destinationPath 'archives'
$extractPath = Join-Path $destinationPath 'packages'
$binPath = Join-Path $destinationPath 'bin'
New-Item -ItemType Directory -Path $downloadPath,$extractPath,$binPath | Out-Null
$proof = [ordered]@{passed=$false; apiVersion=$lock.apiVersion; driver='SwiftShader (MSYS2 ANGLE package)';
    lockSha256=(Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant(); packages=@(); files=@{}}
try {
    foreach ($package in $lock.packages) {
        $archive = Join-Path $downloadPath $package.archive
        $cached = Join-Path $taskRoot "$ArchiveCache/$($package.archive)"
        if (Test-Path -LiteralPath $cached) { Copy-Item -LiteralPath $cached -Destination $archive }
        else { Invoke-WebRequest -Uri $package.url -OutFile $archive }
        if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.sha256) { throw "Package hash mismatch: $archive" }
        & "$env:WINDIR/System32/tar.exe" -xf $archive -C $extractPath @($package.files)
        if ($LASTEXITCODE -ne 0) { throw "Package extraction failed: $archive" }
        foreach ($file in $package.files) {
            $source = Join-Path $extractPath $file
            if ($file.EndsWith('.dll')) { Copy-Item -LiteralPath $source -Destination $binPath }
        }
        $proof.packages += @{archive=$package.archive; sha256=$package.sha256; url=$package.url}
    }
    # The Windows loader recognizes a relative pathname by its backslash.
    $icd = @{file_format_version='1.0.0'; ICD=@{library_path='.\bin\libvk_swiftshader.dll'; api_version=$lock.apiVersion}}
    $icd | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $destinationPath 'vk_swiftshader_icd.json') -Encoding utf8
    foreach ($file in Get-ChildItem -LiteralPath $binPath -File) { $proof.files[$file.Name] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    $proof.passed = $proof.files.Count -eq 4
    if (-not $proof.passed) { throw 'Expected exactly the ICD and its three non-system runtime DLLs.' }
} finally {
    $proof | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $destinationPath 'manifest.json') -Encoding utf8
}
Write-Output "Installed pinned software Vulkan ICD: $destinationPath/vk_swiftshader_icd.json"
