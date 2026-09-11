param([ValidateSet('Release','Debug')][string]$Configuration = 'Release', [switch]$NoRestore)
$ErrorActionPreference = 'Stop'
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'Visual Studio 2022 C++ build tools are required.' }
$buildArgs = @("$PSScriptRoot\VoiceBridge.vcxproj", '/m', "/p:Configuration=$Configuration", '/p:Platform=x64', '/verbosity:minimal', '/nologo')
if (-not $NoRestore) { $buildArgs += '/restore' }
& $msbuild @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Native build failed: $LASTEXITCODE" }
