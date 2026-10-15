// 2026-10-13 - AI - 从 dock.cpp 拆出：Dock 右键菜单 / 窗口过程 / 托盘图标。
// 拆分原因：dock.cpp 单文件 2450 行（2026-10-13 审查意见）——
// 菜单与窗口消息处理是内聚的一坨，独立成文件后 dock.cpp 只剩
// 生命周期 / 布局 / 渲染 / 扫描。
#include "dock_internal.h"

#include "dock.h"
#include "menu.h"
#include "appaction.h"
#include "apps.h"
#include "util.h"
#include "theme.h"

#include <shellapi.h>

namespace mydock {
namespace {

// MakeTrayIcon 用的小助手（dock.cpp 渲染侧另有一份同名实现，各自独立）
void MenuAddRoundRect(Gdiplus::GraphicsPath& p, const Gdiplus::RectF& r, float radius) {
    Gdiplus::RectF c{r.X + radius, r.Y + radius,
                     r.Width - radius * 2, r.Height - radius * 2};
    p.AddArc(r.X, r.Y, radius * 2, radius * 2, 180, 90);
    p.AddArc(c.X + c.Width, r.Y, radius * 2, radius * 2, 270, 90);
    p.AddArc(c.X + c.Width, c.Y + c.Height, radius * 2, radius * 2, 0, 90);
    p.AddArc(r.X, c.Y + c.Height, radius * 2, radius * 2, 90, 90);
    p.CloseFigure();
}

}  // namespace

void Dock::ShowMenu(int x, int y) {
    // ★ 自己画，不用 TrackPopupMenu。
    //   系统菜单是另一个年代的灰底方角，跟这层毛玻璃凑不到一起；而且它是个
    //   独立的不透明窗口，盖在玻璃上像贴了张纸。PopupMenu 开的是一个分层窗口，
    //   底是「自己背后那块屏幕」模糊出来的，跟 Dock 同一套材质 —— 实现在 menu.cpp。
    //
    // ★ 菜单分两段，看右键落在哪儿：
    //     落在某个图标上 → 先给「这个应用」能做的事（打开/在文件夹里找/移出 Dock），
    //                      下面再接 MyDock 自己的设置项；
    //     落在空白处     → 只有设置项。
    //   以前不分公司青红皂白都只给设置项，「从 Dock 移除」只能跑去启动台做，
    //   绕一大圈。应用那一组跟启动台共用同一份定义（appaction.h）。
    int idx = (hover_ >= 0 && hover_ < static_cast<int>(apps_.size())) ? hover_ : -1;

    // ★ 菜单跑的是它自己的一条消息循环，这段时间里 Dock 照样在收 WM_TIMER，
    //   运行区可能已经增删过 —— 那时同一个下标就不指着同一个图标了。
    //   所以先把「当时右键的是哪一个」记下来，等菜单回来再按它重新定位。
    std::wstring menuKey;
    bool menuOnDock = false;
    if (idx >= 0) {
        const AppItem& a = apps_[static_cast<size_t>(idx)];
        // ★ 内建格子（启动台/废纸篓/日历）没有路径，用清单里那一行的原文
        //   （@launchpad 等）当身份 —— 排序、重定位都靠它对回 pinned.txt。
        menuKey = (a.kind != AppKind::File)
                      ? SpecialLine(a.kind)
                      : NormalizePath(a.lnkPath.empty() ? a.target : a.lnkPath);
        menuOnDock = (idx < pinnedCount_);
    }

    auto line = [](std::vector<MenuItem>& v, int cmd, const wchar_t* label,
                   bool checked = false) {
        MenuItem it;
        it.cmd = cmd;
        it.label = label;
        it.checked = checked;
        v.push_back(std::move(it));
    };
    auto sep = [](std::vector<MenuItem>& v) {
        MenuItem it;
        it.separator = true;
        v.push_back(std::move(it));
    };

    std::vector<MenuItem> items;
    if (idx >= 0) {
        MenuItem head;
        head.title = true;
        head.tag = apps_[static_cast<size_t>(idx)].kind != AppKind::File ? L"内建"
                 : (idx < pinnedCount_ ? L"应用" : L"运行中");
        head.label = apps_[static_cast<size_t>(idx)].name;
        items.push_back(std::move(head));
        sep(items);

        AppPlaces places;
        // ★ 分隔线右边那一格不算「在 Dock 上」—— 它只是因为正在跑才临时出现。
        //   所以右键给的是「添加到 Dock」（写进 pinned.txt），不是「从 Dock 移除」。
        places.onDock = (idx < pinnedCount_);
        places.onLaunchpad = false;   // 启动台那一页的事归启动台管，这儿不掺和
        places.userAdded = AppIsUserAdded(apps_[static_cast<size_t>(idx)]);
        AppendAppActions(items, apps_[static_cast<size_t>(idx)], places);
        // 2026-10-11 - AI - 固定区图标的排序入口：左移 / 右移。
        // 拖拽排序要做一整套拖放命中和插入位计算，先用菜单项落地 ——
        // pinned.txt 的行序就是图标顺序，交换相邻两行即可。
        if (menuOnDock) {
            const int n = static_cast<int>(apps_.size());
            if (idx > 0)            line(items, 20, L"在 Dock 上左移");
            if (idx < n - 1)        line(items, 21, L"在 Dock 上右移");
        }
        sep(items);
    } else {
        // 点在空白处：这一段是 MyDock 自己，不是某个应用。
        // 照样给个标题行 —— 两种右键只差中间那一段，上下结构一致，
        // 菜单才不会"看人下菜碟"似的忽长忽短。
        MenuItem head;
        head.title = true;
        head.tag = L"设置";
        head.label = L"MyDock";
        items.push_back(std::move(head));
        sep(items);
    }

    line(items, 12, L"打开启动台");
    line(items, 2, L"刷新应用列表");
    sep(items);
    line(items, 11, L"悬停高亮", glow_ > 0.0f);
    line(items, 13, L"贴边自动升起", riseEnabled_);
    sep(items);
    // 用户从启动台里「移出」的应用，靠启动台左上角那个「+」重新挑一个回来，
    // 不再给「一键全部恢复」—— 那样会把以前故意移出去的也一起放回来。
    line(items, 3, L"退出 MyDock");

    POINT pt = {x, y};
    ClientToScreen(hwnd_, &pt);
    Log(L"右键菜单：%zu 项（图标上=%d），锚点 (%ld,%ld)", items.size(), idx, pt.x, pt.y);
    menuOpen_ = true;
    const int cmd = menu_.Show(hwnd_, pt.x, pt.y, items);
    menuOpen_ = false;
    Log(L"右键菜单：返回 cmd=%d", cmd);

    // 按下标失效的可能，重新找回刚才右键的那一个
    if (cmd != 0 && !menuKey.empty()) {
        idx = -1;
        for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
            const AppItem& a = apps_[static_cast<size_t>(i)];
            const std::wstring k = (a.kind != AppKind::File)
                                       ? SpecialLine(a.kind)
                                       : NormalizePath(a.lnkPath.empty() ? a.target : a.lnkPath);
            if (k == menuKey) {
                idx = i;
                break;
            }
        }
    }

    if (IsAppCmd(cmd)) {
        if (idx >= 0) {
            // ★ 拷一份：下面「从 Dock 移除」会把这一格从 apps_ 里删掉，
            //   拿着引用继续走就是在读已经释放的内存。
            const AppItem app = apps_[static_cast<size_t>(idx)];
            // ★ 内建格子的「打开」就是它自己那个动作（开启动台 / 开回收站 / 开日历）。
            //   它没有 target 也没有 lnkPath，交给 RunAppAction 只会得出「启动不了」。
            if (app.kind != AppKind::File && cmd == kAppOpen) {
                LaunchOrActivate(idx);
                return;
            }
            const AppOutcome r = RunAppAction(cmd, app);
            // 移除之后立刻重排 —— 让用户当场看见它没了好过下次重启才发现
            if (r.pinnedChanged) ReloadApps();
        }
        return;
    }

    if (cmd == 20 || cmd == 21) {
        // 排序：交换 pinned.txt 相邻两行，重排整条 Dock
        if (MovePinned(menuKey, cmd == 20 ? -1 : 1)) {
            ReloadApps();
        } else {
            Log(L"排序：%ls 没有动（边界或清单里没找到）", menuKey.c_str());
        }
    } else if (cmd == 12) {
        launcher_.Open();
    } else if (cmd == 2) {
        // 重新扫一遍开始菜单：新装的应用会出现在启动台里。
        // ★ 后台扫（跟启动时同一条路），扫完 OnAppsScanned 无缝替换 ——
        //   以前是同步扫，点一下菜单整个界面卡住几百毫秒到一秒。
        DockStartFullScan(hwnd_);
    } else if (cmd == 3) {
        DestroyWindow(hwnd_);
    } else if (cmd == 11) {
        ApplyGlow(glow_ > 0.0f ? 0.0f : 1.0f);
        SaveSettings();
        Log(L"悬停高亮：%ls", glow_ > 0.0f ? L"开" : L"关");
    } else if (cmd == 13) {
        riseEnabled_ = !riseEnabled_;
        SaveSettings();
        Log(L"贴边自动升起：%ls", riseEnabled_ ? L"开" : L"关");
    }
}

LRESULT CALLBACK Dock::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dock* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Dock*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = h;
    } else {
        self = reinterpret_cast<Dock*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT Dock::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            int mx = static_cast<short>(LOWORD(lp));
            int my = static_cast<short>(HIWORD(lp));
            if (!tracking_) {
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd_;
                TrackMouseEvent(&tme);
                tracking_ = true;
            }
            mouseX_ = mx;
            mouseInside_ = true;
            hover_ = HitTest(mx, my);
            ++mouseMoves_;
            // 这里以前还调了一次 RefreshRunning —— 它要枚举系统里所有顶层窗口、
            // 逐个 OpenProcess，一次几十毫秒。挂在鼠标移动上意味着滑动过程中
            // 每隔 0.7 秒卡一帧，看着也是一顿。挪去轮询定时器里做了。
            ComputeTarget();

            // 不靠定时器出帧：定时器实测最快也就 15.6ms 一次，而鼠标每 6ms 就来一个点，
            // 等它等于白白多出一整帧的延迟（手感就是"拖"）。这里直接画，只做限流。
            const DWORD now = GetTickCount();
            if (now - lastRenderTick_ >= kFrameMinMs) {
                RequestRender();
            } else {
                RequestAnim();
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            tracking_ = false;
            mouseInside_ = false;
            hover_ = -1;
            // 注意这里不清 mouseX_。
            // 清掉的话，布局会从「锚定在鼠标处」瞬间切成「整条居中」——鼠标刚离开、
            // Dock 还要在屏幕上停 1.2 秒，那一下图标会整体跳一下（实测单帧 151px）。
            // 留着它，放大就以原位置为轴平滑地缩回去，跟 macOS 一样。
            // 真正该归零的时机是 Dock 收完、藏起来的时候。
            ComputeTarget();
            RequestAnim();
            return 0;
        }
        case WM_LBUTTONUP: {
            const int mx = static_cast<short>(LOWORD(lp));
            const int my = static_cast<short>(HIWORD(lp));
            const int idx = HitTest(mx, my);
            if (idx >= 0) OnIconClick(idx, mx, my);
            return 0;
        }
        case WM_RBUTTONUP: {
            ShowMenu(static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp)));
            return 0;
        }
        case WM_HOTKEY:
            if (wp == kHotkeyLaunchpad) launcher_.Toggle();
            return 0;
        case kMsgPinnedChanged:
            // 用户在启动台里增删了固定项。Dock 上出现哪些应用完全由那份清单决定，
            // 所以它必须跟着重建 —— 这就是「启动台 + 手动固定」必须绑在一起做的原因。
            ReloadApps();
            return 0;
        case kMsgHideNow:
            HideNow();
            return 0;
        case WM_TIMER: {
            if (wp == kTimerAnim) {
                Tick();
                return 0;
            }
            // 轮询鼠标：贴到屏幕最底边就升起来，鼠标走开一段距离后收回去
            if (wp != kTimerPoll) break;
            // 主题文件热重载：theme.ini 存盘后下一次 Paint 就是新配色
            ThemeReloadIfNeeded();
            POINT pt = {};
            GetCursorPos(&pt);
            // 注意：这里不能用 GetWindowRect —— Dock 收起时窗口被整体移到屏幕外，
            // 那个矩形也跟着跑出屏幕，鼠标永远落不进去，就永远唤不出来
            // （只能靠贴屏幕最底边）。所以按「展开时」的位置算。
            //
            // ★ 2026-10-10 按用户要求改了这两条判据。
            //
            //   ① 升起来：只有鼠标真的压到屏幕最底下那条线（edgePx_）才升。
            //      原来是「鼠标进到 Dock 展开时那一整块就算」—— 那一块有
            //      baseSize_ + padBottom_ 那么高（≈70 物理像素），等于离屏幕底
            //      还有一指宽就冒出来了，在别的应用里往下挪鼠标老是被它截住。
            //
            //   ② 收回去：不看时间，看鼠标**离开 Dock 多远**。
            //      原来是「离开 1.2 秒就收」—— 鼠标明明还停在 Dock 边缘上晃，
            //      到点也照样缩回去；反过来，鼠标早跑远了却还要等够 1.2 秒。
            //      改成距离之后语义就对了：你走开了我才收，而且是立刻收。
            // ★ 先认一遍「现在该站哪块屏」。收起状态下才允许换屏 ——
            //   展开时换会让 Dock 在用户眼皮底下跳走（见 UpdateMonitor）。
            //   这一句同时刷新了 monDpi_，所以下面的 DockDpi() 都是新屏的。
            UpdateMonitor(!wantVisible_);
            const int bodyL = winX_;
            const int bodyR = winX_ + w_;
            const int bodyT = winY_ + padTop_ + labelH_;   // 图标行的上沿
            const int bodyB = winY_ + h_ + DockDpi(6);         // 一直贴到屏幕底
            // 贴的是「这块屏」的底边，不是主屏的。
            // 副屏比主屏矮时，拿主屏高度算出来的那条判定线会落在副屏外面，
            // 结果就是在副屏底部划拉半天 Dock 毫无反应 —— 这正是以前的行为。
            // 顺带要求 x 也落在这块屏上：两块屏上下错开时，主屏的底边 y
            // 可能正好等于副屏的 bottom，只比 y 会误判。
            const bool atEdge = pt.y >= monFull_.bottom - DockDpi(edgePx_) &&
                                pt.x >= monFull_.left && pt.x < monFull_.right;
            const bool overDock = pt.x >= bodyL && pt.x < bodyR &&
                                  pt.y >= bodyT && pt.y <= bodyB;
            // 到 Dock 那块矩形的距离（在矩形里就是 0）
            double away = 0.0;
            if (!overDock) {
                const int px = pt.x, py = pt.y;
                const double dx = static_cast<double>(
                    std::max(0, std::max(bodyL - px, px - bodyR)));
                const double dy = static_cast<double>(
                    std::max(0, std::max(bodyT - py, py - bodyB)));
                away = std::sqrt(dx * dx + dy * dy);
            }
            DWORD now = GetTickCount();

            // 有全屏应用压在前面时完全不出现 —— 否则它会盖住应用底部的按钮
            if (fullscreenCheckDue_ == 0 || now - fullscreenCheckDue_ >= 500) {
                fullscreenCheckDue_ = now;
                const bool fs = FullscreenAppOnTop();
                if (fs != fullscreenNow_) {
                    fullscreenNow_ = fs;
                    Log(fs ? L"检测到全屏应用 —— Dock 暂停弹出"
                           : L"全屏应用已退出 —— Dock 恢复");
                }
            }
            if (fullscreenNow_) {
                if (wantVisible_) SetVisible(false);
                return 0;
            }
            // 启动台开着的时候 Dock 也别冒出来 —— 整块屏幕都是它，
            // 底下再顶一条 dock 既难看也用不上
            if (launcher_.IsOpen()) {
                if (wantVisible_) SetVisible(false);
                return 0;
            }

            // ★ 菜单开着的时候别收起 Dock。
            //   菜单是贴着图标弹出来的，鼠标要移上去必然离开 Dock 那一条 ——
            //   不挡这一下的话，离开 1.2 秒后 Dock 就滑走了，菜单孤零零飘在那里。
            if (menuOpen_) {
                lastInside_ = now;
                if (!wantVisible_) SetVisible(true);
                return 0;
            }
            if (atEdge || overDock) {
                lastInside_ = now;             // 在边上/在 Dock 上：不算走开
            }

            // ★ 2026-10-13 - AI - 用户定版：Dock **只在鼠标真的到达屏幕最
            //   底边**时弹起（atEdge，3px 触发带），立即升起、无停留计时。
            //   overDock（鼠标在 Dock 身上/其常驻矩形内）只负责「别收」，
            //   绝不触发升起 —— 它覆盖底边上方 ~70px，正是挡按钮的那一条。
            //   riseEnabled_（右键菜单「贴边自动升起」）关掉后贴边不冒头。
            if (!wantVisible_ && riseEnabled_ && atEdge) {
                SetVisible(true);
            }

            if (wantVisible_ && now >= graceUntil_ &&
                away >= static_cast<double>(DockDpi(hideDistPx_))) {
                // ★ 只有「真的走开了」才收。停在 Dock 边上（阈值以内）不算走开 ——
                //   这一条是「距离判据」相对「时间判据」最实在的好处。
                SetVisible(false);
            }
            // 顺便刷一下「哪些应用正在运行」（内部自限 0.7 秒一次）
            RefreshRunning(false);

            // 每秒汇总一次帧率与耗时分布（只在开了 MYDOCK_TRACE 时）
            if (TraceLevel() && statFrames_ > 0) {
                DWORD now2 = GetTickCount();
                if (statWindow_ == 0) statWindow_ = now2;
                if (now2 - statWindow_ >= 1000) {
                    double sec = (now2 - statWindow_) / 1000.0;
                    double f = static_cast<double>(statFrames_);
                    double hz = 0.0;
                    {
                        // 显示器的实际刷新率
                        DEVMODEW dm = {};
                        dm.dmSize = sizeof(dm);
                        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
                            hz = dm.dmDisplayFrequency;
                    }
                    Log(L"FPS %.1f（%lu 帧 / %.1fs）  绘制 %.1fms [背景 %.1f|图标 %.1f|提交 %.1f]  "
                        L"枚举 %0.1fms  鼠标 %d/s  屏幕 %.0fHz",
                        f / sec, statFrames_, sec,
                        statTotalUs_ / f / 1000.0,
                        statBgUs_ / f / 1000.0, statIconUs_ / f / 1000.0, statUlwUs_ / f / 1000.0,
                        statRefreshWorstMs_, mouseMoves_, hz);
                    Log(L"   出帧周期  min %.1f  均 %.1f  max %.1f ms（屏幕一个刷新周期 %.2f ms，周期最好是它的整数倍）",
                        statGapMinUs_ / 1000.0, statGapUs_ / f / 1000.0, statGapMaxUs_ / 1000.0,
                        hz > 0 ? 1000.0 / hz : 0.0);
                    Log(L"   周期分布 [<6,<8,<10,<12,<14,<16,<20,≥20]=%d,%d,%d,%d,%d,%d,%d,%d",
                        statGapHist_[0], statGapHist_[1], statGapHist_[2], statGapHist_[3],
                        statGapHist_[4], statGapHist_[5], statGapHist_[6], statGapHist_[7]);
                    statFrames_ = 0;
                    statWindow_ = now2;
                    statGapUs_ = statTotalUs_ = statBgUs_ = statIconUs_ = statUlwUs_ = 0.0;
                    statRefreshWorstMs_ = statRefreshTotalMs_ = 0.0;
                    statRefreshCount_ = 0;
                    statGapMinUs_ = 1e9;
                    statGapMaxUs_ = 0.0;
                    for (int i = 0; i < 8; ++i) statGapHist_[i] = 0;
                    mouseMoves_ = 0;
                }
            }
            return 0;
        }
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_NCHITTEST: {
            // 窗口比可见的胶囊高一截（上面留给放大的图标和名字标签）。
            // 那部分如果照样吃鼠标，就会挡住底下的窗口点不到东西 —— 让它直接穿透。
            POINT pt = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            ScreenToClient(hwnd_, &pt);
            if (pt.y < padTop_ + labelH_) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_ERASEBKGND:
            return 1;
        // 2026-10-11 - AI - 所在显示器的 DPI 变了（改缩放、把窗口拖到另一块屏）
        //
        // 不开 Per-Monitor DPI 感知的话，系统会直接把我们的位图按旧比例拉伸
        // 铺上去 —— 字糊，而且尺寸跟着错。开了之后系统改成发这条消息，
        // 让我们自己重算。lp 是系统建议的新矩形，我们用不上：Dock 是分层窗口，
        // 尺寸完全由 FitMetrics 按图标数算，位置由 ApplyMonitorGeometry 按屏算。
        case WM_DPICHANGED: {
            const int newDpi = static_cast<int>(LOWORD(wp));   // 新的 X 轴 DPI
            if (newDpi > 0) {
                // 同一块屏上改缩放时，MonitorFromPoint 要等系统真的把新 DPI
                // 生效后才给得出来，所以这里以消息带的为准，顺手同步进状态。
                monDpi_ = newDpi;
                g_dockDpi = newDpi;
                OnDpiChanged(newDpi);
            }
            return 0;
        }
        case WM_DISPLAYCHANGE:
            // 换分辨率 / 插拔显示器：屏的位置和宽度都可能变了，重认一次。
            // 收起时才允许换屏（跟轮询里一样），所以这里传 !wantVisible_。
            UpdateMonitor(!wantVisible_);
            ApplyMonitorGeometry();
            if (wantVisible_) {
                CaptureBackdrop();
                BuildPanel();
                Render();
            }
            return 0;
        // 后台线程枚举完「哪些应用在运行」之后投回来的通知
        case kMsgRunningScanned:
            OnRunningScanned();
            return 0;
        // 全量扫描完成
        case kMsgAppsScanned:
            OnAppsScanned();
            return 0;
        // 顶栏 ⏏ 菜单的「显示 Dock」投过来的
        case kMsgShowNow:
            SetVisible(true);
            return 0;
        // 托盘图标的鼠标事件（Shell_NotifyIcon 回调）
        case kMsgTray:
            if (lp == WM_LBUTTONUP) {
                // 左键：跟鼠标贴底边一样地唤出来
                SetVisible(true);
            } else if (lp == WM_RBUTTONUP) {
                ShowTrayMenu();
            }
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd_, kHotkeyLaunchpad);
            RemoveTrayIcon();   // 不移的话图标会变成「幽灵」，鼠标挪过去才消失
            // ★ 先把两个后台线程收掉，再让消息循环结束。
            //   运行检测线程拿着进程句柄，扫描线程拿着 COM 和图标位图，
            //   谁都不能在进程退出时还悬着。
            StopRunningScan();
            WaitFullScan();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ------------------------------------------------------ 托盘
void Dock::AddTrayIcon() {
    trayIcon_ = MakeTrayIcon();
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = kMsgTray;
    nid_.hIcon = trayIcon_;
    lstrcpynW(nid_.szTip, L"MyDock", static_cast<int>(sizeof(nid_.szTip) / sizeof(wchar_t)));
    if (!Shell_NotifyIconW(NIM_ADD, &nid_)) {
        Log(L"托盘：图标添加失败（错误码 %lu）", GetLastError());
    }
}

void Dock::RemoveTrayIcon() {
    if (nid_.hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        nid_.hWnd = nullptr;
    }
    if (trayIcon_) {
        DestroyIcon(trayIcon_);
        trayIcon_ = nullptr;
    }
}

void Dock::ShowTrayMenu() {
    POINT pt = {};
    GetCursorPos(&pt);

    std::vector<MenuItem> items;
    MenuItem head;
    head.title = true;
    head.tag = L"托盘";
    head.label = L"MyDock";
    items.push_back(std::move(head));
    MenuItem s1;
    s1.separator = true;
    items.push_back(std::move(s1));

    auto line = [](std::vector<MenuItem>& v, int cmd, const wchar_t* label,
                   bool checked = false) {
        MenuItem it;
        it.cmd = cmd;
        it.label = label;
        it.checked = checked;
        v.push_back(std::move(it));
    };
    auto sep = [](std::vector<MenuItem>& v) {
        MenuItem it;
        it.separator = true;
        v.push_back(std::move(it));
    };

    line(items, 12, L"打开启动台");
    line(items, 30, L"显示 Dock");
    sep(items);
    line(items, 31, L"开机自启", IsAutoStart());
    line(items, 32, L"重启 MyDock");
    sep(items);
    line(items, 3, L"退出 MyDock");

    menuOpen_ = true;
    const int cmd = menu_.Show(hwnd_, pt.x, pt.y, items);
    menuOpen_ = false;

    switch (cmd) {
        case 12:
            launcher_.Open();
            break;
        case 30:
            SetVisible(true);
            break;
        case 31:
            // 失败时菜单项上的勾会和实际状态不符 —— 所以失败就再采一遍并重画
            if (!SetAutoStart(!IsAutoStart())) {
                Log(L"自启：切换失败，保持原状");
            }
            break;
        case 32:
            QueueRestart();   // util 里安排 1 秒后拉起新进程，这里只管退场
            DestroyWindow(hwnd_);
            break;
        case 3:
            DestroyWindow(hwnd_);
            break;
        default:
            break;
    }
}

// 2026-10-11 - AI - 托盘图标 + 开机自启
//
// Dock 是个没有窗口按钮的常驻程序：崩溃了、被任务管理器杀了，用户只能去
// 翻安装目录。托盘图标解决三件事 —— 「它还活着」的可见凭据、左键一键唤出、
// 右键菜单里放重启和退出。开机自启写 HKCU 的 Run 键（不需要管理员）。

// 运行时画一个托盘图标：深色圆角底 + 底部一排白点（mini Dock）。
// 项目没有资源文件（没嵌 .ico），运行时画省掉 windres 那一套。
HICON MakeTrayIcon() {
    constexpr int S = 32;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;      // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!color || !bits) return nullptr;

    {
        // Bitmap 只包住 bits 不拥有它，出了作用域 bits 仍归 color 位图
        Gdiplus::Bitmap bmp(S, S, 4 * S, PixelFormat32bppARGB,
                            static_cast<BYTE*>(bits));
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        Gdiplus::GraphicsPath p;
        MenuAddRoundRect(p, Gdiplus::RectF(2.0f, 4.0f, 28.0f, 24.0f), 8.0f);
        Gdiplus::SolidBrush bg(Gdiplus::Color(235, 30, 30, 34));
        g.FillPath(&bg, &p);
        Gdiplus::Pen rim(Gdiplus::Color(120, 255, 255, 255), 1.0f);
        g.DrawPath(&rim, &p);

        // 底部一排五个白点 = mini Dock
        Gdiplus::SolidBrush dot(Gdiplus::Color(255, 244, 244, 247));
        for (int i = 0; i < 5; ++i) {
            g.FillEllipse(&dot, 6.4f + i * 4.4f, 22.0f, 3.2f, 3.2f);
        }
    }

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ic;
}

}  // namespace mydock
