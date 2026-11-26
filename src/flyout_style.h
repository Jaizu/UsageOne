#pragma once

// Fonts and colour helper shared by the panel's source files.

#include <d2d1_1.h>

inline constexpr wchar_t kTextFont[] = L"Segoe UI Variable Text";
inline constexpr wchar_t kDisplayFont[] = L"Segoe UI Variable Display";
inline constexpr wchar_t kIconFont[] = L"Segoe Fluent Icons";

// 0xRRGGBB and an alpha.
inline D2D1_COLOR_F Rgba(UINT rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
