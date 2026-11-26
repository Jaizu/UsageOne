# Closes the running UsageOne, builds bin\UsageOne.exe and launches it.
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\bin\UsageOne.exe"

Get-Process UsageOne -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process UsageOne -ErrorAction SilentlyContinue | Wait-Process -Timeout 10

& cmd /c "`"$root\build.cmd`""
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# Launch through WMI rather than Start-Process: a direct child of a packaged
# app (e.g. the Claude desktop app) inherits its environment and AppData
# virtualization. UsageOne also drops Claude's variables before running claude.
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = "`"$exe`""; CurrentDirectory = "$root\bin" }
if ($r.ReturnValue -ne 0) { throw "Launch failed ($($r.ReturnValue))." }
