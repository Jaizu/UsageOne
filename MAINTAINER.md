# Maintainer's setup

Notes about the maintainer's own machine and preferences, kept apart from the project docs so the rest stays generic. CLAUDE.md imports this file.

## Folders

- This repository: `C:\Users\depre\Documents\UsageOne` (private on GitHub as `Jaizu/UsageOne`). The running copy is `bin\UsageOne.exe` here, and the Run key points to it.
- VolumeOne, where the panel, Settings sections, colour picker and updater come from: `C:\Users\depre\Documents\Simracing\VolumeOne`.
- BatteryOne, the Stream Deck plugin pattern (app serves its state on a pipe, plugin draws the key): `C:\Users\depre\Documents\Simracing\BatteryOne`.
- usage-meter-for-claude (Electron widget used before, kept for reference): `C:\Users\depre\Documents\usage-meter-for-claude`.
- Claude Code CLI: `C:\Users\depre\.local\bin\claude.exe`.

## Preferences

- Writes in Spanish; Windows is in English, so the UI shows English.
- Windows 11 look like the system flyouts, light on resources, no web/Electron widgets.
- Triple screens (Eyefinity): the panel must survive display changes.
- Uses a Stream Deck; their other plugins use `com.jaizu.*` UUIDs.
- Bar colour: a custom gold. README screenshots use it on dark and green on light (`tools\screenshots.ps1`).
- Donations: https://ko-fi.com/jaizu

## History

Replaces two widgets tried before: clawd-usage-winwidget (PowerShell/WPF, slow settings) and usage-meter-for-claude (Electron, needed a claude.ai sign-in, lived behind all windows).
