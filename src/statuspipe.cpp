#include "statuspipe.h"
#include "log.h"

namespace StatusPipe {

static HWND g_host;
static SRWLOCK g_lock = SRWLOCK_INIT;
static std::string g_json;  // UTF-8, with the newline

static std::string Utf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static long long UnixSeconds(ULONGLONG ticks) {
    const ULONGLONG kUnixEpoch = 116444736000000000ULL;  // 1970-01-01 in FILETIME ticks
    return ticks ? (long long)((ticks - kUnixEpoch) / Usage::kSecond) : 0;
}

void Publish(const UsageState& state) {
    static const char* kStatus[] = {"none", "ok", "noClaude", "notLoggedIn", "noLimits", "failed"};
    int session = -1, week = -1;
    long long sessionReset = 0, weekReset = 0;
    for (const Limit& l : state.data.limits) {
        if (l.key == L"session") session = l.percent, sessionReset = UnixSeconds(l.resetAt);
        if (l.key == L"all models") week = l.percent, weekReset = UnixSeconds(l.resetAt);
    }
    std::string plan;
    for (char c : Utf8(state.data.plan.name))
        if (c != '"' && c != '\\') plan += c;

    char json[512];
    sprintf_s(json,
              "{\"status\":\"%s\",\"probing\":%s,\"plan\":\"%s\",\"session\":%d,\"sessionReset\":%lld,"
              "\"week\":%d,\"weekReset\":%lld}\n",
              kStatus[state.last], state.probing ? "true" : "false", plan.c_str(), session, sessionReset, week,
              weekReset);
    AcquireSRWLockExclusive(&g_lock);
    g_json = json;
    ReleaseSRWLockExclusive(&g_lock);
}

// One client at a time, one request per connection: a line in, a line out.
static DWORD WINAPI Serve(void*) {
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(STATUS_PIPE_NAME, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                       4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LOG(L"pipe: CreateNamedPipe failed (%lu)", GetLastError());
            return 1;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char request[64] = {};
            DWORD got = 0, total = 0;
            while (total < sizeof(request) - 1 &&
                   ReadFile(pipe, request + total, sizeof(request) - 1 - total, &got, nullptr) && got) {
                total += got;
                if (memchr(request, '\n', total)) break;
            }
            if (strncmp(request, "refresh", 7) == 0) PostMessageW(g_host, WM_APP_REFRESH, 0, 0);

            AcquireSRWLockShared(&g_lock);
            std::string reply = g_json;
            ReleaseSRWLockShared(&g_lock);
            DWORD written = 0;
            WriteFile(pipe, reply.data(), (DWORD)reply.size(), &written, nullptr);
            FlushFileBuffers(pipe);
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}

void Start(HWND host) {
    g_host = host;
    if (HANDLE h = CreateThread(nullptr, 0, Serve, nullptr, 0, nullptr)) CloseHandle(h);
}

}  // namespace StatusPipe
