[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) { throw 'Visual Studio C++ Build Tools and Windows SDK are required.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ x64/x86 build component was not found.' }
$setup = Join-Path $vs 'VC\Auxiliary\Build\vcvarsall.bat'
$out = Join-Path $root "artifacts\win-$Architecture"
New-Item -ItemType Directory -Force $out | Out-Null
$target = if ($Architecture -eq 'arm64') { 'amd64_arm64' } else { 'amd64' }
$command = 'set "PATH={4};%PATH%" && call "{0}" {1} >nul && cl /nologo /std:c++17 /EHsc /W4 /WX /O2 /MT /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0602 "{2}" /Fo"{3}\desktop-rescue.obj" /Fe"{3}\desktop-rescue.exe" /link /Brepro /DYNAMICBASE /NXCOMPAT user32.lib advapi32.lib psapi.lib' -f $setup, $target, (Join-Path $root 'src\main.cpp'), $out, (Split-Path $vswhere)
& $env:ComSpec /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Compiler exited with $LASTEXITCODE." }
if ($Architecture -ne 'arm64' -or [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture -eq 'Arm64') {
    & (Join-Path $out 'desktop-rescue.exe') self-test
    if ($LASTEXITCODE -ne 0) { throw 'Native self-tests failed.' }
} else {
    Write-Warning 'ARM64 cross-build: self-tests require a Windows ARM64 host. x64 CI runs the shared logic tests.'
}
if ($Package) {
    Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE'), (Join-Path $root 'COMMERCIAL-LICENSE.md') $out
    New-Item -ItemType Directory -Path (Join-Path $out 'mcp') -Force | Out-Null
    Copy-Item (Join-Path $root 'mcp\server.py'), (Join-Path $root 'mcp\requirements.txt') (Join-Path $out 'mcp')
    New-Item -ItemType Directory -Path (Join-Path $out 'docs'), (Join-Path $out 'tools') -Force | Out-Null
    Copy-Item (Join-Path $root 'docs\*.md') (Join-Path $out 'docs')
    Copy-Item (Join-Path $root 'tools\Measure-Idle.ps1') (Join-Path $out 'tools')
    $zip = Join-Path $root "artifacts\desktop-rescue-win-$Architecture.zip"
    Compress-Archive -Path (Join-Path $out 'desktop-rescue.exe'), (Join-Path $out '*.md'), (Join-Path $out 'LICENSE'), (Join-Path $out 'mcp'), (Join-Path $out 'docs'), (Join-Path $out 'tools') -DestinationPath $zip -Force
    Get-FileHash $zip -Algorithm SHA256 | Format-List Algorithm,Hash,Path
}
