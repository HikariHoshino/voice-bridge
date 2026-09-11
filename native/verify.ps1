param([switch]$Launch)
$ErrorActionPreference = 'Stop'
$exe = Join-Path $PSScriptRoot 'bin\Release\VoiceBridge.exe'
$results = Join-Path $PSScriptRoot 'test-output'
New-Item -ItemType Directory -Force -Path $results | Out-Null
if ($Launch) {
    $config = Join-Path $results 'ui-config.json'
    $taskProcess = Start-Process -FilePath $exe -ArgumentList @('--config', "`"$config`"", '--bind', '127.0.0.1', '--port', '18765') -WindowStyle Hidden -PassThru
    Write-Output "Native test process: $($taskProcess.Id)"
} else {
    $report = Join-Path $results 'protocol-tests.json'
    $taskProcess = Start-Process -FilePath $exe -ArgumentList @('--self-test', "`"$report`"") -WindowStyle Hidden -PassThru
    if (-not $taskProcess.WaitForExit(60000)) { throw "Test process $($taskProcess.Id) exceeded 60 seconds." }
    if (Test-Path $report) { Get-Content -LiteralPath $report -Encoding UTF8 }
    if ($taskProcess.ExitCode -ne 0) { throw "Native tests failed: $($taskProcess.ExitCode)" }
}
