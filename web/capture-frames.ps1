# Captures real on-screen frames of the main window (kept topmost for the few seconds of the run) during one automatic
# 展开 and 收起 (--demo-toggle), keeps the frames where the edge is moving and stitches them into one strip,
# to look for tearing: stale pixels in the area the edge exposes. Isolated config, port 18765, no mouse input.
param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$Output, [int]$PerDirection = 8)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class F {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[F]::SetProcessDPIAware() | Out-Null
$dir = Split-Path -Parent ([IO.Path]::GetFullPath($Output)); $config = Join-Path $dir 'frames-config.json'
'{"close_action":"exit"}' | Set-Content $config -Encoding ascii
$p = Start-Process $Executable -ArgumentList @('--config', "`"$config`"", '--bind', '127.0.0.1', '--port', '18765', '--demo-toggle', '1500') -PassThru
try {
  while ($p.MainWindowHandle -eq 0) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $h = $p.MainWindowHandle
  [F]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013) | Out-Null   # HWND_TOPMOST, no move/size/activate: screen copies see this window
  $frames = New-Object System.Collections.Generic.List[object]
  $clock = [Diagnostics.Stopwatch]::StartNew(); $lastHeight = -1; $stillSince = 0
  while ($clock.ElapsedMilliseconds -lt 6000) {
    $r = New-Object F+RECT; [F]::GetWindowRect($h, [ref]$r) | Out-Null
    $height = $r.Bottom - $r.Top
    if ($height -ne $lastHeight) {
      $bmp = New-Object Drawing.Bitmap ($r.Right - $r.Left), 760
      $g = [Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.Left, $r.Top, 0, 0, $bmp.Size); $g.Dispose()
      $frames.Add([pscustomobject]@{ ms = $clock.ElapsedMilliseconds; h = $height; bmp = $bmp }); $lastHeight = $height
    }
  }
  # Split the frames into animations at pauses longer than 200ms; the first is 展开, the second 收起.
  $bursts = New-Object System.Collections.Generic.List[object]; $current = $null; $previous = -1000
  foreach ($f in ($frames | Select-Object -Skip 1)) {
    if ($f.ms - $previous -gt 200) { $current = New-Object System.Collections.Generic.List[object]; $bursts.Add($current) }
    $current.Add($f); $previous = $f.ms
  }
  $bursts = @($bursts | Where-Object { $_.Count -ge 3 })   # drop the one-step height fix at startup
  if ($bursts.Count -lt 2) { throw "captured $($bursts.Count) animation(s), expected 2" }
  $sample = { param($list) $step = [math]::Max(1, [math]::Floor($list.Count / $PerDirection)); for ($i = 0; $i -lt $list.Count -and ($i / $step) -lt $PerDirection; $i += $step) { $list[$i] } }
  $rows = @((& $sample $bursts[0]), (& $sample $bursts[1]))
  $moving = $bursts[0].Count + $bursts[1].Count
  $scale = 0.5; $cw = [int]($frames[0].bmp.Width * $scale); $ch = [int](760 * $scale)
  $sheet = New-Object Drawing.Bitmap ($cw * $PerDirection), ($ch * 2); $g = [Drawing.Graphics]::FromImage($sheet); $g.Clear([Drawing.Color]::Black)
  for ($row = 0; $row -lt 2; $row++) { $col = 0; foreach ($f in $rows[$row]) { $g.DrawImage($f.bmp, $col * $cw, $row * $ch, $cw, $ch); $col++ } }
  $g.Dispose(); $sheet.Save($Output); $sheet.Dispose(); $frames | ForEach-Object { $_.bmp.Dispose() }
  "frames: expand $($bursts[0].Count), collapse $($bursts[1].Count); saved $Output"
} finally { if (-not $p.HasExited) { [F]::PostMessage($p.MainWindowHandle, 0x10, 0, 0) | Out-Null; if (-not $p.WaitForExit(5000)) { $p.Kill() } } }
