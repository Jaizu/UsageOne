// Entry point of UsageOne.StreamDeck.exe.
//   (from Stream Deck)  -port N -pluginUUID X -registerEvent Y -info JSON
//   /install            install or update the plugin in Stream Deck
//   /make-images <dir>  write the manifest's PNGs

#include "plugin.h"

#include <shellapi.h>

static std::string Utf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 2 && wcscmp(argv[1], L"/install") == 0) return Plugin::Install();
    if (argc >= 3 && wcscmp(argv[1], L"/make-images") == 0) {
        Plugin::KeyPainter painter;
        return painter.Init() && painter.WriteManifestImages(argv[2]) ? 0 : 1;
    }

    int port = 0;
    std::string uuid, registerEvent;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (wcscmp(argv[i], L"-port") == 0) port = _wtoi(argv[i + 1]);
        else if (wcscmp(argv[i], L"-pluginUUID") == 0) uuid = Utf8(argv[i + 1]);
        else if (wcscmp(argv[i], L"-registerEvent") == 0) registerEvent = Utf8(argv[i + 1]);
    }
    if (!port || uuid.empty() || registerEvent.empty()) {
        MessageBoxW(nullptr, L"This program is started by the Stream Deck app. Install it from UsageOne's Settings.",
                    APP_NAME, MB_ICONINFORMATION);
        return 1;
    }
    return Plugin::Run(port, uuid, registerEvent);
}
