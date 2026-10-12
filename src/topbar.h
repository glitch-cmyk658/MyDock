// 2026-10-09 - AI - 顶部状态栏：左边圆环 + 当前应用名，右边时钟
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>

#include "menu.h"
#include "status.h"
#include "util.h"   // 2026-10-11 - AI - DpiAt / PrimaryMonitor
#include "flyout.h"    // 2026-10-11 - AI - 时钟点开的日历面板
#include "ccenter.h"   // 2026-10-11 - AI - 自绘控制中心

namespace mydock {

// 一条贴在主屏顶上的细条，长得像 macOS 的菜单栏：
//
//   ◯  WorkBuddy                    ᛒ ᯤ 🖥 🔋 🔇 ⏏ 🔍 ⚙   10月9日 周五 22:05
//   └ 点开是系统菜单  └ 当前应用名    └ 状态图标（手画矢量，见 DrawIconRow）
//
// ★ 为什么是一个独立窗口，而不是画在 Dock 上：
//   Dock 是底部那条，会自己收起；状态栏要一直挂着。两件事的生命周期不一样。
//
// ★ 为什么不用系统菜单条（SetMenu / TrackPopupMenu）：
//   跟启动台那份菜单同一个理由 —— 系统控件长什么样由主题决定，圆角、字号、
//   悬停高亮全都改不动。这里统一用 PopupMenu 自己画。
//
// ★ 为什么左边是个**圆环**而不是苹果 logo：
//   这是给 Windows 写的 Dock，摆个被咬一口的苹果既不对题也不体面。
class TopBar {
    // 持有裸指针资源（Bitmap/HDC/HBITMAP），默认拷贝语义 = 双重释放隐患。
    // cppcheck 六个类各报一条之后统一禁掉拷贝（2026-10-12）
        TopBar(const TopBar&) = delete;
    TopBar& operator=(const TopBar&) = delete;

public:
    TopBar() = default;
    ~TopBar();

    // 建窗口。settings.ini 里 [topbar] enabled=0 的时候直接不建（返回 false）。
    bool Create(HINSTANCE inst);

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

    void CreateBackBuffer(int w, int h);
    void Render();
    void Tick();                  // 500ms：刷时钟、刷前台应用、查有没有全屏应用
    void AnimTick();              // 16ms：只推进颜色/文字的渐变，期间不重拍背景
    // 采样「条子下面那条」的亮度，决定转浅还是转深。
    // immediate=true：直接到位，不渐变（开机第一帧用）
    void UpdateTheme(bool immediate = false);
    bool RefreshTexts();          // 重新取一遍文字；变了返回 true
    bool StepFade(double dtMs);   // 推进渐变；返回 true 表示还在动
    bool HitLogo(int x, int y) const;
    void DrawIconRow(Gdiplus::Graphics& g, float textLeft);   // 右侧那排状态图标
    int  HitIcon(int x, int y) const;                         // 命中第几个图标，-1 没有
    // 右侧那排状态图标点开的菜单（2026-10-10）。跟左上角那个系统菜单的区别：
    //   内容是**现问的**（网络列表、设备列表每次弹开时扫一遍），
    //   而且菜单要**右对齐**到那个图标 —— macOS 的状态菜单就是这么落的。
    void ShowIconMenu(int id);
    void RunIconMenuCommand(int id, int cmd);
    void ShowSystemMenu();
    void RunCommand(int cmd);
    void SendWinKey(WORD vk);       // 给系统发一次 Win+X 热键
    bool ForegroundFullscreen() const;
    void EnsureOnTop();          // 被别的置顶窗口压住时把自己再提上去
    // 藏起来/亮回来。releaseSpace 只对「藏」有效：全屏应用要整块屏幕，
    // 得把我们占着的那条工作区还回去；启动台是自己人，不用还。
    void SetHidden(bool hidden, bool releaseSpace = true);
    void CaptureBackdrop();       // 拍下自己身后的屏幕并模糊 —— 毛玻璃的底
    void RequestRedraw(bool needBackdrop);
    void EnsureAnim();            // 有东西在渐变时把 16ms 定时器开起来

    // ---- 把自己登记成系统的 AppBar，让屏幕顶上这条真的「占位」 ----
    // 见 topbar.cpp 里的长注释：不占位的话，最大化的窗口和桌面图标都会钻到条子底下。
    void RegisterAppBar();
    void UnregisterAppBar();
    void ApplyAppBarPos(bool onlyIfChanged = false);
    void OnAppBarNotify(WPARAM wp);

    HINSTANCE inst_ = nullptr;
    HWND     hwnd_ = nullptr;
    HDC      dc_ = nullptr;
    HBITMAP  bmp_ = nullptr;
    void*    bits_ = nullptr;

    int  w_ = 0, h_ = 0;
    int  barX_ = 0, barY_ = 0;    // 窗口当前落在屏幕的哪里（AppBar 会把我们摆好）

    // 2026-10-11 - AI - 顶部状态栏只出现在主屏，所以尺寸按**主屏**的 DPI 算。
    // 以前用 util 的 Dpi()，那个取的是系统 DPI —— 单显示器时两者一样，
    // 多显示器且主屏不是系统 DPI 那块时就会差一截。
    // 内部一律走 Td()，别再直接调 Dpi()。
    int  dpi_ = 96;
    int  Td(int px) const { return DpiAt(px, dpi_); }
    int  hoverLogo_ = 0;          // 鼠标停在圆环上时，整块亮成 macOS 蓝
    int  hoverIcon_ = -1;         // 鼠标停在第几个状态图标上（-1 = 没有）
    bool tracking_ = false;       // 已经向系统要了 WM_MOUSELEAVE（要一次就够）
    bool hidden_ = false;         // 当前是不是因为全屏应用而藏着
    bool animating_ = false;
    bool reserve_ = true;         // settings.ini [topbar] reserve：占不占那一截工作区
    bool appbar_ = false;         // 已经登记成 AppBar 了吗
    CalendarFlyout flyout_;       // 2026-10-11 - AI - 点时钟弹出的日历面板
    std::vector<std::wstring> trayNames_;  // 2026-10-12 - AI - 托盘菜单项（完整名，匹配用）
    std::vector<Gdiplus::Bitmap*> trayIcons_;  // 行首图标位图（菜单生存期，重建时清）
    ControlCenter ccenter_;       // 2026-10-11 - AI - 点开关图标弹出的控制中心
    bool spaceReleased_ = false;  // 藏起来的时候把工作区还回去了，亮回来要重新登记
    bool inAppBarPos_ = false;    // 正在摆位置，防 ABN_POSCHANGED 回环

    std::wstring clockText_;      // 右边：日期 + 时间
    std::wstring appText_;        // 左边：当前应用名
    std::wstring appPrev_;        // 换应用时，上一段名字用来做交叉淡出
    float        appFade_ = 1.0f; // 0 = 全是旧名字，1 = 全是新名字

    // ---- 浅色 / 深色 ----
    //
    // 判据是「条子下面那条有多亮」：前台应用最大化时，条子下面就是它的标题栏
    // （深色应用 → 深，浅色应用 → 浅）；桌面上没东西时就是壁纸。
    // dark_ 是**渐变用的中间量**（0 = 全浅，1 = 全深），不是开关 ——
    // 存成布尔然后一帧翻过去的话，切应用时整条会「啪」一下跳色。
    float dark_ = 0.0f;           // 当前
    float darkTarget_ = 0.0f;     // 想去哪儿
    bool  darkMode_ = false;      // 判定结果（带回差，避免在临界点上反复横跳）
    DWORD lastLumCheck_ = 0;
    // settings.ini [topbar] theme=auto|light|dark —— 0 自动 / 1 锁浅 / 2 锁深。
    // 自动是默认；锁死那个口子留着，是因为「按应用切」偶尔会撞上用户不认同的取样
    // （比如某个应用把自己的标题栏涂成深色、内容却是白的），有个兜底开关。
    int   themeForce_ = 0;

    // 圆环的颜色跟着当前应用走（每个应用一个色），切换时渐变过去。
    // 存浮点是因为要逐帧插值，直接存 Gdiplus::Color 会把小数截掉。
    float accR_ = 60.0f, accG_ = 60.0f, accB_ = 67.0f;
    float accTR_ = 60.0f, accTG_ = 60.0f, accTB_ = 67.0f;

    Gdiplus::Bitmap* backdrop_ = nullptr;   // 身后那块屏幕，模糊过的
    DWORD lastBackdrop_ = 0;                // 上次拍屏是什么时候

    // ---- 右侧那排状态图标 ----
    SysStatus sys_;                         // 电池/音量/Wi-Fi/蓝牙 的当前状态
    // 图标的位置在 Render 里算一次就存下来（要靠时钟文字的实际宽度才能定），
    // 鼠标点击时直接用这份缓存，不用再量一遍。
    //
    // 一格 = 图案 + 可选的右边标签（现在只有电池用，充电时显示「80%」）。
    // 所以存三份：图案中心（画图用）、格子左边、格子宽度（排布和命中的用）。
    float iconCX_[16] = {};                 // 图案的中心 x
    float iconItemL_[16] = {};              // 每格的左边缘
    float iconItemW_[16] = {};              // 每格的宽度（含标签）
    std::wstring iconLabel_[16];            // 每格右边跟的文字，空 = 没有
    int   iconCount_ = 0;
    float iconCy_ = 0.0f;                   // 整排的竖直中心
    float iconGlyph_ = 0.0f;                // 图案边长
    float iconHit_ = 0.0f;                  // 命中方框边长

    PopupMenu menu_;              // 左边的系统菜单，跟 Dock 那份共用外观

    // 状态图标菜单里那一列网络 / 设备。
    // ★ 菜单是**弹出时才扫**的，执行时要知道「第 i 项对应哪个网络」，
    //   所以扫完的结果得留着，不能只留在局部变量里。
    std::vector<WifiNet>      menuWifi_;
    std::vector<std::wstring> menuBt_;
};

}  // namespace mydock
