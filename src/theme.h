// 2026-10-12 - AI - 主题调色板：把散在六个文件里的语义颜色集中成命名色板
//
// 设计要点：
//   • 深浅两套内建色板（Pal(false) / Pal(true)），默认值与重构前的硬编码
//     逐字节一致 —— 不放 theme.ini 时视觉零变化。
//   • %APPDATA%\MyDock\theme.ini 可按 key 覆盖任意颜色（AARRGGBB），
//     缺的 key 落回内建默认。见仓库根目录 theme.example.ini。
//   • ThemeReloadIfNeeded() 检查文件 mtime，变了就重载 —— Dock 的 120ms
//     轮询里挂一次即可，改主题文件立刻生效，不用重启。
//   • 只收「语义色」。一次性结构色（阴影、渐变端点、hover 洗色）留在原地，
//     它们是排版不是主题。

#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <string>

namespace mydock {

struct ThemePalette {
    Gdiplus::Color accentBlue;    // macOS 系统蓝：磁贴/菜单高亮/启动台强调
    Gdiplus::Color textPrimary;   // 主文字
    Gdiplus::Color textSecondary; // 次文字（星期行/副标题）
    Gdiplus::Color textTertiary;  // 弱文字（相邻月日期/快捷键提示）
    Gdiplus::Color surfaceCard;   // 卡片/磁贴底
    Gdiplus::Color surfacePanel;  // 面板底（菜单板，不透明）
    Gdiplus::Color panelTint;     // 半透明面板 tint（控制中心/日历底）
    Gdiplus::Color panelRim;      // 半透明面板描边
    Gdiplus::Color separator;     // 分隔线
    Gdiplus::Color danger;        // 红：周末日期/低电量/今天圆
    Gdiplus::Color trackBg;       // 滑条槽
    Gdiplus::Color sliderFill;    // 滑条已填充段
    Gdiplus::Color iconOnTile;    // 磁贴上的图标
    Gdiplus::Color labelBg;       // Dock 标签气泡底
    Gdiplus::Color labelFg;       // Dock 标签字
    Gdiplus::Color dockGlass;     // Dock 玻璃 tint
    Gdiplus::Color dockRunDot;    // 运行中指示点
    Gdiplus::Color calendarTint;  // 日历面板 tint
    Gdiplus::Color launchTextDim; // 启动台占位字
};

// dk=false → 浅色版，true → 深色版
const ThemePalette& Pal(bool dk);

// %APPDATA%\MyDock\theme.ini 的 mtime 变了就重载。可在高频轮询里调用。
void ThemeReloadIfNeeded();

// 从 ini 文本加载（测试用）：
//   [light] / [dark] 分节，key=AARRGGBB（十六进制，可带 # 或 0x 前缀）
//   未列出的 key 保持内建默认；未知节/键/坏值忽略。
bool ThemeLoadFromText(const std::wstring& text);

}  // namespace mydock
