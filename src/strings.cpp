#include "strings.h"

static const Strings kEs = {
    L"Uso de Claude",
    L"Sesión actual",
    L"Semana · todos los modelos",
    L"Semana · %s",
    L"Se restablece en %s · %s",
    L"Se restablece el %s (en %s)",
    L"Sin empezar",
    L"Hora punta",
    L"Fuera de hora punta",
    L"Termina en %s",
    L"Empieza en %s",
    L"Actualizado hace %s",
    L"Actualizado ahora",
    L"Actualizando…",
    L"No se pudo actualizar · datos de hace %s",
    L"Próxima en %s",
    L"Cargando…",
    L"No se encontró Claude Code. Instálalo y ejecuta «claude» una vez para iniciar sesión.",
    L"Claude Code no tiene la sesión iniciada. Abre una terminal, ejecuta «claude» e inicia sesión.",
    L"Claude Code no ha devuelto los límites del plan. ¿Está usando una clave de API en vez de la suscripción?",
    L"No se pudo leer el uso de Claude Code. Se volverá a intentar.",
    L"%s · periodo hasta el %s",
    L"Plan %s",
    L"Ajustes",
    L"Iniciar con Windows",
    L"Mostrar la hora punta",
    L"Estrella de Claude en la bandeja",
    L"Límites semanales por modelo",
    L"Mostrar %s",
    L"Color de las barras",
    L"Desde el 60 % pasan a amarillo y desde el 85 % a rojo.",
    L"Acento",
    L"Blanco",
    L"Verde",
    L"Otro",
    L"Ver el uso en claude.ai",
    L"Actualizar ahora",
    L"Salir",
    L"mañana",
    {L"dom", L"lun", L"mar", L"mié", L"jue", L"vie", L"sáb"},
    {L"ene", L"feb", L"mar", L"abr", L"may", L"jun", L"jul", L"ago", L"sept", L"oct", L"nov", L"dic"},
};

static const Strings kEn = {
    L"Claude usage",
    L"Current session",
    L"Weekly · all models",
    L"Weekly · %s",
    L"Resets in %s · %s",
    L"Resets %s (in %s)",
    L"Not started",
    L"Peak hours",
    L"Off-peak",
    L"Ends in %s",
    L"Starts in %s",
    L"Updated %s ago",
    L"Updated just now",
    L"Updating…",
    L"Couldn't update · data from %s ago",
    L"Next in %s",
    L"Loading…",
    L"Claude Code wasn't found. Install it and run “claude” once to sign in.",
    L"Claude Code isn't signed in. Open a terminal, run “claude” and sign in.",
    L"Claude Code didn't report plan limits. Is it using an API key instead of the subscription?",
    L"Couldn't read the usage from Claude Code. It will try again.",
    L"%s · period until %s",
    L"%s plan",
    L"Settings",
    L"Start with Windows",
    L"Show peak hours",
    L"Claude star in the tray",
    L"Per-model weekly limits",
    L"Show %s",
    L"Bar colour",
    L"From 60% they turn yellow, from 85% red.",
    L"Accent",
    L"White",
    L"Green",
    L"Custom",
    L"Open usage on claude.ai",
    L"Refresh now",
    L"Exit",
    L"tomorrow",
    {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"},
    {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"},
};

const Strings& S() {
    static const Strings* s = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH ? &kEs : &kEn;
    return *s;
}

std::wstring Format(const wchar_t* fmt, const std::wstring& a, const std::wstring& b) {
    wchar_t buf[512];
    swprintf_s(buf, fmt, a.c_str(), b.c_str());
    return buf;
}

std::wstring Duration(ULONGLONG t) {
    ULONGLONG mins = t / Usage::kMinute;
    wchar_t buf[64];
    if (mins < 1)
        return L"<1 min";
    if (mins < 60)
        swprintf_s(buf, L"%llu min", mins);
    else if (mins < 24 * 60)
        swprintf_s(buf, L"%llu h %llu min", mins / 60, mins % 60);
    else
        swprintf_s(buf, L"%llu d %llu h", mins / (24 * 60), (mins / 60) % 24);
    return buf;
}

static bool ToLocal(ULONGLONG t, SYSTEMTIME* local) {
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME utc;
    return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(nullptr, &utc, local);
}

static int DayNumber(const SYSTEMTIME& t) {  // days since an epoch, for "same day" / "tomorrow"
    FILETIME ft;
    SYSTEMTIME d = t;
    d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0;
    SystemTimeToFileTime(&d, &ft);
    return (int)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / Usage::kDay);
}

std::wstring Clock(ULONGLONG utc) {
    SYSTEMTIME t, now;
    if (!ToLocal(utc, &t)) return {};
    GetLocalTime(&now);
    wchar_t hm[16];
    swprintf_s(hm, L"%d:%02d", t.wHour, t.wMinute);
    int diff = DayNumber(t) - DayNumber(now);
    if (diff == 0) return hm;
    if (diff == 1) return std::wstring(S().tomorrow) + L" " + hm;
    return std::wstring(S().days[t.wDayOfWeek]) + L" " + hm;
}

std::wstring ShortDate(ULONGLONG utc) {
    SYSTEMTIME t;
    if (!ToLocal(utc, &t)) return {};
    return std::to_wstring(t.wDay) + L" " + S().months[t.wMonth - 1];
}

std::wstring LimitName(const std::wstring& key) {
    if (key == L"session") return S().session;
    if (key == L"all models") return S().weekAll;
    return Format(S().weekModel, key);
}

std::wstring ResetLine(const Limit& l) {
    if (!l.resetAt) return S().notStarted;
    ULONGLONG now = Usage::NowTicks();
    ULONGLONG left = l.resetAt > now ? l.resetAt - now : 0;
    if (left < Usage::kDay) return Format(S().resetsIn, Duration(left), Clock(l.resetAt));
    return Format(S().resetsAt, Clock(l.resetAt), Duration(left));
}
