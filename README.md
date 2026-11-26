# UsageOne

Your Claude plan limits in the Windows 11 tray: the current session, the weekly limits, when each one resets, your plan and the peak hours, in a panel with the Windows look (acrylic, shadow, accent colour) that opens right above the tray icon.

- **No sign-in.** The numbers come from Claude Code (`claude /usage`), with the sign-in it already has. No tokens are spent and no credentials are read.
- **Tray icon.** Claude Code's mascot (or the Claude star) in the taskbar's colour. Hover for all the limits.
- **Settings.** Start with Windows, peak hours, per-model weekly limits, tray icon, and the bar colour (accent, white, green or any colour).
- **Stream Deck.** A key with the session and weekly bars, drawn like the panel; tap it to refresh, hold it to open Claude.
- **Native and small.** One exe, no runtime.

Requires Windows 10/11 and [Claude Code](https://docs.claude.com/en/docs/claude-code) signed in.

## Build

```
build.cmd
```

Needs Visual Studio (or the Build Tools) with "Desktop development with C++". It builds `bin\UsageOne.exe` and the Stream Deck plugin `bin\UsageOne.StreamDeck.exe`.

## Stream Deck plugin

```
powershell -ExecutionPolicy Bypass -File tools\install-plugin.ps1
```

Copies the plugin into Stream Deck's plugin folder (with its manifest and images) and restarts Stream Deck. Then drag **UsageOne â€º Claude usage** onto a key. The key reads the numbers from the running UsageOne; if UsageOne is closed, pressing the key starts it.
