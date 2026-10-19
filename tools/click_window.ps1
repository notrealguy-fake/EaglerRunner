# click_window.ps1 - click at window-relative coordinates inside EaglerRunner.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\click_window.ps1 -X 790 -Y 296
#
# Coordinates are pixels relative to the top-left of the captured window image
# (same geometry as tools\capture_window.ps1 output).

param(
  [Parameter(Mandatory=$true)][int]$X,
  [Parameter(Mandatory=$true)][int]$Y,
  [string]$ProcessName = "EaglerRunner"
)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ErMouse {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(140);
    mouse_event(0x02, 0, 0, 0, IntPtr.Zero); // down
    System.Threading.Thread.Sleep(70);
    mouse_event(0x04, 0, 0, 0, IntPtr.Zero); // up
  }
}
'@

[ErMouse]::SetProcessDPIAware() | Out-Null
$proc = Get-Process -Name $ProcessName -ErrorAction Stop |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { throw "No $ProcessName window found." }

[ErMouse]::SetForegroundWindow($proc.MainWindowHandle) | Out-Null
$rect = New-Object ErMouse+RECT
[ErMouse]::GetWindowRect($proc.MainWindowHandle, [ref]$rect) | Out-Null
Start-Sleep -Milliseconds 400
[ErMouse]::Click($rect.Left + $X, $rect.Top + $Y)
Write-Output "clicked at $($rect.Left + $X),$($rect.Top + $Y)"
