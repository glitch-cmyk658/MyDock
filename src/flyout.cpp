// 2026-10-11 - AI - 日历面板的实现：月历渲染 + 模态交互
//
// ★ 日期换算全部走天数表 + 自己算闰年，不碰 localtime：
//   _mkgmtime / localtime_s 会受系统时区影响，而「10 月 1 号是周几」
//   是个纯日历问题，跟时区无关 —— 用表算一次就对，不用赌环境。
#include "flyout.h"
#include "util.h"

#include <cwchar>

namespace mydock {
namespace {

const wchar_t* kClass = L"MyDockCalendarFlyout";

// 平年每月天数；闰年 2 月单独加一天
int DaysInMonth(int y, int m) {
    static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 30;
    int d = md[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) d = 29;
    return d;
}

// 某年某月 1 号是周几（0 = 周日）。蔡勒公式化简版，仅用于公历。
int WeekdayOfFirst(int y, int m) {
    // 把 1/2 月当成上一年的 13/14 月（这个公式的惯例）
    if (m < 3) { m += 12; --y; }
    const int K = y % 100, J = y / 100;
    const int q = 1;
    const int h = (q + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7;   // 蔡勒的 0=周六 → 转成 0=周日
}

}  // namespace

CalendarFlyout::~CalendarFlyout() {
    if (dc_) DeleteDC(dc_);
    if (bmp_) DeleteObject(bmp_);
    delete backdrop_;
}

bool CalendarFlyout::Toggle(HINSTANCE inst, int ax, int ay, bool dark) {
    inst_ = inst;
    dark_ = dark;

    GetLocalTime(&today_);
    month_ = today_;

    // 布局常量（全按当前 DPI 算；面板固定在主屏，跟顶栏一致）
    pad_   = Dpi(14);
    cellW_ = Dpi(40);
    cellH_ = Dpi(34);
    const int weekH = Dpi(24);
    const int footH = Dpi(30);
    arrowW_ = Dpi(34);
    arrowY_ = pad_;
    headH_  = Dpi(42);

    w_ = cellW_ * 7 + pad_ * 2;
    h_ = pad_ * 2 + headH_ + weekH + cellH_ * 6 + footH;
    gridTop_ = pad_ + headH_ + weekH;

    swprintf(title_, 32, L"%d年%d月", month_.wYear, month_.wMonth);
    static const wchar_t* wk[] = {L"星期日", L"星期一", L"星期二", L"星期三",
                                  L"星期四", L"星期五", L"星期六"};
    todayLine_ = std::to_wstring(today_.wMonth) + L"月" +
                 std::to_wstring(today_.wDay) + L"日 " + wk[today_.wDayOfWeek];

    Layout();

    // 位置：锚点所在屏的右上角。ax 只是用来认屏（多屏时副屏上点时钟，
    // 面板也该出现在那块屏的右上，而不是主屏）。
    const MonitorInfo mon = MonitorAt(POINT{ax, ay});
    winX_ = mon.full.right - w_ - Dpi(10);
    winY_ = ay + Dpi(6);

    EnsureWindow(inst);
    if (!hwnd_) return false;

    CreateBackBuffer(w_, h_);
    CaptureBackdrop();
    Render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    SetCapture(hwnd_);
    SetForegroundWindow(hwnd_);   // 要键盘才有 ESC

    // 模态循环 —— 和 PopupMenu::Show 同一套
    open_ = true;
    looping_ = true;
    MSG msg = {};
    while (looping_) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            PostQuitMessage(0);   // 收到 WM_QUIT：转交出去再退
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
    ShowWindow(hwnd_, SW_HIDE);
    return false;
}

void CalendarFlyout::EnsureWindow(HINSTANCE inst) {
    if (hwnd_) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);   // 重复注册失败无所谓，类已在了

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kClass, L"日历", WS_POPUP,
        winX_, winY_, w_, h_, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log(L"日历面板：创建窗口失败，错误码 %lu", GetLastError());
    }
}

void CalendarFlyout::CreateBackBuffer(int w, int h) {
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }

    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);
    bmp_ = CreateCompatibleBitmap(screen, w, h);
    ReleaseDC(nullptr, screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* p = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &p, nullptr, 0);
    if (!dib || !p) return;
    // 把 DIB 选进内存 DC，之后每帧直接写 bits_
    DeleteObject(SelectObject(dc_, dib));
    // 注意：保留 dib 的引用计数 —— 它被选进 dc_ 了，析构时随 dc_ 释放
    (void)dib;
    bits_ = p;
}

void CalendarFlyout::CaptureBackdrop() {
    delete backdrop_;
    backdrop_ = nullptr;
    // 面板还没显示，现在拍到的就是它背后那块屏幕
    Gdiplus::Bitmap* shot = CaptureScreenArea(winX_, winY_, w_, h_);
    if (!shot) return;
    backdrop_ = BlurBitmap(shot, 16);   // BlurBitmap 会接管并释放 shot
}

void CalendarFlyout::AddMonths(int delta) {
    int y = month_.wYear, m = month_.wMonth + delta;
    while (m > 12) { m -= 12; ++y; }
    while (m < 1)  { m += 12; --y; }
    month_.wYear = static_cast<WORD>(y);
    month_.wMonth = static_cast<WORD>(m);
    swprintf(title_, 32, L"%d年%d月", month_.wYear, month_.wMonth);
    Layout();
}

void CalendarFlyout::Layout() {
    const int y = month_.wYear, m = month_.wMonth;
    const int dim  = DaysInMonth(y, m);
    // 上个月的最后几天，填满网格开头（周一起始）
    const int dimPrev = DaysInMonth(m == 1 ? y - 1 : y, m == 1 ? 12 : m - 1);
    const int offset = (WeekdayOfFirst(y, m) + 6) % 7;   // 周一 = 0

    for (int i = 0; i < 42; ++i) {
        int day;
        bool cur;
        if (i < offset) {
            day = dimPrev - offset + 1 + i;
            cur = false;
        } else if (i < offset + dim) {
            day = i - offset + 1;
            cur = true;
        } else {
            day = i - (offset + dim) + 1;
            cur = false;
        }
        const bool today = cur && y == today_.wYear && m == today_.wMonth &&
                           day == today_.wDay;
        cells_[i] = {day, cur, today};
    }
}

void CalendarFlyout::Finish() {
    open_ = false;
    looping_ = false;
}

void CalendarFlyout::Render() {
    if (!dc_ || !bits_) return;
    memset(bits_, 0, static_cast<size_t>(w_) * h_ * 4);

    Gdiplus::Graphics g(dc_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    // 面板形状：圆角矩形。圆角外保持 alpha=0，窗口看起来就是一块圆角玻璃
    Gdiplus::GraphicsPath panel;
    AddRoundRect(panel, Gdiplus::RectF(0.5f, 0.5f,
                 static_cast<float>(w_ - 1), static_cast<float>(h_ - 1)),
                 static_cast<float>(Dpi(10)));

    // ---- 配色：照 macOS 日历来的（2026-10-11 上网核对过的几条硬规格）----
    //   今天 = 白字**红**底圆（Apple Calendar 里今天就是红圈，不是蓝）；
    //   周六/周日列的数字用红色；相邻月的日期淡灰；星期行灰。
    //   浅色底白卡（不透明度 97%），深色底 #1C1C1E —— 都是 macOS 系统色的近似值。
    const bool dk = dark_;
    const Gdiplus::Color cBgTint = dk ? Gdiplus::Color(232, 28, 28, 30)
                                      : Gdiplus::Color(246, 252, 252, 254);
    const Gdiplus::Color cRim    = dk ? Gdiplus::Color(40, 255, 255, 255)
                                      : Gdiplus::Color(28, 0, 0, 0);
    const Gdiplus::Color cTitle  = dk ? Gdiplus::Color(255, 245, 245, 247)
                                      : Gdiplus::Color(255, 29, 29, 31);
    const Gdiplus::Color cWeek   = dk ? Gdiplus::Color(255, 142, 142, 147)
                                      : Gdiplus::Color(255, 134, 134, 139);
    const Gdiplus::Color cDay    = dk ? Gdiplus::Color(255, 242, 242, 247)
                                      : Gdiplus::Color(255, 29, 29, 31);
    const Gdiplus::Color cDim    = dk ? Gdiplus::Color(150, 99, 99, 102)
                                      : Gdiplus::Color(150, 199, 199, 204);
    const Gdiplus::Color cWeekend= dk ? Gdiplus::Color(255, 255, 69, 58)    // #FF453A
                                      : Gdiplus::Color(255, 255, 59, 48);   // #FF3B30
    const Gdiplus::Color cToday  = dk ? Gdiplus::Color(255, 255, 69, 58)
                                      : Gdiplus::Color(255, 255, 59, 48);

    Gdiplus::SolidBrush brTitle(cTitle);
    Gdiplus::SolidBrush brWeek(cWeek);
    Gdiplus::SolidBrush brDay(cDay);
    Gdiplus::SolidBrush brDim(cDim);
    Gdiplus::SolidBrush brWknd(cWeekend);

    // ★ 字号单位必须是 UnitPixel。Gdiplus::Font 默认按 point 解释，12「pt」
    //   在高 DPI 的 Graphics 上会被放大到 20+ 物理像素 —— 第一版就是这样，
    //   字大到把「12」挤出格子，又因为 StringFormat 默认允许换行，
    //   溢出的字符被折到第二行再被格子裁掉，看起来就是「两位数只显示一位」。
    //   项目里 menu/dock/launcher 的 Font 全都显式给了 UnitPixel，只有这里漏了。
    Gdiplus::Font fTitle(L"Microsoft YaHei UI", Dpi(15),
                         Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fSmall(L"Microsoft YaHei UI", Dpi(11),
                         Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fDay(L"Microsoft YaHei UI", Dpi(13),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    // NoWrap 同理：数字绝不允许折行，放不下就让它出去（实际上 13px 的
    // 两位数远小于 40px 的格子，正常永远放得下）。
    Gdiplus::StringFormat cf(Gdiplus::StringFormatFlagsNoWrap);

    g.SetClip(&panel);
    if (backdrop_) {
        g.DrawImage(backdrop_, 0, 0, w_, h_);
    }
    Gdiplus::SolidBrush tint(cBgTint);
    g.FillRectangle(&tint, 0, 0, w_, h_);
    g.ResetClip();

    Gdiplus::Pen rim(cRim, 1.0f);
    g.DrawPath(&rim, &panel);

    cf.SetAlignment(Gdiplus::StringAlignmentCenter);
    cf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    // ---- 头部：‹ 2026年10月 › ----
    {
        const int headCy = pad_ + Dpi(21);
        Gdiplus::RectF tb(static_cast<float>(pad_ + arrowW_),
                          static_cast<float>(headCy - Dpi(14)),
                          static_cast<float>(w_ - (pad_ + arrowW_) * 2),
                          static_cast<float>(Dpi(28)));
        g.DrawString(title_, -1, &fTitle, tb, &cf, &brTitle);

        // 箭头画小一点、命中区大一点 —— 小三角很难戳中
        Gdiplus::SolidBrush brArrow(cWeek);
        Gdiplus::RectF la(static_cast<float>(pad_), static_cast<float>(arrowY_),
                          static_cast<float>(arrowW_),
                          static_cast<float>(headH_));
        g.DrawString(L"\x2039", 1, &fTitle, la, &cf, &brArrow);
        Gdiplus::RectF ra(static_cast<float>(w_ - pad_ - arrowW_),
                          static_cast<float>(arrowY_),
                          static_cast<float>(arrowW_),
                          static_cast<float>(headH_));
        g.DrawString(L"\x203A", 1, &fTitle, ra, &cf, &brArrow);
    }

    // ---- 星期行：周一起始 ----
    {
        static const wchar_t* wk[7] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};
        const int y = gridTop_ - Dpi(24);
        for (int i = 0; i < 7; ++i) {
            Gdiplus::RectF c(static_cast<float>(pad_ + i * cellW_),
                             static_cast<float>(y),
                             static_cast<float>(cellW_),
                             static_cast<float>(Dpi(24)));
            g.DrawString(wk[i], 1, &fSmall, c, &cf, &brWeek);
        }
    }

    // ---- 日期网格 ----
    for (int i = 0; i < 42; ++i) {
        const int row = i / 7, col = i % 7;
        const int cx = pad_ + col * cellW_;
        const int cy = gridTop_ + row * cellH_;
        const Cell& c = cells_[i];

        if (c.today) {
            // 今天：蓝圆 + 白字。圆的直径取格子的 0.78，留一点呼吸感
            const float d = static_cast<float>(cellH_) * 0.78f;
            Gdiplus::SolidBrush brToday(cToday);
            g.FillEllipse(&brToday,
                          static_cast<float>(cx) + (cellW_ - d) / 2,
                          static_cast<float>(cy) + (cellH_ - d) / 2, d, d);
            wchar_t num[8] = {};
            swprintf(num, 8, L"%d", c.day);
            Gdiplus::SolidBrush brWhite(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::RectF rc(static_cast<float>(cx), static_cast<float>(cy),
                              static_cast<float>(cellW_),
                              static_cast<float>(cellH_));
            g.DrawString(num, -1, &fDay, rc, &cf, &brWhite);
        } else {
            wchar_t num[8] = {};
            swprintf(num, 8, L"%d", c.day);
            Gdiplus::RectF rc(static_cast<float>(cx), static_cast<float>(cy),
                              static_cast<float>(cellW_),
                              static_cast<float>(cellH_));
            // 周六/周日列用红色 —— Apple Calendar 的老规矩
            Gdiplus::Brush* b = c.cur ? static_cast<Gdiplus::Brush*>(
                                    col >= 5 ? &brWknd : &brDay)
                                  : static_cast<Gdiplus::Brush*>(&brDim);
            g.DrawString(num, -1, &fDay, rc, &cf, b);
        }
    }

    // ---- 底部：完整的今天 ----
    {
        Gdiplus::RectF fb(static_cast<float>(pad_),
                          static_cast<float>(h_ - pad_ - Dpi(26)),
                          static_cast<float>(w_ - pad_ * 2),
                          static_cast<float>(Dpi(26)));
        g.DrawString(todayLine_.c_str(), -1, &fSmall, fb, &cf, &brWeek);
    }

    // 贴上屏幕（分层窗口）
    HDC screen = GetDC(nullptr);
    POINT src = {0, 0};
    POINT dst = {winX_, winY_};
    SIZE size = {w_, h_};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK CalendarFlyout::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    CalendarFlyout* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<CalendarFlyout*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = h;   // 见 PopupMenu::WndProc 里那篇说明
    } else {
        self = reinterpret_cast<CalendarFlyout*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT CalendarFlyout::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            const int mx = static_cast<short>(LOWORD(lp));
            const int my = static_cast<short>(HIWORD(lp));
            // 点在面板外 = 关掉（SetCapture 把外面的点击也送来了）
            if (mx < 0 || my < 0 || mx >= w_ || my >= h_) { Finish(); return 0; }
            // 头部左右箭头：翻月。命中区取整个箭头格子，好点中
            const int headCy = arrowY_;
            if (my >= headCy && my < headCy + Dpi(42)) {
                if (mx >= pad_ && mx < pad_ + arrowW_) {
                    AddMonths(-1); Render(); return 0;
                }
                if (mx >= w_ - pad_ - arrowW_ && mx < w_ - pad_) {
                    AddMonths(1); Render(); return 0;
                }
            }
            // 点日期：纯展示面板，没有可点的东西 —— 不关、不动，装没听见。
            // 误触一个数字就把面板关掉反而烦人。
            return 0;
        }
        case WM_RBUTTONDOWN:
            Finish();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Finish();
            return 0;
        case WM_DESTROY:
            // 面板窗口被外力销毁（退出流程）：别让模态循环吊死
            Finish();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace mydock
