#include "streamdeck.h"
#include "log.h"

#include <shlobj.h>

namespace StreamDeck {

enum { kPluginExeResource = 2, kPluginManifestResource = 3 };  // res\app.rc

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void ReadState(PluginState& state) {
    std::wstring data = KnownFolder(FOLDERID_RoamingAppData) + L"\\Elgato\\StreamDeck";
    state.streamDeck = Exists(data) || Exists(KnownFolder(FOLDERID_ProgramFiles) + L"\\Elgato\\StreamDeck\\StreamDeck.exe");

    // The installed manifest's "Version": "1.0.0.0" -> "1.0.0".
    state.installed.clear();
    std::wstring manifest = data + L"\\Plugins\\" PLUGIN_UUID L".sdPlugin\\manifest.json";
    HANDLE f = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char text[8192];
    DWORD n = 0;
    ReadFile(f, text, sizeof(text) - 1, &n, nullptr);
    CloseHandle(f);
    text[n] = 0;
    const char* v = strstr(text, "\"Version\"");
    if (v) v = strchr(v + 9, ':');
    if (v) v = strchr(v, '"');
    int major = 0, minor = 0, patch = 0;
    if (v && sscanf_s(v + 1, "%d.%d.%d", &major, &minor, &patch) == 3)
        state.installed = std::to_wstring(major) + L"." + std::to_wstring(minor) + L"." + std::to_wstring(patch);
}

static bool ExtractResource(int id, const std::wstring& path) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = res ? LoadResource(nullptr, res) : nullptr;
    const void* bytes = data ? LockResource(data) : nullptr;
    if (!bytes) return false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = SizeofResource(nullptr, res), written = 0;
    bool ok = WriteFile(f, bytes, size, &written, nullptr) && written == size;
    CloseHandle(f);
    return ok;
}

bool InstallPlugin() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + APP_NAME L"Plugin";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring exe = dir + L"\\UsageOne.StreamDeck.exe";
    if (!ExtractResource(kPluginExeResource, exe) || !ExtractResource(kPluginManifestResource, dir + L"\\manifest.json")) {
        LOG(L"stream deck: could not write the plugin to %s", dir.c_str());
        return false;
    }

    std::wstring cmd = L"\"" + exe + L"\" /install";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
    DWORD code = 1;
    bool finished = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return finished && code == 0;
}

}  // namespace StreamDeck
