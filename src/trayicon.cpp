#include "trayicon.h"

#include <cmath>

namespace {

// Claude Code's mascot, from the block characters of its welcome screen
//    ▐▛███▜▌
//   ▝▜█████▛▘
//     ▘▘ ▝▝
// Each quadrant of a character cell is one pixel here, twice as tall as wide
// (a cell is about 1:2), so every row below counts as two units of height:
// 16 units wide, 10 tall. The two gaps in the second row are the eyes.
const char* const kMascot[] = {
    "..############..",
    "..##.######.##..",
    "################",
    "..############..",
    "...#.#....#.#...",
};
const int kMascotW = 16, kMascotRows = 5, kRowUnits = 2;

bool MascotAt(float ux, float uy) {  // units, from the top-left of the art
    if (ux < 0 || uy < 0 || ux >= kMascotW || uy >= kMascotRows * kRowUnits) return false;
    return kMascot[(int)uy / kRowUnits][(int)ux] == '#';
}

// The star: twelve rays, longer and shorter in turn with a little
// irregularity, like the hand-drawn spark: angle (degrees) and length
// (fraction of the radius).
struct Ray { float angle, length; };
const Ray kRays[] = {
    {0, 1.00f}, {31, 0.74f}, {58, 0.94f}, {92, 0.70f}, {121, 1.00f}, {149, 0.76f},
    {182, 0.90f}, {208, 0.72f}, {239, 0.98f}, {272, 0.75f}, {301, 0.92f}, {329, 0.70f},
};
const float kHub = 0.13f, kRootHalfWidth = 0.115f, kTipHalfWidth = 0.075f;

bool StarAt(float x, float y) {  // relative to the centre, in units of the radius
    if (x * x + y * y < kHub * kHub) return true;
    for (const Ray& r : kRays) {
        float a = (r.angle - 90.0f) * 3.14159265f / 180.0f;  // first ray points up
        float bx = std::cos(a) * r.length, by = std::sin(a) * r.length;
        float len2 = bx * bx + by * by;
        float t = std::clamp((x * bx + y * by) / len2, 0.0f, 1.0f);
        float dx = x - t * bx, dy = y - t * by;
        float w = kRootHalfWidth + (kTipHalfWidth - kRootHalfWidth) * t;  // tapered
        if (dx * dx + dy * dy < w * w) return true;
    }
    return false;
}

// Coverage of each pixel, from ss x ss samples of `inside(px, py)` (pixel coordinates).
template <class F> std::vector<BYTE> Rasterize(int size, F inside) {
    std::vector<BYTE> mask(size * size, 0);
    const int ss = 8;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            int hits = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++)
                    if (inside(x + (sx + 0.5f) / ss, y + (sy + 0.5f) / ss)) hits++;
            mask[y * size + x] = (BYTE)(hits * 255 / (ss * ss));
        }
    return mask;
}

}  // namespace

std::vector<BYTE> MascotMask(int size) {
    // As wide as the icon; at 16 px that's one pixel per unit, so it stays sharp.
    float unit = size / (float)kMascotW;
    float top = std::round((size - kMascotRows * kRowUnits * unit) / 2);
    return Rasterize(size, [&](float px, float py) { return MascotAt(px / unit, (py - top) / unit); });
}

std::vector<BYTE> StarMask(int size) {
    float c = size / 2.0f, radius = size / 2.0f - 0.25f;
    return Rasterize(size, [&](float px, float py) { return StarAt((px - c) / radius, (py - c) / radius); });
}

HICON MakeTrayIcon(int size, COLORREF color, bool star) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) return nullptr;
    std::vector<BYTE> mask = star ? StarMask(size) : MascotMask(size);
    auto px = (BYTE*)bits;
    for (int i = 0; i < size * size; i++, px += 4) {  // premultiplied BGRA
        BYTE a = mask[i];
        px[0] = (BYTE)(GetBValue(color) * a / 255);
        px[1] = (BYTE)(GetGValue(color) * a / 255);
        px[2] = (BYTE)(GetRValue(color) * a / 255);
        px[3] = a;
    }
    HBITMAP monoMask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii = {TRUE, 0, 0, monoMask, bmp};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(monoMask);
    DeleteObject(bmp);
    return icon;
}

bool WriteIconPreview(const wchar_t* path) {
    const int sizes[] = {16, 20, 24, 32, 64};
    int w = 8, h = 2 * (64 + 8) + 8;
    for (int s : sizes) w += s + 8;
    std::vector<DWORD> img(w * h, 0xFF202020);  // dark taskbar grey
    for (int row = 0; row < 2; row++) {
        int x0 = 8, y0 = 8 + row * (64 + 8);
        for (int s : sizes) {
            std::vector<BYTE> m = row == 0 ? MascotMask(s) : StarMask(s);
            for (int y = 0; y < s; y++)
                for (int x = 0; x < s; x++) {
                    BYTE v = (BYTE)(0x20 + (255 - 0x20) * m[y * s + x] / 255);
                    img[(y0 + y) * w + x0 + x] = 0xFF000000 | (v << 16) | (v << 8) | v;
                }
            x0 += s + 8;
        }
    }
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)(img.size() * 4);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written;
    WriteFile(f, &fh, sizeof(fh), &written, nullptr);
    WriteFile(f, &ih, sizeof(ih), &written, nullptr);
    WriteFile(f, img.data(), (DWORD)(img.size() * 4), &written, nullptr);
    CloseHandle(f);
    return true;
}
