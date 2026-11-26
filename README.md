# UsageOne

Your Claude plan limits in the Windows 11 tray: the current session, the weekly limits, when each one resets, your plan and the peak hours, in a panel with the Windows look (acrylic, shadow, accent colour) that opens right above the tray icon.

- **No sign-in.** The numbers come from Claude Code (`claude /usage`), with the sign-in it already has. No tokens are spent and no credentials are read.
- **Tray number.** The icon shows the session percentage (or the week's), amber from 60 % and red from 85 %. Hover for all the limits.
- **Native and small.** One exe, no runtime.

Requires Windows 10/11 and [Claude Code](https://docs.claude.com/en/docs/claude-code) signed in.

## Build

```
build.cmd
```

Needs Visual Studio (or the Build Tools) with "Desktop development with C++". The exe is `bin\UsageOne.exe`.
