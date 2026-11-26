# UsageOne

Tray app for Windows 11 that shows the Claude plan limits (5-hour session, weekly, per-model weekly), the plan, peak hours and reset countdowns. Native C++ (Win32 + Windows.UI.Composition + Direct2D/DirectWrite), no runtime dependencies. The user writes in Spanish; the UI is Spanish/English by the Windows language.

## After every code change

Always run the dev script at the end of each change set, without being asked:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev.ps1
```

It closes the running UsageOne, builds `bin\UsageOne.exe` and relaunches it. Don't add `2>&1` (any stderr line trips `$ErrorActionPreference = "Stop"`). The log is `%LOCALAPPDATA%\UsageOne\UsageOne.log`.

## How it works

- **Data** (`src/usage.cpp`): runs `claude -p /usage --output-format text --no-session-persistence` and parses `Current session:` / `Current week (<name>):` lines. No credentials are read or sent: Claude Code uses its own sign-in, and `/usage` spends no tokens.
  - The child gets our environment **minus `CLAUDE*` and `ANTHROPIC_*` variables**. Started from inside a Claude desktop session, those (e.g. `ANTHROPIC_BASE_URL`) make `/usage` omit the plan limits.
  - `claude.exe` is looked up on PATH, then `%USERPROFILE%\.local\bin`, then `claude.cmd`.
  - The plan comes from `oauthAccount` in `~/.claude.json` (`organizationType`, `organizationRateLimitTier`, `subscriptionCreatedAt`). The period end is estimated as the next monthly anniversary of `subscriptionCreatedAt`; the cancellation date is not available locally.
- **Panel** (`src/flyout.cpp`): taken from VolumeOne (`..\Simracing\VolumeOne\src\flyout.cpp`): composition visual tree with the acrylic host backdrop, drop shadow, slide-in, gear/back footer with view transition, low-level mouse hook to close on outside clicks. Placed like Quick Settings, 12 DIPs from the work-area corner next to the tray, on the tray icon's monitor (re-computed on every open, so display changes are fine).
- **Tray** (`src/main.cpp`): the session % (or the week, setting) drawn as a number; amber from 60 %, red from 85 %. Left click toggles the panel, right click opens the menu.
- **Settings**: `HKCU\Software\UsageOne` (`ShowPeak`, `TrayWeek`, `RefreshMinutes`); start with Windows is the `UsageOne` value in the Run key (`"...\UsageOne.exe" /background`).
- Peak hours: Monday-Friday 13:00-19:00 UTC (the schedule the Usage Meter widget used).

## Background

Replaces two widgets tried before: clawd-usage-winwidget (PowerShell/WPF, slow settings) and usage-meter-for-claude (Electron, needed a claude.ai sign-in, lived behind all windows). Its source is in `..\usage-meter-for-claude` for reference.
