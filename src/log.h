#pragma once

#include "common.h"

// Diagnostic log: %LOCALAPPDATA%\UsageOne\UsageOne.log, started anew on each
// launch (the previous one is kept as UsageOne.old.log).
namespace Log {

void Init();
// printf-style, one line, thread-safe.
void Write(const wchar_t* fmt, ...);
// The folder the log is in (created if missing).
std::wstring Folder();

}  // namespace Log

#define LOG(...) Log::Write(__VA_ARGS__)
