// 2026-10-09 - AI - Dock 条窗口：布局、悬停放大、点击启动或切换
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <set>
#include <string>
#include <vector>

#include "apps.h"
#include "launcher.h"
#include "menu.h"
#include "recents.h"

namespace mydock {

class Dock {
public:
    ~Dock();
    bool Create(HINSTANCE inst);
    HWND Handle() const { return hwnd_; }

    // 用户在启动台里增删了固定项之后，重建 Dock 自己的图标列表。
    // 现在 Dock 上出现哪些应用，完全由 pinned.txt 决定 —— 它不再自己去「猜」前 16 个。
    void ReloadApps();

    // 立刻收起、不做滑动动画。启动台要拍背景之前会先喊一声。
    void HideNow();
private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

    void CreateBackBuffer(int w, int h);
    void ComputeTarget();            // 依鼠标位置算出每个图标「想去」的高亮强度
    void LayoutFromSizes();          // 排一次版。图标尺寸恒定，所以只在开机/换列表时调
    void FitMetrics(int n);          // 按图标个数定尺寸，保证整行真的放得进屏幕
    bool StepGlow(double dtMs);      // 高亮强度逐帧逼近目标 —— 「缓缓亮起」就靠它
    void Tick();                     // 一帧动画：推进高亮、推进滑出
    void RequestRender();            // 限流后立刻出一帧（不等定时器）
    void RequestAnim();              // 需要动的时候才开 16ms 定时器
    void Render();                   // 画进后台位图再 UpdateLayeredWindow
    int  HitTest(int x, int y) const;
    void RefreshRunning(bool force); // 刷新"哪些应用正在运行"
    // 后台线程负责枚举（见 dock.cpp 里 RunScanProc），这两个是它的开关
    void StartRunningScan();
    void StopRunningScan();
    void OnRunningScanned();  // 后台线程枚举完推回来的通知

    // 开始菜单的全量扫描也在后台（FullScanProc），完成后的接手动作
    void OnAppsScanned();
    bool launcherBuilt_ = false;   // 启动台是否已拿到全量列表（扫描完成前是 false）
    void LaunchOrActivate(int index);
    void ShowMenu(int x, int y);

    // 2026-10-11 - AI - 左键：有「别的地方可去」的应用先弹菜单，其余照常直接启动
    void OnIconClick(int idx, int mx, int my);
    void ShowExtrasMenu(int idx, const AppItem& app,
                        const std::vector<AppExtra>& extras, int mx, int my);

    // 2026-10-11 - AI - Dock 右侧那一区：正在跑、但没固定在 Dock 上
    //
    // macOS 的 Dock 在分隔线右边放「正在运行的应用」，Windows 的任务栏也是这个思路。
    // 固定区写死在 pinned.txt，运行区是活的一一开关应用就变。
    struct RunEntry {
        std::wstring path;      // 小写的 exe 路径
        DWORD lastSeen = 0;     // 最后一次还看得见它的时刻
    };
    // 刷新运行区；返回 true 表示有增删，调用方要重建列表
    bool UpdateRunningExtras(const std::set<std::wstring>& paths, DWORD now);
    // 固定区 + 运行区 → apps_，然后整套重排
    void RebuildList();
    void SetVisible(bool vis);       // 滑出 / 收回 Dock（带动画）
    void CaptureBackdrop();          // 抓 Dock 底下的屏幕画面并模糊，做磨砂玻璃底
    void BuildPanel();               // 把玻璃+色调+颗粒+反光预先合成一整块，供每帧直接贴
    void BarGeometry(float& top, float& bottom, float& height) const;  // 胶囊条的垂直几何
    void LoadSettings();             // 读 %APPDATA%\MyDock\settings.ini
    void SaveSettings();             // 写回去
    // 自动收起的两条判据（settings.ini 里可以改，见 LoadSettings）：
    //   edgePx_     —— 鼠标压到屏幕最底下多少逻辑像素以内，Dock 才升起来
    //   hideDistPx_ —— 鼠标离开 Dock 多远之后，Dock 才降下去
    void UpdateRiseRules();
    void ApplyGlow(float v);         // 改悬停高亮强度，立刻重画
    // 换掉整个应用列表之后重来一遍：重算尺寸、重排、重截背景、重画
    void ApplyApps(std::vector<AppItem>&& list);

    // 2026-10-11 - AI - 多显示器：Dock 站哪块屏，就按那块屏算位置和尺寸
    //
    // 以前用的是 SM_CXSCREEN / SM_CYSCREEN，那两个只对应主屏。接了第二块屏之后
    // （尤其两块缩放不一样）就会两头错：图标尺寸按主屏的 DPI 算，画到副屏上
    // 大一圈；位置按主屏高度算，副屏比主屏矮时 Dock 直接落到屏幕外面去 ——
    // 表现就是「在副屏底部划拉半天，Dock 毫无反应」。
    void UpdateMonitor(bool allowSwitch);  // 按鼠标位置刷新目标屏
    void ApplyMonitorGeometry();           // 按当前屏重算 winX_/winY_ 并挪窗口
    void OnDpiChanged(int newDpi);         // 那块屏的 DPI 变了：整条 Dock 重排

    // 2026-10-11 - AI - 托盘图标 + 开机自启
    // Dock 没有自己的窗口按钮，进程死了只能手动找 exe —— 托盘图标是
    // 「它还活着」的可见凭据，也是崩溃后重开的入口。
    void AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu();
    // 从全量列表里按 pinned.txt 的顺序挑出 Dock 要显示的那些
    std::vector<AppItem> PickFromPinned(std::vector<AppItem>& all);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    void* bits_ = nullptr;
    int w_ = 0, h_ = 0;
    int winX_ = 0, winY_ = 0;

    // 当前目标屏（鼠标所在那块）。Dock 展开时不允许换屏，见 UpdateMonitor。
    HMONITOR monitor_ = nullptr;
    RECT monFull_{};
    int  monDpi_ = 96;

    HICON trayIcon_ = nullptr;         // 运行时画的托盘小图标
    NOTIFYICONDATAW nid_ = {};

    Gdiplus::Bitmap* glass_ = nullptr;  // 模糊后的背景，磨砂玻璃的底
    Gdiplus::Bitmap* noise_ = nullptr;  // 噪点纹理，给玻璃加颗粒感
    Gdiplus::Bitmap* panel_ = nullptr;  // 玻璃+色调+颗粒+反光合成的整块（每帧直接贴，不重画）

    std::vector<AppItem> apps_;      // 真正画出来的那一排 = 固定区 + 运行区
    std::vector<AppItem> pinnedApps_;   // 固定区，只有 pinned.txt 变了才重建
    std::vector<RunEntry> runEntries_;  // 运行区，顺序就是首次出现的顺序
    int pinnedCount_ = 0;               // apps_ 里前多少个属于固定区（分隔线画在它后面）
    int sepW_ = 0;                      // 两区之间那条分隔线占的宽度
    std::vector<float> x_;           // 每个图标的左边距（开机排一次，之后再不改动）
    std::vector<float> curSize_;     // 图标边长。这一版取消了放大，恒等于 baseSize_
    std::vector<float> targetSize_;  // 同上，保留是为了让排版/命中/标签那几处不用改
    std::vector<float> hl_;          // 每个图标当前的高亮强度 0~1（逐帧逼近 hlTarget_）
    std::vector<float> hlTarget_;    // 鼠标位置决定的目标高亮强度
    std::vector<bool> running_;

    int hover_ = -1;
    int mouseX_ = -1;
    bool tracking_ = false;
    bool mouseInside_ = false;       // 鼠标是否就贴在 Dock 上（决定放大是否插值）
    bool shown_ = true;              // 窗口当前是否真的可见
    bool flush_ = false;             // 每帧是否等一次 DWM 合成（对齐显示器节拍）
    bool wantVisible_ = true;        // 逻辑上要不要显示
    bool animating_ = false;
    float slide_ = 0.0f;             // 0 = 完全露出，1 = 完全滑出屏幕
    float slideTarget_ = 0.0f;
    int edgePx_ = 3;          // 「屏幕最底部」有多厚（逻辑像素）
    int hideDistPx_ = 40;     // 离开 Dock 多远才收起（逻辑像素）
    DWORD lastInside_ = 0;
    DWORD lastRenderTick_ = 0;       // 上一帧是什么时候画出来的（用来限流）
    DWORD fullscreenCheckDue_ = 0;   // 上次检查「有没有全屏应用」的时刻
    bool  fullscreenNow_ = false;
    DWORD graceUntil_ = 0;           // 启动后的保护期，在这之前不自动收起

    int labelH_ = 0;
    int baseSize_ = 0;
    int gap_ = 0;
    int padX_ = 0;
    int padTop_ = 0;
    int padBottom_ = 0;
    int glowRadius_ = 0;   // 高亮的影响半径：鼠标离多远还能「带亮」旁边的图标

    // 这一版把「悬停放大」整个去掉了，改成「悬停高亮」（方案二）。
    // 理由：放大让整行凭空多出约 154px 宽，这笔宽度只有三个出口 —— 让 dock 变宽、
    // 让空隙缩小、让图标相对光标漂移 —— 三者守恒，堵住一个另一个就冒出来，
    // 怎么调都会留一层抖动。高亮完全不占宽度，于是布局变成常量：
    // 图标尺寸恒定、位置恒定、行宽恒定，抖动在物理上不可能发生。
    int   barL_ = 0;      // 胶囊左边缘（窗口坐标）
    int   barW_ = 0;      // 胶囊宽度（固定）
    int   barPad_ = 0;    // 胶囊两端留白
    float rowW_ = 0.0f;   // 整行的实际宽度（常量）

    // 悬停高亮的强度，0~1。0 = 关掉。可以在 Dock 上右键切换，也会记到 settings.ini。
    float glow_ = 1.0f;

    DWORD lastRunningCheck_ = 0;

    // 帧率与耗时统计（只在 MYDOCK_TRACE 打开时被读写，平时零开销）
    DWORD statFrames_ = 0;
    DWORD statWindow_ = 0;
    double statGapUs_ = 0.0;    // 上一帧结束 → 这一帧开始，即"等"了多久
    double statTotalUs_ = 0.0;  // 一帧绘制总共多久
    double statBgUs_ = 0.0;     // 其中：画玻璃背景
    double statIconUs_ = 0.0;   // 其中：画图标
    double statUlwUs_ = 0.0;    // 其中：提交给窗口
    double lastRenderEndUs_ = 0.0;
    double statRefreshWorstMs_ = 0.0;  // 枚举「哪些应用在运行」最久花了多少
    double statRefreshTotalMs_ = 0.0;
    int    statRefreshCount_ = 0;
    int    statTotalFrames_ = 0;       // 累计帧数（用来算长期帧率）

    // 帧间隔的分布 —— 平均帧率看不出一顿一顿，得看**周期**本身稳不稳
    double statGapMinUs_ = 1e9;
    double statGapMaxUs_ = 0.0;
    int    statGapHist_[8] = {0};      // <6 / <8 / <10 / <12 / <14 / <16 / <20 / ≥20 ms
    double lastRenderStartUs_ = 0.0;
    int    mouseMoves_ = 0;            // 这一秒收到了多少个鼠标移动消息

    Launcher launcher_;                // 全屏启动台（F4 打开，或者右键菜单里点）

    PopupMenu menu_;                   // 右键菜单：自绘的，跟启动台共用同一套外观
    bool menuOpen_ = false;            // 菜单弹出期间不让 Dock 收起（见 WM_TIMER 里那条）
};

}
