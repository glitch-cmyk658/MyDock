// 2026-10-12 - AI - 主题调色板实现：内建默认 + theme.ini 覆盖 + mtime 热重载
#include "theme.h"

#include "util.h"   // ConfigDir

#include <shlwapi.h>
#include <cwchar>

namespace mydock {
namespace {

// 内建默认 = 2026-10-12 重构前散在各文件的硬编码原值（逐字节一致）
ThemePalette MakeLight() {
    ThemePalette p{};
    p.accentBlue    = Gdiplus::Color(255,   0, 122, 255);
    p.textPrimary   = Gdiplus::Color(255,  29,  29,  31);
    p.textSecondary = Gdiplus::Color(255, 134, 134, 139);
    p.textTertiary  = Gdiplus::Color(150, 199, 199, 204);
    p.surfaceCard   = Gdiplus::Color(255, 255, 255, 255);
    p.surfacePanel  = Gdiplus::Color(255, 246, 246, 248);
    p.panelTint     = Gdiplus::Color(242, 242, 242, 246);
    p.panelRim      = Gdiplus::Color( 26,   0,   0,   0);
    p.separator     = Gdiplus::Color( 24,   0,   0,   0);
    p.danger        = Gdiplus::Color(255, 255,  59,  48);
    p.trackBg       = Gdiplus::Color(255, 226, 226, 230);
    p.sliderFill    = Gdiplus::Color(255,  90,  90,  96);
    p.iconOnTile    = Gdiplus::Color(255,  70,  70,  78);
    p.labelBg       = Gdiplus::Color(240, 255, 255, 255);
    p.labelFg       = Gdiplus::Color(255,  28,  28,  34);
    p.dockGlass     = Gdiplus::Color(104, 252, 252, 255);
    p.dockRunDot    = Gdiplus::Color(255,  70, 132, 232);
    p.calendarTint  = Gdiplus::Color(246, 252, 252, 254);
    p.launchTextDim = Gdiplus::Color(118,  10,  10,  14);
    return p;
}

ThemePalette MakeDark() {
    ThemePalette p{};
    p.accentBlue    = Gdiplus::Color(255,  10, 132, 255);
    p.textPrimary   = Gdiplus::Color(255, 242, 242, 247);
    p.textSecondary = Gdiplus::Color(255, 142, 142, 147);
    p.textTertiary  = Gdiplus::Color(150,  99,  99, 102);
    p.surfaceCard   = Gdiplus::Color(255,  44,  44,  46);
    p.surfacePanel  = Gdiplus::Color(255,  28,  28,  30);
    p.panelTint     = Gdiplus::Color(226,  24,  24,  27);
    p.panelRim      = Gdiplus::Color( 40, 255, 255, 255);
    p.separator     = Gdiplus::Color(255,  72,  72,  74);
    p.danger        = Gdiplus::Color(255, 255,  69,  58);
    p.trackBg       = Gdiplus::Color(255,  90,  90,  94);
    p.sliderFill    = Gdiplus::Color(255, 242, 242, 247);
    p.iconOnTile    = Gdiplus::Color(255, 220, 220, 226);
    p.labelBg       = Gdiplus::Color(240,  45,  45,  52);
    p.labelFg       = Gdiplus::Color(255, 240, 240, 246);
    p.dockGlass     = Gdiplus::Color(104,  28,  28,  34);
    p.dockRunDot    = Gdiplus::Color(255,  70, 132, 232);
    p.calendarTint  = Gdiplus::Color(232,  28,  28,  30);
    p.launchTextDim = Gdiplus::Color(118,  10,  10,  14);
    return p;
}

// key 名 → 成员指针。加新颜色时在这里登记一行。
struct ThemeKey {
    const wchar_t* key;
    Gdiplus::Color ThemePalette::*ptr;
};

const ThemeKey kKeys[] = {
    {L"accentBlue",    &ThemePalette::accentBlue},
    {L"textPrimary",   &ThemePalette::textPrimary},
    {L"textSecondary", &ThemePalette::textSecondary},
    {L"textTertiary",  &ThemePalette::textTertiary},
    {L"surfaceCard",   &ThemePalette::surfaceCard},
    {L"surfacePanel",  &ThemePalette::surfacePanel},
    {L"panelTint",     &ThemePalette::panelTint},
    {L"panelRim",      &ThemePalette::panelRim},
    {L"separator",     &ThemePalette::separator},
    {L"danger",        &ThemePalette::danger},
    {L"trackBg",       &ThemePalette::trackBg},
    {L"sliderFill",    &ThemePalette::sliderFill},
    {L"iconOnTile",    &ThemePalette::iconOnTile},
    {L"labelBg",       &ThemePalette::labelBg},
    {L"labelFg",       &ThemePalette::labelFg},
    {L"dockGlass",     &ThemePalette::dockGlass},
    {L"dockRunDot",    &ThemePalette::dockRunDot},
    {L"calendarTint",  &ThemePalette::calendarTint},
    {L"launchTextDim", &ThemePalette::launchTextDim},
};

ThemePalette g_pal[2] = {MakeLight(), MakeDark()};   // [0]=浅 [1]=深
FILETIME    g_iniTime{};                             // 上次加载时文件的 mtime
bool        g_timeKnown = false;

bool HexToColor(const wchar_t* s, Gdiplus::Color& out) {
    if (!s || !*s) return false;
    if (*s == L'#') ++s;
    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) s += 2;
    if (wcslen(s) != 8) return false;
    wchar_t* end = nullptr;
    const unsigned long v = wcstoul(s, &end, 16);
    if (!end || *end) return false;
    out = Gdiplus::Color(static_cast<BYTE>((v >> 24) & 0xFF),
                         static_cast<BYTE>((v >> 16) & 0xFF),
                         static_cast<BYTE>((v >> 8) & 0xFF),
                         static_cast<BYTE>(v & 0xFF));
    return true;
}

}  // namespace

const ThemePalette& Pal(bool dk) {
    return g_pal[dk ? 1 : 0];
}

bool ThemeLoadFromText(const std::wstring& text) {
    ThemePalette light = MakeLight(), dark = MakeDark();
    ThemePalette* cur = &light;

    wchar_t* buf = _wcsdup(text.c_str());
    if (!buf) return false;

    for (wchar_t* line = wcstok(buf, L"\r\n"); line; line = wcstok(nullptr, L"\r\n")) {
        // 去两端空白
        while (*line == L' ' || *line == L'\t') ++line;
        wchar_t* e = line + wcslen(line);
        while (e > line && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
        if (!*line || *line == L';' || *line == L'#') continue;

        if (*line == L'[') {
            const wchar_t* close = wcschr(line, L']');
            if (!close) continue;
            if (_wcsnicmp(line + 1, L"light", close - line - 1) == 0) cur = &light;
            else if (_wcsnicmp(line + 1, L"dark", close - line - 1) == 0) cur = &dark;
            continue;
        }

        wchar_t* eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t* key = line;
        wchar_t* val = eq + 1;
        while (*val == L' ' || *val == L'\t') ++val;
        wchar_t* ke = eq;
        while (ke > key && (ke[-1] == L' ' || ke[-1] == L'\t')) *--ke = 0;

        Gdiplus::Color c;
        if (!HexToColor(val, c)) continue;   // 坏值忽略，保持默认
        for (const ThemeKey& k : kKeys) {
            if (_wcsicmp(key, k.key) == 0) {
                cur->*k.ptr = c;
                break;
            }
        }
    }
    free(buf);

    g_pal[0] = light;
    g_pal[1] = dark;
    return true;
}

void ThemeReloadIfNeeded() {
    const std::wstring dir = ConfigDir();
    if (dir.empty()) return;
    const std::wstring path = dir + L"\\theme.ini";

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    FILETIME now{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        // 文件不存在 = 用内建默认；只有「之前存在、现在没了」才需要回落
        if (g_timeKnown && (g_iniTime.dwLowDateTime || g_iniTime.dwHighDateTime)) {
            g_pal[0] = MakeLight();
            g_pal[1] = MakeDark();
            g_iniTime = {};
            Log(L"主题：theme.ini 已删除，回落内建配色");
        }
        g_timeKnown = true;
        return;
    }
    now = fa.ftLastWriteTime;
    if (g_timeKnown &&
        now.dwLowDateTime == g_iniTime.dwLowDateTime &&
        now.dwHighDateTime == g_iniTime.dwHighDateTime) return;   // 没变
    g_iniTime = now;
    g_timeKnown = true;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    std::wstring text;
    char raw[4096];
    size_t n = 0;
    while ((n = fread(raw, 1, sizeof(raw), f)) > 0) {
        // 文件按 UTF-8 读，转宽字符
        int wn = MultiByteToWideChar(CP_UTF8, 0, raw, static_cast<int>(n),
                                     nullptr, 0);
        if (wn > 0) {
            std::wstring chunk(static_cast<size_t>(wn), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, raw, static_cast<int>(n), &chunk[0], wn);
            text += chunk;
        }
    }
    fclose(f);

    ThemeLoadFromText(text);
    Log(L"主题：theme.ini 已加载（浅/深两套）");
}

}  // namespace mydock
