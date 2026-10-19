# send_keys.ps1 - focus the EaglerRunner window and send keystrokes to it.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\send_keys.ps1 -Keys "{F6}"
#   powershell -ExecutionPolicy Bypass -File tools\send_keys.ps1 -Keys "hello"

param(
  [string]$Keys = "{F6}",
  [string]$ProcessName = "EaglerRunner"
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ErFocus {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
}
'@

$proc = Get-Process -Name $ProcessName -ErrorAction Stop |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No $ProcessName window found." }

[ErFocus]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 600
[System.Windows.Forms.SendKeys]::SendWait($Keys)
Write-Output "sent: $Keys"
