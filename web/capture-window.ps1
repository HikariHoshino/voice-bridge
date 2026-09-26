# Launches the shell with an isolated config, captures the real on-screen window (DWM frame included) and closes it.
param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$Output, [int]$DelayMs = 2500)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class Win {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int s);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[Win]::SetProcessDPIAware() | Out-Null
$dir = Split-Path -Parent ([IO.Path]::GetFullPath($Output))
$process = Start-Process $Executable -ArgumentList @('--config', "`"$dir\capture-config.json`"", '--bind', '127.0.0.1', '--port', '18765') -PassThru
try {
  $deadline = (Get-Date).AddSeconds(10)
  while ($process.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100; $process.Refresh() }
  if ($process.MainWindowHandle -eq 0) { throw 'Window did not appear.' }
  [Win]::SetForegroundWindow($process.MainWindowHandle) | Out-Null
  Start-Sleep -Milliseconds $DelayMs
  $r = New-Object Win+RECT
  [Win]::DwmGetWindowAttribute($process.MainWindowHandle, 9, [ref]$r, 16) | Out-Null   # DWMWA_EXTENDED_FRAME_BOUNDS
  $pad = 16; $w = $r.Right - $r.Left + 2 * $pad; $h = $r.Bottom - $r.Top + 2 * $pad
  $bitmap = New-Object Drawing.Bitmap $w, $h
  $graphics = [Drawing.Graphics]::FromImage($bitmap)
  $graphics.CopyFromScreen($r.Left - $pad, $r.Top - $pad, 0, 0, (New-Object Drawing.Size $w, $h))
  $bitmap.Save($Output, [Drawing.Imaging.ImageFormat]::Png); $graphics.Dispose(); $bitmap.Dispose()
  "window {0}x{1} at {2},{3}" -f ($r.Right - $r.Left), ($r.Bottom - $r.Top), $r.Left, $r.Top
} finally {
  if (-not $process.HasExited) { [Win]::PostMessage($process.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; if (-not $process.WaitForExit(5000)) { $process.Kill() } }
}
