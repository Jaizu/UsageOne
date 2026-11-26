# UsageOne

Tray app for Windows 11 that shows the Claude plan limits (5-hour session, weekly, per-model weekly), the plan, peak hours and reset countdowns, with a Stream Deck plugin. Native C++ (Win32 + Windows.UI.Composition + Direct2D/DirectWrite), no runtime dependencies. The UI is Spanish or English, by the Windows language.

The maintainer's own setup (their folders, related projects, preferences) is in @MAINTAINER.md.

## After every code change

Run the dev script at the end of each change set, without being asked:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev.ps1
```

It closes the running UsageOne, builds a development build (`build.cmd dev`) into `bin\` and relaunches it. Don't add `2>&1` (any stderr line trips `$ErrorActionPreference = "Stop"`). The log is `%LOCALAPPDATA%\UsageOne\UsageOne.log`.

Keep the code readable and split by responsibility (one file per piece, see the map below). Development-only code (timing, screenshot and test switches) goes under `#ifdef USAGEONE_DEV`, which only `build.cmd dev` defines; releases are built without it.

## Code map

| File | What it does |
| --- | --- |
| `src/main.cpp` | Startup, single instance, tray icon and menu, refresh timer, wiring of the pieces below |
| `src/usage.cpp` | Reads the usage from Claude Code and the plan from `~/.claude.json` |
| `src/flyout.cpp` | The panel: composition visual tree, main view, Settings (collapsible sections), input, animations |
| `src/colorpicker.cpp` | The custom bar colour: hex field and hue/saturation picker |
| `src/flyout_style.h` | Fonts and the colour helper shared by the panel's files |
| `src/statuspipe.cpp` | The pipe the Stream Deck plugin reads the numbers from |
| `src/streamdeck.cpp` | Installing the plugin built into the exe, and reading its installed version |
| `src/updater.cpp` | In-app updates from GitHub releases |
| `src/settings.cpp`, `src/strings.cpp`, `src/trayicon.cpp`, `src/log.cpp` | Settings in the registry, UI text, tray icon shapes, the log |
| `src/plugin/` | The Stream Deck plugin (`UsageOne.StreamDeck.exe`); `plugin.h` lists its files |

## How it works

- **Data** (`src/usage.cpp`): runs `claude -p /usage --output-format text --no-session-persistence` and parses `Current session:` / `Current week (<name>):` lines. No credentials are read or sent: Claude Code uses its own sign-in, and `/usage` spends no tokens.
  - The child gets our environment **minus `CLAUDE*` and `ANTHROPIC_*` variables**. Started from inside a Claude desktop session, those (e.g. `ANTHROPIC_BASE_URL`) make `/usage` omit the plan limits.
  - `claude.exe` is looked up on PATH, then `%USERPROFILE%\.local\bin`, then `claude.cmd`.
  - Reset times are printed in the system's time zone and parsed as local time.
  - The plan comes from `oauthAccount` in `~/.claude.json` (`organizationType`, `organizationRateLimitTier`). Only the plan name is shown: the billing/renewal date and a scheduled downgrade are not available locally, nor from the OAuth profile API (`subscription_status` stays `active` until the period ends), and estimating the date from `subscriptionCreatedAt` was wrong (an upgrade moves the billing day).
- **Panel** (`src/flyout.cpp`), from VolumeOne: composition visual tree with the acrylic host backdrop, drop shadow, slide-in, gear/back footer with view transition, low-level mouse hook to close on outside clicks. Placed like Quick Settings, 12 DIPs from the work-area corner next to the tray, on the tray icon's monitor (re-computed on every open, so display changes are fine). On hide it goes back to the main view and redraws while hidden, so the next open never flashes the previous view.
- **Settings view**: collapsible sections (General, Panel, Stream Deck) from VolumeOne, then the Updates card and the claude.ai link.
- **Tray** (`src/main.cpp`, shapes in `src/trayicon.cpp`): Claude Code's mascot (16x5 pixel art; one pixel per unit at 16 px, so it stays sharp) or, as a setting, the Claude star, in the taskbar's text colour (`SystemUsesLightTheme`: white on a dark taskbar, black on a light one; redrawn on theme change). The numbers are in the tooltip. Left click toggles the panel, right click opens the menu.
- **Refresh button**: spins while reading, finishes its turn back to rest, and can't be pressed (nor F5) until then.
- **Bars** open at their value (no animation on open) and ease only when new numbers arrive while the panel is open. Colour below 60 %: accent, white, green or custom; yellow from 60, red from 85.
- **Per-model weekly rows** ("Weekly · Fable") only appear when `/usage` reports them and can be hidden in Settings (the toggle is named after them); the tooltip follows the same setting.
- **Settings storage**: `HKCU\Software\UsageOne` (`ShowPeak`, `StarIcon`, `ShowModelWeeks`, `BarColor` 0-3 accent/white/green/custom, `BarCustomColor` 0xRRGGBB, `RefreshMinutes`, and `AppPath`, written on each start for the plugin). Start with Windows is the `UsageOne` value in the Run key (`"...\UsageOne.exe" /background`).
- **Stream Deck plugin** (`src/plugin/`, manifest `streamdeck/manifest.json`, UUID `com.jaizu.usageone`): built first by `build.cmd` into `obj\` and embedded in `UsageOne.exe` (RCDATA 2 and 3 in `res/app.rc`). Settings → Stream Deck → Install writes both to `%TEMP%\UsageOnePlugin` and runs `UsageOne.StreamDeck.exe /install`, which copies itself into Stream Deck's plugins with the manifest stamped with the app version, draws the images and restarts Stream Deck. Settings offers "Update" when the installed version differs from the app's.
  - The app serves one JSON line on `\\.\pipe\UsageOne` (`status` / `refresh`, see `src/statuspipe.h`). The plugin polls it every 5 s (1 s while reading), draws the key with Direct2D into a PNG and sends it over a WinHTTP WebSocket. A tap (on release) refreshes, or starts UsageOne from `AppPath`; holding 500 ms opens the Claude desktop app (`shell:AppsFolder\Claude_pzs8sxrjxfjjc!Claude`, the same id everywhere; claude.ai if missing). Log: `%LOCALAPPDATA%\UsageOne\StreamDeck.log`.
- **Updates** (`src/updater.cpp`, from VolumeOne): checks `api.github.com/repos/Jaizu/UsageOne/releases/latest` at start and daily, shows a banner and "What's new" in Settings, downloads `UsageOne.exe` (checked against the asset's SHA-256), swaps it in (the running exe is renamed to `.old`) and restarts with `/updated <pid>`. Only works once the repository's releases are public.
- **Peak hours**: Monday-Friday 13:00-19:00 UTC.

## Versions and releases

- The version is in `src/version.h` (also used by the plugin's manifest). Bump it for every user-visible change and add a section to `CHANGELOG.md` (its notes are the release notes and the "What's new" text).
- `.github/workflows/release.yml` builds every push to `main` and runs `tools\release.ps1 -IfNew`, which publishes `v<version>` with `UsageOne.exe` when that version has no release yet.

## Development tools

- `tools\dev.ps1`: development build and relaunch.
- `tools\screenshots.ps1`: the README screenshots (`/theme light|dark`, `/settings`; development builds).
- `tools\measure.ps1`: the README's performance numbers (start, panel open, memory, CPU; development builds, `/toggle`).
- `tools\install-plugin.ps1`: installs the plugin from `obj\` without going through Settings.
- `tools\use-release.ps1`: replaces the local build with the latest release.
- Launch tools through WMI (`Win32_Process.Create`), as these scripts do, when running from the Claude desktop app: its child processes get a virtualized registry and AppData, and can't see other apps' windows.
