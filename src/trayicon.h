#pragma once

#include "common.h"

// Tray icon shapes as anti-aliased coverage masks (size x size bytes, 0..255):
// Claude Code's mascot (the pixel crab of its welcome screen) or the Claude
// star (a spark of uneven rays).
std::vector<BYTE> MascotMask(int size);
std::vector<BYTE> StarMask(int size);

// A size x size tray icon of one of them in one colour.
HICON MakeTrayIcon(int size, COLORREF color, bool star);

// Writes both shapes at several sizes to a 32-bit BMP, for checking them:
// UsageOne.exe /icon-preview <file.bmp>
bool WriteIconPreview(const wchar_t* path);
