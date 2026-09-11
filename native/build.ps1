param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [switch]$Restore
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath
if (-not $installation) {
    throw 'Visual Studio with the Native Desktop workload was not found.'
}

$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$solution = Join-Path $PSScriptRoot 'VoiceBridge.sln'
$arguments = @(
    $solution,
    "/p:Configuration=$Configuration",
    '/p:Platform=x64',
    '/m',
    '/v:minimal'
)
if ($Restore) {
    $arguments += '/restore'
}

$environment = [System.Collections.Generic.Dictionary[string,string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
Get-ChildItem Env: | ForEach-Object {
    $environment[$_.Name] = $_.Value
}

$start = [System.Diagnostics.ProcessStartInfo]::new()
$start.FileName = $msbuild
$start.UseShellExecute = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.WorkingDirectory = Split-Path $PSScriptRoot -Parent
$start.Environment.Clear()
foreach ($entry in $environment.GetEnumerator()) {
    $start.Environment[$entry.Key] = $entry.Value
}
foreach ($argument in $arguments) {
    [void]$start.ArgumentList.Add($argument)
}

$process = [System.Diagnostics.Process]::Start($start)
$stdout = $process.StandardOutput.ReadToEnd()
$stderr = $process.StandardError.ReadToEnd()
$process.WaitForExit()
if ($stdout) { Write-Output $stdout.TrimEnd() }
if ($stderr) { Write-Error $stderr.TrimEnd() }
exit $process.ExitCode
