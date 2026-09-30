<#
.SYNOPSIS
  Builds the distributable zip: PhoneLink Studio + the plugin, ready to give to friends.

.EXAMPLE
  cmake --preset release; cmake --build --preset release
  pwsh tools/package.ps1            # -> dist/PhoneLink-<version>.zip
#>
param(
    [string]$BuildDir = "build",
    [string]$OutDir = "dist"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$asi = Join-Path $BuildDir "PhoneLink.asi"
if (-not (Test-Path $asi)) { throw "$asi not found - build first (cmake --build --preset release)" }
$version = (Select-String -Path CMakeLists.txt -Pattern 'project\(PhoneLink VERSION ([0-9.]+)').Matches[0].Groups[1].Value

$name = "PhoneLink-$version"
$stage = Join-Path $OutDir $name
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage/studio/plugin" | Out-Null

# Studio (Python app) without local state
Copy-Item studio/app.py, studio/calls.py, studio/plugin.py, studio/requirements.txt, studio/Start.bat "$stage/studio/"
Copy-Item -Recurse studio/static, studio/public "$stage/studio/"
# The plugin, where Studio's "Install / update plugin" button looks for it
Copy-Item $asi, PhoneLink.ini "$stage/studio/plugin/"
Copy-Item README.md "$stage/"
if (Test-Path LICENSE) { Copy-Item LICENSE "$stage/" }
Copy-Item -Recurse docs "$stage/docs"

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage/*" -DestinationPath $zip
Write-Host "Created $zip"
if ($env:GITHUB_OUTPUT) {
    "zip=$zip" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
    "version=$version" | Out-File -Append -Encoding utf8 $env:GITHUB_OUTPUT
}
