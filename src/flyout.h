// 2026-10-11 - AI - 顶栏时钟点开的日历小面板
//
// 点「10月11日 周日 18:28」那一块，右上角弹出当月月历：
// 今天画蓝圈、‹ › 可以翻月、点面板外面或按 ESC 关掉。
// 外观走顶栏同一套（毛玻璃底 + 跟随深浅色），交互模式照抄 PopupMenu：
// 一个模态消息循环，关掉才返回 —— Dock 的轮询定时器在这条循环里照常跑，
// 所以面板开着时 Dock 该收还是收。
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>

namespace mydock {

class CalendarFlyout {
    // 持有裸指针资源（Bitmap/HDC/HBITMAP），默认拷贝语义 = 双重释放隐患。
    // cppcheck 六个类各报一条之后统一禁掉拷贝（2026-10-12）
        CalendarFlyout(const CalendarFlyout&) = delete;
    CalendarFlyout& operator=(const CalendarFlyout&) = delete;

public:
    CalendarFlyout() = default;
    ~CalendarFlyout();

    // (ax, ay) = 锚点（屏幕坐标，一般给时钟区左缘和顶栏高度）。
    // dark = 按深色画。阻塞到用户关掉为止，永远返回 false（没有可选项可给）。
    bool Toggle(HINSTANCE inst, int ax, int ay, bool dark);

    bool IsOpen() const { return open_; }

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void EnsureWindow(HINSTANCE inst);
    void CreateBackBuffer(int w, int h);
    void CaptureBackdrop();               // 拍面板背后的画面做毛玻璃（只拍一次）
    void Render();
    void Finish();                        // 关掉：跳出模态循环
    void AddMonths(int delta);            // ‹ › 用；自动处理跨年
    void Layout();                        // 按当前月份摆好 42 个格子

    HWND     hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HDC      dc_ = nullptr;
    HBITMAP  bmp_ = nullptr;
    void*    bits_ = nullptr;
    Gdiplus::Bitmap* backdrop_ = nullptr; // 毛玻璃底，翻月不重拍（背后没变）

    int  w_ = 0, h_ = 0, pad_ = 0;
    int  winX_ = 0, winY_ = 0;
    bool open_ = false;
    bool looping_ = false;
    bool dark_ = true;

    // 日期网格：6 行 × 7 列（周一起始），首尾是相邻月份的日子（画淡一点）
    struct Cell { int day; bool cur; bool today; };
    Cell cells_[42] = {};
    int  gridTop_ = 0;                    // 网格第一行的 y
    int  cellW_ = 0, cellH_ = 0;
    int  headH_ = 0;                      // 头部（标题+箭头）那一行的高
    int  arrowW_ = 0, arrowY_ = 0;        // ‹ › 命中区（左右各一个，等宽）
    wchar_t title_[32] = {};              // 「2026年10月」
    std::wstring todayLine_;              // 底部「10月11日 星期日」
    SYSTEMTIME month_{};                  // 正在显示的哪个月
    SYSTEMTIME today_{};                  // 真正的今天（高亮和底部文字用）
};

}  // namespace mydock
