# build.ps1 - configure and build EaglerRunner with the MSVC toolchain.
#
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1
param(
  [ValidateSet("Release", "Debug")] [string]$Config = "Release",
  [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

# --- locate Visual Studio / Build Tools -------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
  throw "vswhere.exe not found. Install Visual Studio 2022 Build Tools with the C++ workload."
}
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
  throw "No Visual Studio installation with the C++ toolset was found."
}
Write-Host "Visual Studio: $vsPath"

$cmake = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path $cmake)) { $cmake = "cmake" }

# --- WebView2 SDK -----------------------------------------------------------
if (-not (Test-Path "third_party\webview2\build\native\include\WebView2.h")) {
  Write-Host "WebView2 SDK missing - downloading it ..."
  & powershell -ExecutionPolicy Bypass -File "third_party\fetch_webview2.ps1"
}

# --- build ------------------------------------------------------------------
if ($Clean -and (Test-Path "build")) { Remove-Item "build" -Recurse -Force }

& $cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

& $cmake --build build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$exe = Join-Path $root "build\Release\EaglerRunner.exe"
if ($Config -eq "Debug") { $exe = Join-Path $root "build\Debug\EaglerRunner.exe" }

if (Test-Path $exe) {
  $size = [math]::Round((Get-Item $exe).Length / 1KB, 1)
  Write-Host ""
  Write-Host "Built: $exe ($size KB)" -ForegroundColor Green
} else {
  throw "Build reported success but $exe was not produced."
}
