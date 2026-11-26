#pragma once

#include "common.h"

// Kept in HKCU\Software\UsageOne. "Start with Windows" is the Run key itself.
struct Settings {
    bool showPeak = true;      // peak hours row
    bool trayWeek = false;     // the tray number shows the week instead of the session
    int refreshMinutes = 5;

    void Load();
    void Save() const;
};

bool GetStartWithWindows();
void SetStartWithWindows(bool enable);
