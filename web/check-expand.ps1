# Clicks 展开/收起 through UI Automation on an isolated instance and samples the real window height every ~1ms,
# to verify the edge animates smoothly (many small monotonic steps over ~240ms) instead of snapping.
param([Parameter(Mandatory)][string]$Executable, [string]$Work)
$ErrorActionPreference = 'Stop'
if (-not $Work) { $Work = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'test-output' }
New-Item -ItemType Directory -Force $Work | Out-Null
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class R {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int x, int y, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static void Click(int x, int y) { SetCursorPos(x, y); mouse_event(2, 0, 0, 0, UIntPtr.Zero); mouse_event(4, 0, 0, 0, UIntPtr.Zero); }
}
'@
[R]::SetProcessDPIAware() | Out-Null
$config = "$Work\expand-check.json"; '{"close_action":"exit"}' | Set-Content $config -Encoding ascii
$p = Start-Process $Executable -ArgumentList @('--config', "`"$config`"", '--bind', '127.0.0.1', '--port', '18765') -PassThru
try {
  $deadline = (Get-Date).AddSeconds(10)
  while ($p.MainWindowHandle -eq 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100; $p.Refresh() }
  $hwnd = $p.MainWindowHandle; [R]::SetForegroundWindow($hwnd) | Out-Null; Start-Sleep -Milliseconds 2500
  $window = [Windows.Automation.AutomationElement]::FromHandle($hwnd)
  function Toggle-And-Sample($name) {
    $condition = New-Object Windows.Automation.AndCondition @(
      (New-Object Windows.Automation.PropertyCondition ([Windows.Automation.AutomationElement]::NameProperty), $name),
      (New-Object Windows.Automation.PropertyCondition ([Windows.Automation.AutomationElement]::ControlTypeProperty), ([Windows.Automation.ControlType]::Button)))
    $button = $null; $until = (Get-Date).AddSeconds(5)
    while (-not $button -and (Get-Date) -lt $until) { $button = $window.FindFirst([Windows.Automation.TreeScope]::Descendants, $condition); if (-not $button) { Start-Sleep -Milliseconds 200 } }
    if (-not $button) { throw "button '$name' not found" }
    $samples = New-Object System.Collections.Generic.List[object]
    $box = $button.Current.BoundingRectangle
    $clock = [Diagnostics.Stopwatch]::StartNew()
    [R]::Click([int]($box.X + $box.Width / 2), [int]($box.Y + $box.Height / 2))
    while ($clock.ElapsedMilliseconds -lt 600) { $r = New-Object R+RECT; [R]::GetWindowRect($hwnd, [ref]$r) | Out-Null; $samples.Add([pscustomobject]@{ ms = $clock.Elapsed.TotalMilliseconds; h = $r.Bottom - $r.Top }) }
    $changes = @(); $last = $samples[0].h
    foreach ($s in $samples) { if ($s.h -ne $last) { $changes += [pscustomobject]@{ ms = [math]::Round($s.ms, 1); h = $s.h; step = $s.h - $last }; $last = $s.h } }
    $from = $samples[0].h; $to = $samples[-1].h
    $biggest = ($changes | Measure-Object -Property step -Maximum -Minimum)
    $gap = 0; for ($i = 1; $i -lt $changes.Count; $i++) { $gap = [math]::Max($gap, $changes[$i].ms - $changes[$i - 1].ms) }
    $span = if ($changes.Count) { [math]::Round($changes[-1].ms - $changes[0].ms) } else { 0 }
    $largestShare = if ($to -ne $from) { [math]::Round([math]::Max([math]::Abs($biggest.Maximum), [math]::Abs($biggest.Minimum)) / [math]::Abs($to - $from) * 100) } else { 0 }
    if ($env:VB_TRACE) { ($changes | ForEach-Object { "$($_.ms):$($_.step)" }) -join ' ' | Write-Host }
    [pscustomobject]@{ Action = $name; From = $from; To = $to; Steps = $changes.Count; StartMs = $(if ($changes.Count) { [math]::Round($changes[0].ms) } else { -1 }); SpanMs = $span; LongestGapMs = [math]::Round($gap); LargestStepPct = $largestShare
      Smooth = ($changes.Count -ge 8) -and ($gap -le 34) -and ($largestShare -le 30) -and ($span -ge 150) }
  }
  $open = Toggle-And-Sample '展开电脑到手机'; Start-Sleep -Milliseconds 500
  $close = Toggle-And-Sample '收起电脑到手机'
  $open, $close | Format-Table -AutoSize | Out-String -Width 160
  if (-not ($open.Smooth -and $close.Smooth)) { exit 1 }
} finally { if (-not $p.HasExited) { [R]::PostMessage($p.MainWindowHandle, 0x10, 0, 0) | Out-Null; if (-not $p.WaitForExit(5000)) { $p.Kill() } } }
