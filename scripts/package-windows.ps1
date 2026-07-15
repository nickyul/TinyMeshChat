param([string]$Preset = 'windows-release')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
cmake --preset $Preset
cmake --build --preset $Preset
$build = Join-Path $root "build/$Preset"
$distRoot = Join-Path $root 'dist'
$dist = Join-Path $distRoot 'TinyMeshChat'
if (Test-Path -LiteralPath $dist) { Remove-Item -LiteralPath $dist -Recurse -Force }
New-Item -ItemType Directory -Path $dist | Out-Null
$exe = Get-ChildItem -LiteralPath $build -Recurse -Filter TinyMeshChat.exe | Select-Object -First 1
if (-not $exe) { throw 'TinyMeshChat.exe was not produced' }
Copy-Item -LiteralPath $exe.FullName -Destination $dist
Copy-Item -LiteralPath (Join-Path $root 'config/default-config.json') -Destination $dist
$deploy = Get-Command windeployqt -ErrorAction SilentlyContinue
if (-not $deploy -and $env:QT_ROOT) {
  $candidate = Join-Path $env:QT_ROOT 'bin/windeployqt.exe'
  if (Test-Path -LiteralPath $candidate) { $deploy = Get-Item -LiteralPath $candidate }
}
if (-not $deploy) {
  $candidate = Get-ChildItem -Path 'C:\Qt' -Recurse -Filter windeployqt.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match 'msvc2022_64[\\/]bin' } |
    Sort-Object FullName -Descending | Select-Object -First 1
  if ($candidate) { $deploy = $candidate }
}
if (-not $deploy) { throw 'windeployqt.exe was not found; set QT_ROOT or add Qt bin to PATH' }
& $deploy.FullName --release --no-translations --compiler-runtime (Join-Path $dist 'TinyMeshChat.exe')

Get-ChildItem -LiteralPath $exe.DirectoryName -Filter *.dll -File |
  ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $dist -Force }
$sqlite = Join-Path $dist 'sqldrivers/qsqlite.dll'
if (-not (Test-Path -LiteralPath $sqlite)) { throw 'Qt SQLite plugin is missing from the package' }
$dataChannel = Join-Path $dist 'datachannel.dll'
if (-not (Test-Path -LiteralPath $dataChannel)) { throw 'libdatachannel runtime is missing from the package' }
$zip = Join-Path $distRoot 'TinyMeshChat.zip'
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -LiteralPath $dist -DestinationPath $zip
Write-Host "Created $zip"
