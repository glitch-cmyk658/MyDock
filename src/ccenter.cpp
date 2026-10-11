// 2026-10-11 - AI - 控制中心的实现：磁贴 + 滑条 + 模态交互
//
// 交互模式照抄 PopupMenu / CalendarFlyout：SetCapture + 自己的消息循环，
// 点面板外面 / ESC 关掉。拖滑条的时候用 WM_MOUSEMOVE 实时改音量/亮度 ——
// 系统滑条就是这个手感，松手才生效的话像坏了。
#include "ccenter.h"
#include "svgicon.h"
#include "util.h"

#include <shellapi.h>
#include <cstring>
#include <cwchar>

namespace mydock {
namespace {

const wchar_t* kClass = L"MyDockControlCenter";

// macOS 系统色（浅色 #007AFF / 深色 #0A84FF；灰钮浅 #E9E9EB 深 #48484A）
Gdiplus::Color KnobOn(bool dk) {
    return dk ? Gdiplus::Color(255, 10, 132, 255) : Gdiplus::Color(255, 0, 122, 255);
}
Gdiplus::Color KnobOff(bool dk) {
    return dk ? Gdiplus::Color(255, 72, 72, 74) : Gdiplus::Color(255, 233, 233, 235);
}
Gdiplus::Color KnobGlyph(bool dk, bool on) {
    // 打开的钮上图标是白的；关着的钮上图标本色（浅色下黑，深色下白）
    return dk ? Gdiplus::Color(255, 242, 242, 247)
              : (on ? Gdiplus::Color(255, 255, 255, 255)
                    : Gdiplus::Color(255, 29, 29, 31));
}
Gdiplus::Color TileBg(bool dk) {
    return dk ? Gdiplus::Color(255, 44, 44, 46) : Gdiplus::Color(255, 255, 255, 255);
}

// Lucide 表里没有的几个符号，path 直接内嵌（Lucide ISC 许可）
bool IconByName(const char* name, SvgIcon& out) {
    if (LucideIcon(name, out)) return true;
    if (!std::strcmp(name, "sun")) {
        // 太阳 = 圆 + 八根光线
        SvgParsePath("M12 3v1M12 20v1M3 12h1M20 12h1M5.6 5.6l.7.7"
                     "M17.7 17.7l.7.7M6.3 17.7l-.7.7M18.4 5.6l-.7.7", out);
        SvgAddCircle(out, 12, 12, 4);
        return true;
    }
    if (!std::strcmp(name, "moon")) {
        return SvgParsePath("M12 3a6 6 0 0 0 9 9 9 9 0 1 1-9-9Z", out);
    }
    return false;
}

}  // namespace

ControlCenter::~ControlCenter() {
    if (dc_) DeleteDC(dc_);
    if (bmp_) DeleteObject(bmp_);
    delete backdrop_;
}

bool ControlCenter::Toggle(HINSTANCE inst, int ax, int ay, bool dark,
                           const SysStatus& sys) {
    inst_ = inst;
    dark_ = dark;
    sys_ = sys;
    const DWORD t0 = GetTickCount();
    // ★ 每个外部调用单独打点。上一版就是靠「日志停在哪个调用之后」定位崩溃的，
    //   这些 Log 平时看着啰嗦，出事的时候就是案发现场的监控录像。
    Log(L"控制中心：查 Wi-Fi 开关…");
    wifiOn_ = GetWifiEnabled();
    Log(L"控制中心：查 SSID…");
    ssid_ = WifiCurrentSsid();        // 缓存一次：Render 每帧都画副标题，
                                      // 每帧都去 WlanOpen/Query/Close 又慢又会抖
    Log(L"控制中心：查亮度能力…");
    hasBright_ = GetBrightness(bright_);
    Log(L"控制中心：状态采集 %lu ms（wifi%s，ssid=%ls，亮度%s）", GetTickCount() - t0,
        wifiOn_ ? L"开" : L"关", ssid_.c_str(), hasBright_ ? L"可调" : L"不可调");

    // ---- 布局：2×2 磁贴 + 两条滑条 + 底部行 ----
    pad_   = Dpi(14);
    tileW_ = Dpi(146);
    tileH_ = Dpi(62);
    const int gap   = Dpi(10);
    const int slideH= Dpi(38);
    const int footH = Dpi(36);

    w_ = pad_ * 2 + tileW_ * 2 + gap;
    const int nSlide = hasBright_ ? 2 : 1;
    h_ = pad_ + tileH_ * 2 + gap + nSlide * (slideH + Dpi(6)) + Dpi(2) + footH + Dpi(4);

    for (int i = 0; i < 4; ++i) {
        const int col = i % 2, row = i / 2;
        tiles_[i] = { pad_ + col * (tileW_ + gap), pad_ + row * (tileH_ + gap),
                      tileW_, tileH_ };
        // 圆钮：磁贴里垂直居中、靠左
        const int d = Dpi(38);
        knob_[i] = { tiles_[i].x + Dpi(12),
                     tiles_[i].y + (tileH_ - d) / 2, d, d };
    }
    // 亮度不支持时音量顶到第一条的位置 —— 布局按「实际条数」排，别留空行
    const int y1 = pad_ + tileH_ * 2 + gap + Dpi(4);
    if (hasBright_) {
        slider_[0] = { pad_, y1, tileW_ * 2 + gap, slideH };                 // 亮度
        slider_[1] = { pad_, y1 + slideH + Dpi(6), tileW_ * 2 + gap, slideH }; // 音量
    } else {
        slider_[0] = { pad_, y1, tileW_ * 2 + gap, slideH };                 // 音量
        slider_[1] = { 0, 0, 0, 0 };                                         // 不存在
    }
    gear_ = { pad_, y1 + (hasBright_ ? 2 : 1) * (slideH + Dpi(6)) - Dpi(2),
              w_ - pad_ * 2, footH };

    // 位置：锚点所在屏右上角
    const MonitorInfo mon = MonitorAt(POINT{ax, ay});
    winX_ = mon.full.right - w_ - Dpi(10);
    winY_ = ay + Dpi(6);

    Log(L"控制中心：打开于 (%d,%d) %dx%d", winX_, winY_, w_, h_);
    EnsureWindow(inst);
    if (!hwnd_) return false;

    CreateBackBuffer(w_, h_);
    CaptureBackdrop();
    Render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);

    SetCapture(hwnd_);
    SetForegroundWindow(hwnd_);

    open_ = true;
    looping_ = true;
    MSG msg = {};
    while (looping_) {
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
            PostQuitMessage(0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (GetCapture() == hwnd_) ReleaseCapture();
    ShowWindow(hwnd_, SW_HIDE);
    return false;
}

void ControlCenter::EnsureWindow(HINSTANCE inst) {
    if (hwnd_) return;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);   // 重复注册失败无所谓

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kClass, L"控制中心", WS_POPUP,
        winX_, winY_, w_, h_, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log(L"控制中心：创建窗口失败，错误码 %lu", GetLastError());
    }
}

void ControlCenter::CreateBackBuffer(int w, int h) {
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
    DeleteObject(SelectObject(dc_, dib));
    bits_ = p;
}

void ControlCenter::CaptureBackdrop() {
    delete backdrop_;
    backdrop_ = nullptr;
    Gdiplus::Bitmap* shot = CaptureScreenArea(winX_, winY_, w_, h_);
    if (!shot) return;
    backdrop_ = BlurBitmap(shot, 16);
}

void ControlCenter::RefreshSys() {
    PollSysStatus(sys_);
}

void ControlCenter::DrawTile(Gdiplus::Graphics& g, int idx, const wchar_t* title,
                             const wchar_t* sub, const char* lucideName, bool on) {
    const Rct& t = tiles_[idx];
    const bool dk = dark_;

    // 磁贴底
    Gdiplus::GraphicsPath p;
    AddRoundRect(p, Gdiplus::RectF(static_cast<float>(t.x), static_cast<float>(t.y),
                                   static_cast<float>(t.w), static_cast<float>(t.h)),
                 static_cast<float>(Dpi(12)));
    Gdiplus::SolidBrush bg(TileBg(dk));
    g.FillPath(&bg, &p);

    // 圆形开关钮
    const Rct& k = knob_[idx];
    Gdiplus::SolidBrush knobBr(on ? KnobOn(dk) : KnobOff(dk));
    g.FillEllipse(&knobBr, static_cast<float>(k.x), static_cast<float>(k.y),
                  static_cast<float>(k.w), static_cast<float>(k.h));

    // 钮上的图标
    SvgIcon ic;
    if (IconByName(lucideName, ic)) {
        SvgDraw(g, ic, static_cast<float>(k.x + k.w / 2),
                static_cast<float>(k.y + k.h / 2),
                static_cast<float>(Dpi(19)), 2.6f, KnobGlyph(dk, on));
    }

    // 文字：标题 + 副标题
    const Gdiplus::Color cT = dk ? Gdiplus::Color(255, 242, 242, 247)
                                 : Gdiplus::Color(255, 29, 29, 31);
    const Gdiplus::Color cS = dk ? Gdiplus::Color(255, 152, 152, 159)
                                 : Gdiplus::Color(255, 120, 120, 128);
    Gdiplus::SolidBrush brT(cT);
    Gdiplus::SolidBrush brS(cS);
    Gdiplus::Font fT(L"Microsoft YaHei UI", Dpi(12),
                     Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font fS(L"Microsoft YaHei UI", Dpi(11),
                     Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    // NoWrap：别让长 SSID 折行，超出就截断
    Gdiplus::StringFormat cf(Gdiplus::StringFormatFlagsNoWrap);
    cf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    const int tx = t.x + Dpi(58);
    const int tw = t.x + t.w - Dpi(8) - tx;
    Gdiplus::RectF r1(static_cast<float>(tx), static_cast<float>(t.y + Dpi(10)),
                      static_cast<float>(tw), static_cast<float>(Dpi(20)));
    Gdiplus::RectF r2(static_cast<float>(tx), static_cast<float>(t.y + Dpi(32)),
                      static_cast<float>(tw), static_cast<float>(Dpi(18)));
    g.DrawString(title, -1, &fT, r1, &cf, &brT);
    g.DrawString(sub,   -1, &fS, r2, &cf, &brS);
}

void ControlCenter::DrawSlider(Gdiplus::Graphics& g, int idx, const char* lucideName,
                               int pct) {
    const Rct& s = slider_[idx];
    const bool dk = dark_;

    // 滑条底板（macOS 的滑条在一条独立的浅色横板上）
    Gdiplus::GraphicsPath p;
    AddRoundRect(p, Gdiplus::RectF(static_cast<float>(s.x), static_cast<float>(s.y),
                                   static_cast<float>(s.w), static_cast<float>(s.h)),
                 static_cast<float>(Dpi(12)));
    Gdiplus::SolidBrush bg(dk ? Gdiplus::Color(255, 44, 44, 46)
                              : Gdiplus::Color(255, 255, 255, 255));
    g.FillPath(&bg, &p);

    // 左侧图标
    SvgIcon ic;
    if (IconByName(lucideName, ic)) {
        const Gdiplus::Color cIc = dk ? Gdiplus::Color(255, 220, 220, 226)
                                      : Gdiplus::Color(255, 70, 70, 78);
        SvgDraw(g, ic, static_cast<float>(s.x + Dpi(26)),
                static_cast<float>(s.y + s.h / 2),
                static_cast<float>(Dpi(18)), 2.6f, cIc);
    }

    // 轨道：细圆角条。填充部分白色，未填充部分淡灰
    const int tx = s.x + Dpi(46);
    const int tw = s.w - Dpi(46) - Dpi(16);
    const int cy = s.y + s.h / 2;
    const int barH = Dpi(5);
    Gdiplus::SolidBrush track(dk ? Gdiplus::Color(255, 90, 90, 94)
                                 : Gdiplus::Color(255, 226, 226, 230));
    Gdiplus::SolidBrush fill(dk ? Gdiplus::Color(255, 242, 242, 247)
                                : Gdiplus::Color(255, 90, 90, 96));
    Gdiplus::RectF rt(static_cast<float>(tx), static_cast<float>(cy - barH / 2),
                      static_cast<float>(tw), static_cast<float>(barH));
    g.FillRectangle(&track, rt);
    const int fw = tw * pct / 100;
    if (fw > 0) {
        Gdiplus::RectF rf(static_cast<float>(tx), static_cast<float>(cy - barH / 2),
                          static_cast<float>(fw), static_cast<float>(barH));
        g.FillRectangle(&fill, rf);
    }
    // 滑块：白球带描边
    const int d = Dpi(14);
    const int kx = tx + fw - d / 2;
    // 白球两种主题通用（深浅由上面的 ring 描边区分）——
    // 此前写成三元两分支同值，读起来像漏了一个主题
    Gdiplus::SolidBrush ball(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::Pen ring(dk ? Gdiplus::Color(80, 0, 0, 0) : Gdiplus::Color(40, 0, 0, 0), 1.0f);
    g.FillEllipse(&ball, static_cast<float>(kx), static_cast<float>(cy - d / 2),
                  static_cast<float>(d), static_cast<float>(d));
    g.DrawEllipse(&ring, static_cast<float>(kx), static_cast<float>(cy - d / 2),
                  static_cast<float>(d), static_cast<float>(d));
}

void ControlCenter::Render() {
    if (!dc_ || !bits_) return;
    memset(bits_, 0, static_cast<size_t>(w_) * h_ * 4);

    Gdiplus::Graphics g(dc_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    Gdiplus::GraphicsPath panel;
    AddRoundRect(panel, Gdiplus::RectF(0.5f, 0.5f,
                 static_cast<float>(w_ - 1), static_cast<float>(h_ - 1)),
                 static_cast<float>(Dpi(14)));
    g.SetClip(&panel);
    if (backdrop_) g.DrawImage(backdrop_, 0, 0, w_, h_);
    Gdiplus::SolidBrush tint(dark_ ? Gdiplus::Color(226, 24, 24, 27)
                                   : Gdiplus::Color(238, 242, 242, 246));
    g.FillRectangle(&tint, 0, 0, w_, h_);
    g.ResetClip();
    Gdiplus::Pen rim(dark_ ? Gdiplus::Color(40, 255, 255, 255)
                           : Gdiplus::Color(26, 0, 0, 0), 1.0f);
    g.DrawPath(&rim, &panel);

    // ---- 磁贴 ----
    const bool wifiUp = sys_.wifiUp && wifiOn_;
    // ★ 副标题用打开时缓存的 ssid_。以前每帧都调 WifiCurrentSsid()
    //   （WlanOpen/Query/Close 一整套），拖滑条时一秒几十次 —— 又卡又没必要。
    const wchar_t* wifiSub =
        wifiOn_ ? (wifiUp ? (ssid_.empty() ? L"已连接" : ssid_.c_str()) : L"未连接")
                : L"Wi-Fi 已关闭";
    DrawTile(g, 0, L"Wi-Fi", wifiSub, "wifi", wifiOn_);
    DrawTile(g, 1, L"蓝牙", L"在设置中管理", "bluetooth", sys_.btPresent);
    DrawTile(g, 2, L"显示器", L"投影 (Win+P)", "monitor", false);
    DrawTile(g, 3, L"声音", sys_.muted ? L"已静音" : L"未静音",
             sys_.muted ? "volume-x" : "volume-2", !sys_.muted);

    // ---- 滑条（亮度这台机器不支持就整条藏掉）----
    if (hasBright_) {
        DrawSlider(g, 0, "sun", bright_);
        DrawSlider(g, 1, "volume-2", sys_.volume);
    } else {
        DrawSlider(g, 0, "volume-2", sys_.volume);
    }

    // ---- 底部：电池 + 设置 ----
    {
        wchar_t bat[32] = L"";
        if (sys_.hasBattery) {
            swprintf(bat, 32, L"%d%%", sys_.pct);
        }
        const Gdiplus::Color cT = dark_ ? Gdiplus::Color(255, 242, 242, 247)
                                        : Gdiplus::Color(255, 29, 29, 31);
        Gdiplus::SolidBrush br(cT);
        Gdiplus::Font f(L"Microsoft YaHei UI", Dpi(12),
                        Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::StringFormat cf(Gdiplus::StringFormatFlagsNoWrap);
        cf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        // 电池图标 + 电量（就画在左边）
        SvgIcon ic;
        if (sys_.hasBattery && LucideIcon(sys_.charging ? "bolt" : "battery", ic)) {
            SvgDraw(g, ic, static_cast<float>(gear_.x + Dpi(16)),
                    static_cast<float>(gear_.y + gear_.h / 2),
                    static_cast<float>(Dpi(18)), 2.6f, cT);
        }
        Gdiplus::RectF rb(static_cast<float>(gear_.x + Dpi(30)),
                          static_cast<float>(gear_.y),
                          static_cast<float>(Dpi(80)), static_cast<float>(gear_.h));
        g.DrawString(sys_.hasBattery ? bat : L"台式机", -1, &f, rb, &cf, &br);

        // 右下角齿轮 → 系统设置（Lucide 没有 settings，用三条滑杆代）
        SvgIcon gear;
        if (LucideIcon("sliders-horizontal", gear)) {
            SvgDraw(g, gear, static_cast<float>(gear_.x + gear_.w - Dpi(20)),
                    static_cast<float>(gear_.y + gear_.h / 2),
                    static_cast<float>(Dpi(18)), 2.6f, cT);
        }
    }

    HDC screen = GetDC(nullptr);
    POINT src = {0, 0};
    POINT dst = {winX_, winY_};
    SIZE size = {w_, h_};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK ControlCenter::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ControlCenter* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<ControlCenter*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = h;
    } else {
        self = reinterpret_cast<ControlCenter*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT ControlCenter::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_MOUSEMOVE: {
            const bool down = (msg == WM_LBUTTONDOWN);
            // 拖滑条：按下任意一条后，移动只改那条
            if (!down && drag_ < 0) break;
            const int mx = static_cast<short>(LOWORD(lp));
            const int my = static_cast<short>(HIWORD(lp));
            if (mx < 0 || my < 0 || mx >= w_ || my >= h_) {
                if (down) Finish();
                return 0;
            }

            if (down) {
                // 命中判定：磁贴 / 齿轮
                for (int i = 0; i < 4; ++i) {
                    const Rct& t = tiles_[i];
                    if (mx < t.x || my < t.y || mx >= t.x + t.w || my >= t.y + t.h) continue;
                    if (i == 0) {
                        const Rct& k = knob_[0];
                        const bool onKnob = mx >= k.x - Dpi(4) && mx < k.x + k.w + Dpi(4);
                        if (onKnob) {
                            // 真开关 Wi-Fi
                            if (SetWifiEnabled(!wifiOn_)) {
                                wifiOn_ = !wifiOn_;
                                RefreshSys();
                            }
                        } else {
                            // 磁贴其余部分：网络设置页
                            ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi",
                                          nullptr, nullptr, SW_SHOWNORMAL);
                            Finish();
                        }
                    } else if (i == 1) {
                        ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth",
                                      nullptr, nullptr, SW_SHOWNORMAL);
                        Finish();
                    } else if (i == 2) {
                        // Win+P：系统自己的投影菜单，没有别的入口能唤它
                        INPUT in[4] = {};
                        in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_LWIN;
                        in[1].type = INPUT_KEYBOARD; in[1].ki.wVk = 'P';
                        in[2].type = INPUT_KEYBOARD; in[2].ki.wVk = 'P';
                        in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                        in[3].type = INPUT_KEYBOARD; in[3].ki.wVk = VK_LWIN;
                        in[3].ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(4, in, sizeof(INPUT));
                        Finish();
                    } else {
                        // 圆钮 = 切静音；别处 = 声音设置
                        const Rct& k = knob_[3];
                        if (mx >= k.x - Dpi(4) && mx < k.x + k.w + Dpi(4)) {
                            SetMuted(!sys_.muted);
                            RefreshSys();
                        } else {
                            ShellExecuteW(nullptr, L"open", L"ms-settings:sound",
                                          nullptr, nullptr, SW_SHOWNORMAL);
                            Finish();
                        }
                    }
                    Render();
                    return 0;
                }
                // 齿轮 → 设置主页
                if (mx >= gear_.x && mx < gear_.x + gear_.w &&
                    my >= gear_.y && my < gear_.y + gear_.h) {
                    ShellExecuteW(nullptr, L"open", L"ms-settings:", nullptr, nullptr,
                                  SW_SHOWNORMAL);
                    Finish();
                    return 0;
                }
                // 滑条：按下进入拖动状态。
                // drag_ 存「槽位下标」，几何和类型都从槽位 + hasBright_ 推，
                // 不再维护「类型编号 → 槽位」的映射 —— 那层映射正是错位 bug 的来源。
                for (int slot = 0; slot < 2; ++slot) {
                    const Rct& r = slider_[slot];
                    if (r.w <= 0) continue;
                    if (my >= r.y && my < r.y + r.h && mx >= r.x && mx < r.x + r.w) {
                        drag_ = slot;
                        break;
                    }
                }
                if (drag_ < 0) return 0;   // 点在空白处：不动也不关（误触保护）
            }

            // 拖动中：按 x 算百分比。
            // ★ 几何必须用「槽位」本身（slider_[drag_]）。
            //   以前 drag_ 存类型编号（1 亮度 2 音量），再拿编号-1 当槽位 ——
            //   没有亮度滑条时音量画在 0 号槽，编号 2-1 = 1 号槽却是个空槽，
            //   轨道宽度算出负数，(x-tx)*100/负数 直接爆成 100 ——
            //   「一碰音量就拉满」就是它。
            if (drag_ >= 0) {
                const Rct& r = slider_[drag_];
                const int tx = r.x + Dpi(46);
                const int tw = r.w - Dpi(46) - Dpi(16);
                int pct = (tw > 0) ? (mx - tx) * 100 / tw : 0;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                if (hasBright_ && drag_ == 0) {
                    if (SetBrightness(pct)) bright_ = pct;
                    else { drag_ = -1; return 0; }
                } else {
                    SetVolumePct(pct);
                    RefreshSys();
                }
                Render();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            drag_ = -1;
            return 0;
        case WM_RBUTTONDOWN:
            Finish();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) Finish();
            return 0;
        case WM_DESTROY:
            Finish();
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace mydock

namespace mydock {
void ControlCenter::Finish() {
    open_ = false;
    looping_ = false;
}
}  // namespace mydock
