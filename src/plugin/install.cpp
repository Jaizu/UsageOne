// Installing the plugin into Stream Deck: UsageOne runs `UsageOne.StreamDeck.exe
// /install` from a temp folder (both files come out of UsageOne.exe's
// resources). Stream Deck is closed while the files are replaced, then started
// again so it loads the plugin.

#include "plugin.h"

#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

namespace Plugin {

// Processes with this exe name (not this one); `path` gets the first one's exe.
static std::vector<DWORD> FindProcesses(const wchar_t* name, std::wstring* path = nullptr) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = {sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, name) != 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
        pids.push_back(pe.th32ProcessID);
        if (path && path->empty()) {
            if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
                wchar_t buf[MAX_PATH];
                DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(h, 0, buf, &n)) *path = buf;
                CloseHandle(h);
            }
        }
    }
    CloseHandle(snap);
    return pids;
}

static void StopProcesses(const std::vector<DWORD>& pids) {
    for (DWORD pid : pids) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
}

static std::string ReadText(const std::wstring& path) {
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return text;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) text.append(buf, n);
    CloseHandle(f);
    return text;
}

static bool WriteText(const std::wstring& path, const std::string& text) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &written, nullptr) && written == text.size();
    CloseHandle(f);
    return ok;
}

// The manifest with this build's version ("1.2.3" -> "1.2.3.0"), so the plugin
// always carries the version of the UsageOne it came with.
static std::string StampVersion(std::string manifest) {
    size_t key = manifest.find("\"Version\"");
    size_t open = key == std::string::npos ? key : manifest.find('"', manifest.find(':', key));
    size_t close = open == std::string::npos ? open : manifest.find('"', open + 1);
    if (close != std::string::npos) manifest.replace(open + 1, close - open - 1, APP_VERSION_A ".0");
    return manifest;
}

int Install() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring here = self;
    here.resize(here.find_last_of(L'\\'));
    std::string manifest = ReadText(here + L"\\manifest.json");
    if (manifest.empty()) manifest = ReadText(here + L"\\..\\streamdeck\\manifest.json");  // running from the repo
    if (manifest.empty()) {
        Log("install: manifest.json not found next to %ls", self);
        return 1;
    }

    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return 1;
    std::wstring dest = std::wstring(roaming) + L"\\Elgato\\StreamDeck\\Plugins\\" PLUGIN_UUID L".sdPlugin";
    CoTaskMemFree(roaming);

    // Stream Deck keeps the plugin's exe open, and only reads new plugins at start.
    std::wstring streamDeck;
    std::vector<DWORD> running = FindProcesses(L"StreamDeck.exe", &streamDeck);
    StopProcesses(running);
    StopProcesses(FindProcesses(L"UsageOne.StreamDeck.exe"));

    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    KeyPainter painter;
    bool ok = CopyFileW(self, (dest + L"\\UsageOne.StreamDeck.exe").c_str(), FALSE) &&
              WriteText(dest + L"\\manifest.json", StampVersion(manifest)) && painter.Init() &&
              painter.WriteManifestImages(dest + L"\\imgs");
    Log("install %s to %ls: %s", APP_VERSION_A, dest.c_str(), ok ? "ok" : "FAILED");

    if (!running.empty() && !streamDeck.empty()) {
        std::wstring dir = streamDeck.substr(0, streamDeck.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", streamDeck.c_str(), nullptr, dir.c_str(), SW_SHOWMINNOACTIVE);
    }
    return ok ? 0 : 1;
}

}  // namespace Plugin
