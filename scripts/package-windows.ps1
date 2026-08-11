Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$preset = 'windows-release'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build/$preset"
$distRoot = Join-Path $root 'dist'
$dist = Join-Path $distRoot 'TinyMeshChat'
$exePath = Join-Path $build 'Release/TinyMeshChat.exe'
$exeDestination = Join-Path $dist 'TinyMeshChat.exe'
$versionFile = Join-Path $build 'tmc-app-version.txt'
$updaterEnabled = $env:TMC_ENABLE_UPDATER -eq '1'
$buildVelopack = $env:TMC_BUILD_VELOPACK -eq '1'

if ($buildVelopack -and -not $updaterEnabled) {
    throw 'TMC_BUILD_VELOPACK=1 requires TMC_ENABLE_UPDATER=1'
}

if ([string]::IsNullOrWhiteSpace($env:QT_ROOT)) {
    throw 'QT_ROOT is not set'
}
$deployPath = Join-Path $env:QT_ROOT 'bin/windeployqt.exe'
if (-not (Test-Path -LiteralPath $deployPath -PathType Leaf)) {
    throw "windeployqt.exe was not found at $deployPath"
}

$configureArguments = @('--preset', $preset, '-DTMC_ENABLE_UPDATER=OFF')
$velopackRoot = $env:TMC_VELOPACK_ROOT
if ($updaterEnabled) {
    if ([string]::IsNullOrWhiteSpace($velopackRoot)) {
        $velopackRoot = Join-Path $root 'build/tools/velopack'
        & (Join-Path $PSScriptRoot 'bootstrap-velopack.ps1') -Destination $velopackRoot
    }
    $configureArguments[-1] = '-DTMC_ENABLE_UPDATER=ON'
    $configureArguments += "-DTMC_VELOPACK_ROOT=$velopackRoot"
}
cmake @configureArguments
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
    'juice.dll'
    'srtp2.dll'
    'libcrypto-3-x64.dll'
    'libssl-3-x64.dll'
    'opus.dll'
    'abseil_dll.dll'
)
foreach ($dll in $requiredDlls) {
    $dllPath = Join-Path $dist $dll
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
        throw "Required runtime library is missing from the package: $dll"
    }
}
if ($updaterEnabled) {
    $velopackRuntime = Join-Path $dist 'velopack_libc.dll'
    if (-not (Test-Path -LiteralPath $velopackRuntime -PathType Leaf)) {
        throw 'Velopack runtime is missing from the package'
    }
}

function Invoke-PackagedSmoke {
    param(
        [Parameter(Mandatory)]
        [string[]]$Arguments,
        [switch]$WriteConsoleQuit
    )

    $smokeData = Join-Path ([System.IO.Path]::GetTempPath()) ("TinyMeshChat-smoke-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $smokeData -Force | Out-Null
    $process = $null
    try {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $exeDestination
        $startInfo.WorkingDirectory = $dist
        $startInfo.UseShellExecute = $false
        $startInfo.RedirectStandardInput = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        $startInfo.EnvironmentVariables['TMC_DATA_DIR'] = $smokeData
        $startInfo.Arguments = ($Arguments | ForEach-Object {
                '"' + $_.Replace('"', '\"') + '"'
            }) -join ' '

        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) {
            throw "Failed to start packaged smoke: $($Arguments -join ' ')"
        }
        if ($WriteConsoleQuit) {
            $process.StandardInput.AutoFlush = $true
            $process.StandardInput.WriteLine('/quit')
            $process.StandardInput.Flush()
        }
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) {
            $process.Kill()
            throw "Packaged smoke timed out: $($Arguments -join ' ')"
        }
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0) {
            throw "Packaged smoke failed with exit code $($process.ExitCode):`n$stdout`n$stderr"
        }
    }
    finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
        if (Test-Path -LiteralPath $smokeData) {
            foreach ($attempt in 1..5) {
                try {
                    Remove-Item -LiteralPath $smokeData -Recurse -Force -ErrorAction Stop
                    break
                }
                catch {
                    if ($attempt -eq 5) {
                        throw
                    }
                    Start-Sleep -Milliseconds 200
                }
            }
        }
    }
}

Invoke-PackagedSmoke -Arguments @('--console', '--display-name', 'CI Smoke') -WriteConsoleQuit
Invoke-PackagedSmoke -Arguments @('--qml-smoke', '--display-name', 'CI Smoke')

$zip = Join-Path $distRoot 'TinyMeshChat.zip'
$portableMarker = Join-Path $dist 'tinymesh-portable.marker'
[System.IO.File]::WriteAllText(
    $portableMarker,
    "TinyMesh Chat standalone portable package.`n",
    [System.Text.UTF8Encoding]::new($false)
)
if (Test-Path -LiteralPath $zip) {
    Remove-Item -LiteralPath $zip -Force
}
Compress-Archive -LiteralPath $dist -DestinationPath $zip
$archiveEntries = tar -tf $zip
if ($LASTEXITCODE -ne 0 -or
    $archiveEntries -notcontains 'TinyMeshChat/TinyMeshChat.exe' -or
    $archiveEntries -notcontains 'TinyMeshChat/tinymesh-portable.marker') {
    throw 'Portable archive verification failed'
}
Write-Host "Created $zip"

if ($buildVelopack) {
    Remove-Item -LiteralPath $portableMarker -Force
    dotnet tool restore
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet tool restore failed with exit code $LASTEXITCODE"
    }
    $velopackOutput = Join-Path $distRoot 'velopack-win-x64'
    New-Item -ItemType Directory -Path $velopackOutput -Force | Out-Null
    if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
        throw "CMake did not produce the application version file: $versionFile"
    }
    $version = [System.IO.File]::ReadAllText($versionFile).Trim()
    [System.IO.File]::WriteAllText(
        (Join-Path $distRoot 'tmc-app-version.txt'),
        "$version`n",
        [System.Text.UTF8Encoding]::new($false)
    )
    dotnet tool run vpk pack `
        --packId TinyMeshChat.Win `
        --packTitle 'TinyMesh Chat' `
        --packVersion $version `
        --packDir $dist `
        --mainExe TinyMeshChat.exe `
        --runtime win-x64 `
        --channel win-x64 `
        --noPortable true `
        --outputDir $velopackOutput
    if ($LASTEXITCODE -ne 0) {
        throw "Velopack packaging failed with exit code $LASTEXITCODE"
    }
    if (-not (Get-ChildItem -LiteralPath $velopackOutput -File | Where-Object Name -Like 'releases.*.json')) {
        throw 'Velopack release feed was not produced'
    }
}
