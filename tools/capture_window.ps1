# capture_window.ps1 - grab a PNG of the running EaglerRunner window.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\capture_window.ps1 [-OutPath out.png]
#
# Tries PrintWindow (works when the window is off-screen), falls back to a
# plain screen grab of the window rectangle.

param(
  [string]$ProcessName = "EaglerRunner",
  [string]$OutPath = "$env:TEMP\opencode\eaglerrunner_window.png"
)

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ErWinCap {
  [DllImport("user32.dll", SetLastError=true)] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
'@

$proc = Get-Process -Name $ProcessName -ErrorAction Stop |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No $ProcessName window found." }

$hwnd = $proc.MainWindowHandle
[ErWinCap]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 500

$rect = New-Object ErWinCap+RECT
[ErWinCap]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "Bad window rect." }

$dir = Split-Path -Parent $OutPath
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }

$bmp = New-Object System.Drawing.Bitmap($w, $h)
$g = [System.Drawing.Graphics]::FromImage($bmp)

# 1) PrintWindow with PW_RENDERFULLCONTENT (captures DirectComposition content).
$hdc = $g.GetHdc()
$ok = [ErWinCap]::PrintWindow($hwnd, $hdc, 2)
$g.ReleaseHdc($hdc)

$bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png)

# 2) Also keep a straight screen grab next to it - one of the two will be correct.
$screenPath = [System.IO.Path]::ChangeExtension($OutPath, $null) + "_screen.png"
$g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
$bmp.Save($screenPath, [System.Drawing.Imaging.ImageFormat]::Png)

$g.Dispose(); $bmp.Dispose()
Write-Output "printwindow=$ok"
Write-Output $OutPath
Write-Output $screenPath
