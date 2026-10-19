# fetch_webview2.ps1 - downloads the Microsoft.Web.WebView2 SDK into third_party/webview2
#
# Usage:  powershell -ExecutionPolicy Bypass -File third_party/fetch_webview2.ps1
param(
  [string]$Version = "1.0.2903.40"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$target = Join-Path $here "webview2"
$nupkg = Join-Path $here "webview2.nupkg"
$url = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"

Write-Host "Downloading WebView2 SDK $Version ..."
Invoke-WebRequest -Uri $url -OutFile $nupkg -UseBasicParsing

if (Test-Path $target) { Remove-Item $target -Recurse -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::ExtractToDirectory($nupkg, $target)

Write-Host "Extracted to $target"
Get-ChildItem (Join-Path $target "build\native\include") | Select-Object -ExpandProperty Name
