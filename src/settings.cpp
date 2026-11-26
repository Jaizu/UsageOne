#include "settings.h"

static const wchar_t kKey[] = L"Software\\" APP_NAME;
static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static DWORD ReadDword(const wchar_t* name, DWORD fallback) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return fallback;
    return v;
}

static void WriteDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kKey, name, REG_DWORD, &v, sizeof(v));
}

void Settings::Load() {
    showPeak = ReadDword(L"ShowPeak", 1) != 0;
    starIcon = ReadDword(L"StarIcon", 0) != 0;
    showModelWeeks = ReadDword(L"ShowModelWeeks", 1) != 0;
    barColor = std::min<DWORD>(ReadDword(L"BarColor", BarAccent), BarCustom);
    barCustomColor = ReadDword(L"BarCustomColor", 0xD97757) & 0xFFFFFF;
    refreshMinutes = (int)std::clamp<DWORD>(ReadDword(L"RefreshMinutes", 5), 1, 60);
}

void Settings::Save() const {
    WriteDword(L"ShowPeak", showPeak);
    WriteDword(L"StarIcon", starIcon);
    WriteDword(L"ShowModelWeeks", showModelWeeks);
    WriteDword(L"BarColor", barColor);
    WriteDword(L"BarCustomColor", barCustomColor);
    WriteDword(L"RefreshMinutes", refreshMinutes);
}

static std::wstring RunCommand() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return L"\"" + std::wstring(exe) + L"\" /background";
}

bool GetStartWithWindows() {
    wchar_t buf[MAX_PATH * 2];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, APP_NAME, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return false;
    return _wcsicmp(buf, RunCommand().c_str()) == 0;
}

void SetStartWithWindows(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        std::wstring cmd = RunCommand();
        RegSetValueExW(key, APP_NAME, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }
    RegCloseKey(key);
}
