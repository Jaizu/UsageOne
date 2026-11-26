# Installs (or updates) the Stream Deck plugin from bin\ (build it first with
# tools\dev.ps1) and restarts Stream Deck so it loads it.
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\bin\UsageOne.StreamDeck.exe"
if (-not (Test-Path $exe)) { throw "Build first: tools\dev.ps1" }

# Through WMI, outside the Claude desktop app's process tree: its children
# get AppData virtualization, and the plugin folder must be the real one.
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = "`"$exe`" /install"; CurrentDirectory = "$root\bin" }
if ($r.ReturnValue -ne 0) { throw "Launch failed ($($r.ReturnValue))." }
Wait-Process -Id $r.ProcessId -Timeout 60 -ErrorAction SilentlyContinue

$log = "$env:LOCALAPPDATA\UsageOne\StreamDeck.log"
if (Test-Path $log) { Get-Content $log -Tail 1 }
