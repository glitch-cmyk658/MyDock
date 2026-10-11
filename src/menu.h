// 2026-10-09 - AI - 自绘菜单：macOS 那套圆角 + 整块蓝色高亮，Dock 与启动台共用
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>

namespace mydock {

struct MenuItem {
    int         cmd = 0;
    std::wstring label;
    bool        separator = false;   // 分隔线，不可点
    bool        checked = false;     // 左侧打个勾，表示当前状态
    bool        title = false;       // 标题行（被操作的对象名）：粗体灰字，不可点
    std::wstring tag = L"应用";      // 标题行右下角的小字；空串就不画
};

// 量一遍菜单该多大。宽度跟着最长的那一项走 —— 定死宽度的话，
// 短菜单留一堆空白、长名字（Anaconda PowerShell Prompt）又会被截断。
void MeasureMenu(const std::vector<MenuItem>& items, int& w, int& h, int& rowH);

// 预生成一张「菜单的柔和投影」：尺寸是 (w + 2*pad) × (h + 2*pad)。
//
// ★ 这个函数是给调用方**缓存**用的：弹菜单时算一次，之后每帧直接贴。
//   阴影是模糊出来的，每帧重算的话鼠标在菜单上一动就会卡。
// ★ 为什么不做成 DrawMenuBox 内部顺手画：那样两处（分层窗口 / 启动台画布）
//   都得每帧重算一遍同样的东西。
// 用完自己 delete，返回 nullptr 时按「没有投影」处理即可。
Gdiplus::Bitmap* MakeMenuShadow(int w, int h, int pad, float radius);

// 画一整块菜单：圆角底板 + 每一项（勾号、悬停蓝条、文字）。
// plateAlpha 是底板的不透明度，0~255。默认 246 —— 见下面 PopupMenu 里的取舍说明。
// shadow 是 MakeMenuShadow 出来的那张（可为空，就是不画投影）；
// 它会被贴在菜单框左上角往外的 pad 像素处，pad 从位图尺寸反推。
void DrawMenuBox(Gdiplus::Graphics& g, const std::vector<MenuItem>& items,
                 int x, int y, int w, int h, int rowH, int hoverIndex,
                 BYTE plateAlpha = 246, Gdiplus::Bitmap* shadow = nullptr);

// 投影四周要留多少像素（逻辑像素）。窗口尺寸 = 菜单 + 这个数的两倍。
int MenuShadowPad();

// 命中的是第几项（下标）；-1 = 没落在菜单里，-2 = 落在标题或分隔线上
int MenuHitItem(const std::vector<MenuItem>& items, int x, int y, int w, int rowH,
                int px, int py);

// ---- 独立的小窗口菜单（给 Dock 用）----
//
// ★ 为什么不用 TrackPopupMenu：
//   那个是系统控件，长什么样由系统主题决定 —— Win10/11 上就是灰底方角、
//   悬停在左边画一条竖线的老式菜单，跟 Dock 这层毛玻璃完全不是一回事。
//   而且它是独立的一个普通窗口，盖在毛玻璃上像贴了一张不透光的纸片。
//   自己开一个分层窗口画，才能跟 Dock 用同一套圆角、同一套材质。
class PopupMenu {
    // 持有裸指针资源（Bitmap/HDC/HBITMAP），默认拷贝语义 = 双重释放隐患。
    // cppcheck 六个类各报一条之后统一禁掉拷贝（2026-10-12）
        PopupMenu(const PopupMenu&) = delete;
    PopupMenu& operator=(const PopupMenu&) = delete;

public:
    PopupMenu() = default;
    ~PopupMenu();

    // 弹出并阻塞到这里，用户点了一项或取消才返回。
    // (ax, ay) 是屏幕坐标的锚点（一般是鼠标位置）；返回被点的 cmd，0 = 取消。
    int Show(HWND owner, int ax, int ay, const std::vector<MenuItem>& items);

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void EnsureWindow();
    void CreateBackBuffer(int w, int h);
    void BuildBackdrop(int sx, int sy, int w, int h);   // 拍菜单背后的画面并模糊
    void Render();
    void Finish(int cmd);            // 收工：记录结果并跳出这条消息循环

    HWND     hwnd_ = nullptr;
    HDC      dc_ = nullptr;
    HBITMAP  bmp_ = nullptr;
    void*    bits_ = nullptr;

    int      winX_ = 0, winY_ = 0;   // 窗口在屏幕上的位置（Render 贴窗口时要带上）
    int      winW_ = 0, winH_ = 0;   // 窗口尺寸（= 菜单 + 四周给投影留的边）
    int      menuW_ = 0, menuH_ = 0, rowH_ = 0;
    int      pad_ = 0;               // 菜单框到窗口边缘的距离，给投影用

    std::vector<MenuItem> items_;
    int      hover_ = -1;
    int      result_ = 0;
    bool     looping_ = false;

    Gdiplus::Bitmap* backdrop_ = nullptr;   // 菜单背后的桌面，模糊过的
    Gdiplus::Bitmap* shadow_ = nullptr;     // 菜单的投影（模糊好的，尺寸定下来时算一次）
};

}  // namespace mydock
