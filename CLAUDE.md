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
  - The plan comes from `oauthAccount` in `~/.claude.json` (`organizationType`, `organizationRateLimitTier`). Only the plan name is shown: the billing/renewal date and a scheduled downgrade are not available locally, nor from the OAuth profile API (`subscription_status` stays `active` until the period ends), and estimating the date from `subscriptionCreatedAt` was wrong (an upgrade moves the billing day).
- **Panel** (`src/flyout.cpp`): taken from VolumeOne (`..\Simracing\VolumeOne\src\flyout.cpp`): composition visual tree with the acrylic host backdrop, drop shadow, slide-in, gear/back footer with view transition, low-level mouse hook to close on outside clicks. Placed like Quick Settings, 12 DIPs from the work-area corner next to the tray, on the tray icon's monitor (re-computed on every open, so display changes are fine).
- **Tray** (`src/main.cpp`, shapes in `src/trayicon.cpp`): Claude Code's mascot (16x5 pixel art from its welcome-screen block characters; one pixel per unit at 16 px, so it stays sharp) or, as a setting, the Claude star, in the taskbar's text colour (white on dark, black on light). The numbers are in the tooltip. Left click toggles the panel, right click opens the menu. `UsageOne.exe /icon-preview <file.bmp>` draws both shapes at 16-64 px for checking.
- **Bars** open at their value (no animation on open, the user didn't like it) and ease only when new numbers arrive while the panel is open.
- **Settings**: `HKCU\Software\UsageOne` (`ShowPeak`, `StarIcon`, `ShowModelWeeks`, `BarColor` 0-3 accent/white/green/custom, `BarCustomColor` 0xRRGGBB, `RefreshMinutes`, and `AppPath`, written on each start for the plugin); start with Windows is the `UsageOne` value in the Run key (`"...\UsageOne.exe" /background`). The running copy must be `Documents\UsageOne\bin\UsageOne.exe` (the Run key points there).
- **Per-model weekly rows** ("Weekly · Fable") only appear when `/usage` reports them (the plan has them) and can be hidden in Settings; the tooltip follows the same setting.
- **Bar colour** (below 60 %; yellow from 60, red from 85): accent, white, green or custom, with a hex field (a layered EDIT child over the drawn field) and a hue/saturation picker, ported from VolumeOne's meter colour.
- **Stream Deck plugin** (`src/plugin.cpp` → `bin\UsageOne.StreamDeck.exe`, manifest in `streamdeck\`, UUID `com.jaizu.usageone`): the BatteryOne pattern in native C++. The app serves one JSON line on the pipe `\\.\pipe\UsageOne` (`status` / `refresh`, see `PublishStatus` in `main.cpp`); the plugin polls it every 5 s (1 s while probing), draws the key with Direct2D into a WIC bitmap (same fonts, colours and bar colour setting as the panel) and sends it as a PNG data URL over a WinHTTP WebSocket. A tap (on release) = refresh, or start UsageOne from `AppPath` if it's closed; holding it 500 ms opens the Claude desktop app (`shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude`, claude.ai if missing). Install/update with `tools\install-plugin.ps1` (runs `UsageOne.StreamDeck.exe /install` through WMI so the AppData writes aren't virtualized by the Claude app, and restarts Stream Deck). `/make-images <dir>` renders the manifest PNGs. Log: `%LOCALAPPDATA%\UsageOne\StreamDeck.log`.
- Peak hours: Monday-Friday 13:00-19:00 UTC (the schedule the Usage Meter widget used).

## Background

Replaces two widgets tried before: clawd-usage-winwidget (PowerShell/WPF, slow settings) and usage-meter-for-claude (Electron, needed a claude.ai sign-in, lived behind all windows). Its source is in `..\usage-meter-for-claude` for reference.
