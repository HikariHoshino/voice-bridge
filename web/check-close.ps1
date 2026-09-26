# Verifies the three close_action behaviours on a real, isolated instance (no daily config, port 18765).
param([Parameter(Mandatory)][string]$Executable, [string]$Work)
$ErrorActionPreference = 'Stop'
if (-not $Work) { $Work = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'test-output' }
New-Item -ItemType Directory -Force $Work | Out-Null
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class W {
  public delegate bool Enum(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Enum f, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowThreadProcessId(IntPtr h, out int pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
'@
function Windows($processId) {
  $found = New-Object System.Collections.ArrayList
  [W]::EnumWindows({ param($h, $l) $p = 0; [W]::GetWindowThreadProcessId($h, [ref]$p) | Out-Null
    if ($p -eq $processId) { $b = New-Object Text.StringBuilder 64; [W]::GetWindowText($h, $b, 64) | Out-Null
      if ($b.Length) { [void]$found.Add([pscustomobject]@{ Handle = $h; Title = $b.ToString(); Visible = [W]::IsWindowVisible($h); Enabled = [W]::IsWindowEnabled($h) }) } }
    $true }, [IntPtr]::Zero) | Out-Null
  $found
}
function Launch($action) {
  $config = "$Work\close-$action.json"
  "{`"close_action`":`"$action`"}" | Set-Content $config -Encoding ascii
  $p = Start-Process $Executable -ArgumentList @('--config', "`"$config`"", '--bind', '127.0.0.1', '--port', '18765') -PassThru
  $deadline = (Get-Date).AddSeconds(10)
  do { Start-Sleep -Milliseconds 200; $main = Windows $p.Id | Where-Object { $_.Title -eq '声桥' -and $_.Visible } } until ($main -or (Get-Date) -gt $deadline)
  if (-not $main) { throw "$action : main window did not appear" }
  Start-Sleep -Milliseconds 1500
  return $p, $main
}
$results = @()
# exit: closing the main window ends the process.
$p, $main = Launch 'exit'; [W]::PostMessage($main.Handle, 0x10, 0, 0) | Out-Null
$results += [pscustomobject]@{ Check = 'close_action=exit ends the process'; Passed = $p.WaitForExit(5000) }
# tray: the window hides, the process and service stay alive.
$p, $main = Launch 'tray'; [W]::PostMessage($main.Handle, 0x10, 0, 0) | Out-Null; Start-Sleep -Milliseconds 800
$state = Windows $p.Id | Where-Object { $_.Title -eq '声桥' }
$alive = try { (Invoke-WebRequest 'http://127.0.0.1:18765/' -UseBasicParsing -TimeoutSec 3).StatusCode } catch { $_.Exception.Response.StatusCode.value__ }
$results += [pscustomobject]@{ Check = 'close_action=tray hides the window and keeps the service'; Passed = (-not $p.HasExited) -and (-not $state.Visible) -and ($alive -eq 403) }
$p.Kill(); $p.WaitForExit()
# ask: a modal prompt opens and the main window is disabled until it is dismissed.
$p, $main = Launch 'ask'; [W]::PostMessage($main.Handle, 0x10, 0, 0) | Out-Null; Start-Sleep -Milliseconds 1500
$prompt = Windows $p.Id | Where-Object { $_.Title -eq '关闭声桥' -and $_.Visible }
$mainState = Windows $p.Id | Where-Object { $_.Title -eq '声桥' }
$results += [pscustomobject]@{ Check = 'close_action=ask shows the prompt and disables the main window'; Passed = [bool]$prompt -and -not $mainState.Enabled -and -not $p.HasExited }
if ($prompt) { [W]::PostMessage($prompt.Handle, 0x10, 0, 0) | Out-Null; Start-Sleep -Milliseconds 800 }
$mainState = Windows $p.Id | Where-Object { $_.Title -eq '声桥' }
$results += [pscustomobject]@{ Check = 'dismissing the prompt re-enables the main window'; Passed = $mainState.Enabled -and $mainState.Visible -and -not (Windows $p.Id | Where-Object { $_.Title -eq '关闭声桥' }) }
$p.Kill(); $p.WaitForExit()
$results | Format-Table -AutoSize | Out-String -Width 160
if ($results.Passed -contains $false) { exit 1 }
