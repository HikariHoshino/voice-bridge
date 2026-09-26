param([ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputName = 'Release', [Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot "bin\$OutputName"
if (-not (Test-Path -LiteralPath (Join-Path $source 'VoiceBridge.exe'))) { throw 'Build the WebView2 app first.' }
if (Test-Path -LiteralPath $Destination) { throw 'Destination already exists; choose a new directory to preserve it.' }
New-Item -ItemType Directory -Path $Destination | Out-Null
# The WebView2 loader is linked statically and the CRT is static, so no DLLs ship.
foreach ($item in 'VoiceBridge.exe', 'mobile.html', 'ui', 'assets') { Copy-Item -LiteralPath (Join-Path $source $item) -Destination $Destination -Recurse }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'README.md') -Destination $Destination
$licenses = New-Item -ItemType Directory -Path (Join-Path $Destination 'licenses')
Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '..\native\vendor') -Filter '*LICENSE*' -File | Copy-Item -Destination $licenses
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\native\vendor\qrcodegen.hpp') -Destination $licenses
$webview = Get-ChildItem -LiteralPath (Join-Path $PSScriptRoot '..\native\.packages\microsoft.web.webview2') -Directory | Sort-Object Name -Descending | Select-Object -First 1
foreach ($notice in 'LICENSE.txt', 'NOTICE.txt') { Copy-Item -LiteralPath (Join-Path $webview.FullName $notice) -Destination (Join-Path $licenses "WebView2-$notice") }
Get-FileHash -LiteralPath (Join-Path $Destination 'VoiceBridge.exe') -Algorithm SHA256
"{0:N1} MB" -f ((Get-ChildItem $Destination -Recurse -File | Measure-Object Length -Sum).Sum / 1MB)
Write-Output "Portable app: $Destination"
