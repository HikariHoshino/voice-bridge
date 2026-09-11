param([ValidateSet('Release','Debug')][string]$Configuration = 'Release', [switch]$NoRestore, [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$OutputName = '')
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'Visual Studio 2022 C++ build tools are required.' }
$buildArgs = @("$PSScriptRoot\VoiceBridge.vcxproj", '/m', "/p:Configuration=$Configuration", '/p:Platform=x64', '/verbosity:minimal', '/nologo')
if (-not $NoRestore) { $buildArgs += '/restore' }
if ($OutputName) { $buildArgs += "/p:OutDir=$PSScriptRoot/bin/$OutputName/" }
& $msbuild @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
$outputFolder = Join-Path $PSScriptRoot $(if ($OutputName) { "bin\$OutputName" } else { "bin\$Configuration" })
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
$redist = Get-ChildItem -LiteralPath (Join-Path $installation 'VC\Redist\MSVC') -Directory | Where-Object Name -Match '^\d+\.' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$crt = Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT'
if (-not (Test-Path -LiteralPath $crt)) { throw 'The x64 MSVC redistributable directory is missing.' }
Get-ChildItem -LiteralPath $crt -Filter '*.dll' -File | Copy-Item -Destination $outputFolder
