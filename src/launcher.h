// 2026-10-09 - AI - 启动台窗口：全屏应用网格、打字筛选、固定/取消固定到 Dock
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>

#include "apps.h"
#include "menu.h"

namespace mydock {

// 用户在启动台里改了固定项之后，拿这条消息通知 Dock 重建它自己的图标列表。
// Dock 现在只认 pinned 清单，所以它得知道清单变了。
constexpr UINT kMsgPinnedChanged = WM_APP + 1;

// 启动台要拍背景了，让 Dock 立刻收起（不要滑动动画）。
// ★ 这条是必需的：Dock 平时是靠 120ms 一次的轮询才收起来的，而启动台是在
//   打开的那一瞬间就拍下桌面当背景 —— 不等这一下的话，Dock 会被拍进背景里，
//   模糊压暗之后看着就像 Dock 还浮在启动台上面。
constexpr UINT kMsgHideNow = WM_APP + 2;

class Launcher {
public:
    ~Launcher();

    // owner 是 Dock 的窗口句柄，用来：① 关闭时把焦点还回去
    //                              ② 固定项变动后通知它重建自己的图标列表
    bool Create(HINSTANCE inst, HWND owner, std::vector<AppItem> allApps);

    HWND Handle() const { return hwnd_; }
    bool IsOpen() const { return open_; }

    // Dock 重建清单时要拿全量列表（它自己只留那十几个）
    const std::vector<AppItem>& AllApps() const { return apps_; }

    // 重新扫描之后换掉全量列表（旧的图标随之释放）
    void SetApps(std::vector<AppItem>&& list);
    void SetDockWindow(HWND d) { dock_ = d; }

    void Toggle();
    void Open();
    void Close();

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);

    void CreateBackBuffer(int w, int h);
    void DropHidden();                 // 按 hidden.txt 把「已移出启动台」的剔掉
    void LayoutGrid();                 // 按窗口大小和当前条目数算行列，定每格的尺寸
    void ApplyFilter();                // 依输入的字筛出要显示的条目
    void EnsureIcons();                // 图标是打开时才加载（Shell 取图一次几十毫秒）
    void CaptureBackdrop();            // 趁窗口没显示，拍下它背后的桌面
    void BuildBackground();            // 模糊 + 压暗，合一张全屏底图（只做一次）
    void Composite();                  // 底图 + 所有图标 + 名字 —— 合成到 canvas_
    void Render();                     // 贴 canvas_，再补悬停框和搜索框
    void RequestAnim();
    void Tick();
    bool StepFade(double dtMs);
    void SearchBoxRect(int& x, int& y, int& w, int& h) const;  // 搜索框的屏幕矩形
    void ImeFollowCaret();   // 把输入法的候选窗挪到搜索框下面，别飘在屏幕左上角
    int  HitTest(int x, int y) const;  // 命中的是「页内第几格」，不是 apps_ 的下标
    int  AppIndex(int slot) const;     // 页内格号 → apps_ 的下标（-1 = 没有）
    int  PageCount() const;
    int  PageSlots() const;            // 当前这一页实际有几格（最后一页会少一点）
    bool ChangePage(int delta);        // 翻页，翻不动就返回 false
    void LaunchIndex(int slot);
    void ShowAppMenu(int x, int y, int slot);
    // 把一个应用从这一页里拿掉（界面这一份；写清单那份在 appaction.cpp）
    void RemoveFromList(int idx);

    // ---- 「+」：手动往启动台里加一个 exe / lnk ----
    void AddButtonRect(int& x, int& y, int& w, int& h) const;
    bool HitAddButton(int x, int y) const;
    void AddAppFromDialog();       // 开文件选择框挑文件，加进来并记到 added.txt
    void AddAppByPath(const std::wstring& path);
    void DrawAddButton(Gdiplus::Graphics& g);
    void MergeAddedApps();         // 把 added.txt 里的手动添加项并进 apps_

    // ---- 右键菜单 ----
    //
    // ★ 菜单的「长相」一份代码写在 menu.h/menu.cpp 里，Dock 和启动台共用。
    //   系统那个 TrackPopupMenu 长什么样由主题决定 —— Win10/11 上就是灰底方角、
    //   悬停在左边画一条竖线的老式菜单，跟这套毛玻璃完全不是一回事。
    void BuildMenu(int slot);
    void LayoutMenu(int x, int y);   // 摆位：贴着图标，并在屏幕内收好
    void DrawMenu(Gdiplus::Graphics& g);
    int  MenuHit(int x, int y) const;   // 命中的项号，-1 = 没点在菜单里
    void CloseMenu();
    void RunMenuCmd(int cmd, int slot);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND dock_ = nullptr;
    HWND prevFocus_ = nullptr;   // 打开之前的前台窗口，退出时还给它
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    void* bits_ = nullptr;
    bool open_ = false;
    bool animating_ = false;
    bool inMenu_ = false;     // 右键菜单弹出期间，别因为失焦把自己关掉
    bool dirty_ = true;       // 画布需要重新合成（筛选字变了、条目变了）
    bool redraw_ = false;     // ★ 画布没变、只是要重画一遍（换选中格就是这种情况）
                              //   少了它，悬停高亮永远不显示：见 Tick() 里的说明

    // 菜单状态。菜单活在启动台这一张全屏画布上，不是独立窗口 ——
    // 所以这些坐标全是窗口内的坐标，跟着画布一起淡入淡出。
    bool menuOpen_ = false;
    int menuSlot_ = -1;            // 菜单针对第几格
    int menuX_ = 0, menuY_ = 0;    // 左上角
    int menuW_ = 0, menuH_ = 0;    // 尺寸
    int menuRowH_ = 0;             // 每项高
    int menuHover_ = -1;           // 鼠标停在第几项（-1 = 没停）
    std::vector<MenuItem> menuItems_;
    // 菜单的投影。模糊一次不便宜，而这里鼠标每动一下都要重画一帧 ——
    // 尺寸定下来的时候算一次就够，之后每帧直接贴。
    Gdiplus::Bitmap* menuShadow_ = nullptr;
    int menuShadowPad_ = 0;

    bool addHover_ = false;        // 鼠标是否停在左上角的「+」上

    int w_ = 0, h_ = 0;

    std::vector<AppItem> apps_;      // 全部应用
    std::vector<int> shown_;         // 筛选后要显示的，存的是 apps_ 里的下标
    std::wstring query_;             // 用户敲进去的字
    std::wstring compQuery_;         // 输入法正在拼、还没上屏的那一段

    // 网格几何（LayoutGrid 算一次）
    int iconSize_ = 0;
    int cellW_ = 0, cellH_ = 0;
    int cols_ = 0, rows_ = 0;
    int gridX_ = 0, gridY_ = 0;      // 网格左上角（已经居中过了）
    int page_ = 0;                   // 第几页（放不下一页时才会有第二页）
    int perPage_ = 0;                // 一页放得下几格

    int active_ = -1;                // 当前选中的格号（键盘和鼠标共用）

    Gdiplus::Bitmap* glass_ = nullptr;   // 模糊后的桌面
    Gdiplus::Bitmap* noise_ = nullptr;
    Gdiplus::Bitmap* bg_ = nullptr;      // 模糊 + 压暗，全屏底图（只合成一次）
    Gdiplus::Bitmap* canvas_ = nullptr;  // bg_ + 图标 + 名字；只在筛选字变了才重新合成一次

    float fade_ = 0.0f;          // 0 = 完全透明，1 = 完全显示
    float fadeTarget_ = 0.0f;
    DWORD lastFrameTick_ = 0;
    DWORD fadeLastTick_ = 0;
};

}  // namespace mydock
