// Talking to UsageOne through its status pipe, starting it, opening Claude,
// and the plugin's log.

#include "plugin.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdarg>
#include <mutex>

namespace Plugin {

// --- Log ----------------------------------------------------------------------

static std::mutex g_logLock;
static std::wstring g_logPath;

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_logPath.empty()) {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return;
        std::wstring dir = std::wstring(local) + L"\\" APP_NAME;
        CoTaskMemFree(local);
        CreateDirectoryW(dir.c_str(), nullptr);
        g_logPath = dir + L"\\StreamDeck.log";
        // Kept small: started over once it passes 256 KB.
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g_logPath.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024)
            DeleteFileW(g_logPath.c_str());
    }
    char line[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = sprintf_s(line, "%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, args);
    va_end(args);
    n = std::min(n, (int)sizeof(line) - 3);
    line[n++] = '\r';
    line[n++] = '\n';
    HANDLE f = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, line, n, &written, nullptr);
    CloseHandle(f);
}

// --- JSON -----------------------------------------------------------------------

std::string JsonValue(const std::string& s, const char* key) {
    std::string quoted = std::string("\"") + key + "\"";
    size_t p = s.find(quoted);
    if (p == std::string::npos) return {};
    p += quoted.size();
    auto skipSpace = [&] {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
    };
    skipSpace();
    if (p >= s.size() || s[p] != ':') return {};
    p++;
    skipSpace();
    if (p < s.size() && s[p] == '"') {
        std::string out;
        for (p++; p < s.size() && s[p] != '"'; p++) {
            if (s[p] == '\\' && p + 1 < s.size()) p++;
            out += s[p];
        }
        return out;
    }
    size_t end = s.find_first_of(",}] \r\n", p);
    return s.substr(p, end == std::string::npos ? std::string::npos : end - p);
}

// --- UsageOne -------------------------------------------------------------------

bool Query(const char* command, Status* out) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 3 && pipe == INVALID_HANDLE_VALUE; i++) {
        pipe = CreateFileW(STATUS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        bool busy = pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY;
        if (pipe == INVALID_HANDLE_VALUE && (!busy || !WaitNamedPipeW(STATUS_PIPE_NAME, 1000))) return false;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;

    std::string request = std::string(command) + "\n", reply;
    DWORD n = 0;
    if (WriteFile(pipe, request.data(), (DWORD)request.size(), &n, nullptr)) {
        char buf[1024];
        while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n) {
            reply.append(buf, n);
            if (reply.find('\n') != std::string::npos) break;
        }
    }
    CloseHandle(pipe);
    if (reply.empty()) return false;

    auto number = [&](const char* key, long long fallback) {
        std::string v = JsonValue(reply, key);
        return v.empty() ? fallback : _atoi64(v.c_str());
    };
    Status s;
    s.running = true;
    s.status = JsonValue(reply, "status");
    s.probing = JsonValue(reply, "probing") == "true";
    s.session = (int)number("session", -1);
    s.week = (int)number("week", -1);
    s.sessionReset = number("sessionReset", 0);
    s.weekReset = number("weekReset", 0);
    *out = s;
    return true;
}

bool StartUsageOne() {
    wchar_t exe[MAX_PATH] = {};
    DWORD size = sizeof(exe);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\" APP_NAME, L"AppPath", RRF_RT_REG_SZ, nullptr, exe, &size) !=
            ERROR_SUCCESS ||
        GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES)
        return false;
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L'\\'));
    Log("starting UsageOne");
    return (INT_PTR)ShellExecuteW(nullptr, L"open", exe, L"/background", dir.c_str(), SW_SHOWNORMAL) > 32;
}

// --- Claude -------------------------------------------------------------------

void OpenClaude() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // ShellExecute wants STA
    // The Claude desktop app's id in the Microsoft Store package (the same on every PC).
    INT_PTR r = (INT_PTR)ShellExecuteW(nullptr, L"open", L"shell:AppsFolder\\Claude_pzs8sxrjxfjjc!Claude", nullptr,
                                       nullptr, SW_SHOWNORMAL);
    if (r <= 32) r = (INT_PTR)ShellExecuteW(nullptr, L"open", L"https://claude.ai", nullptr, nullptr, SW_SHOWNORMAL);
    Log("long press: opening Claude (%s)", r > 32 ? "ok" : "failed");
}

}  // namespace Plugin
