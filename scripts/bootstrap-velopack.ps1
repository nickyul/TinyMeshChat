param(
    [Parameter(Mandatory)]
    [string]$Destination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$version = '1.2.0'
$expectedSha256 = '547262ed7a1ab1ff62f580aa53851ede2f1a451ac61b8974eb7bc01117488835'
$url = "https://github.com/velopack/velopack/releases/download/$version/velopack_libc_$version.zip"
$destinationPath = [System.IO.Path]::GetFullPath($Destination)
$header = Join-Path $destinationPath 'include/Velopack.hpp'
if (Test-Path -LiteralPath $header -PathType Leaf) {
    Write-Host "Velopack SDK is already available at $destinationPath"
    exit 0
}

$archive = Join-Path ([System.IO.Path]::GetTempPath()) "velopack_libc_$version-$([guid]::NewGuid()).zip"
try {
    Invoke-WebRequest -Uri $url -OutFile $archive
    $actualSha256 = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualSha256 -ne $expectedSha256) {
        throw "Velopack SDK checksum mismatch: $actualSha256"
    }
    New-Item -ItemType Directory -Path $destinationPath -Force | Out-Null
    Expand-Archive -LiteralPath $archive -DestinationPath $destinationPath -Force
    if (-not (Test-Path -LiteralPath $header -PathType Leaf)) {
        throw 'Velopack.hpp was not found after extracting the SDK'
    }
}
finally {
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
}

Write-Host "Installed Velopack SDK $version to $destinationPath"
