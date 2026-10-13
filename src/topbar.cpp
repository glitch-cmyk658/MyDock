// 2026-10-09 - AI - 顶部状态栏的实现：分层细条 + 毛玻璃 + 圆环 + 时钟
#include "topbar.h"
#include "svgicon.h"
#include "flyout.h"   // 2026-10-11 - AI - 时钟点开的日历面板
#include "util.h"
#include "theme.h"
#include "trayuia.h"
#include <msctf.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

#include <dwmapi.h>
#include <shellapi.h>

namespace mydock {
namespace {

// 主屏 DPI。TopBar 的 dpi_ 是成员，但下面那几个自由函数（SampleRectFor 之类）
// 也要按同一把尺子量，所以在这儿留一份。TopBar::Create 和 WM_DPICHANGED 里同步。
int g_topDpi = 96;
int TdFree(int px) { return DpiAt(px, g_topDpi); }

const wchar_t* kClass = L"MyDockTopBarClass";
const UINT_PTR kTimerTick = 1;    // 500ms：查全屏、刷文字
const UINT_PTR kTimerAnim = 2;    // 16ms：只在渐变的时候开
const UINT kMsgAppBar = WM_APP + 40;   // 系统回调我们 AppBar 通知用的消息号

const int kCmdAbout    = 1;
const int kCmdSettings = 2;
const int kCmdStore    = 3;
const int kCmdTaskMgr  = 4;
const int kCmdWorkshop = 5;
const int kCmdSleep    = 6;
const int kCmdRestart  = 7;
const int kCmdShutdown = 8;
const int kCmdQuit     = 11;  // 退出 MyDock 本身（不是关机/注销那类系统动作）

// ---- 右侧状态图标菜单的命令号（2026-10-10）----
//
// ★ 命令号分三段，谁也别撞谁：
//     1..99    顶部栏左侧那个系统菜单（上面这批）
//     101..199 应用操作（appaction.h，Dock 和启动台共用）
//     150+     这里这批状态菜单的固定项
//     200/300  网络列表 / 蓝牙设备列表（内容是弹出时才扫的，按下标排）
//   之前吃过一次亏：两边往同一个菜单里塞项，号撞了就串台。
const int kStSettings = 150;   // 打开「这一项」对应的设置页（按图标 id 决定开哪页）
const int kStMute     = 151;   // 静音 / 取消静音
const int kStProject  = 152;   // Win+P 投影
const int kStComputer = 153;   // 打开此电脑
const int kStShowDock = 154;   // 唤出 Dock（顶栏 ⏏ 菜单）
const int kStAutoStart = 155;  // 开机自启开关（勾选态）
const int kStRestartDock = 156; // 重启 MyDock
const int kStDiskMgmt = 157;   // 磁盘管理
const int kStTrayBase = 400;   // 托盘应用菜单项（基址 + 序号）
const int kImeBase    = 500;   // 输入法菜单项（基址 + 序号）
// TSF 的 ActivateProfile 用 flag（MinGW 头里没定义这个常量）
#ifndef TF_IPP_FLAG_DONTCARECURRENTINPUTLANGUAGE
#define TF_IPP_FLAG_DONTCARECURRENTINPUTLANGUAGE 0x00000001
#endif
const int kStWifiBase = 200;   // 200..249
const int kStBtBase   = 300;   // 300..349

const wchar_t* kWeekName[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};

// 前台窗口属于哪个 exe（只要文件名，不要路径）。
// 拿「进程名」而不是「窗口标题」，是因为标题里混着文档名（"报告.docx - Word"），
// 而 macOS 菜单栏左边显示的是**应用名**。
//
// ★ 返回 false 表示「这一拍问不出应用名」，调用方要保持上一次的名字。
//   什么情况会问不出来：前台是我们**自己**的窗口 —— 系统菜单一弹出来就立刻
//   成了前台窗口，照直取进程名的话，左边会当场跳成 "MyDock"，很难看。
bool ForegroundAppName(std::wstring& out) {
    out.clear();
    HWND fg = GetForegroundWindow();
    if (!fg) return true;

    // 自己的窗口（状态栏 / 菜单 / 启动台 / Dock）不算「前台应用」
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (wcsncmp(cls, L"MyDock", 6) == 0) return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0) return true;

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return true;
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(h, 0, path, &len);
    CloseHandle(h);
    if (!ok || path[0] == L'\0') return true;

    const wchar_t* slash = wcsrchr(path, L'\\');
    std::wstring name = slash ? (slash + 1) : path;
    // 去掉 ".exe" —— 显示 "Steam" 而不是 "steam.exe"
    if (name.size() > 4) {
        std::wstring ext = name.substr(name.size() - 4);
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (ext == L".exe") name.resize(name.size() - 4);
    }
    // UWP 应用统一挂在 ApplicationFrameHost 上，那个名字没有意义，宁可空着
    if (name == L"ApplicationFrameHost") {
        out.clear();
        return true;
    }
    out = name;
    return true;
}

// 每个应用配一个固定的颜色，圆环用它。同一个应用每次都是同一个色，
// 不同应用散在色环上 —— 用户说「不同的应用也有不同的颜色」。
//
// 为什么不用图标里的主色：要把 256×256 的图标逐像素统计一遍才知道主色，
// 为了一个 26px 高的圆环不值当；而且很多图标的主色是灰白，取出来跟背景糊在一起。
// 拿名字算哈希反而稳：同一份名单每次开机颜色都不变。
Gdiplus::Color AccentForApp(const std::wstring& s) {
    if (s.empty()) return Gdiplus::Color(255, 84, 84, 92);

    unsigned h = 2166136261u;                       // FNV-1a
    for (wchar_t c : s) {
        h ^= static_cast<unsigned>(c);
        h *= 16777619u;
    }
    const float hue = static_cast<float>(h % 360u) / 360.0f;

    // 固定饱和度/亮度：太艳像塑料，太淡又跟玻璃底糊在一起。
    // 亮度压到 0.42 —— 浅色底上要够深才看得清。
    const float sat = 0.62f;
    const float lum = 0.42f;
    const float q = lum * (1.0f + sat);
    const float p = 2.0f * lum - q;

    auto hue2rgb = [](float pp, float qq, float t) {
        if (t < 0.0f) t += 1.0f;
        if (t > 1.0f) t -= 1.0f;
        if (t < 1.0f / 6.0f) return pp + (qq - pp) * 6.0f * t;
        if (t < 1.0f / 2.0f) return qq;
        if (t < 2.0f / 3.0f) return pp + (qq - pp) * (2.0f / 3.0f - t) * 6.0f;
        return pp;
    };
    const BYTE r = static_cast<BYTE>(hue2rgb(p, q, hue + 1.0f / 3.0f) * 255.0f + 0.5f);
    const BYTE g = static_cast<BYTE>(hue2rgb(p, q, hue) * 255.0f + 0.5f);
    const BYTE b = static_cast<BYTE>(hue2rgb(p, q, hue - 1.0f / 3.0f) * 255.0f + 0.5f);
    return Gdiplus::Color(255, r, g, b);
}

// ---- 浅色 / 深色 --------------------------------------------------------
//
// 判据：量一量「当前应用」的平均亮度 —— 亮就是浅色应用，暗就是深色应用。
//
// ★ 为什么不用系统的「应用主题」：Windows 那个开关（AppsUseLightTheme）是
//   **全局**一份，不是按应用给的。而用户要的就是「浅色应用配浅色、深色应用配深色」，
//   只能自己看。
//
// ★ 量哪儿：前台窗口**自己**顶上那一条（标题栏）。
//   这才叫「看这个应用是什么颜色」—— 窗口摆在屏幕哪、是不是最大化，都不影响。
//   一开始我量的是「条子下面那一条」，那个做法只有在应用**最大化**时才看得到应用
//   （顶栏是占位的，最大化的窗口从它下面开始画），窗口一小，量到的就全是壁纸：
//   桌面上摆着深蓝壁纸 + 开着一个浅色窗口，那条子会一直是深色的，不对。
//   所以现在优先量窗口自己；前台是桌面（没开窗口）时才退回量壁纸。
//
// ★ 为什么要取一段平均：标题栏上混着图标、文字、最小化按钮，
//   单点采样会撞上某一块，亮度上下乱跳。整条平均下来稳。
bool SampleRectFor(HWND fg, int barBottom, int sw, int sh, RECT& rc) {
    RECT r = {};
    bool usable = false;
    if (fg && IsWindowVisible(fg) && !IsIconic(fg)) {
        wchar_t cls[64] = {};
        GetClassNameW(fg, cls, 64);
        // 桌面、任务栏、我们自己的窗口都不算「应用」，量它们等于量壁纸/量自己
        if (wcsncmp(cls, L"MyDock", 6) != 0 && _wcsicmp(cls, L"Progman") != 0 &&
            _wcsicmp(cls, L"WorkerW") != 0 && _wcsicmp(cls, L"Shell_TrayWnd") != 0 &&
            _wcsicmp(cls, L"Shell_SecondaryTrayWnd") != 0) {
            if (GetWindowRect(fg, &r) && (r.right - r.left) >= TdFree(240)) usable = true;
        }
    }

    const int hh = TdFree(20);   // 标题栏的高度量级，再往下就吃到内容区了
    if (usable) {
        rc.left = std::max<LONG>(r.left + 8, 0);
        rc.right = std::min<LONG>(r.right - 8, sw);
        // 窗口比顶栏还高（最大化就是这样，它的矩形顶上还比 0 更靠上），
        // 那就从顶栏**下面**开始量 —— 那一段才是它的标题栏。
        // 这一步同时也挡住了「量到自己身上」：顶栏是置顶的，窗口压不住它。
        rc.top = std::max<LONG>(r.top + TdFree(6), barBottom + TdFree(2));
        rc.bottom = rc.top + hh;
        if (rc.right - rc.left < TdFree(120)) return false;
        if (rc.bottom > sh) return false;
        return true;
    }

    // 前台是桌面：量顶栏下面那条，也就是壁纸 —— 深色壁纸配深色栏，同样讲得通
    rc.left = 0;
    rc.right = sw;
    rc.top = barBottom + TdFree(2);
    rc.bottom = rc.top + hh;
    return rc.bottom <= sh;
}

double SampleLuminance(const RECT& rc) {
    const int y = rc.top;
    const int w = rc.right - rc.left;
    const int hh = rc.bottom - rc.top;
    if (w <= 0 || hh <= 0 || y < 0) return -1.0;

    Gdiplus::Bitmap* shot = CaptureScreenArea(rc.left, y, w, hh);
    if (!shot) return -1.0;

    double lum = -1.0;
    Gdiplus::Rect all(0, 0, shot->GetWidth(), shot->GetHeight());
    Gdiplus::BitmapData data = {};
    if (shot->LockBits(&all, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) ==
        Gdiplus::Ok) {
        double sum = 0.0;
        long long cnt = 0;
        const BYTE* base = static_cast<const BYTE*>(data.Scan0);
        for (UINT row = 0; row < data.Height; ++row) {
            const BYTE* line = base + static_cast<size_t>(row) *
                                      static_cast<size_t>(data.Stride);
            // 隔 4 个像素取一个：一条 24 行高的带子，取密了没有意义，只是白算
            for (UINT x = 0; x < data.Width; x += 4) {
                const BYTE* p = line + static_cast<size_t>(x) * 4;
                // 32bppARGB 在内存里是 B,G,R,A
                sum += 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
                ++cnt;
            }
        }
        shot->UnlockBits(&data);
        if (cnt > 0) lum = sum / static_cast<double>(cnt) / 255.0;
    }
    delete shot;
    return lum;
}

// 两套颜色之间插值：t=0 取浅色那套，t=1 取深色那套。
// 主题是**渐变**过去的（见 TopBar::dark_），所以每一处颜色都得能取中间值。
Gdiplus::Color Mix(Gdiplus::Color a, Gdiplus::Color b, float t) {
    auto lerp = [t](BYTE x, BYTE y) {
        const float v = static_cast<float>(x) +
                        (static_cast<float>(y) - static_cast<float>(x)) * t;
        return static_cast<BYTE>(v + 0.5f);
    };
    return Gdiplus::Color(lerp(a.GetA(), b.GetA()), lerp(a.GetR(), b.GetR()),
                          lerp(a.GetG(), b.GetG()), lerp(a.GetB(), b.GetB()));
}

// 文字的墨色：浅色底上是近黑，深色底上是近白
Gdiplus::Color TextInk(float dark, BYTE alpha) {
    return Mix(Gdiplus::Color(alpha, 26, 26, 28),
               Gdiplus::Color(alpha, 238, 238, 243), dark);
}

// ---- 右侧那排状态图标 ----------------------------------------------------
//
// 参考图（用户发的 MyDockFinder 截图）里右边是这样一排，从左到右：
//
//     蓝牙  Wi-Fi  显示器  电池  音量  ⏏  搜索  控制中心        →  时间
//
// ★ 2026-10-10 换了实现。以前是手画 GDI+ 矢量（坐标用 0..1 归一化），
//   比例靠对着参考图数像素，线条怎么调都差口气 —— 用户原话：
//   「你这画的还是不太得劲，要不你到网上找找有没有 mac 的图标」。
//   现在改用 Lucide 的 SVG 矢量数据（ISC 许可），解析器在 svgicon.cpp。
//
//   为什么不用 SF Symbols：那是 Apple 的版权资产，许可明确不允许
//   这样跨平台扒用。Lucide 是 Feather 的社区续作，同为 24 格、2px 线、
//   round cap/join 的线性风格，最接近，而且许可干净（可商用）。
//
//   状态仍然全是**真问系统**的（见 status.h）：Wi-Fi 有几格、电池灌多少、
//   喇叭带不带弧、静音画不画叉 —— 换成选不同的图标名，比手画省事也更准。
enum IconId {
    kIcBt = 0,
    kIcWifi,
    kIcDisplay,
    kIcBattery,
    kIcVolume,
    kIcIme,
    kIcEject,
    kIcSearch,
    kIcToggles,
    kIconCount
};

// dark = 0 浅色底 / 1 深色底（渐变用，见 TopBar::dark_）
Gdiplus::Color IconInk(bool dimmed, float dark) {
    // 深灰而不是纯黑：纯黑在玻璃底上会显得比周围的字更「重」。
    // 深色模式下反过来用近白 —— 不然整条转深之后图标会一起沉进背景里。
    return Mix(Gdiplus::Color(dimmed ? 118 : 236, 28, 28, 32),
               Gdiplus::Color(dimmed ? 132 : 236, 240, 240, 245), dark);
}

// 线宽按 24 格算（Lucide 的规格是 viewBox 24、stroke 2）。
// 我们这排图案 26 物理 px 上下，2.7 换算过去 ≈ 3.4 物理 px ——
// 跟参考图里量到的 3.5 px 基本一致，比 Lucide 默认的 2 粗一档，
// 因为在这么小的尺寸下细线会发虚。
constexpr float kIconStrokeIn24 = 2.7f;

// 电池的横向拉伸系数。Lucide 原版是 1.57:1，用户要「长一点，
// 不然电量剩余没有参考性」，拉到 1.3 倍 ≈ 2:1（和 macOS / Windows
// 的电池一个感觉），同时内区够长，剩三成还是六成一眼能读出来。
constexpr float kBatterySx = 1.30f;

// 画第 id 个图标。cx/cy 是中心，s 是「24 格映射到多大」。
void DrawStatusIcon(Gdiplus::Graphics& g, int id, float cx, float cy, float s,
                    const SysStatus& st, float dark) {
    SvgIcon ic;

    switch (id) {
        case kIcBt:
            // 蓝牙符文就直接用 Lucide 的，形状是标准的那一个
            if (LucideIcon("bluetooth", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(!st.btPresent, dark));
            break;
        case kIcWifi: {
            // Lucide 正好给了四档：三条弧 / 两条 / 一条 / 光一个点。
            // 它的几何跟参考图是同一套设计 —— 最外那条弧是
            // `M2 8.82 a15 15 0 0 1 20 0`，弦长 20、半径 15，反解出的圆心
            // 正好落在底下那个点上（我们上一轮手画时量出来的结论也是这个，
            // 只是手画控制不住线条）。
            //
            // 所以这里不用自己画了：按**真实信号档位**挑图标名就行。
            //
            // ★ 但颜色要分开管：「信号不好就把整个图标压下去」是用户点名要的。
            //   只剩一格的时候，那个小弧加一个点看着就像**没画完**；
            //   压成灰色之后它读起来才变成「这个不太好使」。
            //   没连上的当然更灰 —— macOS 也是这套语言，它的模板图标在
            //   不可用状态一律压到 35% 不透明度。
            //   两格以上才算"正常"，用满色。
            const char* n = "wifi-zero";
            bool dim = !st.wifiUp;
            if (st.wifiUp) {
                if (st.wifiBars >= 3)      n = "wifi";
                else if (st.wifiBars == 2) n = "wifi-high";
                else { n = "wifi-low"; dim = true; }   // 一格：形状照实，颜色压下去
            }
            if (LucideIcon(n, ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(dim, dark));
            break;
        }
        case kIcDisplay:
            if (LucideIcon("monitor", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(false, dark));
            break;
        case kIcIme:
            // 输入法槽位：languages 图形 + 右边的「中 / A」标签（见 Render）
            if (LucideIcon("languages", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(false, dark));
            break;
        case kIcBattery: {
            if (!st.hasBattery) return;          // 台式机：这一格留空

            // ① 外框 + 正极。横向拉长（kBatterySx），内区才够长，
            //    一眼才读得出剩几成 —— 用户要的「有参考性」就是这个。
            if (LucideIcon("battery", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(false, dark),
                        kBatterySx, 1.0f);

            // ② 里面画什么，看供电状态。
            //    判据是 onAC 而不是 charging —— 电池充满之后系统会把
            //    「正在充电」这一位摘掉，但电源明明还插着。
            const bool plugged = st.charging || st.onAC;
            if (plugged) {
                // 接着电源：画闪电（macOS 也是这么表达的，比插头简洁）。
                // 闪电单独画，圆心对到**电池框的中心** —— 电池框在 24 格里
                // 是 x 2.7..18.7、y 6..18，所以中心是 (10.7, 12)。
                float bx, by;
                SvgMap(cx, cy, s, kBatterySx, 1.0f, 10.7f, 12.0f, bx, by);
                SvgIcon bolt;
                // 0.35：zap 在 24 格里占 19.5 格高，缩到 0.35 倍正好填满
                //       电池内区的大半（电池内区只有 7.6 格高）
                if (LucideIcon("bolt", bolt))
                    SvgDraw(g, bolt, bx, by, s * 0.35f, kIconStrokeIn24,
                            IconInk(false, dark), kBatterySx, 1.0f);
            } else if (st.pct > 0) {
                // 用电池：按百分比灌电量条。
                // 做法是「整条先画一遍、再用 clip 裁出百分比那一段」。
                float x0, y0, x1, y1;
                SvgMap(cx, cy, s, kBatterySx, 1.0f, 4.8f, 8.2f, x0, y0);
                SvgMap(cx, cy, s, kBatterySx, 1.0f,
                       4.8f + 12.4f * (static_cast<float>(st.pct) / 100.0f),
                       15.8f, x1, y1);
                SvgIcon lvl;
                if (x1 - x0 > 1.0f && LucideIcon("battery-level", lvl)) {
                    Gdiplus::Color fc = IconInk(false, dark);
                    if (st.pct <= 10) fc = Pal(dark > 0.5f).danger;  // 低电量给个红
                    const Gdiplus::GraphicsState gs = g.Save();
                    g.SetClip(Gdiplus::RectF(x0, y0, x1 - x0, y1 - y0));
                    SvgDraw(g, lvl, cx, cy, s, kIconStrokeIn24, fc, kBatterySx, 1.0f);
                    g.Restore(gs);
                }
            }
            break;
        }
        case kIcVolume: {
            // 喇叭 + 声波，四档按**真实音量**选：静音 / 低 / 中 / 高。
            //
            // ★ 音量 0 也算「没声音」，跟静音一样画那个叉。
            //   实测这台机器的默认播放设备就是「静音关着、音量滑到 0」，
            //   只认 muted 的话会画出一个带着声波弧的喇叭 —— 看着很正常，
            //   其实一点声音都没有。
            // Lucide 只有三档：无弧（volume）/ 一条弧 / 两条弧。
            // ★ 不要用「无弧」那一档 —— 光秃秃一个喇叭看着像坏了。
            //   有声就用带弧的，音量越大弧越多；静音才画叉。
            const char* n = "volume-2";
            if (st.volKnown) {
                if (st.muted || st.volume <= 0)  n = "volume-x";
                else if (st.volume < 50)         n = "volume-1";
                else                             n = "volume-2";
            }
            if (LucideIcon(n, ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(!st.volKnown, dark));
            break;
        }
        case kIcEject:
            // ⏏ 到现在也没接功能（看不出参考图里它是干什么的），
            //   所以只画形状、点了是空动作 —— 不做成一个猜错的功能。
            if (LucideIcon("eject", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(false, dark));
            break;
        case kIcSearch:
            if (LucideIcon("search", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(false, dark));
            break;
        case kIcToggles:
            // 控制中心。参考图里那个图标是 MyDockFinder 自己画的「两个开关」，
            // Lucide 里没有一模一样的，用滑块（sliders-horizontal）——
            // 表意同样是「调设置」，风格也统一。
            if (LucideIcon("sliders-horizontal", ic))
                SvgDraw(g, ic, cx, cy, s, kIconStrokeIn24, IconInk(false, dark));
            break;
        default:
            break;
    }
}

}  // namespace

TopBar::~TopBar() {
    for (Gdiplus::Bitmap* b : trayIcons_) delete b;
    trayIcons_.clear();
    if (hwnd_) {
        KillTimer(hwnd_, kTimerTick);
        KillTimer(hwnd_, kTimerAnim);
        // 先注销 AppBar 再销毁窗口：不然系统手里会留着一个已经死掉的句柄，
        // 那块工作区就再也还不回来了（桌面上永远缺一条）。
        UnregisterAppBar();
        DestroyWindow(hwnd_);
    }
    delete backdrop_;
    if (dc_) DeleteDC(dc_);
    if (bmp_) DeleteObject(bmp_);
}

bool TopBar::Create(HINSTANCE inst) {
    inst_ = inst;

    if (!ConfigFlag(L"topbar", L"enabled", true)) {
        Log(L"顶部状态栏：settings.ini 里关着，不创建");
        return false;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc)) {
            Log(L"顶部状态栏：注册窗口类失败，错误码 %lu", GetLastError());
            return false;
        }
        registered = true;
    }

    // ★ 主屏的 DPI 和宽度。状态栏只出现在主屏（AppBar 也是登记在主屏上），
    //   所以不能用 SM_CXSCREEN —— 那个在多显示器下确实是主屏宽度，但配合
    //   Dpi()（系统 DPI）用时，一旦主屏缩放 ≠ 系统 DPI，条子的高度就差一截。
    const MonitorInfo prim = PrimaryMonitor();
    dpi_ = prim.dpi;
    g_topDpi = prim.dpi;
    w_ = prim.full.right - prim.full.left;
    h_ = Td(26);

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
                            kClass, L"MyDock 状态栏", WS_POPUP,
                            0, 0, w_, h_, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log(L"顶部状态栏：创建窗口失败，错误码 %lu", GetLastError());
        return false;
    }

    CreateBackBuffer(w_, h_);
    RefreshTexts();
    PollSysStatus(sys_);            // 第一帧就要有电池/音量/信号，不能等半秒

    // ★ 先把自己登记成 AppBar（= 屏幕顶上的「任务栏」），再决定站哪儿。
    //
    //   顺序很讲究：登记之前，系统的工作区还是整屏，我们这时候 SetWindowPos
    //   只是「把自己摆到 0,0」；登记之后由 AppBar 机制接管位置，它给的位置
    //   才是真正不跟别人打架的那个（比如任务栏被挪到顶上时，它会让我们往下让）。
    reserve_ = ConfigFlag(L"topbar", L"reserve", true);
    if (reserve_) RegisterAppBar();
    if (!appbar_) {
        // 登记失败（或者 settings.ini 里 reserve=0）：退回「浮在最上面」的老样子。
        // 能用，但最大化的窗口和桌面图标会被我们压掉顶上一条。
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, w_, h_, SWP_NOACTIVATE);
        barX_ = 0;
        barY_ = 0;
    }

    // settings.ini 里可以把主题锁死：[topbar] theme=auto|light|dark
    {
        wchar_t buf[16] = {};
        const std::wstring ini = ConfigDir() + L"\\settings.ini";
        GetPrivateProfileStringW(L"topbar", L"theme", L"auto", buf, 16, ini.c_str());
        if (_wcsicmp(buf, L"light") == 0) themeForce_ = 1;
        else if (_wcsicmp(buf, L"dark") == 0) themeForce_ = 2;
        else themeForce_ = 0;
    }

    UpdateTheme(true);              // 先定下浅色/深色，再拍第一帧，免得「白变黑」被看见
    RequestRedraw(true);            // 第一次把背景拍下来，玻璃才有东西可透
    SetTimer(hwnd_, kTimerTick, 500, nullptr);

    Log(L"顶部状态栏就绪：%dx%d，位置 (%d,%d)，AppBar=%s",
        w_, h_, barX_, barY_, appbar_ ? L"开" : L"关");
    return true;
}

void TopBar::CreateBackBuffer(int w, int h) {
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }

    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (bmp_) SelectObject(dc_, bmp_);
    ReleaseDC(nullptr, screen);
}

// ★ 拍屏之前必须先把自个儿藏起来，否则拍到的就是自己上一帧
//   （半透明白底 + 文字），糊完再当背景贴上去，会是一层鬼影。
//   所以：藏 → 等一次 DWM 合成（不然 Hide 还没落到屏幕上）→ 拍 → 亮回来。
void TopBar::CaptureBackdrop() {
    delete backdrop_;
    backdrop_ = nullptr;
    if (!hwnd_ || w_ <= 0 || h_ <= 0) return;

    const bool wasVisible = !hidden_;
    ShowWindow(hwnd_, SW_HIDE);
    if (wasVisible) DwmFlush();     // 等窗口真的从屏幕消失

    Gdiplus::Bitmap* shot = CaptureScreenArea(barX_, barY_, w_, h_);
    if (shot) backdrop_ = BlurBitmap(shot, 6);   // 会接管并释放 shot

    if (wasVisible) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd_, HWND_TOPMOST, barX_, barY_, w_, h_,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    lastBackdrop_ = GetTickCount();
}

// 重新取一遍要显示的文字。返回 true 表示有变化、得重画。
//
// ★ 时间字符串是手拼的，不是 swprintf 出来的。
//   MinGW 的 swprintf 走标准 C 的语义：%s 指**窄**字符串，而我们的星期名是
//   wchar_t*。于是 "周五" 被当成 char* 一个字节一个字节地往宽串里塞，
//   屏幕上就出来一串鬼画符（用户看到的「奇怪的字符」就是这个）。
//   要么写 %ls，要么干脆别用格式化 —— 这里选手拼，绕开整个坑。
bool TopBar::RefreshTexts() {
    bool changed = false;

    SYSTEMTIME t = {};
    GetLocalTime(&t);
    auto two = [](int v) {
        return v < 10 ? (std::wstring(L"0") + std::to_wstring(v)) : std::to_wstring(v);
    };
    std::wstring clk = std::to_wstring(t.wMonth) + L"月" + std::to_wstring(t.wDay) + L"日 " +
                       kWeekName[t.wDayOfWeek % 7] + L" " + two(t.wHour) + L":" + two(t.wMinute);
    if (clockText_ != clk) {
        clockText_ = clk;
        changed = true;
    }

    // 前台是咱自己的窗口时（系统菜单刚弹出来）不更新 —— 名字保持上一个应用，
    // 不然菜单一开，左边就跳成 "MyDock"。
    std::wstring app;
    if (ForegroundAppName(app) && appText_ != app) {
        // ★ 换应用了：旧名字留着做交叉淡出，圆环的颜色也渐变过去。
        //   直接换掉的话，文字和颜色都是一瞬间跳过去，很生硬。
        appPrev_ = appText_;
        appText_ = app;
        appFade_ = 0.0f;

        const Gdiplus::Color c = AccentForApp(app);
        accTR_ = static_cast<float>(c.GetR());
        accTG_ = static_cast<float>(c.GetG());
        accTB_ = static_cast<float>(c.GetB());
        EnsureAnim();
        changed = true;
    }
    return changed;
}

// 渐变：用指数逼近，跟 Dock 那边的悬停高亮同一套手感。
// 返回 true 表示还在动 —— 只要还有一丁点差就得继续开定时器，不然会卡在中途。
bool TopBar::StepFade(double dtMs) {
    const float k = static_cast<float>(1.0 - std::exp(-dtMs / 90.0));
    bool moving = false;

    if (appFade_ < 1.0f) {
        appFade_ = std::min(1.0f, appFade_ + static_cast<float>(dtMs / 260.0));
        if (appFade_ < 1.0f) moving = true;
    }

    auto approach = [&](float& cur, float target) {
        const float d = target - cur;
        if (std::fabs(d) < 1.0f) {
            cur = target;
            return false;
        }
        cur += d * k;
        return true;
    };
    if (approach(accR_, accTR_)) moving = true;
    if (approach(accG_, accTG_)) moving = true;
    if (approach(accB_, accTB_)) moving = true;

    // 浅色 ↔ 深色。这一路是 0~1 的小数量级，不能用上面那个「差不到 1 就算到了」
    // 的判据（那会让 dark_ 在 0.4 上就停下），单独给一个更小的容差。
    {
        const float dd = darkTarget_ - dark_;
        if (std::fabs(dd) > 0.004f) {
            dark_ += dd * k;
            moving = true;
        } else {
            dark_ = darkTarget_;
        }
    }

    return moving;
}

// 量一下当前应用有多亮，决定整条转浅还是转深。
//
// ★ 回差：0.42 以下转深，0.55 以上转浅，中间那一段保持原样。
//   没有这段回差的话，正好卡在临界点上的窗口（比如上白下灰的那种）会每半秒
//   翻一次 —— 整条一直在深浅之间闪，比不变还难受。
// ★ 量哪儿见 SampleRectFor 上面的长注释；rc/lum 只在「真的采样了」时才有意义，
//   所以日志得分开写两种。
void TopBar::UpdateTheme(bool immediate) {
    if (!hwnd_ || hidden_) return;
    lastLumCheck_ = GetTickCount();

    bool want = darkMode_;
    RECT rc = {};
    double lum = -1.0;

    if (themeForce_) {
        // settings.ini 里锁死了（[topbar] theme=light|dark），不采样
        want = (themeForce_ == 2);
    } else {
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        if (!SampleRectFor(GetForegroundWindow(), barY_ + h_, sw, sh, rc)) return;
        lum = SampleLuminance(rc);
        if (lum < 0.0) return;          // 采不到（拍屏失败之类）：保持现状，别乱切

        if (lum < 0.42) want = true;
        else if (lum > 0.55) want = false;
    }

    if (want == darkMode_ && !immediate) return;

    darkMode_ = want;
    darkTarget_ = want ? 1.0f : 0.0f;
    if (immediate) {
        dark_ = darkTarget_;            // 开机第一帧就位，别让人看见一条白慢慢变黑
        return;
    }
    if (lum >= 0.0) {
        Log(L"顶部状态栏：采样 (%d,%d)-(%d,%d) 平均亮度 %.2f —— 转%s",
            rc.left, rc.top, rc.right, rc.bottom, lum, want ? L"深色" : L"浅色");
    } else {
        Log(L"顶部状态栏：[topbar] theme 锁成%s", want ? L"深色" : L"浅色");
    }
    EnsureAnim();
}

bool TopBar::HitLogo(int x, int y) const {
    // 命中区比那个圆环本身大一圈（就是悬停时高亮的那块），
    // 不然得精准戳在直径十几像素的环上，很难点中。
    return x >= Td(5) && x <= Td(5) + Td(26) && y >= 0 && y < h_;
}

// 右侧那排状态图标。位置在这儿算一次存进成员 —— 鼠标点击时直接用缓存，
// 不用（也没法）再量一遍时钟文字的宽度。
//
// 间距照参考图：图案本身 26 物理 px（15 逻辑 px），相邻图案之间空 31 物理 px
// （18 逻辑 px）—— 也就是中心距 33 逻辑 px。比 macOS 疏一些，但这是用户给的样子。
//
// ★ 排布按「每格自己的宽度」来，不是固定的中心距：电池那格要额外挂一段
//   「80%」的数字，格子和别的就不一样宽了。按固定中心距排的话，数字会顶到
//   隔壁的音量图标上。所以从右往左一格格推：右边缘对齐、格子间留固定的空隙。
void TopBar::DrawIconRow(Gdiplus::Graphics& g, float textLeft) {
    // ★ glyph 的语义变了：现在是「SVG 的 24 格映射到多大」，不再是「图案边长」。
    //   Lucide 的图标内容大概占 20/24 格（四周留了设计边距），所以
    //   Td(17) 换过去大约等于 25 物理 px 的图案 —— 和参考图量到的 26 一致。
    const float glyph = static_cast<float>(Td(17));    // 24 格 → 多少像素
    const float gapI  = static_cast<float>(Td(16));    // 相邻两格之间的视觉空隙
    const float gapEnd= static_cast<float>(Td(26));    // 最右一格 → 时钟文字

    iconGlyph_ = glyph;
    iconCy_    = static_cast<float>(h_) * 0.5f;
    iconHit_   = static_cast<float>(Td(30));           // 悬停高亮 / 命中方框
    iconCount_ = kIconCount;
    // 图标数组容量在编译期钉死 —— 以前留了个永远不触发的运行时钳位，
    // 真加超了也是静默截断，不如编译期直接报错
    static_assert(kIconCount <= static_cast<int>(sizeof(iconCX_) / sizeof(iconCX_[0])),
                  "iconCX_ 容量不够：加状态图标时把数组一并扩了");

    // ---- 这一格右边要不要跟一段文字 ----
    // 现在只有电池用：充电（或接着电源）时电池里面是整块灌满的，
    // 看不出还剩几成，所以把数字写在旁边补上。
    iconLabel_[kIcBattery].clear();
    if (sys_.hasBattery && (sys_.charging || sys_.onAC)) {
        iconLabel_[kIcBattery] = std::to_wstring(sys_.pct) + L"%";
    }

    // 2026-10-12 - AI - 输入法槽：显示**前台窗口**的键盘布局语言。
    // 中文布局（简体 0x0804 / 繁体 0x0C04…）显示「中」，其它显示「A」——
    // 跟 macOS 的「拼 / A」一个表达法。查询很便宜（一次 GetKeyboardLayout）。
    {
        const HWND fg = GetForegroundWindow();
        const DWORD tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const HKL hkl = GetKeyboardLayout(tid);
        const WORD lang = LOWORD(reinterpret_cast<UINT_PTR>(hkl));
        iconLabel_[kIcIme] = (PRIMARYLANGID(lang) == LANG_CHINESE) ? L"中" : L"A";
    }

    // 标签用的字号小一号、用跟时钟同一档的墨色，免得喧宾夺主
    Gdiplus::Font fLabel(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Td(12)),
                         Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat lf(Gdiplus::StringFormatFlagsNoWrap);
    lf.SetAlignment(Gdiplus::StringAlignmentNear);
    lf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    const float labelPad = static_cast<float>(Td(4));

    // ---- 每格多宽 ----
    // 大部分格子就是一个图案那么宽；电池那格要多让出「4px + 数字宽度」。
    // 按实际宽度排布，字才不会压到旁边的图标上。
    float patW[kIconCount];
    for (int i = 0; i < iconCount_; ++i) {
        // 0.95 ≈ 图标内容的实际占宽（20/24 格 + 一点线宽）。
        // 按 1.0 算的话每格会多出两三格的空当，整排看着散。
        // 电池横着拉过（kBatterySx），实际占宽要按拉伸后的算。
        patW[i] = (i == kIcBattery) ? glyph * 1.17f : glyph * 0.95f;
    }

    // ---- 从右往左排 ----
    float cursor = textLeft - gapEnd;           // 最右那一格的右边缘
    for (int i = iconCount_ - 1; i >= 0; --i) {
        float w = patW[i];
        if (!iconLabel_[i].empty()) {
            Gdiplus::RectF probe(0.0f, 0.0f, static_cast<Gdiplus::REAL>(Td(300)),
                                 static_cast<Gdiplus::REAL>(h_));
            Gdiplus::RectF ext;
            g.MeasureString(iconLabel_[i].c_str(), static_cast<INT>(iconLabel_[i].size()),
                            &fLabel, probe, &lf, &ext);
            if (ext.Width > 1.0f) w += labelPad + ext.Width;
        }
        iconItemW_[i] = w;
        iconItemL_[i] = cursor - w;
        iconCX_[i] = iconItemL_[i] + patW[i] * 0.5f;   // 图案在格子的左半边
        cursor = iconItemL_[i] - gapI;
    }

    for (int i = 0; i < iconCount_; ++i) {
        if (i == hoverIcon_) {
            // 高亮块盖住「整格」：电池那一格连右边的百分比数字一起亮，
            // 不然鼠标停在数字上时看不出自己在哪一格。
            const float hw = std::max(iconHit_, iconItemW_[i] + static_cast<float>(Td(6)));
            const float hcx = iconItemL_[i] + iconItemW_[i] * 0.5f;
            Gdiplus::GraphicsPath hp;
            AddRoundRect(hp, Gdiplus::RectF(hcx - hw * 0.5f, iconCy_ - iconHit_ * 0.5f,
                                            hw, iconHit_),
                         static_cast<float>(Td(5)));
            // 浅色底上是「压一层黑」，深色底上反过来 —— 得提亮，
            // 不然整条转深之后，鼠标停哪儿都看不出来。
            Gdiplus::SolidBrush hb(Mix(Gdiplus::Color(34, 0, 0, 0),
                                       Gdiplus::Color(52, 255, 255, 255), dark_));
            g.FillPath(&hb, &hp);
        }
        DrawStatusIcon(g, i, iconCX_[i], iconCy_, glyph, sys_, dark_);

        if (!iconLabel_[i].empty()) {
            Gdiplus::SolidBrush tb(TextInk(dark_, 236));
            const float lx = iconItemL_[i] + patW[i] + labelPad;
            Gdiplus::RectF box(lx, 0.0f, iconItemW_[i] - patW[i] - labelPad,
                               static_cast<float>(h_));
            g.DrawString(iconLabel_[i].c_str(), static_cast<INT>(iconLabel_[i].size()),
                         &fLabel, box, &lf, &tb);
        }
    }
}

int TopBar::HitIcon(int x, int y) const {
    if (iconHit_ <= 0.0f || iconCount_ <= 0) return -1;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    const float half = iconHit_ * 0.5f;
    if (fy < iconCy_ - half || fy > iconCy_ + half) return -1;
    for (int i = 0; i < iconCount_; ++i) {
        const float l = iconCX_[i] - half;
        // 带标签的格子（电池）往右多算一段，点到百分比数字也算点电池
        float r = (iconItemW_[i] > iconGlyph_) ? (iconItemL_[i] + iconItemW_[i])
                                               : (iconCX_[i] + half);
        // 别伸到邻居身上
        if (i + 1 < iconCount_ && r > iconCX_[i + 1] - half) r = iconCX_[i + 1] - half;
        if (fx >= l && fx <= r) return i;
    }
    return -1;
}

// 给系统合成一次 Win+X 热键。
//
// ★ 为什么不直接调「打开快速设置」的接口：那套（IUIHost / Shell 的
//   IApplicationActivationManager）要拿 WinRT 的类 id，还得处理各种版本差异；
//   而 Win+S / Win+A 这两个组合键是系统自己定死的入口，按一下最省事也最稳。
//   用 SendInput 而不是老的 keybd_event：后者在 UIPI 下容易被丢掉半个事件。
void TopBar::SendWinKey(WORD vk) {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = vk;
    in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// 右侧那排状态图标点开来的是什么（2026-10-10）
//
// 参考的是 macOS 的「状态菜单」（status menus / menu bar extras）：
//   点 Wi-Fi 看见的是**可用网络的列表**，点蓝牙看见的是**已配对设备的列表**，
//   最底下永远有一条通往对应设置页的入口。
//   苹果那条设计的关键不是「信息多」，而是**每一行的信息都是真的、点下去有反应**。
//
// ★ 所以这里定下两条：
//   1. 列表是**弹出那一刻现扫**的（见 status.h 里那组按需查询），不是写死的假条目。
//   2. 做不到的事就别列 —— 比如「关掉 Wi-Fi 无线电」在 Windows 上要管理员权限、
//      「连一个蓝牙耳机」没有通用 API。这类宁可省掉或改成打开设置页，
//      也不放一个点了没反应的开关。
void TopBar::ShowIconMenu(int id) {
    std::vector<MenuItem> items;

    auto head = [&](const wchar_t* name, const std::wstring& tag) {
        MenuItem it;
        it.title = true;
        it.label = name;
        it.tag   = tag;
        items.push_back(std::move(it));
    };
    // 说明行：跟标题同一套画法（粗灰字、不可点），用来放「当前连的是谁 /
    // 还剩多久 / 没有找到网络」这类一眼要看的信息。
    auto note = [&](const std::wstring& text) {
        MenuItem it;
        it.title = true;
        it.label = text;
        it.tag.clear();
        items.push_back(std::move(it));
    };
    auto sep = [&]() {
        MenuItem it;
        it.separator = true;
        items.push_back(std::move(it));
    };
    auto line = [&](int cmd, const std::wstring& label, bool checked = false,
                    Gdiplus::Bitmap* icon = nullptr) {
        MenuItem it;
        it.cmd     = cmd;
        it.label   = label;
        it.checked = checked;
        it.icon    = icon;
        items.push_back(std::move(it));
    };
    auto pct = [](int v) { return std::to_wstring(v) + L"%"; };

    switch (id) {
        case kIcWifi: {
            menuWifi_ = EnumWifiNetworks(8);
            const std::wstring cur = WifiCurrentSsid();
            head(L"Wi-Fi", sys_.wifiUp ? L"已连接"
                                       : (sys_.wifiPresent ? L"未连接" : L"无无线网卡"));
            if (!cur.empty()) note(L"当前：" + cur);
            sep();
            if (!sys_.wifiPresent) {
                note(L"这台机器没有无线网卡");
            } else if (menuWifi_.empty()) {
                note(L"没有找到可用的网络");
            } else {
                for (size_t i = 0; i < menuWifi_.size(); ++i) {
                    line(kStWifiBase + static_cast<int>(i),
                         menuWifi_[i].ssid, menuWifi_[i].connected);
                }
            }
            sep();
            line(kStSettings, L"网络和 Internet 设置");
            break;
        }

        case kIcBt: {
            menuBt_ = EnumBtDevices(8);
            head(L"蓝牙", sys_.btPresent ? L"可用" : L"无蓝牙适配器");
            sep();
            if (!sys_.btPresent)      note(L"这台机器没有蓝牙");
            else if (menuBt_.empty()) note(L"没有已配对的设备");
            else for (size_t i = 0; i < menuBt_.size(); ++i)
                line(kStBtBase + static_cast<int>(i), menuBt_[i]);
            sep();
            line(kStSettings, L"蓝牙和设备设置");
            break;
        }

        case kIcBattery: {
            if (sys_.hasBattery) {
                head(L"电池", pct(sys_.pct));
                std::wstring s = sys_.charging ? L"正在充电"
                                               : (sys_.onAC ? L"已接通电源" : L"使用电池");
                // 剩余时间只在**用电池**的时候有意义：插着电时系统经常报不出来，
                // 硬凑一个数字出来就是假的。
                const int mins = BatteryMinutesLeft();
                if (mins > 0 && !sys_.onAC) {
                    s += L" · 还剩 " + std::to_wstring(mins / 60) + L" 小时 "
                       + std::to_wstring(mins % 60) + L" 分";
                }
                note(s);
                sep();
            } else {
                head(L"电池", L"无电池");
                note(L"这是一台台式机，没有电池");
                sep();
            }
            line(kStSettings, L"电源和电池设置");
            break;
        }

        case kIcVolume: {
            head(L"声音", sys_.volKnown ? pct(sys_.volume) : L"未知");
            sep();
            // ★ 文字固定写「静音」，用**打勾**表示当前是不是静音 —— 跟 macOS 那行
            //   一个写法：它是个开关，文字跟着状态变的话，这一行就会在
            //   「静音 / 取消静音」之间跳，看不出当前到底是哪个。
            //   真正想要的是滑块，但那要给菜单加一整套滑块控件，见 README。
            line(kStMute, L"静音", sys_.muted);
            sep();
            line(kStSettings, L"声音设置");
            break;
        }

        case kIcDisplay:
            head(L"显示器", L"");
            sep();
            line(kStSettings, L"显示设置");
            line(kStProject,  L"投影（Win+P）");
            break;

        case kIcIme: {
            // 2026-10-13 - AI - 输入法菜单：列出系统**全部**输入法配置
            //（键盘布局 + 搜狗/微软拼音这类输入处理器），勾选当前生效的。
            // 数据源 TSF（ITfInputProcessorProfileMgr::EnumProfiles）——
            // 只有它能给出 TIP 的友好名和可激活的 clsid/guidProfile；
            // HKL 和注册表在这台机器上只有一条「中文-美式键盘」，看不出 IME。
            head(L"输入法", L"");
            sep();
            imeItems_.clear();

            ITfInputProcessorProfiles* ip = nullptr;
            ITfInputProcessorProfileMgr* mgr = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_ITfInputProcessorProfiles,
                                           reinterpret_cast<void**>(&ip))) && ip) {
                ip->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                   reinterpret_cast<void**>(&mgr));
            }
            if (!mgr) {
                if (ip) ip->Release();
                line(0, L"（枚举输入法失败）");
                break;
            }

            // 当前生效参照：TIP 用 GetActiveProfile，键盘布局用前台 HKL
            const HWND fg = GetForegroundWindow();
            const DWORD fgtid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            const HKL fgHkl = GetKeyboardLayout(fgtid);
            TF_INPUTPROCESSORPROFILE act{};
            const BOOL haveAct =
                SUCCEEDED(mgr->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &act));

            IEnumTfInputProcessorProfiles* en = nullptr;
            if (SUCCEEDED(mgr->EnumProfiles(0, &en)) && en) {   // 0 = 全部语言
                TF_INPUTPROCESSORPROFILE p{};
                ULONG fetched = 0;
                while (en->Next(1, &p, &fetched) == S_OK && fetched == 1) {
                    const int idx = static_cast<int>(imeItems_.size());

                    std::wstring desc;
                    bool active = false;
                    if (p.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
                        BSTR b = nullptr;
                        if (SUCCEEDED(ip->GetLanguageProfileDescription(
                                p.clsid, p.langid, p.guidProfile, &b)) && b) {
                            desc = b;
                            SysFreeString(b);
                        }
                        active = haveAct &&
                                 IsEqualCLSID(act.clsid, p.clsid) &&
                                 IsEqualGUID(act.guidProfile, p.guidProfile);
                    } else {
                        // 键盘布局：注册表 Layout Text（键名 = 8 位布局 id）
                        wchar_t id[16] = {};
                        swprintf_s(id, L"%08X", static_cast<DWORD>(p.langid));
                        wchar_t txt[128] = L"";
                        DWORD sz = sizeof(txt);
                        HKEY k = nullptr;
                        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                                L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts",
                                0, KEY_READ, &k) == ERROR_SUCCESS) {
                            HKEY sk = nullptr;
                            if (RegOpenKeyExW(k, id, 0, KEY_READ, &sk) == ERROR_SUCCESS) {
                                RegGetValueW(sk, nullptr, L"Layout Text",
                                             RRF_RT_REG_SZ, nullptr, txt, &sz);
                                RegCloseKey(sk);
                            }
                            RegCloseKey(k);
                        }
                        desc = txt[0] ? txt
                                      : (L"键盘布局 " + std::wstring(id));
                        active = (LOWORD(reinterpret_cast<UINT_PTR>(p.hkl)) ==
                                  LOWORD(reinterpret_cast<UINT_PTR>(fgHkl)));
                    }
                    if (desc.empty()) continue;

                    ImeItem item;
                    item.type = p.dwProfileType;
                    item.langid = p.langid;
                    item.clsid = p.clsid;
                    item.guidProfile = p.guidProfile;
                    item.hkl = p.hkl;
                    imeItems_.push_back(item);
                    line(kImeBase + idx, desc, active);
                }
                en->Release();
            }
            mgr->Release();
            ip->Release();
            if (imeItems_.empty()) line(0, L"（没有找到输入法）");
            break;
        }

        case kIcEject: {
            // 2026-10-12 - AI - 正式成为「托盘」：列出 Windows 托盘里在跑的
            // 应用（输入法、GHelper 这类平常看不见的）。数据源是注册表
            // NotifyIconSettings —— 系统自己的托盘登记处，不依赖任务栏可见
            // （这台机器的任务栏被整体挪到屏幕外了，UIA 走不通）。
            // 点击行为：有窗口就激活（复用微信唤醒那套），没有就拉起 exe。
            // 原来的 MyDock 系统项全部移去左上角圆圈菜单（ShowSystemMenu）。
            head(L"托盘", L"");
            sep();
            // 上一轮的图标位图先清掉（MenuItem 里只是引用，不拥有）
            for (Gdiplus::Bitmap* b : trayIcons_) delete b;
            trayIcons_.clear();
            trayNames_.clear();

            // 缓存直接上（打开秒出）；过期/为空就在后台重走舞步刷新，
            // 本次先提示，下次点开就是新清单
            int n = 0;
            for (const TrayEntry& t : TrayCachedEntries()) {
                if (n >= 10) {   // 长度限制：用户点名要的
                    Log(L"托盘：%zu 项在跑，只显示前 10 个", TrayCachedEntries().size());
                    break;
                }
                // 展示名截断 —— NVIDIA 那种全名能把菜单撑到一半屏幕宽
                std::wstring label = t.name;
                if (label.size() > 30) label = label.substr(0, 30) + L"…";

                // 行首图标 = 应用真图标（从 exe 提取 32px 转位图）
                HICON ic = nullptr;
                ExtractIconExW(t.exePath.c_str(), 0, nullptr, &ic, 1);
                Gdiplus::Bitmap* bmp = ic ? BitmapFromIcon(ic) : nullptr;
                if (ic) DestroyIcon(ic);
                if (bmp) trayIcons_.push_back(bmp);

                line(kStTrayBase + n, label, false, bmp);
                trayNames_.push_back(t.name);   // 匹配用完整名
                ++n;
            }
            if (trayNames_.empty()) {
                TrayRefreshAsync();
                MenuItem idle;
                idle.cmd = 0;              // 点了没反应（cmd 0 = 取消语义）
                idle.label = L"（托盘列表整理中，稍后再点一次）";
                items.push_back(std::move(idle));
            } else if (TrayCacheAgeMs() > 90 * 1000) {
                TrayRefreshAsync();        // 过期就在后台刷新，供下次使用
            }
            break;
        }

        case kIcSearch:
            // ★ 这两个**不弹菜单**：macOS 上点 Spotlight 是直接开搜索框、
            //   点控制中心是直接开那块面板，它们本来就不是菜单。
            //   硬给它们套一个菜单，是把「点一下就该出结果」变成「点两下」。
            SendWinKey(L'S');
            return;

        case kIcToggles:
            // 2026-10-11 - AI - 不再唤系统的 Win+A 磁贴，弹自己的控制中心
            // （系统那套改不了样式，用户点名重做；能落地的功能见 ccenter.cpp）
            ccenter_.Toggle(inst_, static_cast<int>(iconCX_[kIcToggles]), h_,
                            dark_ > 0.5f, sys_);
            hoverIcon_ = -1;
            Render();
            return;

        default:
            return;
    }

    if (items.empty()) return;

    // 宽度得先量出来，才知道要往左挪多少。
    //
    // ★ 状态菜单要**右对齐**到那个图标（菜单右边跟图标右边齐平），
    //   不是左上角贴在图标上 —— 图标在屏幕最右边，左上角贴过去的话
    //   菜单会整块跑到屏幕外面，被 Show() 里的防越界硬拽回来，落点就歪了。
    int mw = 0, mh = 0, rh = 0;
    MeasureMenu(items, mw, mh, rh);
    if (mw <= 0) return;

    const float right = iconCX_[id] + iconHit_ * 0.5f;
    POINT pt = { static_cast<LONG>(right - static_cast<float>(mw)), static_cast<LONG>(h_) };
    ClientToScreen(hwnd_, &pt);

    const int cmd = menu_.Show(hwnd_, pt.x, pt.y - Td(2), items);
    hoverIcon_ = -1;
    Render();
    if (cmd != 0) RunIconMenuCommand(id, cmd);
}

void TopBar::RunIconMenuCommand(int id, int cmd) {
    auto open = [](const wchar_t* what) {
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", what, nullptr, nullptr,
                                          SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            Log(L"顶部状态栏：打不开 %s（返回 %d）", what,
                static_cast<int>(reinterpret_cast<INT_PTR>(r)));
        }
    };

    // ---- 点了某个输入法 ----
    if (cmd >= kImeBase) {
        const size_t i = static_cast<size_t>(cmd - kImeBase);
        if (i >= imeItems_.size()) return;
        const ImeItem& it = imeItems_[i];

        if (it.type == TF_PROFILETYPE_INPUTPROCESSOR) {
            // 输入处理器（搜狗/微软拼音）：TSF 激活它的 profile。
            // DONTCARECURRENTINPUTLANGUAGE：目标语言和当前输入语言不同也照切。
            ITfInputProcessorProfileMgr* mgr = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_ITfInputProcessorProfileMgr,
                                           reinterpret_cast<void**>(&mgr))) && mgr) {
                const HRESULT hr = mgr->ActivateProfile(
                    TF_PROFILETYPE_INPUTPROCESSOR, it.langid, it.clsid,
                    it.guidProfile, nullptr,
                    TF_IPP_FLAG_DONTCARECURRENTINPUTLANGUAGE);
                Log(L"输入法：ActivateProfile → 0x%08lX",
                    static_cast<unsigned long>(hr));
                mgr->Release();
            }
            return;
        }

        // 键盘布局：经典通道投给前台窗口
        if (it.hkl) {
            const HWND fg = GetForegroundWindow();
            if (fg) PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0,
                                 reinterpret_cast<LPARAM>(it.hkl));
        }
        return;
    }

    // ---- 点了某个托盘应用 ----
    // 后台重走舞步（亮屏外任务栏 → 开溢出区 → 按名 Invoke）——
    // 触发的是应用**自己的托盘点击行为**，和手点托盘完全一致
    if (cmd >= kStTrayBase) {
        const size_t i = static_cast<size_t>(cmd - kStTrayBase);
        if (i >= trayNames_.size()) return;
        TrayInvokeByNameAsync(trayNames_[i]);
        return;
    }

    // ---- 点了某个 Wi-Fi 网络 ----
    if (cmd >= kStWifiBase && cmd < kStBtBase) {
        const size_t i = static_cast<size_t>(cmd - kStWifiBase);
        if (i >= menuWifi_.size()) return;
        const WifiNet n = menuWifi_[i];   // 拷一份：下面有可能改到菜单内容

        if (n.connected) {
            Log(L"Wi-Fi：本来就连着「%ls」，不用重连", n.ssid.c_str());
            return;
        }
        if (n.profile.empty()) {
            // 没连过的网络要输密码，我们没有地方让用户输 —— 别假装能连，
            // 直接把系统的网络页打开，那儿的流程本来就是干这个的。
            Log(L"Wi-Fi：「%ls」没存过配置（要密码），打开设置页", n.ssid.c_str());
            open(L"ms-settings:network-wifi");
            return;
        }
        ConnectWifi(n.profile);
        return;
    }

    // ---- 点了某个蓝牙设备 ----
    // 2026-10-11 - AI - 不再只开设置页：先试着真连（BluetoothSetServiceState
    // 启用音频服务，见 status.cpp 里 ConnectBtDevice 的说明），连不上再退回设置页。
    if (cmd >= kStBtBase) {
        const size_t i = static_cast<size_t>(cmd - kStBtBase);
        if (i < menuBt_.size() && ConnectBtDevice(menuBt_[i])) {
            // 连接是异步的，耳机响还要一两秒。把状态栏刷一遍，
            // 一会儿轮询也会发现「已连接」并更新图标颜色。
            PollSysStatus(sys_);
            Render();
            return;
        }
        Log(L"蓝牙：设备发起连接失败，打开设置页");
        open(L"ms-settings:bluetooth");
        return;
    }

    switch (cmd) {
        case kStSettings:
            // 同一个命令号，开哪一页看是**哪个图标**弹的菜单
            switch (id) {
                case kIcWifi:    open(L"ms-settings:network-wifi"); break;
                case kIcBt:      open(L"ms-settings:bluetooth");    break;
                case kIcDisplay: open(L"ms-settings:display");      break;
                case kIcBattery: open(L"ms-settings:batterysaver"); break;
                case kIcVolume:  open(L"ms-settings:sound");        break;
                default: break;
            }
            break;

        case kStMute:
            if (SetMuted(!sys_.muted)) {
                // ★ 改完立刻自己问一遍并重画：轮询是 500ms 一次，
                //   等它自己发现的话，喇叭图标要慢半拍才变。
                PollSysStatus(sys_);
                Render();
            }
            break;

        case kStShowDock: {
            // Dock 窗口类名固定，投一条「显示」过去 —— 别直接动它的窗口，
            // 可见性/动画状态归它自己管
            const HWND dock = FindWindowW(L"MyDockWindowClass", nullptr);
            if (dock) PostMessageW(dock, WM_APP + 45, 0, 0);
            break;
        }
        case kStAutoStart:
            SetAutoStart(!IsAutoStart());
            break;
        case kStRestartDock:
            // 安排 1 秒后拉起新进程，然后按 kCmdQuit 的路数收场
            QueueRestart();
            DestroyWindow(hwnd_);   // 顶栏自己退；Dock 在 kCmdQuit 的同款链路里收尾
            // fallthrough 不行 —— 顶栏和 Dock 是两个窗口，各自退出
            {
                const HWND dock = FindWindowW(L"MyDockWindowClass", nullptr);
                if (dock) PostMessageW(dock, WM_CLOSE, 0, 0);
            }
            break;
        case kStProject:  SendWinKey(L'P'); break;
        case kStComputer: open(L"shell:MyComputerFolder"); break;
        case kStDiskMgmt: open(L"diskmgmt.msc"); break;
        default: break;
    }
}

void TopBar::Render() {
    if (!dc_ || !bits_ || w_ <= 0 || h_ <= 0) return;

    memset(bits_, 0, static_cast<size_t>(w_) * static_cast<size_t>(h_) * 4);

    Gdiplus::Graphics g(dc_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    // ---- ① 毛玻璃：身后那块屏幕，模糊过的 ----
    //
    // 之前这里是一层纯色，用户一眼就看出「廉价的塑料感」—— 因为它就是一张
    // 灰色的板子，壁纸的颜色一点都透不过来。macOS 的菜单栏好看，靠的就是
    // 这层模糊：颜色从底下漫上来，深浅跟着壁纸走。
    if (backdrop_) {
        // ★ 先提一波饱和度再贴。
        //   光模糊的话，壁纸的颜色会被糊成一团灰 —— 这是「塑料感」的第二层来源。
        //   macOS 的 vibrancy 会把透出来的颜色**加强**再显示，所以底下哪怕是一张
        //   淡蓝的壁纸，菜单栏也是明确带蓝的，而不是灰白。下面这组系数就是
        //   标准的饱和度矩阵（s≈1.35，按人眼对 RGB 敏感度的权重来的）。
        // ★ 注意：MinGW 的 GDI+ 头里 ColorMatrix 就是一块 float m[5][5]，
        //   没有 m00/m01 那种成员名（那是 MSVC 的写法）。得用下标赋。
        Gdiplus::ColorMatrix cm = {};
        cm.m[0][0] = 1.276f; cm.m[0][1] = -0.250f; cm.m[0][2] = -0.025f;
        cm.m[1][0] = -0.075f; cm.m[1][1] = 1.335f; cm.m[1][2] = -0.025f;
        cm.m[2][0] = -0.075f; cm.m[2][1] = -0.250f; cm.m[2][2] = 1.301f;
        cm.m[3][3] = 1.0f;
        cm.m[4][4] = 1.0f;
        Gdiplus::ImageAttributes ia;
        ia.SetColorMatrix(&cm);

        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.DrawImage(backdrop_, Gdiplus::Rect(0, 0, w_, h_),
                    0, 0, backdrop_->GetWidth(), backdrop_->GetHeight(),
                    Gdiplus::UnitPixel, &ia);
        g.SetInterpolationMode(Gdiplus::InterpolationModeDefault);
    }

    // ---- ①b 深色模式：先把透上来的壁纸压暗 ----
    // 这一步只压亮度、不动饱和度：深色菜单栏要的是「底下还是那张壁纸，
    // 只是整体沉下去」，直接换成一块灰就又变成塑料板了。
    if (dark_ > 0.01f) {
        Gdiplus::SolidBrush dim(Gdiplus::Color(static_cast<BYTE>(110.0f * dark_), 0, 0, 0));
        g.FillRectangle(&dim, 0, 0, w_, h_);
    }

    // ---- ② 薄纱 ----
    // 纯模糊会太花（壁纸的云、山的纹理全上来了），盖一层把它压成
    // 「毛玻璃」而不是「糊照片」。这一层的浓度就是玻璃的厚度感。
    //
    // ★ 但这层不能浓。原来给到 190，等于把底下颜色洗掉了九成 ——
    //   屏幕上就是一条接近纯白的带子，跟涂了白漆没区别。macOS 大约在六成上下：
    //   还能看出底下是暖色还是冷色，但纹理已经被抹平了。
    // ★ 深色模式下这层换成深色（并且浓一点，190）：浅色那层白纱哪怕盖满，
    //   压不住一张亮壁纸 —— 底下是白的，上面刷一层薄白还是白的。
    {
        Gdiplus::SolidBrush veil(Mix(Gdiplus::Color(150, 250, 250, 253),
                                     Gdiplus::Color(190, 28, 28, 32), dark_));
        g.FillRectangle(&veil, 0, 0, w_, h_);
    }

    // ---- ③ 自上而下的微渐变 ----
    // 顶部稍亮、底部稍暗，一条平的色块会显得很「塑料」，有这点厚薄之分才像块玻璃。
    // 幅度要小：46px 高的条子上，太明显就变成「上白下灰」的廉价渐变条了。
    {
        Gdiplus::LinearGradientBrush sheen(
            Gdiplus::Point(0, 0), Gdiplus::Point(0, h_),
            Mix(Gdiplus::Color(24, 255, 255, 255),
                Gdiplus::Color(16, 255, 255, 255), dark_),
            Gdiplus::Color(0, 255, 255, 255));
        g.FillRectangle(&sheen, 0, 0, w_, h_);
    }

    // ---- ④ 底边 ----
    // 一条极淡的暗线划开状态栏和下面的窗口；再往上一像素补一条更亮的，
    // 像玻璃的棱。只有暗线的话边界会显得很脏。
    // 深色模式下反过来了：暗线要更重（不然跟下面的深色窗口糊成一片），
    // 那条亮线要收着（不然像描了一道白边）。
    {
        Gdiplus::Pen dark(Mix(Gdiplus::Color(30, 0, 0, 0),
                              Gdiplus::Color(80, 0, 0, 0), dark_), 1.0f);
        g.DrawLine(&dark, 0.0f, static_cast<float>(h_) - 0.5f,
                   static_cast<float>(w_), static_cast<float>(h_) - 0.5f);
        Gdiplus::Pen lite(Mix(Gdiplus::Color(70, 255, 255, 255),
                              Gdiplus::Color(26, 255, 255, 255), dark_), 1.0f);
        g.DrawLine(&lite, 0.0f, static_cast<float>(h_) - 1.5f,
                   static_cast<float>(w_), static_cast<float>(h_) - 1.5f);
    }

    Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Td(12)),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fontBold(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Td(12)),
                           Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt(Gdiplus::StringFormatFlagsNoWrap);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    // ---- ⑤ 左边：圆环 + 应用名 ----
    const float cy = static_cast<float>(h_) / 2.0f;
    const float cx = static_cast<float>(Td(18));

    if (hoverLogo_) {
        Gdiplus::GraphicsPath hp;
        AddRoundRect(hp, Gdiplus::RectF(static_cast<float>(Td(5)), static_cast<float>(Td(3)),
                                        static_cast<float>(Td(26)),
                                        static_cast<float>(h_ - Td(6))),
                     static_cast<float>(Td(5)));
        Gdiplus::SolidBrush hb(Gdiplus::Color(255, 0, 122, 255));
        g.FillPath(&hb, &hp);
    }
    {
        // 一个细圆环。颜色跟着当前应用走 —— 换应用时它渐变过去。
        const float r = static_cast<float>(Td(6));
        const float penW = std::max(1.5f, static_cast<float>(Td(1.6)));
        Gdiplus::Color ringColor;
        if (hoverLogo_) {
            ringColor = Gdiplus::Color(255, 255, 255, 255);
        } else {
            // ★ 深色模式下往白里掺一点。
            //   强调色是按「浅色底上够深才看得清」定的（亮度 0.42，见 AccentForApp），
            //   底色一转深，那些偏暗的色（深蓝、墨绿）会跟背景糊在一起。
            //   不重算色相、只提亮，免得同一个应用换主题就换了个性别。
            const float lift = 0.34f * dark_;
            auto up = [lift](float v) {
                return static_cast<BYTE>(v + (255.0f - v) * lift + 0.5f);
            };
            ringColor = Gdiplus::Color(255, up(accR_), up(accG_), up(accB_));
        }
        Gdiplus::Pen ring(ringColor, penW);
        g.DrawEllipse(&ring, cx - r, cy - r, r * 2.0f, r * 2.0f);
    }

    // 应用名就在圆环右边，跟它挨着（原来放中间，离得远、看着像两个东西）
    if (!appText_.empty()) {
        const Gdiplus::RectF box(static_cast<float>(Td(31)), 0.0f,
                                 static_cast<float>(Td(360)), static_cast<float>(h_));
        fmt.SetAlignment(Gdiplus::StringAlignmentNear);
        // 交叉淡出：旧名字边淡边退，新名字边淡边进
        if (appFade_ < 1.0f && !appPrev_.empty()) {
            Gdiplus::SolidBrush oldBrush(TextInk(
                dark_, static_cast<BYTE>((1.0f - appFade_) * 235.0f)));
            g.DrawString(appPrev_.c_str(), static_cast<INT>(appPrev_.size()), &fontBold,
                         box, &fmt, &oldBrush);
        }
        Gdiplus::SolidBrush nb(TextInk(dark_, static_cast<BYTE>(appFade_ * 235.0f)));
        g.DrawString(appText_.c_str(), static_cast<INT>(appText_.size()), &fontBold,
                     box, &fmt, &nb);
    }

    // ---- ⑥ 右边：状态图标 + 日期 + 时间 ----
    //
    // ★ 图标不能按固定的 x 摆。参考图里时钟只有 "23:19" 短短一截，而我们的
    //   时钟是 "10月9日 周五 23:19"，长得多 —— 按固定位置摆，图标和文字之间
    //   会空出一大块。所以先量一遍时钟文字的实际宽度，再让整排图标贴着它的
    //   左边缘往左排。
    {
        fmt.SetAlignment(Gdiplus::StringAlignmentFar);
        const Gdiplus::RectF clockBox(static_cast<float>(w_ - Td(220)), 0.0f,
                                      static_cast<float>(Td(204)),
                                      static_cast<float>(h_));
        Gdiplus::RectF ext;
        g.MeasureString(clockText_.c_str(), static_cast<INT>(clockText_.size()), &font,
                        clockBox, &fmt, &ext);
        // 量不出来（文字是空的之类）就退回一个保守的右边界，
        // 别让整排图标被甩到屏幕中间去。
        const float textLeft = (ext.Width > 1.0f) ? ext.X
                                                  : static_cast<float>(w_ - Td(140));

        DrawIconRow(g, textLeft);

        Gdiplus::SolidBrush cb(TextInk(dark_, 236));
        g.DrawString(clockText_.c_str(), static_cast<INT>(clockText_.size()), &font,
                     clockBox, &fmt, &cb);
    }

    g.Flush();

    HDC screen = GetDC(nullptr);
    POINT src = {0, 0};
    SIZE size = {w_, h_};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    // ★ 位置传 nullptr，不要传 {0,0}。
    //   传 {0,0} 等于「把窗口挪到屏幕左上角」，跟 AppBar 摆好的位置是两回事
    //   （多显示器或者任务栏也在顶上时，AppBar 给的落点未必是 0,0）。
    //   传 nullptr 就是「位置别动，只换画面」——位置统一交给 SetWindowPos 管。
    UpdateLayeredWindow(hwnd_, screen, nullptr, &size, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// 前台窗口是不是占满了整块屏幕（全屏游戏、全屏视频、F11 的浏览器都算）
bool TopBar::ForegroundFullscreen() const {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg)) return false;

    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    // 桌面和任务栏本身不算「全屏应用」，不然状态栏永远出不来
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }

    // ★ 有标题栏的，一律不算全屏。
    //
    //   这一条是主判据，比下面那个矩形比较靠谱得多。原因：Windows 上
    //   「最大化」和「全屏」在坐标上几乎分不出来 —— 最大化的窗口 GetWindowRect
    //   同样铺满整块屏幕（还多出几个像素的调整边框），IsZoomed 也不是每个
    //   框架都老实返回。（实测踩过：WorkBuddy 这种自带标题栏、尺寸正好铺满的窗口
    //   被当成全屏，顶部栏从头到尾就没显示过。）
    //
    //   但真正全屏的应用一定把标题栏去掉了：游戏、F11 的浏览器、全屏播放的视频，
    //   清一色是无边框的。而带标题栏的窗口用户随时能拖走、能调整 —— 那时候
    //   macOS 的菜单栏是照常挂着的。
    //
    //   万一遇到「有标题栏却真的铺满全屏」的老程序，结果是状态栏多显示一条，
    //   而不是消失。宁可多显示：消失会让人以为程序坏了。
    if (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) return false;

    if (IsZoomed(fg)) return false;

    RECT r = {};
    if (!GetWindowRect(fg, &r)) return false;
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    return (r.left <= 2 && r.top <= 2 && r.right >= sw - 2 && r.bottom >= sh - 2);
}

// ★ TOPMOST 不等于「永远在最前」。
//   置顶窗口之间还有一套 z 序，一个后来才把自己设成置顶的窗口会**插到我们上面**。
//   实测踩到：WorkBuddy 的窗口一被激活就压住整条状态栏，鼠标移开、点了它之后
//   顶部栏整条消失 —— 一开始我以为是「全屏自动隐藏」误判，把 SetHidden 的
//   调用全打了日志才发现一次都没触发，窗口一直好好地在屏幕顶上，只是被盖住了。
//   所以每次 Tick 检查一下：上面有没有一块够大的可见顶层窗口压着，有就重新置顶。
void TopBar::EnsureOnTop() {
    if (!hwnd_ || hidden_) return;
    HWND blocker = nullptr;
    for (HWND w = GetWindow(hwnd_, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w)) continue;

        // 自己弹出来的系统菜单本来就该压在状态栏上，不是「被别人压住」。
        // 不排除的话，菜单一显示就会被自己抢回最前，顶部七像素被啃掉一块。
        wchar_t wcls[64] = {};
        GetClassNameW(w, wcls, 64);
        if (wcsncmp(wcls, L"MyDock", 6) == 0) continue;

        RECT r = {};
        if (!GetWindowRect(w, &r)) break;
        // 只认「真的压在这条带子上」的窗口。
        // 面积太小的是系统辅助窗口（缩略图、输入法候选之类），它们也置顶、
        // 也叫可见，但不遮任何东西 —— 不过滤的话会一直白刷。
        const LONG ww = r.right - r.left;
        const LONG hh = r.bottom - r.top;
        if (ww < 100 || hh < 20) continue;
        if (r.bottom > 0 && r.top < h_ && r.right > 0 && r.left < w_) {
            blocker = w;
            break;
        }
    }
    if (!blocker) return;

    if (TraceOn()) {
        wchar_t cls[64] = {};
        wchar_t tt[128] = {};
        GetClassNameW(blocker, cls, 64);
        GetWindowTextW(blocker, tt, 128);
        Log(L"顶部栏 TRACE：被 %s [%s] 压住，重新置顶", cls, tt);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void TopBar::SetHidden(bool hidden, bool releaseSpace) {
    if (hidden == hidden_) return;
    hidden_ = hidden;
    if (TraceOn()) {
        HWND fg = GetForegroundWindow();
        wchar_t cls[64] = {};
        wchar_t tt[128] = {};
        GetClassNameW(fg, cls, 64);
        GetWindowTextW(fg, tt, 128);
        RECT r = {};
        GetWindowRect(fg, &r);
        Log(L"顶部栏 TRACE：%s ← 前台 class=%s title=[%s] 矩形=(%d,%d)-(%d,%d) "
            L"最大化=%d 标题栏=%d",
            hidden ? L"隐藏" : L"显示", cls, tt, r.left, r.top, r.right, r.bottom,
            IsZoomed(fg) ? 1 : 0,
            (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) ? 1 : 0);
    }

    if (hidden) {
        // ★ 藏起来之前先把占的那截工作区还回去。
        //   全屏游戏/视频要的是整块屏幕，我们要是还占着顶上 46 px，
        //   它的画面就会被压矮一条 —— 那是真的会影响使用，不只是不好看。
        //   启动台那种自己人的全屏窗口不走这条路（releaseSpace=false）：
        //   它本来就盖满整屏，还回去只会让桌面图标白重排一次。
        if (releaseSpace && appbar_) {
            UnregisterAppBar();
            spaceReleased_ = true;
        }
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }

    // 亮回来：先把位置和工作区要回来，再拍背景 —— 背景要按新位置拍。
    if (spaceReleased_) {
        spaceReleased_ = false;
        if (reserve_) RegisterAppBar();
    } else if (appbar_) {
        ApplyAppBarPos();
    }
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    CaptureBackdrop();
    Render();
}

// 2026-10-09 - AI - 顶部栏登记成 AppBar：让屏幕顶上这条真正「占位」
//
// ★ 为什么必须占位，而不是「浮在窗口上面」：
//   浮着的话，最大化的窗口照旧从 y=0 开始画，它的标题栏/菜单栏被我们压掉，
//   只在条子底下露出几个像素的灰边 —— 用户看了直接说「这条子有一道大阴影」。
//   桌面图标更明显：explorer 是按「工作区」摆图标的，工作区还是整屏，
//   于是最上面一排图标有一半藏在条子底下。
//   macOS 的菜单栏从来不浮着，它是**占位**的：屏幕顶上那一条归菜单栏，
//   窗口和桌面都从它下面开始。Windows 上对应的机制就是 AppBar
//   （任务栏本身就是个 AppBar）。登记之后系统自动帮我们做三件事：
//     1) 把顶上一截从工作区（SPI_GETWORKAREA）里去掉；
//     2) 最大化的窗口自动让开这一截；
//     3) 桌面图标自动往下排。
//   我们一行布局代码都不用写。
//
// ★ 代价：这是个**全局**改动，不是只影响我们自己。用户要是把 settings.ini 里的
//   [topbar] reserve 关掉，就退回「浮着」的老样子。
void TopBar::RegisterAppBar() {
    if (appbar_ || !hwnd_) return;

    APPBARDATA abd = {};
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = hwnd_;
    abd.uCallbackMessage = kMsgAppBar;   // 工作区变化时系统回我们这条消息

    if (!SHAppBarMessage(ABM_NEW, &abd)) {
        Log(L"顶部状态栏：登记为 AppBar 失败（错误码 %lu），退回浮在最上面的老做法",
            GetLastError());
        return;
    }
    appbar_ = true;
    ApplyAppBarPos();
    // 日志里报的是**实际**的工作区起点，不是我们自己算的 h_：
    // 顶上要是已经有别的 AppBar（比如 MyDockFinder），我们会被排到它下面，
    // 起点就是两条加起来的高度。调试时这个数字比 h_ 有用得多。
    RECT wa = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    Log(L"顶部状态栏：已登记为 AppBar，我们占 (0,%d)-(%d,%d)，工作区从 y=%d 开始",
        barY_, w_, barY_ + h_, wa.top);
}

void TopBar::UnregisterAppBar() {
    if (!appbar_) return;
    APPBARDATA abd = {};
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = hwnd_;
    SHAppBarMessage(ABM_REMOVE, &abd);
    appbar_ = false;
    Log(L"顶部状态栏：已注销 AppBar，那一截工作区还给了系统");
}

// 把位置报给系统：ABM_QUERYPOS 问「这条边归我的话，能占多高」，
// 再 ABM_SETPOS 定下来。系统会在这一步重排工作区、挪动最大化的窗口。
//
// ★ 高度不让步：QUERYPOS 返回的矩形可能被系统改小（比如别的 AppBar 也在顶上），
//   照它摆我们就会被切掉一截。我们自己知道要多高，问完之后把高度写回 h_。
// ★ h_ 全程不改：它是渲染和布局用的高度。要是让它被系统的返回值带着走，
//   下一次 QUERYPOS 会在这个已经被改小的值上再算，条子会一轮比一轮矮。
void TopBar::ApplyAppBarPos(bool onlyIfChanged) {
    if (!appbar_ || !hwnd_) return;

    APPBARDATA abd = {};
    abd.cbSize = sizeof(APPBARDATA);
    abd.hWnd = hwnd_;
    abd.uEdge = ABE_TOP;
    abd.rc.left = 0;
    abd.rc.top = 0;
    abd.rc.right = GetSystemMetrics(SM_CXSCREEN);
    abd.rc.bottom = h_;

    SHAppBarMessage(ABM_QUERYPOS, &abd);
    abd.rc.bottom = abd.rc.top + h_;

    // ★ 防回环：ABM_SETPOS 会让系统重新摆一遍所有窗口，摆完又会回我们一条
    //   ABN_POSCHANGED。照直再 SETPOS 一次，就可能来回不停一直重排。
    //   所以「被通知」触发的那一次先算一遍：位置没变就什么都不做。
    if (onlyIfChanged && abd.rc.left == barX_ && abd.rc.top == barY_ &&
        abd.rc.right - abd.rc.left == w_) {
        return;
    }

    if (!SHAppBarMessage(ABM_SETPOS, &abd)) {
        Log(L"顶部状态栏：ABM_SETPOS 失败（错误码 %lu）", GetLastError());
        return;
    }

    barX_ = abd.rc.left;
    barY_ = abd.rc.top;
    const int nw = abd.rc.right - abd.rc.left;
    if (nw > 0 && nw != w_) {
        // 宽度变了（换分辨率、或者被别的 AppBar 挤窄了）：重建后缓冲。高度不动。
        w_ = nw;
        CreateBackBuffer(w_, h_);
        RequestRedraw(true);
    }
    SetWindowPos(hwnd_, HWND_TOPMOST, barX_, barY_, w_, h_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void TopBar::OnAppBarNotify(WPARAM wp) {
    switch (wp) {
        case ABN_POSCHANGED:
            // 别的 AppBar（任务栏）动了位置，我们的落点得重新算一遍。
            if (inAppBarPos_) return;   // 正摆着位置就别再进来
            inAppBarPos_ = true;
            ApplyAppBarPos(true);
            inAppBarPos_ = false;
            break;
        case ABN_FULLSCREENAPP:
            // 系统告诉我们有全屏应用开/关了。不在这儿直接改状态（会重入系统），
            // 交给 500ms 的心跳去判 —— 顶多慢半秒，胜在不会把系统绕进去。
            break;
        default:
            break;
    }
}

void TopBar::EnsureAnim() {
    if (animating_) return;
    animating_ = true;
    SetTimer(hwnd_, kTimerAnim, 16, nullptr);
}

void TopBar::AnimTick() {
    const bool moving = StepFade(16.0);
    Render();                       // 渐变期间只重画，不重拍背景（拍一次要闪一下）
    if (!moving) {
        KillTimer(hwnd_, kTimerAnim);
        animating_ = false;
    }
}

void TopBar::RequestRedraw(bool needBackdrop) {
    if (needBackdrop) {
        CaptureBackdrop();
        // 拍屏会顺手重新贴一次窗口，这里再画一遍内容
    }
    Render();
}

void TopBar::Tick() {
    // ① 全屏应用（或启动台）在前台时让位。
    //    启动台也是盖满整屏的窗口，状态栏留在那儿只会被压住，
    //    或者在它拍背景时被一起拍进去（顶部糊一条）。
    HWND lp = FindWindowW(L"MyDockLauncherClass", nullptr);
    const bool launcherUp = (lp != nullptr && IsWindowVisible(lp));
    const bool full = ForegroundFullscreen();

    if (launcherUp || full) {
        // ★ 两种情况让位的方式不一样：
        //   全屏应用（游戏/视频）要整块屏幕 —— 连我们占的那截工作区都得还回去，
        //   不然它的画面会被压矮一条；启动台是自己人，本来就盖满整屏，
        //   占着不动，省得桌面图标被来回重排。
        SetHidden(true, full);
        return;
    }
    SetHidden(false);               // 刚从隐藏恢复的话，这里已经把画面重贴好了

    // ★ 然后确认自己没被别人压住。
    //   必须放在 SetHidden 之后：刚从隐藏恢复时窗口是新亮出来的，
    //   谁在上面还没数过。
    EnsureOnTop();

    // ② 文字变了才重画。背景不跟着每次重画重拍 —— 拍屏要先把窗口藏起来，
    //    一分钟来一次还能接受，每 500ms 来一次就是一直在闪。
    if (RefreshTexts()) {
        const bool stale = (GetTickCount() - lastBackdrop_) > 1500;
        RequestRedraw(stale);
    }

    // ②b 浅色 / 深色：底下那条变暗了就转深，变亮了就转浅。
    //    跟刷文字分开做，是因为它跟文字没关系 —— 换了个同名的应用、
    //    或者只是把窗口换个位置，亮度都会变，而文字一个字没动。
    UpdateTheme();

    // ③ 电池/音量/Wi-Fi/蓝牙 变了也重画。这类东西不影响文字，光靠上面那条
    //    永远刷不到 —— 插上电源、按静音键，图标得当场跟着变。
    //    只重画不重拍背景：拍屏要先把窗口藏起来闪一下，不值得为这点变化付。
    if (PollSysStatus(sys_)) {
        Render();
    }
}

void TopBar::ShowSystemMenu() {
    // 标题行写计算机名 —— 一眼知道自己在操作哪台机器
    wchar_t pcName[64] = L"这台电脑";
    DWORD pcLen = 64;
    GetComputerNameW(pcName, &pcLen);

    std::vector<MenuItem> items;
    MenuItem head;
    head.title = true;
    head.label = pcName;
    head.tag = L"系统";
    items.push_back(head);
    items.push_back({0, L"", true, false});
    items.push_back({kCmdAbout,    L"关于本机"});
    items.push_back({0, L"", true, false});
    items.push_back({kCmdSettings, L"系统设置"});
    items.push_back({kCmdStore,    L"应用商店"});
    items.push_back({0, L"", true, false});
    items.push_back({kCmdTaskMgr,  L"任务管理器"});
    items.push_back({kCmdWorkshop, L"创意工坊"});
    items.push_back({0, L"", true, false});
    // 2026-10-12 - AI - 从 ⏏ 菜单搬来的 MyDock 专属组 + 原驱动器两项：
    //   ⏏ 正式改作托盘（列出 Windows 托盘里在跑的应用），系统项归位圆圈菜单。
    items.push_back({kStShowDock,    L"显示 Dock"});
    items.push_back({kStAutoStart,   L"开机自启", false, IsAutoStart()});
    items.push_back({kStRestartDock, L"重启 MyDock"});
    items.push_back({0, L"", true, false});
    items.push_back({kStComputer,    L"打开此电脑"});
    items.push_back({kStDiskMgmt,    L"磁盘管理"});
    items.push_back({0, L"", true, false});
    items.push_back({kCmdSleep,    L"睡眠"});
    items.push_back({kCmdRestart,  L"重启"});
    items.push_back({kCmdShutdown, L"关机"});
    items.push_back({0, L"", true, false});
    // 「锁定」「注销」原来在这儿，2026-10-10 按用户要求删了：
    //   Win+L 一个快捷键就搞定的事，占菜单两行不值；注销更是几乎没人用。
    // ★ 「退出 MyDock」要跟上面那三个**分开一组**：睡眠/重启/关机动的是这台电脑，
    //   这一项动的是 MyDock 自己。挨在一起，很容易本来想关机却把 Dock 点没了。
    items.push_back({kCmdQuit,     L"退出 MyDock"});

    // 锚点放在圆环下方 —— 菜单左上角贴着它，跟 macOS 的落点一致
    const int cmd = menu_.Show(hwnd_, Td(5), h_ - Td(4), items);
    hoverLogo_ = 0;
    Render();
    if (cmd != 0) RunCommand(cmd);
}

void TopBar::RunCommand(int cmd) {
    // ShellExecuteW 的返回值 <= 32 表示失败。打开 ms-settings: 这类协议时
    // 返回值本身没什么意义（成功也可能回 42），所以只在失败时记一笔日志。
    auto open = [](const wchar_t* what) {
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", what, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) {
            Log(L"顶部状态栏：打不开 %s（返回 %d）", what,
                static_cast<int>(reinterpret_cast<INT_PTR>(r)));
        }
    };

    switch (cmd) {
        case kStShowDock: {
            // Dock 窗口类名固定，投一条「显示」过去 —— 可见性/动画归它自己管
            const HWND dock = FindWindowW(L"MyDockWindowClass", nullptr);
            if (dock) PostMessageW(dock, WM_APP + 45, 0, 0);
            break;
        }
        case kStAutoStart:
            SetAutoStart(!IsAutoStart());
            break;
        case kStRestartDock:
            // 1 秒后由系统拉起新进程（等互斥体释放），顶栏和 Dock 各自退场
            QueueRestart();
            DestroyWindow(hwnd_);
            {
                const HWND dock = FindWindowW(L"MyDockWindowClass", nullptr);
                if (dock) PostMessageW(dock, WM_CLOSE, 0, 0);
            }
            break;
        case kStComputer: open(L"shell:MyComputerFolder"); break;
        case kStDiskMgmt: open(L"diskmgmt.msc"); break;
        case kCmdAbout:
            open(L"winver.exe");
            break;
        case kCmdSettings:
            open(L"ms-settings:");
            break;
        case kCmdStore:
            open(L"ms-windows-store:");
            break;
        case kCmdTaskMgr:
            open(L"taskmgr.exe");
            break;
        case kCmdWorkshop:
            // Steam 的创意工坊。MyDockFinder 的皮肤就是从那儿来的，
            // 所以这一项留了个口子 —— 不用自己去翻 Steam 的目录。
            open(L"steam://url/SteamWorkshopPage");
            break;
        case kCmdSleep:
            // 不直接调 SetSuspendState：那要额外链接 powrprof.lib，
            // 而且普通权限下不一定成功。走 rundll32 是系统自己的做法。
            ShellExecuteW(nullptr, L"open", L"rundll32.exe",
                          L"powrprof.dll,SetSuspendState 0,1,0", nullptr, SW_HIDE);
            break;
        case kCmdRestart:
            // ★ 关机/重启/注销都会毁掉没保存的工作，必须先问一句。
            //   macOS 也是弹确认的，直接执行是拿用户的数据开玩笑。
            if (MessageBoxW(nullptr, L"确定要重启这台电脑吗？", L"MyDock",
                            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES) {
                ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/r /t 0",
                              nullptr, SW_HIDE);
            }
            break;
        case kCmdShutdown:
            if (MessageBoxW(nullptr, L"确定要关机吗？", L"MyDock",
                            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES) {
                ShellExecuteW(nullptr, L"open", L"shutdown.exe", L"/s /t 0",
                              nullptr, SW_HIDE);
            }
            break;
        case kCmdQuit:
            // ★ 退出要「关掉 Dock 那个窗口」，不要在这儿直接 PostQuitMessage。
            //
            //   Dock 的 WM_DESTROY 里会先 UnregisterHotKey 再 PostQuitMessage；
            //   消息循环一退，main 里 TopBar 的析构接着跑，把 AppBar 注销掉
            //   —— 屏幕顶上那 46px 工作区就这么还给了系统。
            //
            //   直接 PostQuitMessage 的话，Dock 窗口从头到尾没人销毁，
            //   它注册的全局热键和那块被挖走的工作区都得等进程真正退出才放手，
            //   运气不好就是桌面上永远缺一条。
            if (HWND dock = FindWindowW(L"MyDockWindowClass", nullptr)) {
                DestroyWindow(dock);
            } else {
                PostQuitMessage(0);   // Dock 没起来也得能退，不能卡在菜单上
            }
            break;
        default:
            break;
    }
}

LRESULT CALLBACK TopBar::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    TopBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<TopBar*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // ★ 必须在这儿就把句柄补上。窗口还在创建过程中，CreateWindowEx 尚未返回，
        //   成员变量此刻是空的 —— 拿空句柄去调 DefWindowProc 会让 NCCREATE 返回
        //   FALSE，窗口直接建不出来（菜单那次就是踩的这个，错误码 1400）。
        if (self) self->hwnd_ = h;
    } else {
        self = reinterpret_cast<TopBar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT TopBar::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd_, nullptr);
            return 0;
        case WM_MOUSEMOVE: {
            // ★ 不主动要的话，鼠标移出去系统不会通知我们，
            //   WM_MOUSELEAVE 永远不来，蓝色高亮就一直留在那儿。
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            const int x = static_cast<short>(LOWORD(lp));
            const int y = static_cast<short>(HIWORD(lp));
            const int want = HitLogo(x, y) ? 1 : 0;
            const int wantIc = HitIcon(x, y);
            if (want != hoverLogo_ || wantIc != hoverIcon_) {
                hoverLogo_ = want;
                hoverIcon_ = wantIc;
                Render();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (hoverLogo_ || hoverIcon_ >= 0) {
                hoverLogo_ = 0;
                hoverIcon_ = -1;
                Render();
            }
            return 0;
        case WM_LBUTTONUP: {
            const int mx = static_cast<short>(LOWORD(lp));
            const int my = static_cast<short>(HIWORD(lp));
            if (HitLogo(mx, my)) {
                ShowSystemMenu();
                return 0;
            }
            const int ic = HitIcon(mx, my);
            // 2026-10-12 - AI - 输入法槽：单击直接切换，不弹菜单。
            //   装了多个输入法 → 经典通道给前台窗口投
            //   WM_INPUTLANGCHANGEREQUEST 切到下一个；
            //   只装了一个 → 借 UIA 点任务栏的「中/英文」指示器切中英。
            if (ic >= 0) ShowIconMenu(ic);
            // 2026-10-11 - AI - 点时钟弹日历。以前这块点了没反应 ——
            //   任务栏的时钟 flyout 是 ShellExperienceHost 的内部窗口，
            //   没有命令行能唤出来，所以干脆自己画一个（见 flyout.cpp）。
            else if (mx >= w_ - Td(220)) {
                const bool dark = dark_ > 0.5f;
                flyout_.Toggle(inst_, mx, h_, dark);
                // 日历是模态面板，关掉后鼠标多半已经离开图标 ——
                // 清掉悬停高亮，免得顶栏上留一块亮着的图标
                hoverIcon_ = -1;
                Render();
            }
            return 0;
        }
        case WM_TIMER:
            if (wp == kTimerTick) Tick();
            else if (wp == kTimerAnim) AnimTick();
            return 0;
        case kMsgAppBar:
            // 系统在工作区变化时回的 AppBar 通知（wParam 是通知码）
            OnAppBarNotify(wp);
            return 0;
        case WM_DISPLAYCHANGE:
            // 换分辨率/换显示器：宽度和 DPI 都得跟着变，不然右边留一截或者盖不住。
            // ★ 走 AppBar 这条线重新报一次位置，让系统把新的工作区也一起算好；
            //   没登记成 AppBar 才退回自己 SetWindowPos。
            {
                const MonitorInfo prim = PrimaryMonitor();
                dpi_ = prim.dpi;
                g_topDpi = prim.dpi;
                w_ = prim.full.right - prim.full.left;
                h_ = Td(26);
            }
            if (appbar_) {
                ApplyAppBarPos();
            } else {
                SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, w_, h_,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            CreateBackBuffer(w_, h_);
            RequestRedraw(true);
            Log(L"顶部状态栏：主屏变成 %d 宽（DPI %d），重排", w_, dpi_);
            return 0;
        // 2026-10-11 - AI - 主屏 DPI 变了（改缩放）。条子高度是 Td(26)，
        // 不重算的话系统会把旧位图拉伸铺上去 —— 字糊，图标位置也跟着偏。
        case WM_DPICHANGED: {
            const int newDpi = static_cast<int>(LOWORD(wp));
            if (newDpi > 0 && newDpi != dpi_) {
                Log(L"顶部状态栏：DPI %d → %d，重排", dpi_, newDpi);
                dpi_ = newDpi;
                g_topDpi = newDpi;
                h_ = Td(26);
                if (appbar_) ApplyAppBarPos();
                else SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, w_, h_,
                                  SWP_NOACTIVATE | SWP_NOZORDER);
                CreateBackBuffer(w_, h_);
                RequestRedraw(true);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace mydock
