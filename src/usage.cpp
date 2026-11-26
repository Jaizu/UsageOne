#include "usage.h"
#include "log.h"

#include <cwctype>

namespace Usage {

ULONGLONG NowTicks() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

static ULONGLONG ToTicks(const FILETIME& ft) { return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

static bool LocalToUtcTicks(const SYSTEMTIME& local, ULONGLONG* out) {
    SYSTEMTIME utc;
    FILETIME ft;
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return false;
    *out = ToTicks(ft);
    return true;
}

static bool UtcTicksToLocal(ULONGLONG t, SYSTEMTIME* local) {
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME utc;
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, local);
}

static bool StartsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// ---------------------------------------------------------------------------
// Running Claude Code

// claude.exe from PATH or the native installer's folder; claude.cmd (npm) as a
// last resort, run through cmd.exe.
static std::wstring FindClaude(bool* viaCmd) {
    *viaCmd = false;
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"claude.exe", nullptr, MAX_PATH, buf, nullptr)) return buf;
    wchar_t home[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH)) {
        std::wstring p = std::wstring(home) + L"\\.local\\bin\\claude.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    if (SearchPathW(nullptr, L"claude.cmd", nullptr, MAX_PATH, buf, nullptr)) {
        *viaCmd = true;
        return buf;
    }
    return {};
}

// Our environment minus Claude Code's and the Anthropic API's variables: if
// UsageOne was started from inside a Claude session, they point Claude Code at
// that session (and /usage then leaves out the plan limits); an API key would
// make it bill the API instead of the subscription.
static std::vector<wchar_t> CleanEnvironment() {
    std::vector<wchar_t> block;
    wchar_t* env = GetEnvironmentStringsW();
    for (const wchar_t* p = env; p && *p; p += wcslen(p) + 1) {
        std::wstring entry = p;
        std::wstring name = Lower(entry.substr(0, entry.find(L'=', 1)));
        if (StartsWith(name, L"claude") || StartsWith(name, L"anthropic_")) continue;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(0);
    }
    if (env) FreeEnvironmentStringsW(env);
    block.push_back(0);
    return block;
}

static bool RunClaude(std::string* output, std::wstring* error) {
    bool viaCmd = false;
    std::wstring exe = FindClaude(&viaCmd);
    if (exe.empty()) {
        *error = L"claude not found";
        return false;
    }
    std::wstring args = L"-p /usage --output-format text --no-session-persistence";
    std::wstring app, cmd;
    if (viaCmd) {
        wchar_t comspec[MAX_PATH] = L"cmd.exe";
        GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH);
        app = comspec;
        cmd = L"cmd.exe /d /s /c \"\"" + exe + L"\" " + args + L"\"";
    } else {
        app = exe;
        cmd = L"\"" + exe + L"\" " + args;
    }

    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE outRead = nullptr, outWrite = nullptr, inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) || !CreatePipe(&inRead, &inWrite, &sa, 0)) {
        *error = L"CreatePipe failed";
        return false;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;  // closed at once: an empty stdin, so -p doesn't wait for input
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> env = CleanEnvironment();
    wchar_t home[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    BOOL ok = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                             env.data(), home[0] ? home : nullptr, &si, &pi);
    DWORD createError = GetLastError();
    CloseHandle(outWrite);
    CloseHandle(inRead);
    CloseHandle(inWrite);
    if (!ok) {
        CloseHandle(outRead);
        *error = L"CreateProcess failed (" + std::to_wstring(createError) + L")";
        return false;
    }

    // Read until the process ends, polling so a hung claude can't hold us forever.
    ULONGLONG start = GetTickCount64();
    bool exited = false, timedOut = false;
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(outRead, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD got = 0;
            if (ReadFile(outRead, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) && got) output->append(buf, got);
            continue;
        }
        if (exited) break;  // drained after exit
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            exited = true;
            continue;
        }
        if (GetTickCount64() - start > 60000) {
            TerminateProcess(pi.hProcess, 1);
            timedOut = true;
            break;
        }
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(outRead);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (timedOut) {
        *error = L"claude timed out";
        return false;
    }
    LOG(L"probe: claude exited %lu after %llu ms, %zu bytes", code, GetTickCount64() - start, output->size());
    return true;
}

// ---------------------------------------------------------------------------
// Parsing /usage

static const wchar_t* const kMonths[] = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
                                         L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};

// "Nov 26, 3:29pm (Europe/Madrid)", "Sep 21, 6am", "Oct 17 at 2:29pm" or just
// "3:29pm": a local time, this year (or next, around New Year).
static ULONGLONG ParseReset(std::wstring s) {
    size_t paren = s.find(L'(');
    if (paren != std::wstring::npos) s.resize(paren);
    for (auto& c : s)
        if (c == L',') c = L' ';
    std::vector<std::wstring> tokens;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && iswspace(s[i])) i++;
        size_t j = i;
        while (j < s.size() && !iswspace(s[j])) j++;
        if (j > i) {
            std::wstring t = Lower(s.substr(i, j - i));
            if (t != L"at") tokens.push_back(t);
        }
        i = j;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    SYSTEMTIME t = now;
    t.wSecond = t.wMilliseconds = 0;
    size_t k = 0;
    bool dated = false;
    if (tokens.size() >= 2 && tokens[0].size() >= 3) {
        for (int m = 0; m < 12; m++) {
            if (tokens[0].compare(0, 3, kMonths[m]) == 0) {
                t.wMonth = (WORD)(m + 1);
                t.wDay = (WORD)_wtoi(tokens[1].c_str());
                k = 2;
                dated = true;
                break;
            }
        }
    }
    if (k >= tokens.size()) return 0;
    std::wstring time;
    for (; k < tokens.size(); k++) time += tokens[k];  // "3:29pm" or "3:29" + "pm"
    int hour = 0, minute = 0;
    const wchar_t* p = time.c_str();
    hour = (int)wcstol(p, (wchar_t**)&p, 10);
    if (*p == L':') minute = (int)wcstol(p + 1, (wchar_t**)&p, 10);
    if (StartsWith(p, L"pm") && hour < 12) hour += 12;
    if (StartsWith(p, L"am") && hour == 12) hour = 0;
    if (hour > 23 || minute > 59 || t.wDay < 1 || t.wDay > 31) return 0;
    t.wHour = (WORD)hour;
    t.wMinute = (WORD)minute;

    ULONGLONG ticks = 0;
    if (!LocalToUtcTicks(t, &ticks)) return 0;
    ULONGLONG nowTicks = NowTicks();
    if (dated && ticks + 30 * kDay < nowTicks) {  // December stamp seen in January, etc.
        t.wYear++;
        LocalToUtcTicks(t, &ticks);
    } else if (!dated && ticks < nowTicks) {
        ticks += kDay;
    }
    return ticks;
}

UsageData Parse(const std::wstring& text) {
    UsageData d;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        size_t a = line.find_first_not_of(L" \t\r");
        if (a == std::wstring::npos) continue;
        line = line.substr(a);

        Limit l;
        std::wstring rest;
        if (StartsWith(line, L"Current session:")) {
            l.key = L"session";
            rest = line.substr(16);
        } else if (StartsWith(line, L"Current week (")) {
            size_t close = line.find(L"):");
            if (close == std::wstring::npos) continue;
            l.key = line.substr(14, close - 14);
            rest = line.substr(close + 2);
        } else {
            continue;
        }
        const wchar_t* p = rest.c_str();
        while (*p == L' ') p++;
        wchar_t* after = nullptr;
        long pct = wcstol(p, &after, 10);
        if (after == p || *after != L'%') continue;
        l.percent = (int)std::clamp(pct, 0L, 100L);
        size_t r = rest.find(L"resets ");
        if (r != std::wstring::npos) l.resetAt = ParseReset(rest.substr(r + 7));
        d.limits.push_back(l);
    }

    if (!d.limits.empty()) {
        d.status = UsageData::Ok;
        return d;
    }
    std::wstring low = Lower(text);
    if (low.find(L"not logged in") != std::wstring::npos || low.find(L"/login") != std::wstring::npos ||
        low.find(L"please log in") != std::wstring::npos || low.find(L"invalid api key") != std::wstring::npos)
        d.status = UsageData::NotLoggedIn;
    else
        d.status = low.find_first_not_of(L" \t\r\n") == std::wstring::npos ? UsageData::Failed : UsageData::NoLimits;
    size_t a = text.find_first_not_of(L" \t\r\n");
    d.detail = a == std::wstring::npos ? L"(no output)" : text.substr(a, std::min<size_t>(200, text.size() - a));
    return d;
}

// ---------------------------------------------------------------------------
// Plan, from the profile Claude Code caches in .claude.json

static std::wstring JsonString(const std::string& json, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return {};
    p = json.find_first_not_of(" \t\r\n", p + k.size());
    if (p == std::string::npos || json[p] != ':') return {};
    p = json.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || json[p] != '"') return {};
    std::string v;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) p++;
        v += json[p];
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, v.data(), (int)v.size(), w.data(), n);
    return w;
}

static ULONGLONG ParseIso(const std::wstring& s) {
    SYSTEMTIME t = {};
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (swscanf_s(s.c_str(), L"%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    t.wYear = (WORD)y;
    t.wMonth = (WORD)mo;
    t.wDay = (WORD)d;
    t.wHour = (WORD)h;
    t.wMinute = (WORD)mi;
    t.wSecond = (WORD)se;
    FILETIME ft;
    return SystemTimeToFileTime(&t, &ft) ? ToTicks(ft) : 0;
}

static int DaysInMonth(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

// The first monthly anniversary of `since` that is still ahead.
static ULONGLONG NextPeriodEnd(ULONGLONG since) {
    SYSTEMTIME start;
    if (!since || !UtcTicksToLocal(since, &start)) return 0;
    ULONGLONG now = NowTicks();
    for (int n = 1; n < 1200; n++) {
        SYSTEMTIME t = start;
        int m = start.wMonth - 1 + n;
        t.wYear = (WORD)(start.wYear + m / 12);
        t.wMonth = (WORD)(m % 12 + 1);
        t.wDay = (WORD)std::min<int>(start.wDay, DaysInMonth(t.wYear, t.wMonth));
        t.wDayOfWeek = 0;
        ULONGLONG ticks;
        if (LocalToUtcTicks(t, &ticks) && ticks > now) return ticks;
    }
    return 0;
}

static std::wstring PlanName(const std::wstring& type, const std::wstring& tier) {
    std::wstring t = Lower(tier), o = Lower(type);
    if (t.find(L"max_20x") != std::wstring::npos) return L"Max 20x";
    if (t.find(L"max_5x") != std::wstring::npos) return L"Max 5x";
    if (o == L"claude_max") return L"Max";
    if (o == L"claude_pro") return L"Pro";
    if (o == L"claude_team") return L"Team";
    if (o == L"claude_enterprise") return L"Enterprise";
    if (o.empty()) return {};
    return L"Free";
}

Plan ReadPlan() {
    Plan plan;
    wchar_t dir[MAX_PATH];
    std::wstring path;
    if (GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", dir, MAX_PATH) && dir[0])
        path = std::wstring(dir) + L"\\.claude.json";
    else if (GetEnvironmentVariableW(L"USERPROFILE", dir, MAX_PATH))
        path = std::wstring(dir) + L"\\.claude.json";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return plan;
    std::string json;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024 * 1024) {
        json.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, json.data(), (DWORD)json.size(), &got, nullptr)) got = 0;
        json.resize(got);
    }
    CloseHandle(f);
    size_t account = json.find("\"oauthAccount\"");
    if (account == std::string::npos) return plan;
    json = json.substr(account);
    plan.name = PlanName(JsonString(json, "organizationType"), JsonString(json, "organizationRateLimitTier"));
    plan.since = ParseIso(JsonString(json, "subscriptionCreatedAt"));
    plan.periodEnd = NextPeriodEnd(plan.since);
    return plan;
}

// ---------------------------------------------------------------------------

UsageData Probe() {
    UsageData d;
    std::string out;
    std::wstring error;
    if (!RunClaude(&out, &error)) {
        d.status = error == L"claude not found" ? UsageData::NoClaude : UsageData::Failed;
        d.detail = error;
    } else {
        int n = MultiByteToWideChar(CP_UTF8, 0, out.data(), (int)out.size(), nullptr, 0);
        std::wstring text(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, out.data(), (int)out.size(), text.data(), n);
        d = Parse(text);
    }
    d.plan = ReadPlan();
    if (d.status == UsageData::Ok) d.fetchedAt = NowTicks();
    LOG(L"probe: status %d, %zu limits, plan '%s'%s%s", (int)d.status, d.limits.size(), d.plan.name.c_str(),
        d.detail.empty() ? L"" : L", detail: ", d.detail.c_str());
    return d;
}

}  // namespace Usage
