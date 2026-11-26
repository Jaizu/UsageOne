# Measures the numbers in the README's Performance section, on a development
# build (tools\dev.ps1: its log has the panel's opening time).
#   - start: from launch to the panel on screen (first open, cold)
#   - open: opening the panel again (warm), several times
#   - memory and CPU of UsageOne and the Stream Deck plugin while idle
#   - how long a usage read (claude /usage) takes
# Don't use the mouse while it runs: a click closes the panel.
param([int]$IdleSeconds = 60, [int]$Opens = 10)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\bin\UsageOne.exe"
$log = "$env:LOCALAPPDATA\UsageOne\UsageOne.log"


function Start-Outside([string]$commandLine) {
    # Through WMI: outside the Claude desktop app's process tree (see tools\dev.ps1).
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = $commandLine; CurrentDirectory = "$root\bin" }
    if ($r.ReturnValue -ne 0) { throw "Launch failed ($($r.ReturnValue))." }
    $r.ProcessId
}
function Get-ShownTimes { Select-String -Path $log -Pattern "shown in ([\d.]+) ms" | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } }
# A second launch with /toggle (development builds) opens or closes the running one's panel.
function Switch-Panel { Start-Outside "`"$exe`" /toggle" | Out-Null; Start-Sleep -Milliseconds 900 }

# Cold start: launch to the panel shown.
Get-Process UsageOne -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
$launched = Get-Date
$appPid = Start-Outside "`"$exe`""
do { Start-Sleep -Milliseconds 50 } until ((Get-ShownTimes).Count -ge 1 -or ((Get-Date) - $launched).TotalSeconds -gt 20)
$coldMs = [int]((Get-Date) - $launched).TotalMilliseconds
Start-Sleep -Seconds 8  # first usage read done
Switch-Panel

# Warm opens: a second launch asks the running one to open its panel.
for ($i = 0; $i -lt $Opens; $i++) {
    Switch-Panel  # open
    Switch-Panel  # close
}
$warm = Get-ShownTimes | Select-Object -Skip 1
$warmStats = $warm | Measure-Object -Average -Maximum

# Idle: panel closed, memory and CPU over a minute.
Start-Sleep -Seconds 5
function Get-Usage([string]$name) {
    $p = Get-Process $name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) { [pscustomobject]@{ Cpu = $p.TotalProcessorTime.TotalSeconds; Ws = $p.WorkingSet64; Private = $p.PrivateMemorySize64 } }
}
$appBefore = Get-Usage UsageOne; $pluginBefore = Get-Usage "UsageOne.StreamDeck"
Start-Sleep -Seconds $IdleSeconds
$appAfter = Get-Usage UsageOne; $pluginAfter = Get-Usage "UsageOne.StreamDeck"

$reads = Select-String -Path $log -Pattern "claude exited 0 after (\d+) ms" | ForEach-Object { [int]$_.Matches[0].Groups[1].Value }

function MB($bytes) { "{0:N1} MB" -f ($bytes / 1MB) }
function CpuPct($before, $after) { "{0:N3} %" -f (($after.Cpu - $before.Cpu) / $IdleSeconds / [Environment]::ProcessorCount * 100) }
""
"UsageOne.exe size:        {0:N0} KB" -f ((Get-Item $exe).Length / 1KB)
"Cold start to panel:      $coldMs ms"
"Panel open (warm):        {0:N1} ms average, {1:N1} ms max over {2}" -f $warmStats.Average, $warmStats.Maximum, $warmStats.Count
"UsageOne idle memory:     working set $(MB $appAfter.Ws), private $(MB $appAfter.Private)"
"UsageOne idle CPU:        $(CpuPct $appBefore $appAfter) of the machine over $IdleSeconds s"
if ($pluginAfter) {
    "Plugin idle memory:       working set $(MB $pluginAfter.Ws), private $(MB $pluginAfter.Private)"
    "Plugin idle CPU:          $(CpuPct $pluginBefore $pluginAfter) of the machine over $IdleSeconds s"
}
if ($reads) { "Usage read (claude):      {0:N0} ms average over {1}" -f ($reads | Measure-Object -Average).Average, $reads.Count }

# As at sign-in: started in the background, the panel never opened.
Get-Process UsageOne -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Start-Outside "`"$exe`" /background" | Out-Null
Start-Sleep -Seconds 15  # first usage read done
$atSignIn = Get-Usage UsageOne
"UsageOne at sign-in:      working set $(MB $atSignIn.Ws), private $(MB $atSignIn.Private) (panel not opened yet)"
