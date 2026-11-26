#pragma once

#include "common.h"

// Usage comes from Claude Code itself: `claude -p /usage` prints the same plan
// limits as the interactive /usage, using Claude Code's own sign-in. It doesn't
// call the model or spend tokens. The plan comes from the account profile
// Claude Code keeps in ~/.claude.json. No credentials are read or sent.

struct Limit {
    std::wstring key;       // "session", or the week's name in /usage: "all models", "Fable"...
    int percent = 0;
    ULONGLONG resetAt = 0;  // UTC FILETIME ticks; 0 = no reset given (window not started)
};

struct Plan {
    std::wstring name;         // "Max 5x", "Pro"... empty if unknown
    ULONGLONG since = 0;       // subscription start, UTC ticks
    ULONGLONG periodEnd = 0;   // end of the current monthly period (estimated from `since`)
};

struct UsageData {
    enum Status { None, Ok, NoClaude, NotLoggedIn, NoLimits, Failed };
    Status status = None;
    std::vector<Limit> limits;
    Plan plan;
    std::wstring detail;     // what went wrong, for the log
    ULONGLONG fetchedAt = 0; // when the limits were read, UTC ticks
};

// What the app shows: the last good data, and how the latest probe went.
struct UsageState {
    UsageData data;                              // last limits read (kept when a later probe fails)
    UsageData::Status last = UsageData::None;    // status of the latest probe
    bool probing = false;
    ULONGLONG nextRefreshTick = 0;               // GetTickCount64() of the next automatic refresh
};

namespace Usage {

constexpr ULONGLONG kSecond = 10000000ULL;
constexpr ULONGLONG kMinute = 60 * kSecond;
constexpr ULONGLONG kHour = 60 * kMinute;
constexpr ULONGLONG kDay = 24 * kHour;

ULONGLONG NowTicks();
// A weekly limit of one model ("Current week (Fable)"), not the session or all models.
inline bool IsModelWeek(const Limit& l) { return l.key != L"session" && l.key != L"all models"; }

// Runs Claude Code and reads the profile; blocking (seconds), call it off the UI thread.
UsageData Probe();
// Parses /usage output (exposed for testing).
UsageData Parse(const std::wstring& text);
Plan ReadPlan();

}  // namespace Usage
