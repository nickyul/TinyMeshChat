Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$preset = 'windows-release'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/$preset"
$distRoot = Join-Path $root 'dist'
$dist = Join-Path $distRoot 'TinyMeshChat'
$exePath = Join-Path $build 'Release/TinyMeshChat.exe'
$exeDestination = Join-Path $dist 'TinyMeshChat.exe'

if ([string]::IsNullOrWhiteSpace($env:QT_ROOT)) {
    throw 'QT_ROOT is not set'
}
$deployPath = Join-Path $env:QT_ROOT 'bin/windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployPath -PathType Leaf)) {
    throw "windeployqt.exe was not found at $deployPath"
}

cmake --preset $preset
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) {
    throw "TinyMeshChat.exe was not produced at $exePath"
}

if (Test-Path -LiteralPath $dist) {
    Remove-Item -LiteralPath $dist -Recurse -Force
}
New-Item -ItemType Directory -Path $dist -Force | Out-Null
Copy-Item -LiteralPath $exePath -Destination $exeDestination

$deployArguments = @(
    '--release'
    '--no-translations'
    '--compiler-runtime'
    '--qmldir'
    (Join-Path $root 'qml')
    $exeDestination
)
& $deployPath @deployArguments
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

$exeDirectory = Split-Path -Parent $exePath
Get-ChildItem -LiteralPath $exeDirectory -Filter '*.dll' -File |
    ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $dist -Force
    }

$requiredDlls = @(
    'datachannel.dll'
    'opus.dll'
    'libspeexdsp.dll'
)
foreach ($dll in $requiredDlls) {
    $dllPath = Join-Path $dist $dll
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
        throw "Required runtime library is missing from the package: $dll"
    }
}

$zip = Join-Path $distRoot 'TinyMeshChat.zip'
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -LiteralPath $dist -DestinationPath $zip
Write-Host "Created $zip"
