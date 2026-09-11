param([switch]$Launch, [switch]$UI, [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputName = 'Release')
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot "bin\$OutputName\VoiceBridge.exe"
$results = Join-Path $PSScriptRoot 'test-output'
New-Item -ItemType Directory -Force -Path $results | Out-Null
if ($Launch) {
    $config = Join-Path $results 'ui-config.json'
    $taskProcess = Start-Process -FilePath $exe -ArgumentList @('--config', "`"$config`"", '--bind', '127.0.0.1', '--port', '18765') -WindowStyle Hidden -PassThru
    Write-Output "Native test process: $($taskProcess.Id)"
} else {
    $report = Join-Path $results $(if ($UI) { 'ui-tests.json' } else { 'protocol-tests.json' })
    $mode = if ($UI) { '--ui-self-test' } else { '--self-test' }
    $started = Get-Date
    $taskProcess = Start-Process -FilePath $exe -ArgumentList @($mode, "`"$report`"") -WindowStyle Hidden -PassThru
    if (-not $taskProcess.WaitForExit(60000)) { throw "Test process $($taskProcess.Id) exceeded 60 seconds." }
    if (-not (Test-Path $report) -or (Get-Item $report).LastWriteTime -lt $started) { throw 'No fresh verification report was produced.' }
    Get-Content -LiteralPath $report -Encoding UTF8
    if ($taskProcess.ExitCode -ne 0) { throw "Native tests failed: $($taskProcess.ExitCode)" }
}
