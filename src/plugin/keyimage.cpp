// Drawing the key: Direct2D into a WIC bitmap, saved as a PNG. The look is the
// panel's: its fonts, its dark palette, and the bar colour setting.

#include "plugin.h"
#include "../settings.h"
#include "../trayicon.h"

#include <d2d1.h>
#include <dwrite.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wincrypt.h>

#include <cmath>
#include <ctime>
#include <functional>

namespace Plugin {

namespace {

const wchar_t kTextFont[] = L"Segoe UI Variable Text";
const wchar_t kDisplayFont[] = L"Segoe UI Variable Display";
const wchar_t kIconFont[] = L"Segoe Fluent Icons";
const wchar_t kGlyphRefresh = 0xE72C;

// --- Text -----------------------------------------------------------------------

struct Texts {
    const wchar_t *closed, *loading, *signIn, *noClaude, *noData;
};
const Texts kSpanish = {L"Pulsa para\nabrir UsageOne", L"Cargando…", L"Inicia sesión\nen Claude Code",
                        L"Falta\nClaude Code", L"Sin datos"};
const Texts kEnglish = {L"Press to\nstart UsageOne", L"Loading…", L"Sign in to\nClaude Code",
                        L"Claude Code\nnot found", L"No data"};

const Texts& T() {
    static const Texts* t = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? &kSpanish : &kEnglish;
    return *t;
}

// "40m", "3h 12m", "1d 6h".
std::wstring ShortDuration(long long seconds) {
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

// --- Colours ------------------------------------------------------------------

D2D1_COLOR_F Rgba(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

// The panel's dark palette: Stream Deck keys sit on black.
struct Palette {
    D2D1_COLOR_F background = Rgba(0x202020), border = Rgba(0xFFFFFF, 0.10f);
    D2D1_COLOR_F text = Rgba(0xFFFFFF), textSecondary = Rgba(0xFFFFFF, 0.786f), textTertiary = Rgba(0xFFFFFF, 0.545f);
    D2D1_COLOR_F track = Rgba(0xFFFFFF, 0.10f);
    D2D1_COLOR_F accent = Rgba(0x99EBFF), warn = Rgba(0xFCE100), danger = Rgba(0xFF99A4);
    D2D1_COLOR_F bar;  // below 60 %: the bar colour setting

    Palette() {
        // The Windows accent, the shade WinUI uses on dark (Light2).
        BYTE shades[32];
        DWORD size = sizeof(shades);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                         L"AccentPalette", RRF_RT_REG_BINARY, nullptr, shades, &size) == ERROR_SUCCESS &&
            size >= 28)
            accent = D2D1::ColorF(shades[4] / 255.0f, shades[5] / 255.0f, shades[6] / 255.0f, 1.0f);
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

}  // namespace

// --- Painter ------------------------------------------------------------------

struct KeyPainter::Impl {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<ID2D1RenderTarget> rt;  // while drawing
    ComPtr<ID2D1SolidColorBrush> brush;

    // Draws at `size` px (the layout is in 144 units) and returns the PNG bytes.
    std::vector<BYTE> Png(int size, const std::function<void()>& draw);

    void Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius = 0);
    ComPtr<IDWriteTextFormat> Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                     DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);
    void Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c);
    float Width(const std::wstring& s, IDWriteTextFormat* f);
    void Bar(float x0, float x1, float y, float h, int percent, const D2D1_COLOR_F& c, const Palette& p);
    void Mascot(const D2D1_RECT_F& r, const D2D1_COLOR_F& c);

    void DrawKey(const Palette& p, const Status& s, long long now);
    void DrawMessage(const Palette& p, const Status& s);
    void DrawFooter(const Palette& p, const Status& s, long long now, float top);
    void DrawRow(const Palette& p, float top, const wchar_t* label, int percent);
};

bool KeyPainter::Init() {
    impl_ = std::make_shared<Impl>();
    return SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, impl_->d2d.GetAddressOf())) &&
           SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         (IUnknown**)impl_->dwrite.GetAddressOf())) &&
           SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl_->wic)));
}

std::vector<BYTE> KeyPainter::Impl::Png(int size, const std::function<void()>& draw) {
    std::vector<BYTE> png;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return png;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                              96, 96);
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt))) return png;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTransform(D2D1::Matrix3x2F::Scale(size / (float)kKeySize, size / (float)kKeySize));
    draw();
    HRESULT hr = rt->EndDraw();
    brush.Reset();
    rt.Reset();
    if (FAILED(hr)) return png;

    // Encode: WIC PNG encoder into a memory stream.
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(nullptr, 0));
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IWICFormatConverter> converter;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (!stream || FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(size, size)) || FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(bmp.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(converter.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
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

// --- Drawing pieces, in 144 units ---------------------------------------------

void KeyPainter::Impl::Fill(const D2D1_RECT_F& r, const D2D1_COLOR_F& c, float radius) {
    brush->SetColor(c);
    if (radius > 0)
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
    else
        rt->FillRectangle(r, brush.Get());
}

ComPtr<IDWriteTextFormat> KeyPainter::Impl::Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                                   DWRITE_TEXT_ALIGNMENT align) {
    ComPtr<IDWriteTextFormat> f;
    if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"", &f)))
        return nullptr;
    f->SetTextAlignment(align);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return f;
}

void KeyPainter::Impl::Text(const std::wstring& s, IDWriteTextFormat* f, const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    if (!f) return;
    brush->SetColor(c);
    rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
}

float KeyPainter::Impl::Width(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> layout;
    if (!f || FAILED(dwrite->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, 1000, 100, &layout))) return 0;
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void KeyPainter::Impl::Bar(float x0, float x1, float y, float h, int percent, const D2D1_COLOR_F& c, const Palette& p) {
    Fill({x0, y, x1, y + h}, p.track, h / 2);
    float w = (x1 - x0) * std::clamp(percent, 0, 100) / 100.0f;
    if (w > 0) Fill({x0, y, x0 + std::max(w, h), y + h}, c, h / 2);
}

// Claude Code's mascot, the tray icon's shape.
void KeyPainter::Impl::Mascot(const D2D1_RECT_F& r, const D2D1_COLOR_F& c) {
    int n = (int)(r.right - r.left) * 2;  // drawn at 2x, scaled to fit
    std::vector<BYTE> mask = MascotMask(n);
    std::vector<BYTE> px(n * n * 4);
    for (int i = 0; i < n * n; i++) {  // premultiplied BGRA
        float a = mask[i] / 255.0f * c.a;
        px[i * 4 + 0] = (BYTE)std::lround(c.b * a * 255);
        px[i * 4 + 1] = (BYTE)std::lround(c.g * a * 255);
        px[i * 4 + 2] = (BYTE)std::lround(c.r * a * 255);
        px[i * 4 + 3] = (BYTE)std::lround(a * 255);
    }
    ComPtr<ID2D1Bitmap> bmp;
    auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(n, n), px.data(), n * 4, props, &bmp)))
        rt->DrawBitmap(bmp.Get(), r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// --- The key --------------------------------------------------------------------

const float kMargin = 14;

void KeyPainter::Impl::DrawKey(const Palette& p, const Status& s, long long now) {
    Fill({0, 0, (float)kKeySize, (float)kKeySize}, p.background);
    brush->SetColor(p.border);
    rt->DrawRoundedRectangle(D2D1::RoundedRect({1.5f, 1.5f, kKeySize - 1.5f, kKeySize - 1.5f}, 18, 18), brush.Get(),
                             2.0f);

    if (!s.running || s.session < 0) {
        DrawMessage(p, s);
    } else if (s.week >= 0) {
        // Session and week, each a row with its bar, like the panel.
        DrawRow(p, 8, L"5h", s.session);
        Bar(kMargin, kKeySize - kMargin, 50, 8, s.session, p.Level(s.session), p);
        DrawRow(p, 62, L"7d", s.week);
        Bar(kMargin, kKeySize - kMargin, 104, 8, s.week, p.Level(s.week), p);
        DrawFooter(p, s, now, 116);
    } else {
        // A plan without a weekly limit: the session, big.
        D2D1_COLOR_F c = p.Level(s.session);
        auto fLabel = Format(kTextFont, 20, DWRITE_FONT_WEIGHT_NORMAL);
        auto fNum = Format(kDisplayFont, 56, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        auto fUnit = Format(kDisplayFont, 28, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        std::wstring num = std::to_wstring(s.session);
        float wNum = Width(num, fNum.Get()), wUnit = Width(L"%", fUnit.Get());
        float left = std::round((kKeySize - wNum - wUnit) / 2);
        Text(L"5h", fLabel.Get(), {kMargin, 10, kKeySize - kMargin, 36}, p.textSecondary);
        Text(num, fNum.Get(), {left, 32, left + wNum + 2, 96}, c);
        Text(L"%", fUnit.Get(), {left + wNum, 52, left + wNum + wUnit + 2, 90}, c);
        Bar(kMargin, kKeySize - kMargin, 100, 10, s.session, c, p);
        DrawFooter(p, s, now, 116);
    }
}

// No numbers: the mascot and why.
void KeyPainter::Impl::DrawMessage(const Palette& p, const Status& s) {
    const wchar_t* msg = !s.running                         ? T().closed
                         : s.status == "notLoggedIn"        ? T().signIn
                         : s.status == "noClaude"           ? T().noClaude
                         : s.status == "none" || s.probing  ? T().loading
                                                            : T().noData;
    Mascot({44, 24, 100, 80}, s.running ? p.text : p.textTertiary);
    auto f = Format(kTextFont, 17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    Text(msg, f.Get(), {6, 84, kKeySize - 6.0f, 134}, p.textSecondary);
}

// A limit: its window on the left, the percentage on the right (a big number
// with a smaller "%", as in the panel).
void KeyPainter::Impl::DrawRow(const Palette& p, float top, const wchar_t* label, int percent) {
    const float numSize = 36, x0 = kMargin, x1 = kKeySize - kMargin;
    D2D1_COLOR_F c = p.Level(percent);
    auto fNum = Format(kDisplayFont, numSize, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fUnit = Format(kDisplayFont, numSize * 0.55f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    auto fLabel = Format(kTextFont, 20, DWRITE_FONT_WEIGHT_NORMAL);
    std::wstring num = std::to_wstring(percent);
    float wNum = Width(num, fNum.Get()), wUnit = Width(L"%", fUnit.Get());
    float h = numSize * 1.1f, unitTop = top + numSize * 0.36f;  // label and "%" share the number's baseline
    Text(label, fLabel.Get(), {x0, unitTop, x1 - wNum - wUnit, top + h}, p.textSecondary);
    Text(num, fNum.Get(), {x1 - wUnit - wNum - 1, top, x1 - wUnit + 2, top + h}, c);
    Text(L"%", fUnit.Get(), {x1 - wUnit, unitTop, x1 + 2, top + h}, c);
}

// When the session and the week reset: "⟳ 40m · 1d 6h". The icon takes the
// accent while UsageOne reads new numbers.
void KeyPainter::Impl::DrawFooter(const Palette& p, const Status& s, long long now, float top) {
    std::wstring text = s.sessionReset ? ShortDuration(s.sessionReset - now) : L"—";
    if (s.week >= 0) text += L" · " + (s.weekReset ? ShortDuration(s.weekReset - now) : std::wstring(L"—"));
    auto f = Format(kTextFont, 17, DWRITE_FONT_WEIGHT_NORMAL);
    auto icon = Format(kIconFont, 14, DWRITE_FONT_WEIGHT_NORMAL);
    const float iconW = 14, gap = 5;
    float textW = Width(text, f.Get());
    float left = std::round((kKeySize - (iconW + gap + textW)) / 2);
    Text(std::wstring(1, kGlyphRefresh), icon.Get(), {left, top, left + iconW + 2, top + 24},
         s.probing ? p.accent : p.textTertiary);
    Text(text, f.Get(), {left + iconW + gap, top, left + iconW + gap + textW + 2, top + 24}, p.textTertiary);
}

// --- Public ---------------------------------------------------------------------

static std::string DataUrl(const std::vector<BYTE>& png) {
    DWORD n = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (png.empty() || !CryptBinaryToStringA(png.data(), (DWORD)png.size(), flags, nullptr, &n)) return {};
    std::string b64(n, '\0');
    CryptBinaryToStringA(png.data(), (DWORD)png.size(), flags, b64.data(), &n);
    b64.resize(n);
    return "data:image/png;base64," + b64;
}

std::string KeyPainter::Render(const Status& status, long long now) {
    Palette p;  // read each time: the accent and the bar colour can change
    return DataUrl(impl_->Png(kKeySize, [&] { impl_->DrawKey(p, status, now); }));
}

static bool WriteBytes(const std::wstring& path, const std::vector<BYTE>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = !data.empty() && WriteFile(f, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    CloseHandle(f);
    return ok;
}

// The mascot for the action list and the category, a sample key for the
// plugin icon, and "Loading" for the key until the plugin draws it.
bool KeyPainter::WriteManifestImages(const std::wstring& folder) {
    CreateDirectoryW(folder.c_str(), nullptr);
    Palette p;
    long long now = (long long)time(nullptr);
    Status sample;
    sample.running = true;
    sample.status = "ok";
    sample.session = 42;
    sample.week = 18;
    sample.sessionReset = now + 2 * 3600 + 15 * 60;
    sample.weekReset = now + 3 * 86400 + 5 * 3600;
    Status loading;
    loading.running = true;
    loading.status = "none";

    enum Kind { MascotIcon, SampleKey, LoadingKey };
    struct Image { const wchar_t* name; int size; Kind kind; };
    const Image images[] = {
        {L"action", 20, MascotIcon},  {L"action@2x", 40, MascotIcon}, {L"category", 28, MascotIcon},
        {L"category@2x", 56, MascotIcon}, {L"plugin", 144, SampleKey}, {L"plugin@2x", 288, SampleKey},
        {L"key", 72, LoadingKey},     {L"key@2x", 144, LoadingKey},
    };
    for (const Image& i : images) {
        auto png = impl_->Png(i.size, [&] {
            if (i.kind == MascotIcon)
                impl_->Mascot({0, 0, (float)kKeySize, (float)kKeySize}, Rgba(0xFFFFFF));
            else
                impl_->DrawKey(p, i.kind == SampleKey ? sample : loading, now);
        });
        if (!WriteBytes(folder + L"\\" + i.name + L".png", png)) return false;
    }
    return true;
}

}  // namespace Plugin
