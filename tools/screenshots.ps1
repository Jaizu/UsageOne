# Takes the README screenshots (docs\screenshot-dark.png, -light.png, -settings.png):
# opens the panel in each theme with /theme (and /settings), captures it from the screen with
# its rounded corners cut out, then starts UsageOne again in the background.
# Build first (tools\dev.ps1). Don't click while it runs: a click closes the panel.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\bin\UsageOne.exe"
$log = "$env:LOCALAPPDATA\UsageOne\UsageOne.log"
New-Item -ItemType Directory -Force "$root\docs" | Out-Null

function Start-UsageOne([string]$arguments) {
    Get-Process UsageOne -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process UsageOne -ErrorAction SilentlyContinue | Wait-Process -Timeout 10
    # Through WMI, as tools\dev.ps1 does (outside the Claude app's environment).
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
        CommandLine = "`"$exe`" $arguments"; CurrentDirectory = "$root\bin" }
    if ($r.ReturnValue -ne 0) { throw "Launch failed ($($r.ReturnValue))." }
}

# Runs a command through WMI and waits for it: outside the Claude desktop
# app's process tree, whose children get a virtualized registry and AppData.
function Invoke-Outside([string]$commandLine) {
    $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = $commandLine }
    if ($r.ReturnValue -ne 0) { throw "Launch failed ($($r.ReturnValue)): $commandLine" }
    Wait-Process -Id $r.ProcessId -Timeout 30 -ErrorAction SilentlyContinue
}
function Set-BarColor([int]$value) {
    Invoke-Outside "reg.exe add HKCU\Software\UsageOne /v BarColor /t REG_DWORD /d $value /f"
}

# Dark keeps your bar colour; light uses green (0-3: accent, white, green,
# custom), which reads well on it. Your setting comes back after.
$query = reg.exe query HKCU\Software\UsageOne /v BarColor 2>$null | Select-String "0x([0-9a-f]+)"
$savedBarColor = if ($query) { [Convert]::ToInt32($query.Matches[0].Groups[1].Value, 16) } else { $null }
$shots = @(
    @{ name = "dark"; args = "/theme dark"; barColor = $savedBarColor },
    @{ name = "light"; args = "/theme light"; barColor = 2 },
    @{ name = "settings"; args = "/theme dark /settings"; barColor = $savedBarColor }
)

try {
foreach ($shot in $shots) {
    if ($null -ne $shot.barColor) { Set-BarColor $shot.barColor }
    Start-UsageOne $shot.args
    Start-Sleep -Seconds 2  # it starts a new log; the old one still says "probe: status 1"
    # Wait for the numbers (the panel opens at start and reads them).
    $deadline = (Get-Date).AddSeconds(60)
    do { Start-Sleep -Milliseconds 500 } until ((Get-Date) -gt $deadline -or
        (Select-String -Path $log -Pattern "probe: status 1" -Quiet -ErrorAction SilentlyContinue))
    Start-Sleep -Milliseconds 1500  # bars and the refresh icon settle

    $shown = Select-String -Path $log -Pattern "panel (-?\d+),(-?\d+),(\d+),(\d+)" | Select-Object -Last 1
    if (-not $shown) { throw "The panel didn't open." }
    $x, $y, $w, $h = $shown.Matches[0].Groups[1..4] | ForEach-Object { [int]$_.Value }
    $scale = [double]((Select-String -Path $log -Pattern "scale (\d+\.\d+)" | Select-Object -Last 1).Matches[0].Groups[1].Value)

    $screen = New-Object Drawing.Bitmap $w, $h
    $g = [Drawing.Graphics]::FromImage($screen)
    $g.CopyFromScreen($x, $y, 0, 0, $screen.Size)
    $g.Dispose()

    # Rounded corners (8 DIPs, as the panel) become transparent.
    $out = New-Object Drawing.Bitmap $w, $h, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($out)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $d = [int][Math]::Round(16 * $scale)
    $path = New-Object Drawing.Drawing2D.GraphicsPath
    $path.AddArc(0, 0, $d, $d, 180, 90)
    $path.AddArc($w - $d - 1, 0, $d, $d, 270, 90)
    $path.AddArc($w - $d - 1, $h - $d - 1, $d, $d, 0, 90)
    $path.AddArc(0, $h - $d - 1, $d, $d, 90, 90)
    $path.CloseFigure()
    $brush = New-Object Drawing.TextureBrush $screen
    $g.FillPath($brush, $path)
    $g.Dispose()
    $out.Save("$root\docs\screenshot-$($shot.name).png", [Drawing.Imaging.ImageFormat]::Png)
    $screen.Dispose(); $out.Dispose()
    "docs\screenshot-$($shot.name).png ($w x $h)"
}
} finally {
    if ($null -ne $savedBarColor) { Set-BarColor $savedBarColor }
    else { Invoke-Outside "reg.exe delete HKCU\Software\UsageOne /v BarColor /f" }
    Start-UsageOne "/background"
}
