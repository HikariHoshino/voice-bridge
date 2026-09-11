param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
if ($LASTEXITCODE) {
    exit $LASTEXITCODE
}

$tests = Join-Path $PSScriptRoot "build\x64\$Configuration\VoiceBridge.Tests.exe"
$testData = Join-Path $PSScriptRoot 'build\test-data'
& $tests $testData
exit $LASTEXITCODE
