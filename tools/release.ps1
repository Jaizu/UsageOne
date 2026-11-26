# Publishes a GitHub release for the version in src\version.h, with
# UsageOne.exe (the Stream Deck plugin is inside it), using that version's
# section of CHANGELOG.md as the release notes. The tag goes on the current
# commit, which must already be pushed.
# The Release workflow runs this with -IfNew on every push to main, which does
# nothing when that version is already released.
# Usage: tools\release.ps1 [-Exe bin\UsageOne.exe] [-IfNew]
param([string]$Exe = "$PSScriptRoot\..\bin\UsageOne.exe", [switch]$IfNew)
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\.."

$h = Get-Content "$root\src\version.h" -Raw
$v = "{0}.{1}.{2}" -f ([regex]::Match($h, 'VERSION_MAJOR (\d+)').Groups[1].Value),
                      ([regex]::Match($h, 'VERSION_MINOR (\d+)').Groups[1].Value),
                      ([regex]::Match($h, 'VERSION_PATCH (\d+)').Groups[1].Value)

if ($IfNew) {
    $released = gh release list --repo Jaizu/UsageOne --limit 1000 --json tagName --jq ".[].tagName"
    if ($LASTEXITCODE -ne 0) { throw "Could not list the releases." }
    if ($released -contains "v$v") { "v$v is already released."; return }
}

if (-not (Test-Path $Exe)) { throw "$Exe not found. Rebuild first." }
$exeVersion = (Get-Item $Exe).VersionInfo.ProductVersion
if ($exeVersion -ne $v) { throw "$Exe is version $exeVersion, but src\version.h says $v. Rebuild first." }

$changelog = [IO.File]::ReadAllText("$root\CHANGELOG.md")
$m = [regex]::Match($changelog, "(?ms)^## $([regex]::Escape($v))\s*\r?\n(.*?)(?=^## |\z)")
if (-not $m.Success) { throw "CHANGELOG.md has no section for $v." }
$notes = $m.Groups[1].Value.Trim()
$notesFile = Join-Path $env:TEMP "usageone-notes-$v.md"
[IO.File]::WriteAllText($notesFile, $notes, (New-Object System.Text.UTF8Encoding $false))  # no BOM

$commit = git -C $root rev-parse HEAD
gh release create "v$v" $Exe --repo Jaizu/UsageOne --target $commit --title "UsageOne $v" --notes-file $notesFile
if ($LASTEXITCODE -ne 0) { throw "Could not create the release." }
