#include "common.h"
#include "flyout.h"
#include "log.h"
#include "settings.h"
#include "trayicon.h"
#include "strings.h"
#include "usage.h"

#include <shellscalingapi.h>

namespace {

const UINT kTrayId = 1;
const UINT_PTR kTimerTrayRetry = 1;
const UINT_PTR kTimerRefresh = 2;
const UINT_PTR kTimerTray = 3;  // tooltip countdowns
const ULONGLONG kStaleOnOpen = 60 * Usage::kSecond;  // opening the panel refreshes older data

enum MenuId { IdRefresh = 1, IdSettings, IdWeb, IdExit };

HINSTANCE g_inst;
HWND g_host;
HANDLE g_runningMutex;
UINT g_taskbarCreated;
Settings g_settings;
UsageState g_state;
Flyout g_flyout;
bool g_flyoutCreated = false;
bool g_trayAdded = false;
HICON g_trayIcon = nullptr;

// --- Dark context menu (undocumented uxtheme exports) ------------------------

void InitDarkMenus() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (setMode) setMode(1);  // AllowDark: follow the system app theme
    if (flush) flush();
}

void FlushMenuThemes() {
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    using FlushMenuThemesFn = void(WINAPI*)();
    if (auto flush = (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136))) flush();
}

// --- Tray icon: the percentage as a number ------------------------------------

bool TaskbarIsLight() {
    DWORD light = 0, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light != 0;
}

std::wstring TrayTip() {
    std::wstring tip;
    for (const Limit& l : g_state.data.limits) {
        if (!g_settings.showModelWeeks && Usage::IsModelWeek(l)) continue;
        if (!tip.empty()) tip += L"\n";
        tip += LimitName(l.key) + L": " + std::to_wstring(l.percent) + L"%";
        if (l.key == L"session" && l.resetAt) {
            ULONGLONG now = Usage::NowTicks();
            tip += L" · " + Duration(l.resetAt > now ? l.resetAt - now : 0);
        }
    }
    if (tip.empty()) tip = APP_NAME;
    if (!g_state.data.plan.name.empty()) tip += L"\n" + Format(S().planOnly, g_state.data.plan.name);
    return tip;
}

// Claude Code's mascot (or the Claude star, a setting) in the taskbar's text
// colour: white on a dark taskbar, black on a light one. The numbers are in
// the tooltip and the panel.
void UpdateTray() {
    bool light = TaskbarIsLight();
    COLORREF color = light ? RGB(0x00, 0x00, 0x00) : RGB(0xFF, 0xFF, 0xFF);

    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | NIF_ICON;
    nid.uCallbackMessage = WM_APP_TRAY;
    HICON oldIcon = g_trayIcon;
    g_trayIcon = MakeTrayIcon(GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()), color, g_settings.starIcon);
    nid.hIcon = g_trayIcon;
    wcsncpy_s(nid.szTip, TrayTip().c_str(), _TRUNCATE);

    if (g_trayAdded && !Shell_NotifyIconW(NIM_MODIFY, &nid)) g_trayAdded = false;
    if (!g_trayAdded) {
        if (Shell_NotifyIconW(NIM_ADD, &nid)) {
            nid.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid);
            g_trayAdded = true;
            KillTimer(g_host, kTimerTrayRetry);
            LOG(L"tray: icon added");
        } else {
            SetTimer(g_host, kTimerTrayRetry, 1000, nullptr);  // explorer not ready yet
        }
    }
    if (oldIcon) DestroyIcon(oldIcon);
}

void RemoveTray() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// --- Status pipe for the Stream Deck plugin -----------------------------------

SRWLOCK g_statusLock = SRWLOCK_INIT;
std::string g_statusJson;  // UTF-8, rebuilt on the UI thread whenever the state changes

std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// {"status":"ok","probing":false,"plan":"Max 5x","session":91,"sessionReset":1764166140,
//  "week":67,"weekReset":1764280000}. Resets are Unix seconds, 0 when not started;
// a missing limit is -1.
void PublishStatus() {
    static const char* kStatus[] = {"none", "ok", "noClaude", "notLoggedIn", "noLimits", "failed"};
    auto unix = [](ULONGLONG t) -> long long { return t ? (long long)((t - 116444736000000000ULL) / Usage::kSecond) : 0; };
    int session = -1, week = -1;
    long long sessionReset = 0, weekReset = 0;
    for (const Limit& l : g_state.data.limits) {
        if (l.key == L"session") session = l.percent, sessionReset = unix(l.resetAt);
        if (l.key == L"all models") week = l.percent, weekReset = unix(l.resetAt);
    }
    std::string plan;
    for (char c : Utf8(g_state.data.plan.name))
        if (c != '"' && c != '\\') plan += c;
    char buf[512];
    sprintf_s(buf,
              "{\"status\":\"%s\",\"probing\":%s,\"plan\":\"%s\",\"session\":%d,\"sessionReset\":%lld,"
              "\"week\":%d,\"weekReset\":%lld}\n",
              kStatus[g_state.last], g_state.probing ? "true" : "false", plan.c_str(), session, sessionReset, week,
              weekReset);
    AcquireSRWLockExclusive(&g_statusLock);
    g_statusJson = buf;
    ReleaseSRWLockExclusive(&g_statusLock);
}

// One client at a time, one request per connection: a line in, a line out.
DWORD WINAPI PipeThread(void*) {
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(STATUS_PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                       4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LOG(L"pipe: CreateNamedPipe failed (%lu)", GetLastError());
            return 1;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char req[64] = {};
            DWORD got = 0, total = 0;
            while (total < sizeof(req) - 1 && ReadFile(pipe, req + total, sizeof(req) - 1 - total, &got, nullptr) && got) {
                total += got;
                if (memchr(req, '\n', total)) break;
            }
            if (strncmp(req, "refresh", 7) == 0) PostMessageW(g_host, WM_APP_REFRESH, 0, 0);
            AcquireSRWLockShared(&g_statusLock);
            std::string reply = g_statusJson;
            ReleaseSRWLockShared(&g_statusLock);
            DWORD wrote = 0;
            WriteFile(pipe, reply.data(), (DWORD)reply.size(), &wrote, nullptr);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

// --- Probing ------------------------------------------------------------------

void StartProbe() {
    if (g_state.probing) return;
    g_state.probing = true;
    PublishStatus();
    if (g_flyoutCreated) g_flyout.OnUsageChanged();
    HANDLE h = CreateThread(nullptr, 0, [](void*) -> DWORD {
        auto d = new UsageData(Usage::Probe());
        PostMessageW(g_host, WM_APP_PROBE_DONE, 0, (LPARAM)d);
        return 0;
    }, nullptr, 0, nullptr);
    if (h)
        CloseHandle(h);
    else {
        g_state.probing = false;
        PublishStatus();
    }
}

void ScheduleRefresh(UINT ms) {
    g_state.nextRefreshTick = GetTickCount64() + ms;
    SetTimer(g_host, kTimerRefresh, ms, nullptr);
}

void OnProbeDone(UsageData* d) {
    g_state.probing = false;
    g_state.last = d->status;
    if (d->status == UsageData::Ok) {
        g_state.data = std::move(*d);
    } else if (!d->plan.name.empty()) {
        g_state.data.plan = d->plan;  // the limits stay from the last good read
    }
    delete d;
    bool ok = g_state.last == UsageData::Ok;
    // Without numbers, try again sooner.
    ScheduleRefresh(ok || !g_state.data.limits.empty() ? g_settings.refreshMinutes * 60000u : 60000u);
    PublishStatus();
    UpdateTray();
    if (g_flyoutCreated) g_flyout.OnUsageChanged();
}

// --- Flyout and menu ----------------------------------------------------------

void EnsureFlyout() {
    if (g_flyoutCreated) return;
    static bool failed = false;
    if (failed) return;
    g_flyoutCreated = g_flyout.Create(g_inst, g_host, &g_state, &g_settings);
    LOG(L"flyout: create %s", g_flyoutCreated ? L"ok" : L"FAILED");
    if (!g_flyoutCreated) {
        failed = true;
        return;
    }
    g_flyout.onRefresh = [] { StartProbe(); };
    g_flyout.onTrayIconChanged = [] { UpdateTray(); };
}

void ShowFlyout(bool settings) {
    EnsureFlyout();
    if (!g_flyoutCreated) return;
    ULONGLONG now = Usage::NowTicks();
    if (!g_flyout.IsVisible() && (!g_state.data.fetchedAt || now - g_state.data.fetchedAt > kStaleOnOpen)) StartProbe();
    g_flyout.Show(settings);
}

void ToggleFlyout() {
    EnsureFlyout();
    if (!g_flyoutCreated) return;
    if (g_flyout.IsVisible())
        g_flyout.Hide();
    else {
        ULONGLONG now = Usage::NowTicks();
        if (!g_state.data.fetchedAt || now - g_state.data.fetchedAt > kStaleOnOpen) StartProbe();
        g_flyout.Toggle();
    }
}

void ShowMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IdRefresh, S().refresh);
    AppendMenuW(menu, MF_STRING, IdSettings, S().settings);
    AppendMenuW(menu, MF_STRING, IdWeb, S().openWeb);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IdExit, S().exit);
    SetForegroundWindow(g_host);
    UINT align = GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | align, x, y, 0, g_host, nullptr);
    PostMessageW(g_host, WM_NULL, 0, 0);
    DestroyMenu(menu);
    switch (cmd) {
        case IdRefresh:
            StartProbe();
            break;
        case IdSettings:
            ShowFlyout(true);
            break;
        case IdWeb:
            ShellExecuteW(nullptr, L"open", L"https://claude.ai/settings/usage", nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IdExit:
            DestroyWindow(g_host);
            break;
    }
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreated && msg != 0) {  // explorer (re)started
        g_trayAdded = false;
        UpdateTray();
        return 0;
    }
    switch (msg) {
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ToggleFlyout();
                    break;
                case WM_CONTEXTMENU:
                    ShowMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
                    break;
            }
            return 0;

        case WM_APP_SHOW_FLYOUT:
            ShowFlyout(false);
            return 0;

        case WM_APP_PROBE_DONE:
            OnProbeDone((UsageData*)lp);
            return 0;

        case WM_APP_REFRESH:
            StartProbe();
            return 0;

        case WM_TIMER:
            if (wp == kTimerRefresh) {
                KillTimer(hwnd, kTimerRefresh);
                StartProbe();
            } else if (wp == kTimerTray) {
                UpdateTray();
            } else if (wp == kTimerTrayRetry) {
                KillTimer(hwnd, kTimerTrayRetry);
                UpdateTray();
            }
            return 0;

        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) {
                LOG(L"resumed from sleep: refreshing");
                StartProbe();
            }
            return TRUE;

        case WM_SETTINGCHANGE:
            if (lp && wcscmp((const wchar_t*)lp, L"ImmersiveColorSet") == 0) {
                FlushMenuThemes();
                UpdateTray();
                if (g_flyoutCreated) g_flyout.OnThemeChanged();
            }
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            UpdateTray();
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    g_inst = inst;

    // Development aid: draw the tray icons at several sizes into a BMP and exit.
    if (const wchar_t* p = wcsstr(cmdLine, L"/icon-preview ")) {
        std::wstring path = p + 14;
        path.erase(std::remove(path.begin(), path.end(), L'"'), path.end());
        return WriteIconPreview(path.c_str()) ? 0 : 1;
    }

    // Single instance: a second launch opens the running one's panel.
    g_runningMutex = CreateMutexW(nullptr, FALSE, RUNNING_MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(HOST_WINDOW_CLASS, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, WM_APP_SHOW_FLYOUT, 0, 0);
        }
        return 0;
    }
    bool background = wcsstr(cmdLine, L"/background") != nullptr;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log::Init();
    InitDarkMenus();
    g_settings.Load();

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = inst;
    wc.lpszClassName = HOST_WINDOW_CLASS;
    RegisterClassExW(&wc);
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, HOST_WINDOW_CLASS, APP_NAME, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_host, g_taskbarCreated, MSGFLT_ALLOW, nullptr);

    g_state.data.plan = Usage::ReadPlan();
    UpdateTray();
    PublishStatus();
    if (HANDLE h = CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr)) CloseHandle(h);
    // Where we are, so the Stream Deck plugin can start us when its key is pressed.
    {
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\" APP_NAME, L"AppPath", REG_SZ, exe, (n + 1) * sizeof(wchar_t));
    }
    StartProbe();
    SetTimer(g_host, kTimerTray, 60 * 1000, nullptr);
    if (!background) PostMessageW(g_host, WM_APP_SHOW_FLYOUT, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    RemoveTray();
    CloseHandle(g_runningMutex);
    CoUninitialize();
    LOG(L"exited");
    return 0;
}
