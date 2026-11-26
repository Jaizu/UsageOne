#pragma once

#include "common.h"

// Kept in HKCU\Software\UsageOne. "Start with Windows" is the Run key itself.
struct Settings {
    bool showPeak = true;      // peak hours row
    bool starIcon = false;     // tray icon: the Claude star instead of the mascot
    bool showModelWeeks = true;  // per-model weekly rows (e.g. "Weekly · Fable"), when the plan has them
    enum BarColor : DWORD { BarAccent, BarWhite, BarGreen, BarCustom };
    DWORD barColor = BarAccent;   // bars below 60 % (from 60 % they turn yellow, from 85 % red)
    DWORD barCustomColor = 0xD97757;  // 0xRRGGBB, with BarCustom (Claude's orange to start)
    int refreshMinutes = 5;

    void Load();
    void Save() const;
};

bool GetStartWithWindows();
void SetStartWithWindows(bool enable);
// Turns it on at the first run, and keeps the entry pointing at this exe.
void InitStartWithWindows();
