// 2026-10-09 - AI - 自绘菜单的实现：外观只有这一份代码，Dock 和启动台共用
#include "menu.h"
#include "util.h"

#include <algorithm>
#include <cmath>

namespace mydock {
namespace {

const wchar_t* kClass = L"MyDockMenuClass";

// 菜单的排版节拍。量尺寸 / 画 / 命中测试三处必须完全一致 ——
// 数字只写在这儿，谁也别自己再抄一份（抄漏一处，点击位置就跟画面对不上）。
struct Metrics {
    int rowH;      // 每一项多高
    int padV;      // 上下内边距（各自，不是合计）
    int sepH;      // 分隔线占的高度
    int leading;   // 文字左起的距离
    int radius;    // 底板圆角
};

// ★ 数值对着 macOS Big Sur 的 NSMenu 抄的（AppKit 的公开度量 + 社区实测）：
//     行高 22pt、字号 13pt、上下内边距各 4pt、文字左起 14pt、圆角 6pt。
//
//   ★ 改之前我们用的是行高 30、内边距上下各 6、圆角 9 —— 每一项都比 macOS
//     松一大截。项一多菜单就拉得老长，行与行之间空荡荡的，整块看着"散"。
//     用户说的「还是不好看」，一大半出在这儿：不是配色不对，是比例不对。
Metrics M() {
    Metrics m;
    m.rowH    = Dpi(22);
    m.padV    = Dpi(4);
    m.sepH    = Dpi(8);
    m.leading = Dpi(14);
    m.radius  = Dpi(6);
    return m;
}

}  // namespace

// ---- 量尺寸 ----
void MeasureMenu(const std::vector<MenuItem>& items, int& w, int& h, int& rowH) {
    const Metrics m = M();
    rowH = m.rowH;

    // 用一个 1x1 的假画布量字宽。之前是把宽度定死的，长名字
    // （Anaconda PowerShell Prompt）必被截断，短菜单又空一大片。
    Gdiplus::Bitmap probe(1, 1, PixelFormat32bppARGB);
    Gdiplus::Graphics pg(&probe);
    Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(13)),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fontBold(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(13)),
                           Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

    float widest = 0.0f;
    bool needTagRoom = false;      // 标题行右下角还挂着一枚小字（"应用"/"系统"）
    int total = m.padV * 2;
    for (const MenuItem& it : items) {
        if (it.separator) {
            total += m.sepH;
            continue;
        }
        if (it.title && !it.tag.empty()) needTagRoom = true;
        Gdiplus::RectF r;
        pg.MeasureString(it.label.c_str(), static_cast<INT>(it.label.size()),
                         it.title ? &fontBold : &font, Gdiplus::PointF(0.0f, 0.0f), &r);
        widest = std::max(widest, r.Width);
        total += rowH;
    }
    // 宽 = 最长的那行字 + 左起 m.leading + 右边同样留 m.leading。
    // 有标题行时右边再让出 tag 的位置，不然小字会压在应用名上。
    w = static_cast<int>(widest) + m.leading * 2 + (needTagRoom ? Dpi(48) : 0);
    w = std::max(w, Dpi(120));
    h = total;
}

// ---- 投影 ----
int MenuShadowPad() { return Dpi(16); }

// ★ 为什么是「模糊」而不是「叠几层描边」：
//   叠描边画出来的是一个等宽的灰圈，外缘还是硬的，远看像给菜单描了道黑边 ——
//   前一版就是这样，被用户点名嫌弃。真正的柔和投影只能靠模糊：
//   拿一个黑色圆角矩形做低通滤波，边缘自然衰减，才有那种"浮在桌面上"的观感。
//   BlurBitmap 是"降采样再放大"，本身就是一次低通滤波，正好干这个。
Gdiplus::Bitmap* MakeMenuShadow(int w, int h, int pad, float radius) {
    const int W = w + pad * 2;
    const int H = h + pad * 2;
    if (W <= 0 || H <= 0 || w <= 0 || h <= 0 || pad <= 0) return nullptr;

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(W, H, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    {
        Gdiplus::Graphics g(bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        // ★ 往里收一点再模糊。
        //   不收的话，投影紧贴菜单边缘那一圈就是全强度，看着像菜单"顶"出来
        //   一道深色边；收进去之后边缘处已经衰减到一半上下，才是"浮着"的观感。
        //   y 方向多收一点点，让投影的重心略偏下 —— 光是从上面来的。
        const float inset = static_cast<float>(pad) * 0.22f;
        Gdiplus::GraphicsPath p;
        AddRoundRect(p, Gdiplus::RectF(static_cast<float>(pad) + inset,
                                       static_cast<float>(pad) + inset * 1.7f,
                                       static_cast<float>(w) - inset * 2.0f,
                                       static_cast<float>(h) - inset * 2.0f),
                     radius + inset);
        // 78/255 ≈ 0.3。再浓就不像"悬浮"，像在菜单下面垫了块黑纸。
        Gdiplus::SolidBrush b(Gdiplus::Color(78, 0, 0, 0));
        g.FillPath(&b, &p);
        g.Flush();
    }
    // 模糊因子取 pad 的一半：模糊半径约等于 pad，
    // 投影正好在留出来的空间里衰减完，不会被窗口边缘切出硬角。
    return BlurBitmap(bmp, std::max(2, pad / 2));   // 会接管并释放 bmp
}

// ---- 画 ----
// ★ 底板的取舍：别为了「毛玻璃」把它做得很透。
//   macOS 的菜单看着像磨砂，是因为它背后还有一层强模糊；我们这里底下虽然也
//   是模糊图（Dock 的毛玻璃 / 启动台拍的桌面），可图标本身色块大又亮，一透
//   出来就花了 —— 实测 PotPlayer 的黄方块直接印在菜单上，像脏了一块。
//   所以底板几乎不透（默认 246/255），靠**圆角 + 投影**把它和背景分开。
void DrawMenuBox(Gdiplus::Graphics& g, const std::vector<MenuItem>& items,
                 int x, int y, int w, int h, int rowH, int hoverIndex,
                 BYTE plateAlpha, Gdiplus::Bitmap* shadow) {
    if (items.empty()) return;
    const Metrics m = M();

    const Gdiplus::RectF box(static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(w), static_cast<float>(h));

    // ① 投影：贴一张已经模糊好的位图，左上角落在菜单框外 pad 像素处。
    //    pad 从位图尺寸反推，调用方不必再传一遍。
    if (shadow && shadow->GetWidth() > w && shadow->GetHeight() > h) {
        const int pad = (shadow->GetWidth() - w) / 2;
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(shadow,
                    Gdiplus::Rect(x - pad, y - pad,
                                  shadow->GetWidth(), shadow->GetHeight()),
                    0, 0, shadow->GetWidth(), shadow->GetHeight(), Gdiplus::UnitPixel);
        g.SetInterpolationMode(Gdiplus::InterpolationModeDefault);
    }

    // ② 底板
    Gdiplus::GraphicsPath bp;
    AddRoundRect(bp, box, static_cast<float>(m.radius));
    Gdiplus::SolidBrush bb(Gdiplus::Color(plateAlpha, 246, 246, 248));
    g.FillPath(&bb, &bp);

    // ③ 一圈极淡的外边界。
    //    ★ 前一版这里描的是一条白色 —— 浅色底上画白线等于什么也没画。
    //      浅色菜单要的是"淡黑线"，把菜单从同样偏亮的背景上切出来。
    Gdiplus::Pen border(Gdiplus::Color(26, 0, 0, 0), 1.0f);
    g.DrawPath(&border, &bp);

    Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(13)),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::Font fontBold(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(13)),
                           Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fontTiny(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(11)),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    Gdiplus::StringFormat fmt(Gdiplus::StringFormatFlagsNoWrap);
    fmt.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    int cy = y + m.padV;
    for (size_t i = 0; i < items.size(); ++i) {
        const MenuItem& it = items[i];
        if (it.separator) {
            // 分隔线左右缩进到跟文字对齐（macOS 就是这么做的），
            // 顶到边上看着像菜单被切了个口子。
            // 颜色也比前一版淡了：0.13 的黑在小尺寸上会显得像一条脏印子。
            Gdiplus::Pen sep(Gdiplus::Color(24, 0, 0, 0), 1.0f);
            const float sy = static_cast<float>(cy + m.sepH / 2) + 0.5f;
            g.DrawLine(&sep, static_cast<float>(x + m.leading), sy,
                       static_cast<float>(x + w - m.leading), sy);
            cy += m.sepH;
            continue;
        }

        const bool hovered = (static_cast<int>(i) == hoverIndex);
        if (!it.title && hovered) {
            // 整块圆角铺满，四周各留一点 —— 高亮条比菜单窄一圈。
            // Win11 那种在左边画一条竖线的老做法在这里显得特别旧。
            const int hoverInset = Dpi(5);
            Gdiplus::GraphicsPath hp;
            AddRoundRect(hp, Gdiplus::RectF(static_cast<float>(x + hoverInset),
                                            static_cast<float>(cy),
                                            static_cast<float>(w - hoverInset * 2),
                                            static_cast<float>(rowH)),
                         static_cast<float>(Dpi(4)));
            Gdiplus::SolidBrush hb(Gdiplus::Color(255, 0, 122, 255));   // macOS 系统蓝
            g.FillPath(&hb, &hp);
        }

        const float textX = static_cast<float>(x + m.leading);
        if (it.title) {
            // 标题 = 被操作的对象名。本机重名的应用不少，不写清楚的话
            // 光看菜单不知道自己在动谁。
            Gdiplus::SolidBrush tb(Gdiplus::Color(150, 0, 0, 0));
            g.DrawString(it.label.c_str(), static_cast<INT>(it.label.size()), &fontBold,
                         Gdiplus::RectF(textX, static_cast<float>(cy),
                                        static_cast<float>(w - m.leading * 2),
                                        static_cast<float>(rowH)), &fmt, &tb);
            Gdiplus::StringFormat rightFmt(Gdiplus::StringFormatFlagsNoWrap);
            rightFmt.SetAlignment(Gdiplus::StringAlignmentFar);
            rightFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            if (!it.tag.empty()) {
                Gdiplus::SolidBrush sb(Gdiplus::Color(115, 0, 0, 0));
                g.DrawString(it.tag.c_str(), static_cast<INT>(it.tag.size()), &fontTiny,
                             Gdiplus::RectF(textX, static_cast<float>(cy),
                                            static_cast<float>(w - m.leading * 2),
                                            static_cast<float>(rowH)), &rightFmt, &sb);
            }
        } else {
            if (it.checked) {
                // 勾画两条线（字体里不一定有 ✓ 这个字形，用字符八成变豆腐块）。
                // ★ 颜色跟着悬停状态走：悬停时底板是蓝的，勾得是白的；
                //   没悬停时底板浅色，白勾几乎看不见（实测撞过这个）。
                const float cx = static_cast<float>(x + Dpi(11));
                const float yy = static_cast<float>(cy + rowH / 2) + 1.0f;
                const float s = static_cast<float>(Dpi(4));
                Gdiplus::Pen tick(hovered ? Gdiplus::Color(255, 255, 255, 255)
                                          : Gdiplus::Color(225, 0, 0, 0),
                                  std::max(1.6f, static_cast<float>(Dpi(1.8))));
                g.DrawLine(&tick, cx - s, yy, cx - s * 0.25f, yy + s * 0.8f);
                g.DrawLine(&tick, cx - s * 0.25f, yy + s * 0.8f, cx + s, yy - s * 0.7f);
            }
            Gdiplus::SolidBrush tb(hovered ? Gdiplus::Color(255, 255, 255, 255)
                                           : Gdiplus::Color(235, 26, 26, 28));
            g.DrawString(it.label.c_str(), static_cast<INT>(it.label.size()), &font,
                         Gdiplus::RectF(textX, static_cast<float>(cy),
                                        static_cast<float>(w - m.leading * 2),
                                        static_cast<float>(rowH)), &fmt, &tb);
        }
        cy += rowH;
    }
}

int MenuHitItem(const std::vector<MenuItem>& items, int x, int y, int w, int rowH,
                int px, int py) {
    const Metrics m = M();
    int total = m.padV * 2;
    for (const MenuItem& it : items) total += it.separator ? m.sepH : rowH;
    if (px < x || px > x + w || py < y || py > y + total) return -1;

    int cy = y + m.padV;
    for (size_t i = 0; i < items.size(); ++i) {
        const int ih = items[i].separator ? m.sepH : rowH;
        if (py >= cy && py < cy + ih) {
            if (items[i].separator || items[i].title) return -2;   // 不可点
            return static_cast<int>(i);
        }
        cy += ih;
    }
    return -1;
}

// =====================================================================
// 独立的菜单窗口（Dock / 顶部状态栏用）
// =====================================================================

PopupMenu::~PopupMenu() {
    if (hwnd_) DestroyWindow(hwnd_);
    delete backdrop_;
    delete shadow_;
    if (dc_) DeleteDC(dc_);
    if (bmp_) DeleteObject(bmp_);
}

void PopupMenu::EnsureWindow() {
    if (hwnd_) return;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        const ATOM a = RegisterClassExW(&wc);
        if (!a) Log(L"菜单：注册窗口类失败，错误码 %lu", GetLastError());
        registered = true;
    }

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                            kClass, L"MyDock 菜单", WS_POPUP,
                            0, 0, winW_, winH_, nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        Log(L"菜单：创建窗口失败，错误码 %lu（窗口 %dx%d）", GetLastError(), winW_, winH_);
    }
}

void PopupMenu::CreateBackBuffer(int w, int h) {
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

// 菜单底下那层：把「它挡住了什么」拍下来模糊一遍。
// 拍之前必须保证自己没在屏幕上，否则会把自己的上一帧也拍进去。
void PopupMenu::BuildBackdrop(int sx, int sy, int w, int h) {
    delete backdrop_;
    backdrop_ = nullptr;
    if (hwnd_ && IsWindowVisible(hwnd_)) ShowWindow(hwnd_, SW_HIDE);
    if (w <= 0 || h <= 0) return;

    Gdiplus::Bitmap* shot = CaptureScreenArea(sx, sy, w, h);
    if (!shot) return;
    backdrop_ = BlurBitmap(shot, 6);   // 会接管并释放 shot
}

void PopupMenu::Render() {
    if (!dc_ || !bits_ || winW_ <= 0 || winH_ <= 0) return;

    memset(bits_, 0, static_cast<size_t>(winW_) * static_cast<size_t>(winH_) * 4);

    Gdiplus::Graphics g(dc_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    const Gdiplus::RectF box(static_cast<float>(pad_), static_cast<float>(pad_),
                             static_cast<float>(menuW_), static_cast<float>(menuH_));
    if (backdrop_) {
        Gdiplus::GraphicsPath clip;
        AddRoundRect(clip, box, static_cast<float>(Dpi(6)));
        Gdiplus::GraphicsState st = g.Save();
        g.SetClip(&clip);
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.DrawImage(backdrop_, Gdiplus::Rect(0, 0, winW_, winH_),
                    0, 0, backdrop_->GetWidth(), backdrop_->GetHeight(),
                    Gdiplus::UnitPixel);
        g.Restore(st);
    }

    // 投影在 Show() 里算好并缓存 —— 鼠标在菜单上一动就要重画一帧，
    // 每帧重做一次模糊是撑不住的。
    DrawMenuBox(g, items_, pad_, pad_, menuW_, menuH_, rowH_, hover_, 232, shadow_);
    g.Flush();

    HDC screen = GetDC(nullptr);
    POINT src = {0, 0};
    // ★ dst 是「这扇窗口该待在哪」，不是源图的偏移。
    //   传 0,0 的话 UpdateLayeredWindow 会顺手把窗口挪到屏幕左上角 ——
    //   SetWindowPos 白摆一次位置。Dock 那边传的是它自己的坐标，这里同理。
    POINT dst = {winX_, winY_};
    SIZE size = {winW_, winH_};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

void PopupMenu::Finish(int cmd) {
    result_ = cmd;
    looping_ = false;
}

int PopupMenu::Show(HWND /*owner*/, int ax, int ay, const std::vector<MenuItem>& items) {
    if (items.empty()) return 0;
    items_ = items;
    MeasureMenu(items_, menuW_, menuH_, rowH_);

    // 窗口比菜单大一圈 —— 多出来的是投影的地方。
    // 分层窗口画不出自己边界以外的东西，不多留这几像素，投影会被切成直角。
    pad_ = MenuShadowPad();
    winW_ = menuW_ + pad_ * 2;
    winH_ = menuH_ + pad_ * 2;

    // 摆位：优先落在锚点右下方；下面放不下就翻到上面去，右边放不下就往左挪。
    //
    // 2026-10-11 - AI - 边界用「锚点所在那块屏」的**整屏矩形**，不是主屏，
    //   也不是工作区。
    //   不是主屏：Dock 会跟到副屏，锚点在副屏上时按主屏裁会把菜单拽走。
    //   不是工作区：TopBar 自己登记成了 AppBar，把工作区顶边推到 y=46 ——
    //   用工作区判边界的话，锚点 y=42（顶栏正下方）反而被判成「越出顶部」，
    //   整张菜单被翻到屏幕底上去（实测就是这样：锚点 (1434,42)，
    //   落点 (1434,1293)，1293 = 工作区底 - 菜单高）。
    //   菜单是 TOPMOST 的临时层，盖住任务栏没有任何问题，所以用整屏。
    const MonitorInfo mon = MonitorAt(POINT{ax, ay});
    const int scrL = mon.full.left;
    const int scrT = mon.full.top;
    const int scrR = mon.full.right;
    const int scrB = mon.full.bottom;
    const int margin = Dpi(8);
    int y = ay;
    if (y + winH_ > scrB - margin) y = ay - winH_;
    if (y < scrT + margin) y = std::max(scrT + margin, scrB - margin - winH_);
    int x = ax;
    if (x + winW_ > scrR - margin) x = scrR - margin - winW_;
    if (x < scrL + margin) x = scrL + margin;
    winX_ = x;
    winY_ = y;
    Log(L"菜单：%zu 项，窗口 %dx%d（菜单 %dx%d + 投影 %d）落在 (%d,%d)，锚点 (%d,%d)",
        items.size(), winW_, winH_, menuW_, menuH_, pad_, x, y, ax, ay);

    EnsureWindow();
    if (!hwnd_) return 0;

    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, winW_, winH_,
                 SWP_HIDEWINDOW | SWP_NOACTIVATE);
    CreateBackBuffer(winW_, winH_);

    // 投影：尺寸这时候才定下来，算一次存着。
    delete shadow_;
    shadow_ = MakeMenuShadow(menuW_, menuH_, pad_, static_cast<float>(Dpi(6)));

    BuildBackdrop(x, y, winW_, winH_);   // 必须在显示之前拍
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, winW_, winH_,
                 SWP_SHOWWINDOW | SWP_NOACTIVATE);

    hover_ = -1;
    result_ = 0;
    Render();

    // ★ 抓鼠标：菜单弹出后，鼠标往往要离开 Dock 才能点到菜单项。
    //   不抓的话，「点在菜单外面」这一类消息根本到不了这个窗口，
    //   菜单就关不掉了（只能靠再右键一次，很难用）。
    SetCapture(hwnd_);
    // 要键盘得先有焦点 —— 否则 ESC 关不掉菜单
    SetForegroundWindow(hwnd_);

    // 自己跑一条消息循环，直到用户选完。GetMessage 会把消息分派给本线程
    // 所有的窗口（Dock 的轮询定时器也在里面），所以 Dock 不会被卡住。
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
    return result_;
}

LRESULT CALLBACK PopupMenu::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PopupMenu* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<PopupMenu*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // ★ 必须在这里就把句柄补上。
        //   窗口还在「被创建」的过程中 —— NCCREATE 之后还有 NCCALCSIZE、CREATE
        //   一串消息会进来，而 CreateWindowEx **还没返回**，
        //   成员变量 hwnd_ 此刻仍是空的。以前就是拿这个空句柄去调
        //   DefWindowProcW：NCCREATE 被返回 FALSE，于是窗口根本建不出来，
        //   CreateWindowEx 报 1400（ERROR_INVALID_WINDOW_HANDLE）。
        if (self) self->hwnd_ = h;
    } else {
        self = reinterpret_cast<PopupMenu*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT PopupMenu::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST:
            return HTCLIENT;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ValidateRect(hwnd_, nullptr);
            return 0;
        case WM_MOUSEMOVE: {
            const int px = static_cast<short>(LOWORD(lp));
            const int py = static_cast<short>(HIWORD(lp));
            const int hit = MenuHitItem(items_, pad_, pad_, menuW_, rowH_, px, py);
            const int want = (hit < 0) ? -1 : hit;
            if (want != hover_) {
                hover_ = want;
                Render();
            }
            return 0;
        }
        case WM_LBUTTONDOWN:
            // 点在菜单外面 = 取消
            if (MenuHitItem(items_, pad_, pad_, menuW_, rowH_,
                            static_cast<short>(LOWORD(lp)),
                            static_cast<short>(HIWORD(lp))) == -1) Finish(0);
            return 0;
        case WM_LBUTTONUP: {
            const int px = static_cast<short>(LOWORD(lp));
            const int py = static_cast<short>(HIWORD(lp));
            const int hit = MenuHitItem(items_, pad_, pad_, menuW_, rowH_, px, py);
            if (hit >= 0) Finish(items_[static_cast<size_t>(hit)].cmd);
            else if (hit == -1) Finish(0);
            return 0;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            Finish(0);
            return 0;
        case WM_KEYDOWN: {
            if (wp == VK_ESCAPE) {
                Finish(0);
                return 0;
            }
            if (wp == VK_UP || wp == VK_DOWN) {
                const int n = static_cast<int>(items_.size());
                int i = hover_;
                for (int step = 0; step < n; ++step) {
                    i += (wp == VK_DOWN) ? 1 : -1;
                    if (i < 0) i = n - 1;
                    if (i >= n) i = 0;
                    if (!items_[static_cast<size_t>(i)].separator &&
                        !items_[static_cast<size_t>(i)].title) break;
                }
                hover_ = (i >= 0 && i < n) ? i : -1;
                Render();
                return 0;
            }
            if (wp == VK_RETURN) {
                if (hover_ >= 0) Finish(items_[static_cast<size_t>(hover_)].cmd);
                else Finish(0);
                return 0;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            // SetCapture / ReleaseCapture 都会带来这条，别被它把自己关掉
            return 0;
        case WM_WINDOWPOSCHANGED:
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace mydock