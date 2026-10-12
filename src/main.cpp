// 2026-10-09 - AI - MyDock 入口：初始化 GDI+/COM，创建 Dock，跑消息循环
#include <windows.h>
#include <gdiplus.h>
#include <mmsystem.h>
#include <objbase.h>

#include "dock.h"
#include "topbar.h"
#include "util.h"

namespace {

// 真正的启动流程。单独抽出来是为了让「清理」只写一遍 ——
// 以前每个失败分支各抄一段 CoUninitialize + GdiplusShutdown，
// 漏一个就是资源泄漏（而且这种漏法平时完全看不出来，只在异常退出时才咬人）。
int RunApp(HINSTANCE inst) {
    // 把系统定时器精度提到 1ms —— 否则 SetTimer(8) 会被截断到 15.6ms，
    // 鼠标在 Dock 上滑动时就会一顿一顿的
    timeBeginPeriod(1);

    // 每个显示器独立 DPI，图标才不会糊。
    //
    // ★ 这一步的结果一定要查，不能当它必然成功。
    //   Windows 允许「父亲把 DPI 感知继承给儿子」：如果启动 MyDock 的那个进程
    //   自己已经是一个固定的 DPI 上下文，我们这次设置就会**静默失败**。
    //   失败之后程序照样跑，只是 GetSystemMetrics 开始返回逻辑像素
    //   （175% 的屏上得到 1463 而不是 2560），我们自己画好的位图会被系统
    //   再拉伸 1.75 倍铺到屏幕上 —— 字就糊了，看着就是「廉价」。
    //   实测踩过：同一个 exe，从资源管理器双击是 2560x46（清晰），
    //   从某些终端启动就变成 1463x26（糊）。
    //   2026-10-10 根因查清了：不是终端的锅，是那时候编译漏了 -mwindows，
    //   链成了「控制台子系统」（见 build.cmd 顶部的说明）。系统每次都要先
    //   给一个控制台，那个控制台的 DPI 上下文就顺手传给了我们。
    //   改成 GUI 子系统之后，从哪启动都是 2560x46。
    const BOOL dpiOk =
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!dpiOk) {
        const DWORD err = GetLastError();
        // 退路：Vista 时代的系统级 DPI 感知。比没有强（至少不糊），
        // 代价是换显示器/改缩放时需要重启程序。
        const BOOL fallback = SetProcessDPIAware();
        mydock::Log(L"DPI：PerMonitorV2 设置失败（错误码 %lu），退路 %s；"
                    L"屏幕 %dx%d",
                    err, fallback ? L"生效" : L"也失败",
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    } else {
        mydock::Log(L"DPI：PerMonitorV2 就绪，屏幕 %dx%d",
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    }

    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &gsi, nullptr) != Gdiplus::Ok) {
        mydock::Log(L"GDI+ 初始化失败，MyDock 退出");
        timeEndPeriod(1);
        return 1;
    }

    // ★ COM 的返回值必须看。「成功」分两档，**两档都要配对 CoUninitialize**：
    //     S_OK    —— 本线程第一次初始化，计数 +1
    //     S_FALSE —— 之前已经初始化过，但这次调用**同样 +1**（文档写明的：
    //                "To close the COM library gracefully on a thread, each
    //                successful call to CoInitialize... must be balanced by
    //                a corresponding call to CoUninitialize"）
    //     RPC_E_CHANGED_MODE —— 别人用了另一种并发模式，这次根本没成功，不配对
    //   以前只认 S_OK，在 S_FALSE 那一档上每次启动都漏一次计数 —— 单次没事，
    //   但这是把「恰好没人再初始化」当成了「计数归零」。
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comOwned = SUCCEEDED(comHr);
    if (comHr == RPC_E_CHANGED_MODE) {
        mydock::Log(L"COM 已被别的并发模式初始化 —— 图标提取会退化，但程序继续跑");
    }

    int rc = 0;
    {
        mydock::Dock dock;
        if (!dock.Create(inst)) {
            mydock::Log(L"启动失败，MyDock 退出");
            rc = 1;
        } else {
            // 顶部状态栏是一条独立的窗口，常驻在屏幕顶上。
            // 它跟 Dock 各过各的（Dock 会自己收起，状态栏不会），所以不挂在 Dock 里面，
            // 而是在这儿跟 Dock 平级地建起来，共用同一条消息循环。
            mydock::TopBar topBar;
            topBar.Create(inst);

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }   // dock / topBar 先析构干净（窗口、GDI 资源、扫描线程都在这里收尾），再往下走

    // 唤醒线程可能还睡在热键等待里 —— 喊停并等它收完，
    // 保证 Log 不会撞上静态析构、注入点击不会落进别的应用
    mydock::StopWakeJob();

    if (comOwned) CoUninitialize();
    Gdiplus::GdiplusShutdown(token);
    timeEndPeriod(1);
    return rc;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    // 同一时间只允许一个实例
    //
    // ★ 句柄要自己关。以前是借「进程退出时系统自动回收」这一条 —— 正常退出
    //   确实没影响，但那是把清理责任外包给了系统；显式关掉之后，失败路径
    //   （比如 COM 初始化失败、Dock 创建失败）才是真的干净退场，
    //   而不是「靠退出兜底」。另外 CreateMutex 成功但已存在时也要先关再退。
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"MyDock_SingleInstance");
    const bool already = (mtx != nullptr && GetLastError() == ERROR_ALREADY_EXISTS);
    if (already) {
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    const int rc = RunApp(inst);
    if (mtx) CloseHandle(mtx);
    return rc;
}
