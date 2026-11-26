#include "log.h"

#include <shlobj.h>
#include <cstdarg>
#include <cstdio>

namespace Log {

static CRITICAL_SECTION g_lock;
static HANDLE g_file = INVALID_HANDLE_VALUE;

std::wstring Folder() {
    std::wstring dir;
    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        dir = std::wstring(local) + L"\\" APP_NAME;
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    CoTaskMemFree(local);
    return dir;
}

void Init() {
    InitializeCriticalSection(&g_lock);
    std::wstring dir = Folder();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\" APP_NAME L".log";
    MoveFileExW(path.c_str(), (dir + L"\\" APP_NAME L".old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    Write(L"%s %s started", APP_NAME, APP_VERSION);
}

void Write(const wchar_t* fmt, ...) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    int n = swprintf_s(line, L"%02d:%02d:%02d.%03d  %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    if (n <= 0) return;
    char utf8[3600];
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8), nullptr, nullptr);
    EnterCriticalSection(&g_lock);
    DWORD written;
    WriteFile(g_file, utf8, bytes, &written, nullptr);
    LeaveCriticalSection(&g_lock);
}

}  // namespace Log
