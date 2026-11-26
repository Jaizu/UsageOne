#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define APP_NAME L"UsageOne"
#include "version.h"

#define APP_VERSION_W2(s) L##s
#define APP_VERSION_W(s) APP_VERSION_W2(s)
#define APP_VERSION APP_VERSION_W(APP_VERSION_A)

#define HOST_WINDOW_CLASS L"UsageOne.Host"
#define RUNNING_MUTEX_NAME L"Local\\UsageOne.Running"

// Window messages shared between modules.
enum : UINT {
    WM_APP_TRAY = WM_APP + 1,  // tray icon callback
    WM_APP_SHOW_FLYOUT,        // second instance asked us to open
    WM_APP_PROBE_DONE,         // usage probe finished (lParam = UsageData*)
    WM_APP_REFRESH,            // the Stream Deck key asked for fresh numbers
};

// The Stream Deck plugin asks the running app for its numbers through this
// pipe: it writes "status" or "refresh" and a newline, and gets one line of
// JSON back (see PipeStatus in main.cpp).
#define STATUS_PIPE_NAME L"\\\\.\\pipe\\UsageOne"
