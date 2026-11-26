// UsageOne's Stream Deck plugin: a key with the session and weekly bars,
// drawn like the panel (same fonts, colours and bar colour setting). The
// numbers come from the running UsageOne through its pipe; pressing the key
// asks for fresh ones, starting UsageOne first if it isn't running.
//
// Stream Deck starts this exe with: -port N -pluginUUID X -registerEvent Y -info JSON
// UsageOne.StreamDeck.exe /make-images <folder> writes the manifest's PNGs.

#include "common.h"
#include "settings.h"
#include "trayicon.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <ctime>
#include <functional>
#include <mutex>
#include <set>
#include <thread>

namespace {

const int kKeySize = 144;  // Stream Deck keys are 72x72, 144x144 on high-DPI models
const auto kPoll = std::chrono::seconds(5);
const auto kPollBusy = std::chrono::seconds(1);  // while UsageOne is reading new numbers

// --- Log: %LOCALAPPDATA%\UsageOne\StreamDeck.log -------------------------------

std::mutex g_logLock;
std::wstring g_logPath;

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logLock);
    if (g_logPath.empty()) {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) return;
        std::wstring dir = std::wstring(local) + L"\\" APP_NAME;
        CoTaskMemFree(local);
        CreateDirectoryW(dir.c_str(), nullptr);
        g_logPath = dir + L"\\StreamDeck.log";
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
    DWORD w;
    WriteFile(f, line, n, &w, nullptr);
    CloseHandle(f);
}

// --- Tiny JSON reading: enough for Stream Deck's events and UsageOne's line ---

std::string JsonValue(const std::string& s, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return {};
    p += k.size();
    auto skip = [&] {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
    };
    skip();
    if (p >= s.size() || s[p] != ':') return {};
    p++;
    skip();
    if (p < s.size() && s[p] == '"') {
        std::string out;
        for (p++; p < s.size() && s[p] != '"'; p++) {
            if (s[p] == '\\' && p + 1 < s.size()) p++;
            out += s[p];
        }
        return out;
    }
    size_t e = s.find_first_of(",}] \r\n", p);
    return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// --- UsageOne -------------------------------------------------------------------

struct Status {
    bool running = false;  // UsageOne answered
    std::string status;    // none, ok, noClaude, notLoggedIn, noLimits, failed
    bool probing = false;
    int session = -1, week = -1;  // -1: not reported
    long long sessionReset = 0, weekReset = 0;  // Unix seconds, 0: window not started
};

// Sends "status" or "refresh" to UsageOne's pipe and reads its one-line answer.
bool Query(const char* command, Status* out) {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 3 && pipe == INVALID_HANDLE_VALUE; i++) {
        pipe = CreateFileW(STATUS_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE && (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeW(STATUS_PIPE_NAME, 1000)))
            return false;
    }
    if (pipe == INVALID_HANDLE_VALUE) return false;
    std::string req = std::string(command) + "\n";
    DWORD n = 0;
    std::string reply;
    if (WriteFile(pipe, req.data(), (DWORD)req.size(), &n, nullptr)) {
        char buf[1024];
        while (ReadFile(pipe, buf, sizeof(buf), &n, nullptr) && n) {
            reply.append(buf, n);
            if (reply.find('\n') != std::string::npos) break;
        }
    }
    CloseHandle(pipe);
    if (reply.empty()) return false;
    Status s;
    s.running = true;
    s.status = JsonValue(reply, "status");
    s.probing = JsonValue(reply, "probing") == "true";
    s.session = atoi(JsonValue(reply, "session").c_str());
    s.week = atoi(JsonValue(reply, "week").c_str());
    s.sessionReset = _atoi64(JsonValue(reply, "sessionReset").c_str());
    s.weekReset = _atoi64(JsonValue(reply, "weekReset").c_str());
    if (JsonValue(reply, "session").empty()) s.session = -1;
    if (JsonValue(reply, "week").empty()) s.week = -1;
    *out = s;
    return true;
}

// Starts UsageOne in the background, from where it last ran.
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

// Holding the key: the Claude desktop app (brought forward if it's already
// open), or claude.ai when it isn't installed.
const DWORD kLongPressMs = 500;

void OpenClaude() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // ShellExecute wants STA
    INT_PTR r = (INT_PTR)ShellExecuteW(nullptr, L"open", L"shell:AppsFolder\\Claude_pzs8sxrjxfjjc!Claude", nullptr,
                                       nullptr, SW_SHOWNORMAL);
    if (r <= 32) r = (INT_PTR)ShellExecuteW(nullptr, L"open", L"https://claude.ai", nullptr, nullptr, SW_SHOWNORMAL);
    Log("long press: opening Claude (%s)", r > 32 ? "ok" : "failed");
}

// --- Text ---------------------------------------------------------------------

struct Texts {
    const wchar_t *closed, *loading, *signIn, *noClaude, *noData;
};
const Texts kEs = {L"Pulsa para\nabrir UsageOne", L"Cargando…", L"Inicia sesión\nen Claude Code",
                   L"Falta\nClaude Code", L"Sin datos"};
const Texts kEn = {L"Press to\nstart UsageOne", L"Loading…", L"Sign in to\nClaude Code", L"Claude Code\nnot found",
                   L"No data"};
const Texts& T() {
    static const Texts* t = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? &kEs : &kEn;
    return *t;
}

// "40m", "3h 12m", "1d 6h".
std::wstring Short(long long seconds) {
    long long m = std::max(0LL, seconds) / 60;
    wchar_t buf[32];
    if (m < 60)
        swprintf_s(buf, L"%lldm", std::max(1LL, m));
    else if (m < 24 * 60)
        swprintf_s(buf, L"%lldh %lldm", m / 60, m % 60);
    else
        swprintf_s(buf, L"%lldd %lldh", m / (24 * 60), (m / 60) % 24);
    return buf;
}

// --- Drawing --------------------------------------------------------------------

D2D1_COLOR_F Rgba(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// The panel's dark palette: Stream Deck keys are always on black.
struct Palette {
    D2D1_COLOR_F background = Rgba(0x202020), border = Rgba(0xFFFFFF, 0.10f);
    D2D1_COLOR_F text = Rgba(0xFFFFFF), textSecondary = Rgba(0xFFFFFF, 0.786f), textTertiary = Rgba(0xFFFFFF, 0.545f);
    D2D1_COLOR_F track = Rgba(0xFFFFFF, 0.10f);
    D2D1_COLOR_F accent = Rgba(0x99EBFF), warn = Rgba(0xFCE100), danger = Rgba(0xFF99A4);
    D2D1_COLOR_F bar;  // below 60 %, from the bar colour setting

    void Load() {
        BYTE shades[32];
        DWORD size = sizeof(shades);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                         L"AccentPalette", RRF_RT_REG_BINARY, nullptr, shades, &size) == ERROR_SUCCESS &&
            size >= 28)
            accent = D2D1::ColorF(shades[4] / 255.0f, shades[5] / 255.0f, shades[6] / 255.0f, 1.0f);  // Light2
        Settings s;
        s.Load();
        switch (s.barColor) {
            case Settings::BarWhite: bar = Rgba(0xFFFFFF); break;
            case Settings::BarGreen: bar = Rgba(0x3CC83C); break;
            case Settings::BarCustom: bar = Rgba(s.barCustomColor); break;
            default: bar = accent; break;
        }
    }
    D2D1_COLOR_F Level(int percent) const { return percent >= 85 ? danger : percent >= 60 ? warn : bar; }
};

class Painter {
public:
    bool Init() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       (IUnknown**)dwrite_.GetAddressOf())) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic_))))
            return false;
        return true;
    }

    // Draws at `size` px (the layout is in 144 units) and returns a PNG.
    std::vector<BYTE> Png(int size, const std::function<void()>& draw) {
        std::vector<BYTE> png;
        ComPtr<IWICBitmap> bmp;
        if (FAILED(wic_->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return png;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  96, 96);
        if (FAILED(d2d_->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt_))) return png;
        rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush_);
        rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        rt_->BeginDraw();
        rt_->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt_->SetTransform(D2D1::Matrix3x2F::Scale(size / (float)kKeySize, size / (float)kKeySize));
        draw();
        HRESULT hr = rt_->EndDraw();
        brush_.Reset();
        rt_.Reset();
        if (FAILED(hr)) return png;

        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(nullptr, 0));
        ComPtr<IWICBitmapEncoder> enc;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IWICFormatConverter> conv;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (!stream || FAILED(wic_->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
            FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(enc->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
            FAILED(frame->SetSize(size, size)) || FAILED(frame->SetPixelFormat(&fmt)) ||
            FAILED(wic_->CreateFormatConverter(&conv)) ||
            FAILED(conv->Initialize(bmp.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                    WICBitmapPaletteTypeCustom)) ||
            FAILED(frame->WriteSource(conv.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(enc->Commit()))
            return png;
        STATSTG st = {};
        stream->Stat(&st, STATFLAG_NONAME);
        png.resize((size_t)st.cbSize.QuadPart);
        LARGE_INTEGER zero = {};
        stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        stream->Read(png.data(), (ULONG)png.size(), &read);
        png.resize(read);
        return png;
    }

    // --- Pieces, in 144 units ---

    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0) {
        brush_->SetColor(c);
        if (radius > 0)
            rt_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
        else
            rt_->FillRectangle(r, brush_.Get());
    }

    void Background(const Palette& p) {
        Fill({0, 0, (float)kKeySize, (float)kKeySize}, p.background);
        brush_->SetColor(p.border);
        rt_->DrawRoundedRectangle(D2D1::RoundedRect({1.5f, 1.5f, kKeySize - 1.5f, kKeySize - 1.5f}, 18, 18),
                                  brush_.Get(), 2.0f);
    }

    ComPtr<IDWriteTextFormat> Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        ComPtr<IDWriteTextFormat> f;
        if (FAILED(dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                             size, L"", &f)))
            return nullptr;
        f->SetTextAlignment(align);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return f;
    }

    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
        if (!f) return;
        brush_->SetColor(c);
        rt_->DrawText(s.c_str(), (UINT32)s.size(), f, r, brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    float Width(const std::wstring& s, IDWriteTextFormat* f) {
        ComPtr<IDWriteTextLayout> layout;
        if (!f || FAILED(dwrite_->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 100, &layout))) return 0;
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        return m.widthIncludingTrailingWhitespace;
    }

    void Bar(float x0, float x1, float y, float h, int percent, const D2D1_COLOR_F& c, const Palette& p) {
        Fill({x0, y, x1, y + h}, p.track, h / 2);
        float w = (x1 - x0) * std::clamp(percent, 0, 100) / 100.0f;
        if (w > 0) Fill({x0, y, x0 + std::max(w, h), y + h}, c, h / 2);
    }

    // Claude Code's mascot, as in the tray.
    void Mascot(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
        int n = (int)(r.right - r.left) * 2;  // drawn at 2x, then scaled to fit
        std::vector<BYTE> mask = MascotMask(n);
        std::vector<BYTE> px(n * n * 4);
        for (int i = 0; i < n * n; i++) {
            float a = mask[i] / 255.0f * c.a;
            px[i * 4 + 0] = (BYTE)std::lround(c.b * a * 255);
            px[i * 4 + 1] = (BYTE)std::lround(c.g * a * 255);
            px[i * 4 + 2] = (BYTE)std::lround(c.r * a * 255);
            px[i * 4 + 3] = (BYTE)std::lround(a * 255);
        }
        ComPtr<ID2D1Bitmap> bmp;
        auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(rt_->CreateBitmap(D2D1::SizeU(n, n), px.data(), n * 4, props, &bmp)))
            rt_->DrawBitmap(bmp.Get(), r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }

    ComPtr<ID2D1RenderTarget> rt_;
    ComPtr<ID2D1SolidColorBrush> brush_;

private:
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
};

const wchar_t kText[] = L"Segoe UI Variable Text";
const wchar_t kDisplay[] = L"Segoe UI Variable Display";
const wchar_t kIcons[] = L"Segoe Fluent Icons";

// The key. `now` is Unix seconds.
void DrawKey(Painter& g, const Palette& p, const Status& s, long long now) {
    g.Background(p);
    const float x0 = 14, x1 = kKeySize - 14;

    if (!s.running || s.session < 0) {
        // No numbers: the mascot and a line saying why.
        const wchar_t* msg = !s.running                     ? T().closed
                             : s.status == "notLoggedIn"    ? T().signIn
                             : s.status == "noClaude"       ? T().noClaude
                             : s.status == "none" || s.probing ? T().loading
                                                            : T().noData;
        g.Mascot({44, 24, 100, 80}, !s.running ? p.textTertiary : p.text);
        auto f = g.Format(kText, 17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
        g.Text(msg, f.Get(), {6, 84, kKeySize - 6.0f, 134}, p.textSecondary);
        return;
    }

    // Footer: when the session and the week reset, like the panel's reset lines.
    auto footer = [&](float top) {
        std::wstring text = s.sessionReset ? Short(s.sessionReset - now) : L"—";
        if (s.week >= 0) text += L" · " + (s.weekReset ? Short(s.weekReset - now) : std::wstring(L"—"));
        auto f = g.Format(kText, 17, DWRITE_FONT_WEIGHT_NORMAL);
        auto icon = g.Format(kIcons, 14, DWRITE_FONT_WEIGHT_NORMAL);
        float tw = g.Width(text, f.Get()), iw = 14, gap = 5;
        float left = std::round((kKeySize - (iw + gap + tw)) / 2);
        g.Text(std::wstring(1, (wchar_t)0xE72C), icon.Get(), {left, top, left + iw + 2, top + 24},
               s.probing ? p.accent : p.textTertiary);
        g.Text(text, f.Get(), {left + iw + gap, top, left + iw + gap + tw + 2, top + 24}, p.textTertiary);
    };

    // A limit's row: its window on the left, the percentage on the right,
    // the number big with a smaller "%" as in the panel.
    auto row = [&](float top, const wchar_t* label, int percent, float numSize, float labelSize) {
        D2D1_COLOR_F c = p.Level(percent);
        auto fNum = g.Format(kDisplay, numSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto fUnit = g.Format(kDisplay, numSize * 0.55f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto fLabel = g.Format(kText, labelSize, DWRITE_FONT_WEIGHT_NORMAL);
        std::wstring num = std::to_wstring(percent);
        float wNum = g.Width(num, fNum.Get()), wUnit = g.Width(L"%", fUnit.Get());
        float h = numSize * 1.1f, unitTop = top + numSize * 0.36f;
        g.Text(label, fLabel.Get(), {x0, unitTop, x1 - wNum - wUnit, top + h}, p.textSecondary);
        g.Text(num, fNum.Get(), {x1 - wUnit - wNum - 1, top, x1 - wUnit + 2, top + h}, c);
        g.Text(L"%", fUnit.Get(), {x1 - wUnit, unitTop, x1 + 2, top + h}, c);
    };

    if (s.week >= 0) {
        row(8, L"5h", s.session, 36, 20);
        g.Bar(x0, x1, 50, 8, s.session, p.Level(s.session), p);
        row(62, L"7d", s.week, 36, 20);
        g.Bar(x0, x1, 104, 8, s.week, p.Level(s.week), p);
        footer(116);
    } else {
        // Only the session (a plan without a weekly limit): one big number.
        auto fLabel = g.Format(kText, 20, DWRITE_FONT_WEIGHT_NORMAL);
        g.Text(L"5h", fLabel.Get(), {x0, 10, x1, 36}, p.textSecondary);
        D2D1_COLOR_F c = p.Level(s.session);
        auto fNum = g.Format(kDisplay, 56, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto fUnit = g.Format(kDisplay, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring num = std::to_wstring(s.session);
        float wNum = g.Width(num, fNum.Get()), wUnit = g.Width(L"%", fUnit.Get());
        float left = std::round((kKeySize - wNum - wUnit) / 2);
        g.Text(num, fNum.Get(), {left, 32, left + wNum + 2, 96}, c);
        g.Text(L"%", fUnit.Get(), {left + wNum, 52, left + wNum + wUnit + 2, 90}, c);
        g.Bar(x0, x1, 100, 10, s.session, c, p);
        footer(116);
    }
}

std::string DataUrl(const std::vector<BYTE>& png) {
    DWORD n = 0;
    if (png.empty() ||
        !CryptBinaryToStringA(png.data(), (DWORD)png.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n))
        return {};
    std::string b64(n, '\0');
    CryptBinaryToStringA(png.data(), (DWORD)png.size(), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &n);
    b64.resize(n);
    return "data:image/png;base64," + b64;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok;
}

// The manifest's images: the mascot for the action list and the category, a
// sample key for the plugin icon, and "Loading" for the key before we draw.
int MakeImages(const std::wstring& dir) {
    Painter g;
    if (!g.Init()) return 1;
    CreateDirectoryW(dir.c_str(), nullptr);
    Palette p;
    p.Load();
    Status sample;
    sample.running = true;
    sample.status = "ok";
    sample.session = 42;
    sample.week = 18;
    long long now = (long long)time(nullptr);
    sample.sessionReset = now + 2 * 3600 + 15 * 60;
    sample.weekReset = now + 3 * 86400 + 5 * 3600;
    Status loading;
    loading.running = true;
    loading.status = "none";

    struct Img { const wchar_t* name; int size; int kind; };  // 0 mascot, 1 sample key, 2 loading key
    const Img imgs[] = {
        {L"action", 20, 0},  {L"action@2x", 40, 0}, {L"category", 28, 0}, {L"category@2x", 56, 0},
        {L"plugin", 144, 1}, {L"plugin@2x", 288, 1}, {L"key", 72, 2},     {L"key@2x", 144, 2},
    };
    for (const Img& i : imgs) {
        auto png = g.Png(i.size, [&] {
            if (i.kind == 0)
                g.Mascot({0, 0, (float)kKeySize, (float)kKeySize}, Rgba(0xFFFFFF));
            else
                DrawKey(g, p, i.kind == 1 ? sample : loading, now);
        });
        if (!WriteFileBytes(dir + L"\\" + i.name + L".png", png)) return 1;
    }
    return 0;
}

// Installs (or updates) the plugin into Stream Deck's plugin folder: this exe,
// the manifest and the images, then restarts Stream Deck so it loads it.
// Run it outside the Claude desktop app's process tree (tools\install-plugin.ps1
// does), or AppData writes can land in that app's private copy.
std::vector<DWORD> FindProcesses(const wchar_t* name, std::wstring* path = nullptr) {
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

void StopProcesses(const std::vector<DWORD>& pids) {
    for (DWORD pid : pids) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(h, 0);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
}

int Install() {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring exeDir = self;
    exeDir.resize(exeDir.find_last_of(L'\\'));
    std::wstring manifest = exeDir + L"\\manifest.json";
    if (GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES)
        manifest = exeDir + L"\\..\\streamdeck\\manifest.json";

    PWSTR roaming = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) return 1;
    std::wstring dest = std::wstring(roaming) + L"\\Elgato\\StreamDeck\\Plugins\\com.jaizu.usageone.sdPlugin";
    CoTaskMemFree(roaming);

    std::wstring streamDeck;
    std::vector<DWORD> sd = FindProcesses(L"StreamDeck.exe", &streamDeck);
    StopProcesses(sd);
    StopProcesses(FindProcesses(L"UsageOne.StreamDeck.exe"));

    SHCreateDirectoryExW(nullptr, dest.c_str(), nullptr);
    bool ok = CopyFileW(self, (dest + L"\\UsageOne.StreamDeck.exe").c_str(), FALSE) &&
              CopyFileW(manifest.c_str(), (dest + L"\\manifest.json").c_str(), FALSE) &&
              MakeImages(dest + L"\\imgs") == 0;
    Log("install to %ls: %s", dest.c_str(), ok ? "ok" : "FAILED");

    if (!sd.empty() && !streamDeck.empty()) {
        std::wstring dir = streamDeck.substr(0, streamDeck.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", streamDeck.c_str(), nullptr, dir.c_str(), SW_SHOWMINNOACTIVE);
    }
    return ok ? 0 : 1;
}

// --- Stream Deck connection -----------------------------------------------------

class Plugin {
public:
    bool Connect(int port) {
        session_ = WinHttpOpen(L"UsageOne", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) return false;
        connect_ = WinHttpConnect(session_, L"127.0.0.1", (INTERNET_PORT)port, 0);
        if (!connect_) return false;
        HINTERNET req = WinHttpOpenRequest(connect_, L"GET", L"/", nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!req) return false;
        bool ok = WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                  WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(req, nullptr);
        if (ok) ws_ = WinHttpWebSocketCompleteUpgrade(req, 0);
        WinHttpCloseHandle(req);
        return ws_ != nullptr;
    }

    void Send(const std::string& json) {
        std::lock_guard<std::mutex> lock(sendLock_);
        if (ws_)
            WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)json.data(), (DWORD)json.size());
    }

    void Run(const std::string& uuid, const std::string& registerEvent) {
        Send("{\"event\":\"" + registerEvent + "\",\"uuid\":\"" + uuid + "\"}");
        std::thread(&Plugin::Worker, this).detach();

        std::string msg;
        std::vector<char> buf(64 * 1024);
        for (;;) {
            DWORD read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
            if (WinHttpWebSocketReceive(ws_, buf.data(), (DWORD)buf.size(), &read, &type) != NO_ERROR) break;
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) break;
            msg.append(buf.data(), read);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                Handle(msg);
                msg.clear();
            }
        }
        Log("connection closed");
    }

private:
    void Handle(const std::string& msg) {
        std::string ev = JsonValue(msg, "event"), context = JsonValue(msg, "context");
        if (ev == "willAppear") {
            std::string image;
            {
                std::lock_guard<std::mutex> lock(lock_);
                contexts_.insert(context);
                image = lastImage_;
            }
            if (!image.empty()) SetImage(context, image);
            Wake(false);
        } else if (ev == "willDisappear") {
            std::lock_guard<std::mutex> lock(lock_);
            contexts_.erase(context);
        } else if (ev == "keyDown") {
            // A tap refreshes (on release); holding it opens Claude.
            int press;
            {
                std::lock_guard<std::mutex> lock(lock_);
                press = ++press_;
                held_ = true;
                longFired_ = false;
            }
            std::thread([this, press, context] {
                Sleep(kLongPressMs);
                {
                    std::lock_guard<std::mutex> lock(lock_);
                    if (press != press_ || !held_) return;
                    longFired_ = true;
                }
                OpenClaude();
                Send("{\"event\":\"showOk\",\"context\":\"" + context + "\"}");
            }).detach();
        } else if (ev == "keyUp") {
            bool tap;
            {
                std::lock_guard<std::mutex> lock(lock_);
                held_ = false;
                tap = !longFired_;
            }
            if (tap) Wake(true);
        } else if (ev == "systemDidWakeUp") {
            Wake(false);
        }
    }

    void Wake(bool keyPressed) {
        {
            std::lock_guard<std::mutex> lock(lock_);
            wake_ = true;
            keyPressed_ = keyPressed_ || keyPressed;
        }
        cv_.notify_one();
    }

    void SetImage(const std::string& context, const std::string& image) {
        Send("{\"event\":\"setImage\",\"context\":\"" + context + "\",\"payload\":{\"image\":\"" + image +
             "\",\"target\":0}}");
    }

    void ShowOnAllKeys(const std::string& image, bool alert) {
        std::set<std::string> contexts;
        {
            std::lock_guard<std::mutex> lock(lock_);
            contexts = contexts_;
            lastImage_ = image;
        }
        for (const std::string& c : contexts) {
            SetImage(c, image);
            if (alert) Send("{\"event\":\"showAlert\",\"context\":\"" + c + "\"}");
        }
    }

    // Polls UsageOne and redraws the key when it changes (the countdowns
    // change it every minute). A key press asks UsageOne for new numbers.
    void Worker() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        Painter g;
        if (!g.Init()) {
            Log("drawing setup failed");
            return;
        }
        std::string shown;
        bool busy = false;
        for (;;) {
            bool pressed;
            {
                std::unique_lock<std::mutex> lock(lock_);
                cv_.wait_for(lock, busy ? kPollBusy : kPoll, [this] { return wake_ || keyPressed_; });
                pressed = keyPressed_;
                keyPressed_ = wake_ = false;
            }
            Status s;
            bool alert = false;
            if (pressed) {
                bool ok = Query("refresh", &s);
                if (!ok && StartUsageOne()) {
                    // It reads the numbers as it starts.
                    for (int i = 0; i < 30 && !ok; i++) {
                        Sleep(500);
                        ok = Query("status", &s);
                    }
                }
                if (ok)
                    s.probing = true;  // it starts reading right after answering
                else
                    alert = true;
                Log("key pressed: %s", ok ? "refreshing" : "UsageOne not available");
            } else {
                Query("status", &s);
            }
            busy = s.probing;

            Palette p;
            p.Load();
            long long now = (long long)time(nullptr);
            std::string image = DataUrl(g.Png(kKeySize, [&] { DrawKey(g, p, s, now); }));
            if (image.empty()) continue;
            bool changed = image != shown;
            {
                std::lock_guard<std::mutex> lock(lock_);
                changed = changed || lastImage_.empty();
            }
            if (changed || alert) {
                ShowOnAllKeys(image, alert);
                shown = image;
            }
        }
    }

    HINTERNET session_ = nullptr, connect_ = nullptr, ws_ = nullptr;
    std::mutex sendLock_;
    std::mutex lock_;
    std::condition_variable cv_;
    bool wake_ = false, keyPressed_ = false;
    int press_ = 0;                          // counts key presses, to tell a hold from the next one
    bool held_ = false, longFired_ = false;  // the key is down / this press already opened Claude
    std::set<std::string> contexts_;
    std::string lastImage_;
};

std::string Narrow(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 3 && wcscmp(argv[1], L"/make-images") == 0) return MakeImages(argv[2]);
    if (argc >= 2 && wcscmp(argv[1], L"/install") == 0) return Install();

    int port = 0;
    std::string uuid, registerEvent;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (wcscmp(argv[i], L"-port") == 0) port = _wtoi(argv[i + 1]);
        else if (wcscmp(argv[i], L"-pluginUUID") == 0) uuid = Narrow(argv[i + 1]);
        else if (wcscmp(argv[i], L"-registerEvent") == 0) registerEvent = Narrow(argv[i + 1]);
    }
    if (!port || uuid.empty() || registerEvent.empty()) {
        MessageBoxW(nullptr, L"This program is started by the Stream Deck app.", APP_NAME, MB_ICONINFORMATION);
        return 1;
    }
    Plugin plugin;
    if (!plugin.Connect(port)) {
        Log("could not connect to Stream Deck on port %d (%lu)", port, GetLastError());
        return 1;
    }
    Log("connected");
    plugin.Run(uuid, registerEvent);
    return 0;
}
