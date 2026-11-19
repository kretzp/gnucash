param(
    [ValidateSet('configure', 'build', 'test', 'test-mcp', 'install', 'run', 'mcp-run', 'version')]
    [string]$Action = 'build'
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$bashPath = Join-Path $projectRoot '.windows-build/msys2/usr/bin/bash.exe'
if (!(Test-Path -LiteralPath $bashPath)) {
    throw 'The project MSYS2 environment is missing. See tools/WINDOWS-BUILD.md.'
}
$oldMsystem = $env:MSYSTEM
$oldChere = $env:CHERE_INVOKING
$oldWindowsLocale = $env:GNC_WINDOWS_LOCALE
try {
    $env:MSYSTEM = 'MINGW64'
    $env:CHERE_INVOKING = '1'
    $cultureName = [Globalization.CultureInfo]::CurrentCulture.Name
    if (!$cultureName) { $cultureName = 'en-US' }
    $env:GNC_WINDOWS_LOCALE = $cultureName.Replace('-', '_') + '.UTF-8'
    Push-Location $projectRoot
    try {
        & $bashPath --login tools/build-windows.sh $Action
        if ($LASTEXITCODE -ne 0) { throw "Windows $Action failed (exit $LASTEXITCODE)." }
    } finally { Pop-Location }
} finally {
    $env:MSYSTEM = $oldMsystem
    $env:CHERE_INVOKING = $oldChere
    $env:GNC_WINDOWS_LOCALE = $oldWindowsLocale
}
