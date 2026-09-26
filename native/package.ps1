param([ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputName = 'Repair', [string]$Destination)
$ErrorActionPreference = 'Stop'
if (-not $Destination) { throw 'Pass a new destination directory for the verified portable app.' }
$source = Join-Path $PSScriptRoot "bin\$OutputName"
if (-not (Test-Path -LiteralPath (Join-Path $source 'VoiceBridge.exe'))) { throw 'Build the native app first.' }
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists; choose a new directory to preserve it.' }
New-Item -ItemType Directory -Path $Destination | Out-Null
# Debug symbols, Windows AI/ML runtimes (ML auto-init is disabled) and unused UI languages are not shipped.
$excluded = @('*.pdb', '*.ilk', 'onnxruntime*.dll', 'DirectML.dll', 'Microsoft.Windows.AI.*', 'Microsoft.Windows.Workloads*', 'Microsoft.Windows.Vision*', 'Microsoft.Windows.Internal.Vision.winmd', 'Microsoft.Windows.SemanticSearch.winmd', 'workloads*.json', 'NpuDetect')
$keptLanguages = @('zh-CN', 'en-us')
Get-ChildItem -LiteralPath $source -Force | Where-Object {
    $name = $_.Name
    if ($excluded | Where-Object { $name -like $_ }) { return $false }
    -not ($_.PSIsContainer -and $name -match '^[a-z]{2,3}(-[A-Za-z]+)+$' -and $keptLanguages -notcontains $name)
} | Copy-Item -Destination $Destination -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md'), (Join-Path $PSScriptRoot 'VALIDATION.md'), (Join-Path $PSScriptRoot 'THIRD_PARTY.md'), (Join-Path $PSScriptRoot 'CHECKPOINTS.md'), (Join-Path $PSScriptRoot 'DIAGNOSTICS.md') -Destination $Destination
$licenses = Join-Path $Destination 'licenses'
New-Item -ItemType Directory -Path $licenses | Out-Null
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot 'vendor') -Filter '*LICENSE*' -File | Copy-Item -Destination $licenses
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'vendor/qrcodegen.hpp') -Destination $licenses
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '.packages') -Directory -Filter 'microsoft.windowsappsdk*' | ForEach-Object {
    $package = $_
    Get-ChildItem -LiteralPath $package.FullName -Directory | ForEach-Object {
        foreach ($notice in @('license.txt', 'NOTICE.txt')) {
            $noticePath = Join-Path $_.FullName $notice
            if (Test-Path -LiteralPath $noticePath) { Copy-Item -LiteralPath $noticePath -Destination (Join-Path $licenses "$($package.Name)-$($_.Name)-$notice") }
        }
    }
}
Get-FileHash -LiteralPath (Join-Path $Destination 'VoiceBridge.exe') -Algorithm SHA256
Write-Output "Portable app: $Destination"
