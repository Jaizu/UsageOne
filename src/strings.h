#pragma once

#include "common.h"
#include "usage.h"

// UI text: Spanish when Windows is in Spanish, English otherwise.
struct Strings {
    const wchar_t* title;
    const wchar_t* session;
    const wchar_t* weekAll;
    const wchar_t* weekModel;      // %s = model name
    const wchar_t* resetsIn;       // %s = duration, %s = clock      (within a day)
    const wchar_t* resetsAt;       // %s = clock, %s = duration      (later)
    const wchar_t* notStarted;
    const wchar_t* peakOn;
    const wchar_t* peakOff;
    const wchar_t* peakEndsIn;     // %s
    const wchar_t* peakStartsIn;   // %s
    const wchar_t* updatedAgo;     // %s
    const wchar_t* updatedNow;
    const wchar_t* updating;
    const wchar_t* updateFailed;   // %s = time since the last good data
    const wchar_t* nextIn;         // %s = m:ss
    const wchar_t* loading;
    const wchar_t* noClaude;
    const wchar_t* notLoggedIn;
    const wchar_t* noLimits;
    const wchar_t* failed;
    const wchar_t* planUntil;      // %s = plan, %s = date
    const wchar_t* planOnly;       // %s = plan
    const wchar_t* settings;
    const wchar_t* startWithWindows;
    const wchar_t* showPeak;
    const wchar_t* starIcon;
    const wchar_t* showModelWeeks;
    const wchar_t* showRow;        // %s = a row's name: "Show Weekly · Fable"
    const wchar_t* streamDeck;
    const wchar_t* pluginLabel;
    const wchar_t* sectionGeneral;
    const wchar_t* sectionPanel;
    const wchar_t* streamDeckMissing;
    const wchar_t* pluginNotInstalled;
    const wchar_t* pluginOutdated;   // %s = installed version
    const wchar_t* pluginInstalled;  // %s = installed version
    const wchar_t* install;
    const wchar_t* reinstall;
    const wchar_t* installing;
    const wchar_t* installFailed;
    const wchar_t* updates;
    const wchar_t* versionLine;      // %s
    const wchar_t* checkUpdates;
    const wchar_t* checking;
    const wchar_t* upToDate;
    const wchar_t* checkFailed;
    const wchar_t* updateAvailable;  // %s
    const wchar_t* updateNow;
    const wchar_t* whatsNew;         // %s
    const wchar_t* barColor;
    const wchar_t* barColorDesc;
    const wchar_t* barAccent;
    const wchar_t* barWhite;
    const wchar_t* barGreen;
    const wchar_t* barCustom;
    const wchar_t* openWeb;
    const wchar_t* refresh;
    const wchar_t* exit;
    const wchar_t* tomorrow;
    const wchar_t* days[7];  // Sunday first
    const wchar_t* months[12];
};

const Strings& S();

std::wstring Format(const wchar_t* fmt, const std::wstring& a, const std::wstring& b = L"");
// "2 h 13 min", "3 d 2 h", "45 min", "<1 min".
std::wstring Duration(ULONGLONG ticks);
// Local time of a UTC instant: "15:29", "mañana 15:29" or "sáb 20:59".
std::wstring Clock(ULONGLONG utcTicks);
// "16 dic".
std::wstring ShortDate(ULONGLONG utcTicks);
std::wstring LimitName(const std::wstring& key);
std::wstring ResetLine(const Limit& l);
