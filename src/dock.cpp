// 2026-10-09 - AI - Dock 条实现：分层窗口绘制、悬停高亮、点击启动/切换
#include "dock.h"
#include "appaction.h"
#include "util.h"
#include "theme.h"
#include "dock_internal.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <winver.h>
#include <shlwapi.h>
#include <objbase.h>   // 2026-10-11 - AI - 全量扫描线程里取图标要 COM
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <mutex>     // 2026-10-11 - AI - 后台扫描线程的快照锁

namespace mydock {

const wchar_t* kClass = L"MyDockWindowClass";
const int kMaxIcons = 16;

#ifndef TIMERV_NO_COALESCING
#define TIMERV_NO_COALESCING 0xFFFFFFFF
#endif

// 2026-10-11 - AI - 这个本地的 ToLower 删掉了，改成统一用 util.h 的 NormalizePath。
//
// 为什么要删而不是留着：留着的话，匿名 namespace 里的这个函数会把
// mydock::NormalizePath 挡住（名字查找先命中同名同参数的内层声明），
// 于是下面每一处「规范化」其实都退化成了单纯的小写 —— 不报错、能跑，
// 但 8.3 短名、\\?\ 前缀这些差异照样漏过去。这种 bug 不会有任何一处报错，
// 只能靠「同名函数在两层 namespace 里各有一份」这条线索提前看出来。
//
// 判「是不是同一个程序」一律走 NormalizePath，见 util.cpp 里它的说明。

// ---------------------------------------------------------------- 多显示器
// 2026-10-11 - AI - Dock 站哪块屏，就按那块屏的 DPI 和尺寸来画
//
// 以前整条 Dock 都用 util 的 Dpi()（= 系统 DPI，也就是主屏的）和
// SM_CXSCREEN / SM_CYSCREEN（同样只有主屏）。单显示器时没问题，
// 一旦接了第二块屏 —— 尤其两块缩放不一样（主屏 175%、副屏 100%）——
// 就是两头错：
//   尺寸错：按 175% 算出来的 48px 图标画到 100% 的屏上，肉眼可见地大一圈；
//   位置错：SM_CYSCREEN 是主屏高度，副屏比它矮时，Dock 会被算到屏幕外面去，
//           等于「在副屏底部划拉半天，Dock 毫无反应」。
//
// 所以 Dock 内部一律走这两个：g_dockDpi 由 UpdateMonitor() 按鼠标所在屏刷新。
int g_dockDpi = 96;
int DockDpi(int px) {
    return MulDiv(px, g_dockDpi, 96);
}

// 枚举顶层窗口时用的上下文：要么收集路径集合，要么找某个 exe 的窗口
struct EnumCtx {
    std::set<std::wstring>* paths;
};

BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    EnumCtx* c = reinterpret_cast<EnumCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;  // 跳过对话框/工具窗口
    // 2026-10-11 - AI - 再排除 WS_EX_TOOLWINDOW
    //
    // ★ 这是任务栏判断「是不是一个正经应用窗口」用的同一条判据，照着它过滤
    //   就不用给这些东西逐个列黑名单：NVIDIA Overlay、输入法候选栏、
    //   各类悬浮条、托盘小面板，全都挂在这个扩展样式上。
    //   （实测这台机器上，加了这一条之后 Overlay 和输入法就不会混进 Dock 了。）
    if (GetWindowLongPtr(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return TRUE;

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;

    wchar_t buf[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    if (QueryFullProcessImageNameW(p, 0, buf, &sz)) {
        c->paths->insert(NormalizePath(buf));
    }
    CloseHandle(p);
    return TRUE;
}

// ---------------------------------------------------------------------------
// 2026-10-11 - AI - 「哪些应用正在运行」的枚举搬到后台线程
//
// 以前这件事在 700ms 一次的定时器里同步做：枚举系统里所有顶层窗口、
// 逐个 OpenProcess 问它的 exe 路径。窗口多（几十上百个）、机器忙、
// 或者撞上权限受限的进程（OpenProcess 会慢）时，这一段能吃掉十几毫秒 ——
// 而这十几毫秒内消息循环是停的，鼠标消息只能堆在队列里，等它干完才一起
// 被处理。用户看到的就是「鼠标划过 Dock 时偶尔顿一下」，而且是偶发的，
// 很难复现。
//
// 现在：后台线程慢慢枚举，把结果放进一个快照里，枚举完 PostMessage 喊一声；
// UI 线程只做「拷一份快照 + 比对」，几微秒的事。
// 消息循环不再被枚举卡住，所以那一下卡顿在物理上消失了。

struct RunScan {
    std::mutex          mtx;
    std::set<std::wstring> paths;   // 后台线程写、UI 线程读，靠 mtx 保护
    double              costMs = 0.0;  // 上一轮枚举本身花了多久（后台线程填）
    HANDLE              stop = nullptr;
    HANDLE              thread = nullptr;
    HWND                hwnd = nullptr;
};
RunScan g_runScan;

double NowUs();   // 定义在文件后半段，这里计时要用

DWORD WINAPI RunScanProc(void*) {
    // ★ 这个线程不初始化 COM。它只用 EnumWindows / OpenProcess /
    //   QueryFullProcessImageName，全是纯 Win32。初始化了反而坏 ——
    //   CoInitialize 会把线程绑成 STA，而我们根本不用 COM。
    for (;;) {
        const double t0 = NowUs();
        std::set<std::wstring> fresh;
        EnumCtx ctx = {&fresh};
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
        {
            std::lock_guard<std::mutex> lk(g_runScan.mtx);
            g_runScan.paths.swap(fresh);
            g_runScan.costMs = (NowUs() - t0) / 1000.0;
        }
        // 喊 UI 线程来取。用 PostMessage 而不是 SendMessage：
        // 后台线程绝不能等 UI 线程，否则两边可能互相等死。
        if (g_runScan.hwnd) PostMessage(g_runScan.hwnd, kMsgRunningScanned, 0, 0);
        // 等 700ms，或者被退出信号叫醒
        if (WaitForSingleObject(g_runScan.stop, 700) == WAIT_OBJECT_0) break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 2026-10-11 - AI - 开始菜单的全量扫描也搬到后台
//
// 扫开始菜单（递归目录 + 解析几十个快捷方式 + 逐个取 256×256 图标）是最贵的一步，
// 应用多、网络盘上有失效快捷方式时能到一秒往上。以前它挡在窗口创建之前，
// 用户双击之后那段时间里界面毫无动静；「刷新应用列表」也一样，点一下卡一下。
//
// 现在：Dock 先按 pinned.txt 把自己那十几格造出来（快，见 MakeItemsFromPinned），
// 窗口立刻出现；全量扫描丢到后台线程，扫完 PostMessage 回来，启动台和 Dock
// 无缝换成高清版本。

struct FullScan {
    std::mutex          mtx;
    std::vector<AppItem> result;   // 扫描结果（连图标一起），UI 线程来取
    bool                ready = false;
    HANDLE              thread = nullptr;
    HWND                hwnd = nullptr;
};
FullScan g_fullScan;

DWORD WINAPI FullScanProc(void*) {
    // 取 256px 图标走 IShellItemImageFactory，那要 COM —— 这个线程自己初始化。
    // ★ SUCCEEDED 就是「要配对 CoUninitialize」。S_FALSE 也是成功：它表示
    //   线程之前已经初始化过，但**这次调用同样加了一层计数**，不减掉就是泄漏。
    //   只有 RPC_E_CHANGED_MODE（失败）才不配对清理。跟 main.cpp 同一条规矩。
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<AppItem> all = EnumerateStartMenuApps();
    {
        std::lock_guard<std::mutex> lk(g_fullScan.mtx);
        g_fullScan.result.swap(all);
        g_fullScan.ready = true;
    }
    if (SUCCEEDED(comHr)) CoUninitialize();
    if (g_fullScan.hwnd) PostMessage(g_fullScan.hwnd, kMsgAppsScanned, 0, 0);
    return 0;
}

void StartFullScan(HWND hwnd) {
    if (g_fullScan.thread) return;   // 已经在扫了，别叠一个上去
    g_fullScan.hwnd = hwnd;
    g_fullScan.thread = CreateThread(nullptr, 0, FullScanProc, nullptr, 0, nullptr);
    if (!g_fullScan.thread) {
        Log(L"全量扫描：起不来后台线程，启动台暂时不会有内容");
    }
}

// 退出前等它收尾。线程还拿着 COM 和一堆图标位图，进程不能先走。
//
// ★ 不设超时。全量扫描实测要 8 秒（75 个应用），设 5 秒的话退出时几乎必然
//   超时，然后就会犯一个更糟的错：把还在跑的线程句柄关掉 —— 线程本身没事，
//   但它接下来还要取图标、还要碰 COM，而调用方马上就要 GdiplusShutdown 了。
//   挂着等几秒是安全的代价：扫描线程里的调用（FindFirstFile / COM）最终都会
//   返回，没有能永久卡死的点。
void WaitFullScan() {
    if (g_fullScan.thread) {
        WaitForSingleObject(g_fullScan.thread, INFINITE);
        CloseHandle(g_fullScan.thread);
        g_fullScan.thread = nullptr;
    }
}

// ---- Dock 右侧那一区：正在跑、但没固定在 Dock 上（2026-10-11）----

// 窗口不见了之后还留多久才真的撤掉那一格。
// ★ 有些应用（Electron 类尤其）切窗口时会先把旧窗口销毁再造新的，
//   没有宽限的话，图标会在这一瞬间消失又出现 —— 用户看到的就是闪一下。
constexpr DWORD kRunGraceMs = 2500;
// 运行区最多几格。再多整条 Dock 会被压得连图标都认不出来。
constexpr int kRunMaxItems = 8;

// 是不是系统组件。它们全都有窗口（TextInputHost、ShellExperienceHost、
// ApplicationFrameHost……），但没有一个是「用户打开的应用」。
// ★ 记事本、画图、写字板确实是应用，只是碰巧也住在 System32 —— 单独放行。
bool IsSystemComponent(const std::wstring& lowerPath) {
    static const wchar_t* kNot[] = {L"notepad.exe", L"mspaint.exe", L"write.exe",
                                    L"wordpad.exe", L"snippingtool.exe", L"calc.exe"};
    const size_t slash = lowerPath.find_last_of(L"\\/");
    const std::wstring leaf =
        (slash == std::wstring::npos) ? lowerPath : lowerPath.substr(slash + 1);
    for (const wchar_t* w : kNot) {
        if (leaf == w) return false;
    }
    // ★ C:\Windows\ 下的一律不算应用。explorer.exe 也在那儿 ——
    //   它的窗口里包含桌面和任务栏这种永远存在的东西，不排除的话运行区会
    //   常年钉着一个「文件资源管理器」，怎么关都关不掉（实测第一次就是这么错的）。
    //   （\windows\ 后面要带反斜杠，所以 WindowsApps 目录下的 UWP 不受影响。）
    if (lowerPath.find(L"\\windows\\") != std::wstring::npos) return true;
    return false;
}

// MyDock 自己。给自己留一格没意义，点下去还变成「切到自己」。
const std::wstring& SelfLowerPath() {
    static std::wstring cached;
    if (cached.empty()) {
        wchar_t buf[MAX_PATH] = {};
        cached = GetModuleFileNameW(nullptr, buf, MAX_PATH) ? NormalizePath(buf) : L"?";
    }
    return cached;
}

// 显示名用 exe 里记的 FileDescription，比文件名好看得多：
//   chrome.exe → Google Chrome    Weixin.exe → 微信    msedge.exe → Microsoft Edge
// 取不到（很多小程序压根不写版本信息）就退回文件名去掉扩展名。
std::wstring PrettyName(const std::wstring& exePath) {
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (sz > 0) {
        std::vector<BYTE> buf(static_cast<size_t>(sz));
        if (GetFileVersionInfoW(exePath.c_str(), 0, sz, buf.data())) {
            // 语言代码页不固定（简体中文是 0804/04b0，英文是 0409/04b0），
            // 硬猜一个会取到空串，所以先问它支持哪些、取第一项
            struct LangCp { WORD lang; WORD cp; };
            LangCp* trans = nullptr;
            UINT tlen = 0;
            if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                               reinterpret_cast<void**>(&trans), &tlen) &&
                trans && tlen >= sizeof(LangCp)) {
                wchar_t key[64] = {};
                swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription",
                           trans[0].lang, trans[0].cp);
                wchar_t* val = nullptr;
                UINT vlen = 0;
                if (VerQueryValueW(buf.data(), key, reinterpret_cast<void**>(&val), &vlen) &&
                    val && vlen > 1 && val[0]) {
                    return val;
                }
            }
        }
    }
    const size_t slash = exePath.find_last_of(L"\\/");
    std::wstring leaf = (slash == std::wstring::npos) ? exePath : exePath.substr(slash + 1);
    const size_t dot = leaf.rfind(L'.');
    if (dot != std::wstring::npos) leaf = leaf.substr(0, dot);
    return leaf;
}

// 2026-10-11 - AI - 由一个 exe 路径造一格
//
// 两处用它：运行区那些「正在跑但没固定」的图标，以及固定区里不在开始菜单那几个
// （用户从运行区右键「添加到 Dock」来的，pinned.txt 里存的是 exe 的完整路径）。
AppItem MakeAppItemFromExe(const std::wstring& exePath) {
    AppItem it;
    it.target = exePath;
    it.lnkPath = exePath;      // 右键「添加到 Dock」写进 pinned.txt 的就是这个
    it.kind = AppKind::File;
    it.name = PrettyName(exePath);

    SHFILEINFOW sfi = {};
    if (SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
        if (sfi.hIcon) DestroyIcon(sfi.hIcon);
        it.iconIndex = sfi.iIcon;
        it.icon = LoadSystemIcon(sfi.iIcon);
    }
    return it;
}

// ---------------------------------------------------------------------------

// 按清单里的每一行直接造一格，不去碰全量列表 —— 启动时的快速路径。
//
// pinned.txt 本身就是一份现成的「缓存」：Dock 上要哪十几格、什么顺序，全写在
// 里面。以前启动时把全量列表扫完再从中挑这几格，白付了全量的时间。
// .lnk 的图标先从系统图像列表取（快），高清 256px 那版等全量扫描完成后换。
std::vector<AppItem> MakeItemsFromPinned(const std::vector<std::wstring>& pinned) {
    std::vector<AppItem> out;
    for (const std::wstring& p : pinned) {
        if (p.empty() || p[0] == L';') continue;
        if (IsSpecialLine(p)) {
            out.push_back(MakeSpecialItem(KindFromLine(p)));
            continue;
        }
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log(L"固定项 %ls 已经找不到了，跳过", p.c_str());
            continue;
        }
        if (_wcsicmp(PathFindExtensionW(p.c_str()), L".lnk") == 0) {
            // .lnk 必须解出真正的 exe —— 不然「已经在运行就切过去」永远不成立
            AppItem it = MakeAppFromFile(p);
            SHFILEINFOW sfi = {};
            if (SHGetFileInfoW(p.c_str(), 0, &sfi, sizeof(sfi),
                               SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_LARGEICON)) {
                if (sfi.hIcon) DestroyIcon(sfi.hIcon);
                it.iconIndex = sfi.iIcon;
                it.icon = LoadSystemIcon(sfi.iIcon);
            }
            out.push_back(std::move(it));
        } else {
            out.push_back(MakeAppItemFromExe(p));
        }
    }
    return out;
}

// 设了环境变量 MYDOCK_TRACE 时打开诊断：
//   1 = 每秒打印一次帧率与耗时分布
//   2 = 再加逐帧的布局数据（用来量化验证图标会不会突然跳一下）
// 有了它就能量化验证，而不是靠肉眼盯录像。
int TraceLevel() {
    static int cached = -2;
    if (cached == -2) {
        wchar_t buf[8] = {};
        cached = (GetEnvironmentVariableW(L"MYDOCK_TRACE", buf, 8) > 0) ? _wtoi(buf) : 0;
    }
    return cached;
}

double NowUs() {
    static LARGE_INTEGER freq = {};
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) * 1000000.0 / static_cast<double>(freq.QuadPart);
}

// MYDOCK_FLUSH=1 时，每画完一帧等一次 DWM 合成。
//
// 为什么需要这个：UpdateLayeredWindow 是「自己往屏幕上贴图」的老式路径，**没有节拍**。
// 鼠标消息一来就画一帧、画完就提交，于是出帧频率被鼠标报点率绑住（常见 125Hz），
// 而屏幕是 165Hz —— 165/125 = 1.32，每一帧在屏幕上停留 6ms 还是 12ms 是不定的，
// 这就是经典的「3:2 抖动」：动作是连续的，看着却一顿一顿。
// DwmFlush 会把我们的提交对齐到 DWM 的合成边界，让出帧节奏跟着显示器走。
// （macOS 那边其实是同一个道理 —— 只不过 WindowServer 天生按刷新率固定节拍取帧，
//   应用不需要自己管这件事。）
bool FlushEnabled() {
    static int cached = -1;
    if (cached < 0) {
        wchar_t buf[8] = {};
        cached = (GetEnvironmentVariableW(L"MYDOCK_NOFLUSH", buf, 8) > 0) ? 0 : 1;
    }
    return cached == 1;
}

// 图标缩小用哪种重采样。这是每帧最贵的一项（6ms / 16 个），用来做 A/B 选型。
// 通过 MYDOCK_INTERP 切换：1=高质量双线性（默认） 2=双线性 3=双三次 4=最近邻
Gdiplus::InterpolationMode InterpMode() {
    static int cached = -1;
    if (cached < 0) {
        wchar_t buf[8] = {};
        cached = (GetEnvironmentVariableW(L"MYDOCK_INTERP", buf, 8) > 0) ? _wtoi(buf) : 1;
    }
    switch (cached) {
        case 2: return Gdiplus::InterpolationModeBilinear;
        case 3: return Gdiplus::InterpolationModeHighQualityBicubic;
        case 4: return Gdiplus::InterpolationModeNearestNeighbor;
        default: return Gdiplus::InterpolationModeHighQualityBilinear;
    }
}

// 读注册表问系统当前是不是浅色主题。这个每帧都要用，而注册表读一次要零点几毫秒，
// 攒进每帧三十几毫秒的预算里就不划算 —— 缓存一秒。
// 当前最前面那个窗口是不是「占满整个屏幕的应用」（游戏、PPT 放映、全屏播放器等）。
// 是的话 Dock 就不该自动冒出来 —— 它会盖住应用底部的按钮。
//
// 两条判据叠起来用：
//   1. SHQueryUserNotificationState —— 系统自己维护的状态。独占全屏的 D3D 程序、
//      全屏演示模式都会在这里报出来。
//   2. 兜底：看前台窗口的矩形是不是盖住了整个显示器。无边框全屏的游戏/播放器
//      不一定被上面那条识别，但矩形一定铺满。
bool FullscreenAppOnTop() {
    QUERY_USER_NOTIFICATION_STATE st = QUNS_ACCEPTS_NOTIFICATIONS;
    if (SUCCEEDED(SHQueryUserNotificationState(&st))) {
        if (st == QUNS_BUSY || st == QUNS_RUNNING_D3D_FULL_SCREEN ||
            st == QUNS_PRESENTATION_MODE) {
            return true;
        }
    }

    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetShellWindow()) return false;

    // 桌面/任务栏自己不算
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (_wcsicmp(cls, L"Progman") == 0 || _wcsicmp(cls, L"WorkerW") == 0 ||
        _wcsicmp(cls, L"Shell_TrayWnd") == 0) {
        return false;
    }

    // ★ 最大化窗口不算全屏 —— 这条不写会出大问题。
    // Windows 上「最大化」的窗口矩形会比显示器大出 8px（左边 -8、上边 -8、右边 +8、
    // 下边 +8），这是 Win32 由来已久的行为。光比矩形的话，任何最大化的窗口都会被
    // 当成全屏，结果就是「只要有个窗口最大化，Dock 就永远不出来了」。
    if (IsZoomed(fg)) return false;

    RECT r = {};
    if (!GetWindowRect(fg, &r)) return false;

    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const RECT& m = mi.rcMonitor;

    // 容差几像素：有些程序会留 1~2 px 的边
    const int tol = 2;
    return r.left <= m.left + tol && r.top <= m.top + tol &&
           r.right >= m.right - tol && r.bottom >= m.bottom - tol;
}

// 读注册表问系统当前是不是浅色主题。这个每帧都要用，而注册表读一次要零点几毫秒，
// 攒进每帧三十几毫秒的预算里就不划算 —— 缓存一秒。
bool IsLightTheme() {
    static bool cached = true;
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (last != 0 && now - last < 1000) return cached;

    DWORD v = 1, sz = sizeof(v);
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        RegQueryValueExW(k, L"SystemUsesLightTheme", nullptr, nullptr,
                         reinterpret_cast<BYTE*>(&v), &sz);
        RegCloseKey(k);
    }
    cached = (v != 0);
    last = now;
    return cached;
}


// 2026-10-13 - AI - 包装器：dock_menu.cpp 的「重建图标缓存」菜单项需要
// 触达匿名 ns 里的全量扫描；这里转一手（名字加前缀避免与匿名版二义）。
void DockStartFullScan(HWND hwnd) { StartFullScan(hwnd); }


Dock::~Dock() {
    // 正常退出时 WM_DESTROY 已经收过线程了；这里兜 Create 失败直接析构的路
    StopRunningScan();
    WaitFullScan();
    delete glass_;
    delete noise_;
    delete panel_;
    glass_ = nullptr;
    noise_ = nullptr;
    panel_ = nullptr;
    // 固定区是 DeepCopy 出来的独立一份，apps_ 里的图标会被 ApplyApps 释放，
    // 但这一份不在 apps_ 里（apps_ 每次都是重新拼的），得自己收掉。
    FreeAppIcons(pinnedApps_);
    FreeAppIcons(apps_);
}

// 下面这三个在文件后半段才有定义，Create 里要用，先给出原型
std::wstring StemOf(const std::wstring& path);
std::vector<AppItem> PickByName(std::vector<AppItem>& all,
                                const std::vector<std::wstring>& names);
std::vector<AppItem> TakeHead(std::vector<AppItem>& all, int count);

bool Dock::Create(HINSTANCE inst) {
    inst_ = inst;

    // ★ 先认出自己该站哪块屏，再算任何尺寸。
    //   下面每一个 DockDpi() 都读 g_dockDpi，而它就是从这块屏上取来的 ——
    //   顺序倒过来的话，前几个尺寸会按上一次留下的 96 算，然后不再重算。
    UpdateMonitor(true);

    baseSize_ = DockDpi(48);
    gap_ = DockDpi(8);
    padX_ = DockDpi(12);
    padTop_ = DockDpi(10);
    padBottom_ = DockDpi(18);
    // 余晖的影响半径（见 ComputeTarget）。定成一个格子宽，在 FitMetrics 里按
    // 实际图标尺寸算 —— 因为图标尺寸会随图标个数自适应，写死会跟不上。
    glowRadius_ = DockDpi(60);

    // ---- 应用列表 ----
    //
    // 2026-10-11 - AI - 启动从「全扫完才见窗口」改成「先见窗口，全量稍后到」。
    //
    // pinned.txt 本身就是一份现成的缓存：Dock 上要哪十几格、按什么顺序，全写在
    // 里面。以前启动时把全量列表扫完（递归目录 + 解析快捷方式 + 逐个取 256px
    // 图标）再从中挑这几格，等于为十几格付了全量的时间 —— 那几百毫秒到一秒里，
    // 用户双击之后屏幕上什么都没有。
    // 现在：先按清单直接造格（快），窗口立刻出现；全量扫描丢给后台线程
    // （StartFullScan，Create 末尾启动），扫完启动台补齐、Dock 换成高清图标。
    // 只有第一次运行（清单还没有）才同步扫全量 —— 反正那是唯一的一次。
    std::vector<std::wstring> pinned = LoadPinnedList();
    std::vector<AppItem> picked;

    if (pinned.empty()) {
        // 第一次运行：给一份初始清单（老来源 MyDockFinder 的 Pinned，或前 N 个），
        // 并且立刻写回文件 —— 有了文件，之后才能在启动台里增删。
        std::vector<AppItem> all = EnumerateStartMenuApps();
        if (all.empty()) {
            Log(L"没找到任何可用应用");
            return false;
        }
        std::vector<AppItem> legacy = LoadPinnedApps();
        if (!legacy.empty()) {
            for (const AppItem& a : legacy) {
                if (!a.lnkPath.empty()) pinned.push_back(a.lnkPath);
            }
            FreeAppIcons(legacy);
        }
        if (pinned.empty()) {
            for (size_t i = 0; i < all.size() && pinned.size() < static_cast<size_t>(kMaxIcons);
                 ++i) {
                pinned.push_back(all[i].lnkPath);
            }
        }
        // 这里先不落盘：下面会按「现在真实的路径」统一写一次，避免写下过期的路径

        labelH_ = DockDpi(28);

        // 启动台先拿走全量副本（Create 按值接收，内部自己搬走）
        std::vector<AppItem> copy = DeepCopyAppList(all);
        launcher_.Create(inst, nullptr, std::move(copy));
        launcherBuilt_ = true;

        picked = PickFromPinned(all);
        if (picked.empty()) {
            picked = TakeHead(all, kMaxIcons);
            Log(L"用不到旧的 pinned 清单，先用前 %zu 个应用，已写入新清单", picked.size());
        }
        FreeAppIcons(all);       // 搬剩下的就没用了
    } else {
        labelH_ = DockDpi(28);
        // 快速路径：只造 pinned 里那十几格，全量列表等后台扫描
        picked = MakeItemsFromPinned(pinned);
    }

    // 不管走哪条路，最后都用「现在真实的路径」写回清单，下次起就不会再对不上了
    std::vector<std::wstring> fixed;
    // ★ 内建格子（启动台/废纸篓/日历）没有 lnkPath，得写回它们自己的 @ 行 ——
    //   写成空行的话，下次启动这格就消失了。
    for (const AppItem& a : picked) {
        fixed.push_back(a.kind == AppKind::File ? a.lnkPath : SpecialLine(a.kind));
    }

    if (!fixed.empty()) SavePinnedList(fixed);

    apps_ = std::move(picked);
    if (apps_.empty()) {
        // 不再因为「一格都没有」就放弃启动：清单可能整体失效（换机器、盘符变了），
        // 等后台扫描完成后 PickFromPinned 会兜底重建。
        Log(L"pinned 清单里现在没有可用的应用，等全量扫描完成后重试");
    }

    // 到这里 apps_ 的图标还是 256 的源图；后面 FitMetrics 定下尺寸再统一缩
    FitMetrics(static_cast<int>(apps_.size()));

    // 图标原图多是 256×256，屏幕上只画 baseSize_ 那么大。这一版图标尺寸恒定，
    // 所以缩一次就够 —— 之后每帧都是 1:1 贴，完全不用重采样。
    for (auto& a : apps_) a.icon = DownscaleIconToMaxSide(a.icon, baseSize_);

    // 位置按「鼠标所在那块屏」算，不是主屏 —— 见 UpdateMonitor 的说明
    winX_ = monFull_.left + ((monFull_.right - monFull_.left) - w_) / 2;
    winY_ = monFull_.bottom - h_ - DockDpi(6);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kClass, L"MyDock", WS_POPUP,
        winX_, winY_, w_, h_, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log(L"创建窗口失败，错误码 %lu", GetLastError());
        return false;
    }

    CreateBackBuffer(w_, h_);
    noise_ = MakeNoiseTexture(96);
    // ★ apps_ 是调用方给的初始列表，它就是「固定区」那一份 —— 必须先记下来。
    //   不记的话，下面 RefreshRunning 第一次跑起来时 pinnedApps_ 还是空的，
    //   它会以为「固定区一个都没有」，把本来就在 Dock 上的 Pulse / 微信 / Pebrel
    //   统统当成「新开的」塞进运行区 —— 实测就错成这样：8 个运行项里有 4 个是
    //   Dock 上常驻的，反过来那 9 个固定项一个都没画出来。
    pinnedApps_ = DeepCopyAppList(apps_);
    const int n = static_cast<int>(apps_.size());
    x_.assign(n, 0.0f);
    curSize_.assign(n, static_cast<float>(baseSize_));
    targetSize_.assign(n, static_cast<float>(baseSize_));
    hl_.assign(n, 0.0f);
    hlTarget_.assign(n, 0.0f);
    running_.assign(n, false);
    LayoutFromSizes();

    LoadSettings();     // 读 settings.ini（内部会画一帧）
    flush_ = FlushEnabled();
    RefreshRunning(true);
    CaptureBackdrop();  // 趁窗口还没显示，先拍下它背后那块桌面
    BuildPanel();
    Render();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    shown_ = true;
    wantVisible_ = true;
    slide_ = 0.0f;
    slideTarget_ = 0.0f;
    // 启动后先多显示一会儿，别刚起来就缩回去让人以为没启动。
    // 注意不能用「lastInside_ = now + 2500」来表示未来时刻 ——
    // DWORD 是无符号的，now - lastInside_ 会下溢成 42 亿，反而立刻触发收起。
    lastInside_ = GetTickCount();
    graceUntil_ = lastInside_ + 2500;
    SetTimer(hwnd_, kTimerPoll, 120, nullptr);

    launcher_.SetDockWindow(hwnd_);   // 固定项变动时它要通知这边重建列表

    // F4 打开启动台 —— 跟 macOS 上 Launchpad 的键位一样。
    // RegisterHotKey 会把 WM_HOTKEY 直接投到这个窗口，即使 Dock 已经收起来了也收得到。
    if (!RegisterHotKey(hwnd_, kHotkeyLaunchpad, 0, VK_F4)) {
        Log(L"注册 F4 失败（可能已被别的程序占用），仍可从右键菜单打开启动台");
    }

    // ★ 不能用开头那个 n：Create 中间 RefreshRunning 可能已经重建过列表
    //   （运行区第一次填充就是），那时 apps_ 早已不是进来时那一份 ——
    //   照 n 报会说出「9 个图标」而窗口其实是按 13 个排的宽。
    Log(L"Dock 就绪：%d 个图标（固定 %d + 运行 %zu），窗口 %dx%d 在 (%d,%d)",
        static_cast<int>(apps_.size()), pinnedCount_, runEntries_.size(),
        w_, h_, winX_, winY_);

    // 窗口建好了才有 hwnd_（托盘回调要投消息给它），所以放在最后
    AddTrayIcon();
    // 窗口建好了才有 hwnd_（线程要拿它投消息），所以放在最后
    StartRunningScan();
    // 快速路径（pinned 直接造格）还没扫全量列表 —— 丢给后台线程，
    // 扫完 OnAppsScanned 会把启动台补上、Dock 换成高清图标。
    if (!launcherBuilt_) StartFullScan(hwnd_);
    return true;
}

void Dock::CreateBackBuffer(int w, int h) {
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }

    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (bmp_) SelectObject(dc_, bmp_);
    ReleaseDC(nullptr, screen);
}

void Dock::ComputeTarget() {
    // 高亮分成两层，各自解决一个问题：
    //
    //   ① 命中项 —— 光标落在哪个图标里，它就是满亮 1.0。
    //      按「图标的横向区间」判定，所以光标在同一个图标里随便挪，亮度都不变；
    //      不会像纯距离函数那样「停在图标中间亮、挪到边上就暗一半」。
    //
    //   ② 余晖项 —— 邻居图标带一点点亮度（按到它最近那条边的距离衰减）。
    //      它只为了一个目的：光标从 A 滑到 B 的那一瞬间，亮度是「淌」过去的，
    //      不是 A 啪一下全灭、B 啪一下全亮。这就是「图标之间的过渡」。
    //      给得很淡（上限两成），保证任何时刻都能一眼看出「当前指着哪个」——
    //      之前按距离铺满整条，光标落在接缝上时两个图标一样亮，看着像同时选了两个。
    const int n = static_cast<int>(apps_.size());
    if (static_cast<int>(hlTarget_.size()) != n) return;
    const bool on = mouseInside_ && mouseX_ >= 0;

    int hit = -1;
    if (on) {
        for (int i = 0; i < n; ++i) {
            if (mouseX_ >= x_[i] && mouseX_ < x_[i] + static_cast<float>(baseSize_)) {
                hit = i;
                break;
            }
        }
        if (hit < 0) {
            // 落在两个图标之间的空隙里：算给最近的那个，免得接缝处谁都不亮
            float bestD = 1e9f;
            for (int i = 0; i < n; ++i) {
                const float d = std::fabs(x_[i] + static_cast<float>(baseSize_) / 2.0f -
                                          static_cast<float>(mouseX_));
                if (d < bestD) { bestD = d; hit = i; }
            }
        }
    }

    const float R = static_cast<float>(glowRadius_);
    const float mx = static_cast<float>(mouseX_);
    for (int i = 0; i < n; ++i) {
        float t = 0.0f;
        if (on) {
            if (i == hit) {
                t = 1.0f;
            } else {
                // 光标到第 i 个图标最近那条边的距离
                const float rightEdge = x_[i] + static_cast<float>(baseSize_);
                float d = 0.0f;
                if (mx < x_[i]) d = x_[i] - mx;
                else if (mx >= rightEdge) d = mx - rightEdge;
                const float u = 1.0f - d / R;
                if (u > 0.0f) t = 0.22f * u;
            }
        }
        hlTarget_[i] = t * glow_;
    }
}

void Dock::LayoutFromSizes() {
    // 图标尺寸恒定，所以排版就是一句等差数列 —— 开机算一次，之后再不会被改动。
    // 这是这一版「不可能抖」的根本原因：位置是常量，没有任何逐帧变化的量参与其中。
    const int n = static_cast<int>(apps_.size());
    if (n == 0 || static_cast<int>(x_.size()) != n) return;
    const float pitch = static_cast<float>(baseSize_ + gap_);
    for (int i = 0; i < n; ++i) {
        float x = static_cast<float>(padX_) + static_cast<float>(i) * pitch;
        // 运行区整体往右让开分隔线那点宽度。命中测试直接读 x_，所以自动跟着走。
        if (pinnedCount_ > 0 && i >= pinnedCount_) x += static_cast<float>(sepW_);
        x_[i] = x;
    }
}

void Dock::FitMetrics(int n) {
    // ---- 把尺寸定下来：整行必须真的放得进屏幕 ----
    // 前几版是靠「图标行比胶囊宽、两端被裁掉一点」硬撑的 —— 那正是
    // 「鼠标放上去，最左边的应用就出了渲染范围」的来源。这里改成先压空隙、
    // 再等比缩图标，保证 n 个图标完整落在胶囊内区里，一个都不溢出。
    // ★ 可用宽度取「Dock 当前所在那块屏」的宽度，不是 SM_CXSCREEN（主屏）。
    //   副屏比主屏窄时，按主屏算出来的宽度会把整条 Dock 顶出屏幕外。
    const int sw = (monFull_.right > monFull_.left) ? (monFull_.right - monFull_.left)
                                                    : GetSystemMetrics(SM_CXSCREEN);
    baseSize_ = DockDpi(48);
    gap_ = DockDpi(8);
    barPad_ = DockDpi(10);

    const int availInner = sw - DockDpi(24) - DockDpi(14) * 2 - barPad_ * 2;
    const int gapMin = DockDpi(4);
    // 2026-10-11 - AI - 固定区和运行区之间那条分隔线要单独占掉一点宽度。
    //   只有「两边都有东西」时才留 —— 运行区空着的时候留一条孤零零的竖线很怪。
    sepW_ = (pinnedCount_ > 0 && pinnedCount_ < n) ? DockDpi(14) : 0;
    const float need0 = static_cast<float>(n) * baseSize_ +
                        static_cast<float>(n - 1) * gap_ + static_cast<float>(sepW_);
    if (need0 > static_cast<float>(availInner) && n >= 1) {
        gap_ = gapMin;   // 空隙先让出来，图标尽量保住原本的大小
        const float need1 = static_cast<float>(n) * baseSize_ +
                            static_cast<float>(n - 1) * gap_ + static_cast<float>(sepW_);
        if (need1 > static_cast<float>(availInner)) {
            // 还不够就等比缩图标（need 对 baseSize_ 是线性的，一次就能算准）
            const float room = static_cast<float>(availInner) -
                               static_cast<float>(n - 1) * gap_ -
                               static_cast<float>(sepW_);
            baseSize_ = static_cast<int>(room / static_cast<float>(n));
            if (baseSize_ < DockDpi(24)) baseSize_ = DockDpi(24);
        }
    }
    rowW_ = static_cast<float>(n) * baseSize_ +
            static_cast<float>(n - 1) * gap_ + static_cast<float>(sepW_);
    glowRadius_ = baseSize_ + gap_;   // 余晖刚好够到紧邻那一个

    // ---- 胶囊几何，一次算死 ----
    barW_ = static_cast<int>(rowW_ + 0.5f) + barPad_ * 2;
    w_ = barW_ + DockDpi(14) * 2;
    if (w_ > sw - DockDpi(24)) w_ = sw - DockDpi(24);
    if (barW_ > w_) barW_ = w_;
    barL_ = (w_ - barW_) / 2;
    // 高度不用再给放大预留空间，整条 dock 比之前矮一截
    h_ = padTop_ + labelH_ + baseSize_ + padBottom_;
    // 图标行紧贴胶囊左内边。尺寸恒定，所以这个位置从此就是常量。
    padX_ = barL_ + barPad_;
}

// ---------------------------------------------------------------- 多显示器
// 2026-10-11 - AI - 三件事：认屏、挪位、DPI 变了重排
void Dock::UpdateMonitor(bool allowSwitch) {
    POINT pt = {};
    if (!GetCursorPos(&pt)) return;
    const MonitorInfo mi = MonitorAt(pt);
    if (!mi.handle) return;

    const bool firstTime = (monitor_ == nullptr);
    const bool switched = (!firstTime && mi.handle != monitor_);
    // ★ 鼠标还压在 Dock 上的时候不许换屏。
    //   允许的话，鼠标在两块屏交界处划一下，Dock 就会当着用户的面从这块屏
    //   跳到那块屏 —— 他明明只是想点个图标。所以只有「收起状态」才允许换
    //   （allowSwitch 由调用方按 wantVisible_ 传进来）。
    if (switched && !allowSwitch) return;

    const bool dpiChanged = (mi.dpi != monDpi_);
    monitor_ = mi.handle;
    monFull_ = mi.full;
    monDpi_ = mi.dpi;
    g_dockDpi = mi.dpi;

    if (firstTime) return;   // 第一次只是记下来，窗口还没建好，动不了

    if (switched) {
        Log(L"Dock 换屏：%d,%d - %d,%d（DPI %d）",
            mi.full.left, mi.full.top, mi.full.right, mi.full.bottom, mi.dpi);
        ApplyMonitorGeometry();
        // 两块屏背后的画面不一样，玻璃底要重拍 —— 否则会带着上一块屏的残影
        if (wantVisible_) {
            CaptureBackdrop();
            BuildPanel();
        }
    }
    if (dpiChanged) OnDpiChanged(mi.dpi);
}

void Dock::ApplyMonitorGeometry() {
    if (!hwnd_) return;
    const int monW = monFull_.right - monFull_.left;
    winX_ = monFull_.left + (monW - w_) / 2;
    winY_ = monFull_.bottom - h_ - DockDpi(6);
    // 别越过这块屏的左右边界 —— 副屏很窄而图标又多时，上面那行会算出负数
    const int margin = DockDpi(4);
    if (winX_ < monFull_.left + margin) winX_ = monFull_.left + margin;
    if (winX_ + w_ > monFull_.right - margin) winX_ = monFull_.right - margin - w_;
    SetWindowPos(hwnd_, nullptr, winX_, winY_, w_, h_, SWP_NOZORDER | SWP_NOACTIVATE);
}

void Dock::OnDpiChanged(int newDpi) {
    if (newDpi <= 0) return;
    Log(L"显示器 DPI 变成 %d，整条 Dock 重排", newDpi);
    monDpi_ = newDpi;
    g_dockDpi = newDpi;
    // 图标早就被缩到「旧 DPI 下的显示尺寸」了，256 的原图在那时已经释放 ——
    // 拿不回来，只能整批重造。DPI 变化是罕见事件（改缩放、拖窗口跨屏），
    // 重扫一次开始菜单的那几百毫秒可以接受。
    ReloadApps();
}

bool Dock::StepGlow(double dtMs) {
    // 高亮不是「瞬间到位」，而是每帧往目标挪一部分 —— 这就是「缓缓亮起」。
    //
    // ★ 关键：挪的幅度要乘上这一帧实际过了多久（dtMs）。
    //   不乘的话，淡入的快慢会跟着帧率走：165fps 时 270ms 亮到位，82fps 时就要 540ms，
    //   帧率一波动，同一个动作就会时快时慢 —— 那本身就是一种「抖」。
    if (dtMs <= 0.0) dtMs = 16.0;
    if (dtMs > 64.0) dtMs = 64.0;
    const double tau = 90.0;  // 时间常数：约 3τ ≈ 270ms 亮到位
    const float k = static_cast<float>(1.0 - std::exp(-dtMs / tau));

    bool moving = false;
    for (size_t i = 0; i < hl_.size(); ++i) {
        const float d = hlTarget_[i] - hl_[i];
        if (std::fabs(d) < 0.002f) {
            if (hl_[i] != hlTarget_[i]) { hl_[i] = hlTarget_[i]; moving = true; }
            continue;
        }
        hl_[i] += d * k;
        moving = true;
    }
    return moving;
}

void Dock::ApplyGlow(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    glow_ = v;
    mouseX_ = -1;
    std::fill(hl_.begin(), hl_.end(), 0.0f);
    std::fill(hlTarget_.begin(), hlTarget_.end(), 0.0f);
    ComputeTarget();
    Render();
}

void Dock::LoadSettings() {
    const std::wstring ini = ConfigDir() + L"\\settings.ini";
    const DWORD attr = GetFileAttributesW(ini.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        // 第一次运行：写一份带说明的配置出去，之后可以自己改
        FILE* f = nullptr;
        if (_wfopen_s(&f, ini.c_str(), L"w, ccs=UTF-16LE") == 0 && f) {
            fwprintf(f, L"; MyDock 配置\n");
            fwprintf(f, L";\n");
            fwprintf(f, L"; [look] glow —— 鼠标悬停时，图标背后那块底色亮起的强度，0~100\n");
            fwprintf(f, L";\n");
            fwprintf(f, L";   这一版没有「悬停放大」了。原因是：放大让整行凭空多出一百多像素宽，\n");
            fwprintf(f, L";   这笔宽度只有三个出口 —— dock 变宽 / 空隙缩小 / 图标相对光标漂移 ——\n");
            fwprintf(f, L";   三者守恒，堵住一个另一个就冒出来，怎么调都留一层抖动。\n");
            fwprintf(f, L";   而「高亮」一点宽度都不占，布局因此是常量：图标尺寸恒定、位置恒定、\n");
            fwprintf(f, L";   行宽恒定，抖动在物理上不可能发生。\n");
            fwprintf(f, L";\n");
            fwprintf(f, L";   100 = 默认强度；0 = 完全关掉高亮\n");
            fwprintf(f, L";   也可以在 Dock 上点右键 → 「悬停高亮」切换开关。\n");
            fwprintf(f, L"\n");
            fwprintf(f, L"[look]\n");
            fwprintf(f, L"glow=100\n");
            fwprintf(f, L"\n");
            fwprintf(f, L"; [dock] edge —— 鼠标压到屏幕最底下多少「逻辑像素」以内，Dock 才升起来\n");
            fwprintf(f, L";\n");
            fwprintf(f, L";   原来是不管鼠标在哪，只要进了 Dock 展开时占的那一整块（约 70 物理像素高）\n");
            fwprintf(f, L";   就冒出来 —— 在别的应用里往下挪鼠标老是被它截住。现在只有真的贴到屏幕\n");
            fwprintf(f, L";   最底下那几行才升。写 0 也能用（鼠标撞到屏幕边缘会停在最后一行上）。\n");
            fwprintf(f, L"\n");
            fwprintf(f, L"; [dock] hidedist —— 鼠标离开 Dock 多远之后，Dock 才降下去（逻辑像素）\n");
            fwprintf(f, L";\n");
            fwprintf(f, L";   原来是「离开 1.2 秒就收」：鼠标还停在 Dock 边上晃，到点也照样缩回去。\n");
            fwprintf(f, L";   改成距离之后语义就对了 —— 你走开了我才收，而且是立刻收。\n");
            fwprintf(f, L"\n");
            fwprintf(f, L"[dock]\n");
            fwprintf(f, L"edge=3\n");
            fwprintf(f, L"hidedist=40\n");
            fclose(f);
        }
    }
    const int g = GetPrivateProfileIntW(L"look", L"glow", 100, ini.c_str());
    ApplyGlow(static_cast<float>(g) / 100.0f);
    Log(L"配置：悬停高亮 glow=%d", g);
    UpdateRiseRules();
}

// [dock] edge / hidedist —— 自动收起的两个阈值。
//
//   edge     = 鼠标压到屏幕最底下几像素以内，Dock 才升起来（默认 3 逻辑像素，
//              175% 缩放下约 5 物理像素）。写 0 也能用：鼠标撞到屏幕边缘会停在
//              最后一行上。调大是为了照顾「鼠标在最后一两像素上抖动」的情况。
//   hidedist = 鼠标离开 Dock 多远才降下去（默认 40 逻辑像素 ≈ 70 物理像素）。
//
// ★ 这两个数都存在 settings.ini 里，就是为了能自己调手感 —— 「多近算贴到底」
//   和「多远算走开」是纯主观的事，写死只会招来回改。
void Dock::UpdateRiseRules() {
    const std::wstring ini = ConfigDir() + L"\\settings.ini";
    edgePx_ = GetPrivateProfileIntW(L"dock", L"edge", 3, ini.c_str());
    riseEnabled_ = GetPrivateProfileIntW(L"dock", L"rise", 1, ini.c_str()) != 0;
    hideDistPx_ = GetPrivateProfileIntW(L"dock", L"hidedist", 40, ini.c_str());
    // ★ 触发带刻意压到 3px：底部有按钮的应用一大堆，触发带一宽
    //   （哪怕 12px）鼠标一靠近就升起、直接挡住那些按钮。
    //   「怼到最边上」是刻意的交互成本 —— 换来应用底部区域的绝对安全。
    if (edgePx_ > 40) edgePx_ = 40;
    if (hideDistPx_ < 4) hideDistPx_ = 4;
    if (hideDistPx_ > 300) hideDistPx_ = 300;
    Log(L"配置：升起的触发线=屏幕底部 %d 逻辑像素内，离开 %d 逻辑像素后收起",
        edgePx_, hideDistPx_);
}

void Dock::SaveSettings() {
    const std::wstring ini = ConfigDir() + L"\\settings.ini";
    wchar_t buf[16] = {};
    swprintf(buf, 16, L"%d", static_cast<int>(glow_ * 100.0f + 0.5f));
    WritePrivateProfileStringW(L"look", L"glow", buf, ini.c_str());
    WritePrivateProfileStringW(L"dock", L"rise", riseEnabled_ ? L"1" : L"0",
                               ini.c_str());
}

void Dock::RequestRender() {
    // 鼠标一动就出一帧，不等定时器（定时器实测最快 15.6ms 一次，等它等于白多一帧延迟）。
    // 这里只推进高亮、然后画；排版不用重算 —— 它是常量。
    const DWORD now = GetTickCount();
    StepGlow(static_cast<double>(now - lastRenderTick_));
    Render();
}

void Dock::RequestAnim() {
    if (animating_) return;
    animating_ = true;
    // 8ms ≈ 125fps。
    //
    // 这条定时器现在只负责「鼠标不在 Dock 上」的那些动画（摊平、收起、弹出）。
    // 鼠标在 Dock 上滑动时走的是 SnapAndRender()，不等定时器 —— 见那里的注释。
    //
    // 关于精度：main 里调了 timeBeginPeriod(1)，但实测光这样还是不够 ——
    // Win10 起 SetTimer 默认参与「定时器合并」，请求 8ms 实际按 15.6ms 的节拍触发
    // （每帧「等待 5.7ms + 绘制 9.9ms」正好卡在 15.6ms 上，帧率被钉在 64）。
    // 显式声明 TIMERV_NO_COALESCING 更严谨，不过在本机上它也只到 64fps。
    SetCoalescableTimer(hwnd_, kTimerAnim, 8, nullptr, TIMERV_NO_COALESCING);
}

void Dock::Tick() {
    // 这条定时器没有可靠的节拍（实测最快也就 15.6ms 一次），所以这里按 16ms
    // 估算着推进高亮。鼠标真在 Dock 上滑动时走的是 RequestRender()，
    // 它用真实帧间隔，不经过这里。
    bool moving = StepGlow(16.0);

    float ds = slideTarget_ - slide_;
    if (std::fabs(ds) > 0.006f) {
        slide_ += ds * 0.25f;
        moving = true;
    } else if (slide_ != slideTarget_) {
        slide_ = slideTarget_;
        moving = true;
    }

    if (moving) {
        Render();
        return;
    }

    // 稳定了，停掉动画定时器省 CPU
    KillTimer(hwnd_, kTimerAnim);
    animating_ = false;
    if (!wantVisible_ && slide_ >= 0.999f && shown_) {
        ShowWindow(hwnd_, SW_HIDE);
        shown_ = false;
        // 已经藏起来了，这时才把高亮归零，下次弹出来是干净的
        mouseX_ = -1;
        std::fill(hl_.begin(), hl_.end(), 0.0f);
        std::fill(hlTarget_.begin(), hlTarget_.end(), 0.0f);
    }
}

void Dock::SetVisible(bool vis) {
    if (wantVisible_ == vis) return;
    wantVisible_ = vis;
    Log(L"Dock %ls", vis ? L"滑出" : L"收起");
    if (vis) {
        CaptureBackdrop();  // 每次叫出来都重拍一次背后的画面
        BuildPanel();       // 重新合成玻璃
    }
    if (vis && !shown_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
    slideTarget_ = vis ? 0.0f : 1.0f;
    RequestAnim();
}

void Dock::CaptureBackdrop() {
    if (!hwnd_ || w_ <= 0 || h_ <= 0) return;

    // 拍的时候窗口不能露在屏幕上，否则会把自己也拍进去
    const bool wasShown = shown_;
    if (wasShown) ShowWindow(hwnd_, SW_HIDE);

    Gdiplus::Bitmap* shot = CaptureScreenArea(winX_, winY_, w_, h_);

    if (wasShown) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    if (!shot) return;

    Gdiplus::Bitmap* blurred = BlurBitmap(shot, 16);
    if (!blurred) return;
    delete glass_;
    glass_ = blurred;
}

void Dock::BarGeometry(float& top, float& bottom, float& height) const {
    bottom = static_cast<float>(h_ - padBottom_ + DockDpi(8));
    height = static_cast<float>(baseSize_ + DockDpi(18));
    top = bottom - height;
}

// 把「玻璃底 + 色调 + 颗粒 + 上沿反光」四层预先合成成整块窗口大小的一张图。
//
// 之所以要预合成：这四层里前两层和最后一层都跟图标放大无关，只有外面那圈胶囊
// 的左右位置会变。以前每帧都把这四层重画一遍，其中"把 1754×241 的模糊背景用
// 双三次重采样画上去"这一句就要十几毫秒 —— 帧率被它一个人卡在 30 以下。
// 现在每帧只做一次 1:1 贴图，那点开销就没了。
void Dock::BuildPanel() {
    delete panel_;
    panel_ = nullptr;
    if (w_ <= 0 || h_ <= 0) return;

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w_, h_, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return;
    }

    const bool light = IsLightTheme();
    float top = 0.0f, bottom = 0.0f, height = 0.0f;
    BarGeometry(top, bottom, height);
    {
        Gdiplus::Graphics g(bmp);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

        if (glass_) {
            g.DrawImage(glass_, Gdiplus::Rect(0, 0, w_, h_),
                        0, 0, glass_->GetWidth(), glass_->GetHeight(), Gdiplus::UnitPixel);
        }

        Gdiplus::SolidBrush tint(light ? Pal(false).dockGlass
                                       : Pal(true).dockGlass);
        g.FillRectangle(&tint, 0, 0, w_, h_);

        if (noise_) {
            Gdiplus::TextureBrush grain(noise_);
            g.FillRectangle(&grain, 0, 0, w_, h_);
        }

        // 垂直渐变跟横向位置无关，所以整幅铺和只铺胶囊等价
        Gdiplus::RectF hi(0.0f, top, static_cast<Gdiplus::REAL>(w_), height * 0.55f);
        Gdiplus::LinearGradientBrush sheen(
            hi,
            light ? Gdiplus::Color(64, 255, 255, 255) : Gdiplus::Color(30, 255, 255, 255),
            Gdiplus::Color(0, 255, 255, 255),
            Gdiplus::LinearGradientModeVertical);
        g.FillRectangle(&sheen, 0, 0, w_, h_);
        g.Flush();
    }
    panel_ = bmp;
}

void Dock::Render() {
    if (!dc_ || !bits_) return;
    const int trace = TraceLevel();
    const double tStart = trace ? NowUs() : 0.0;
    const DWORD frameBegin = GetTickCount();
    memset(bits_, 0, static_cast<size_t>(w_) * static_cast<size_t>(h_) * 4);

    Gdiplus::Graphics g(dc_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    const bool light = IsLightTheme();

    // ---- 背景 ----
    // 玻璃 + 色调 + 颗粒 + 反光已经预合成在 panel_ 里，这里只需要按胶囊的形状贴一次。
    float barBottom = 0.0f, barHeight = 0.0f, barTop = 0.0f;
    BarGeometry(barTop, barBottom, barHeight);

    // 胶囊用开机时定死的几何，不再跟着图标行伸缩 —— 这是「整个 dock 一直在变」的解药。
    Gdiplus::RectF bar(static_cast<Gdiplus::REAL>(barL_) + 0.5f, barTop,
                       static_cast<Gdiplus::REAL>(barW_) - 1.0f, barHeight);
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, bar, static_cast<float>(DockDpi(20)));

    if (panel_) {
        Gdiplus::GraphicsState st = g.Save();
        g.SetClip(&path);
        // 1:1 贴图。这里特意用最近邻 —— 内容和目标尺寸完全一样，
        // 用插值只会让它白白多做一次重采样（那正是之前每帧 19ms 的来源）。
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.DrawImage(panel_, Gdiplus::Rect(0, 0, w_, h_), 0, 0, w_, h_, Gdiplus::UnitPixel);
        g.Restore(st);
    }

    // 外缘细边
    Gdiplus::Pen edge(light ? Gdiplus::Color(80, 110, 110, 125)
                            : Gdiplus::Color(64, 255, 255, 255), 1.0f);
    g.DrawPath(&edge, &path);

    const double tBg = trace ? NowUs() : 0.0;

    // ---- 图标 ----
    // 这里全程都是「缩小」（源图已经预先缩到最大显示尺寸），没有放大。
    // 双三次的价值主要在放大时；纯缩小用高质量双线性（带预滤波）画质几乎一样，
    // 但快得多 —— 换之前这一项独占每帧 8ms。
    g.SetInterpolationMode(InterpMode());
    int n = static_cast<int>(apps_.size());

    // ---- 悬停高亮：图标背后那块底色缓缓亮起 ----
    // 放大换成了高亮（取舍见 dock.h 里那段），关键差别是：高亮一点宽度都不占，
    // 于是图标尺寸和位置都是常量，排版里没有任何逐帧变化的量 —— 抖不动。
    if (glow_ > 0.0f) {
        const float pad = static_cast<float>(DockDpi(5));    // 高亮块比图标大一圈
        const float rad = static_cast<float>(DockDpi(13));   // 圆角
        const float side = static_cast<float>(baseSize_) + pad * 2.0f;
        const float top0 = static_cast<float>(h_ - padBottom_ - baseSize_) - pad;
        for (int i = 0; i < n; ++i) {
            const float a = (i < static_cast<int>(hl_.size())) ? hl_[i] : 0.0f;
            if (a <= 0.003f) continue;
            Gdiplus::GraphicsPath gp;
            AddRoundRect(gp, Gdiplus::RectF(x_[i] - pad, top0, side, side), rad);
            // 深色主题加白、浅色主题加黑，都是「提对比」而不是染色，
            // 免得高亮把图标本身的颜色带偏
            const BYTE al = static_cast<BYTE>(a * 52.0f + 0.5f);
            Gdiplus::SolidBrush br(light ? Gdiplus::Color(al, 16, 18, 26)
                                         : Gdiplus::Color(al, 255, 255, 255));
            g.FillPath(&br, &gp);
        }
    }

    // ---- 图标 ----
    // 尺寸恒定，位置也是整数（padX_ / baseSize_ / gap_ 全是 int），所以这里是
    // 真正的 1:1 贴图，一帧都不用重采样。之前每帧最贵的那一项直接归零。
    //
    // ★ 另外这里刻意不把坐标取整。上一版按整数画：排版算的是小数，落到屏幕上
    //   `(int)(x+0.5)` 各自独立取整，于是相邻图标的实际空隙会在 15 / 16 之间来回翻 ——
    //   空隙每帧真实变化只有 0.1px，取整却带来 1px 的跳，抖动量是真实运动的十倍。
    // ---- 固定区 / 运行区之间的分隔线 ----
    // macOS 的 Dock 上也有一条，位置就在最后一个常驻图标的右边。
    // 只画到图标高度的中间一段（不顶天立地），跟那条细细的竖线更像。
    if (pinnedCount_ > 0 && pinnedCount_ < n && sepW_ > 0) {
        const float sx = x_[pinnedCount_] - static_cast<float>(sepW_) / 2.0f;
        const float base = static_cast<float>(h_ - padBottom_);
        const float s0 = static_cast<float>(baseSize_);
        Gdiplus::Pen pen(light ? Gdiplus::Color(60, 0, 0, 0)
                               : Gdiplus::Color(60, 255, 255, 255), 1.0f);
        g.DrawLine(&pen, sx, base - s0 * 0.80f, sx, base - s0 * 0.20f);
    }

    std::vector<float> dX(static_cast<size_t>(n)), dS(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        dS[static_cast<size_t>(i)] = curSize_[i] > 1.0f ? curSize_[i] : 1.0f;
        dX[static_cast<size_t>(i)] = x_[i];
    }

    for (int i = 0; i < n; ++i) {
        const float s = dS[static_cast<size_t>(i)];
        const float ix = dX[static_cast<size_t>(i)];
        const float iy = static_cast<float>(h_ - padBottom_) - s;

        if (apps_[i].icon) {
            // 保持宽高比，在 s×s 的格子里居中，免得被拉变形（全部走浮点，别取整）
            const float iw = static_cast<float>(apps_[i].icon->GetWidth());
            const float ih = static_cast<float>(apps_[i].icon->GetHeight());
            const float k = (iw > ih) ? (s / iw) : (s / ih);
            const float dw = iw * k;
            const float dh = ih * k;
            g.DrawImage(apps_[i].icon,
                        Gdiplus::RectF(ix + (s - dw) / 2.0f, iy + (s - dh) / 2.0f, dw, dh),
                        0, 0, apps_[i].icon->GetWidth(), apps_[i].icon->GetHeight(),
                        Gdiplus::UnitPixel);
        } else {
            Gdiplus::SolidBrush ph(light ? Gdiplus::Color(255, 200, 200, 208)
                                         : Gdiplus::Color(255, 80, 80, 90));
            g.FillEllipse(&ph, ix + s / 6.0f, iy + s / 6.0f, s * 2.0f / 3.0f, s * 2.0f / 3.0f);
        }

        if (i < static_cast<int>(running_.size()) && running_[i]) {
            const float dot = static_cast<float>(DockDpi(4));
            Gdiplus::SolidBrush dotBrush(Pal(light).dockRunDot);
            g.FillEllipse(&dotBrush, ix + s / 2.0f - dot / 2.0f,
                          static_cast<float>(h_ - padBottom_ + DockDpi(6)), dot, dot);
        }
    }

    // ---- 悬停时在图标上方显示名字 ----
    if (hover_ >= 0 && hover_ < n && !apps_[hover_].name.empty()) {
        const std::wstring& nm = apps_[hover_].name;
        Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(DockDpi(12)),
                           Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::RectF txt = {};
        g.MeasureString(nm.c_str(), static_cast<INT>(nm.size()), &font,
                        Gdiplus::PointF(0.0f, 0.0f), &txt);

        float padTxt = static_cast<float>(DockDpi(9));
        float boxW = txt.Width + padTxt * 2.0f;
        float boxH = txt.Height + static_cast<float>(DockDpi(6));
        float cx = x_[hover_] + curSize_[hover_] / 2.0f;
        float bx = cx - boxW / 2.0f;
        if (bx < 2.0f) bx = 2.0f;
        if (bx + boxW > static_cast<float>(w_) - 2.0f)
            bx = static_cast<float>(w_) - 2.0f - boxW;
        // 贴在图标正上方（图标会随悬停放大，所以用当前尺寸算），别飘到窗口最顶上
        float iconTop = static_cast<float>(h_ - padBottom_) - curSize_[hover_];
        float by = iconTop - boxH - static_cast<float>(DockDpi(4));
        if (by < 2.0f) by = 2.0f;

        Gdiplus::GraphicsPath lp;
        AddRoundRect(lp, Gdiplus::RectF(bx, by, boxW, boxH), static_cast<float>(DockDpi(6)));
        Gdiplus::SolidBrush labelBg(light ? Pal(false).labelBg
                                          : Pal(true).labelBg);
        g.FillPath(&labelBg, &lp);

        Gdiplus::SolidBrush labelFg(light ? Pal(false).labelFg
                                          : Pal(true).labelFg);
        g.DrawString(nm.c_str(), static_cast<INT>(nm.size()), &font,
                     Gdiplus::PointF(bx + padTxt, by + (boxH - txt.Height) / 2.0f), &labelFg);
    }

    const double tIcon = trace ? NowUs() : 0.0;

    // ---- 提交到窗口 ----
    HDC screen = GetDC(nullptr);
    POINT src = {0, 0};
    // 收起时整块内容往下滑出屏幕，滑完再真正隐藏窗口
    int offset = static_cast<int>(slide_ * static_cast<float>(h_ + DockDpi(10)) + 0.5f);
    POINT dst = {winX_, winY_ + offset};
    SIZE size = {w_, h_};
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    if (flush_) DwmFlush();   // 对齐到 DWM 的合成节拍（见 FlushEnabled 的注释）
    // 注意记的是「这一帧开始」的时刻，不是结束的时刻。
    // 记结束时刻的话，限流会变成「上一帧画完之后再等 7ms」，白白多等一截。
    lastRenderTick_ = frameBegin;

    if (trace) {
        const double tEnd = NowUs();
        // ★ 要测的是「出帧周期」= 上一帧开始 → 这一帧开始。
        // 只测「空闲」没意义：如果每帧都忙（绘制 7.7ms + 空闲 1.2ms），
        // 空闲那个数字是 1.2ms，看着很规整，周期却是 8.9ms —— 而 8.9ms 对 165Hz
        // 屏幕来说既不是 1 个刷新周期也不是 2 个，所以照样在抖。
        if (lastRenderStartUs_ > 0.0) {
            const double period = tStart - lastRenderStartUs_;
            statGapUs_ += period;
            if (period < statGapMinUs_) statGapMinUs_ = period;
            if (period > statGapMaxUs_) statGapMaxUs_ = period;
            const int b = period < 6000 ? 0 : period < 8000 ? 1 : period < 10000 ? 2
                        : period < 12000 ? 3 : period < 14000 ? 4 : period < 16000 ? 5
                        : period < 20000 ? 6 : 7;
            ++statGapHist_[b];
        }
        lastRenderStartUs_ = tStart;
        lastRenderEndUs_ = tEnd;
        statTotalUs_ += tEnd - tStart;
        statBgUs_ += tBg - tStart;
        statIconUs_ += tIcon - tBg;
        statUlwUs_ += tEnd - tIcon;
        ++statFrames_;

        if (trace >= 2) {
            int mid = n / 2;
            Log(L"TRACE %d %.2f %.2f %.2f", mouseX_, x_[0], x_[mid], x_[n - 1]);
        }
        if (trace >= 3) {
            // 逐帧记下每个图标「真正画在屏幕上的坐标」（浮点，带一位小数），
            // 用来查细碎抖动：只要相邻帧之间空隙的变化是平滑的，就没有抖动。
            std::wstring line = L"GEO";
            wchar_t tmp[64] = {};
            swprintf(tmp, 64, L" %d", mouseX_);
            line += tmp;
            for (int i = 0; i < n; ++i) {
                swprintf(tmp, 64, L" %.1f,%.1f", dX[static_cast<size_t>(i)],
                         dS[static_cast<size_t>(i)]);
                line += tmp;
            }
            Log(L"%ls", line.c_str());
        }
    }
}

int Dock::HitTest(int x, int y) const {
    for (int i = static_cast<int>(apps_.size()) - 1; i >= 0; --i) {
        int s = static_cast<int>(curSize_[i] + 0.5f);
        int ix = static_cast<int>(x_[i] + 0.5f);
        int iy = h_ - padBottom_ - s;
        if (x >= ix && x < ix + s && y >= iy && y < iy + s) return i;
    }
    return -1;
}

void Dock::RefreshRunning(bool force) {
    DWORD now = GetTickCount();
    if (!force && now - lastRunningCheck_ < 700) return;
    lastRunningCheck_ = now;

    const int trace = TraceLevel();
    const double t0 = trace ? NowUs() : 0.0;

    // ★ 枚举本身在后台线程做完了，这里只拷一份快照出来（十来个字符串）。
    //   以前这一行是 EnumWindows + 逐个 OpenProcess，那段才是卡顿的来源。
    std::set<std::wstring> paths;
    double scanCost = 0.0;
    {
        std::lock_guard<std::mutex> lk(g_runScan.mtx);
        paths = g_runScan.paths;
        scanCost = g_runScan.costMs;
    }
    // 后台线程没起来（创建失败）时退回老办法 —— 慢，但总比运行区空着强
    if (!g_runScan.thread) {
        EnumCtx ctx = {&paths};
        EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));
    }

    // 固定区：真去比对进程列表
    for (int i = 0; i < pinnedCount_ && i < static_cast<int>(apps_.size()); ++i) {
        bool run = false;
        if (!apps_[static_cast<size_t>(i)].target.empty()) {
            run = paths.count(NormalizePath(apps_[static_cast<size_t>(i)].target)) > 0;
        }
        running_[static_cast<size_t>(i)] = run;
    }
    // ★ 运行区一律不画小圆点：那一格出现在分隔线右边，本身就已经说明「它在跑」，
    //   再点一个点是重复表达。
    for (size_t i = static_cast<size_t>(pinnedCount_); i < apps_.size(); ++i) {
        running_[i] = false;
    }

    // ★ 只有运行区真的有人进出才重建整条列表。
    //   稳定期（没人在开关应用）一次都不重排 —— 否则每 700ms 重排一次，
    //   Dock 的宽度和图标位置会一直抖。
    if (UpdateRunningExtras(paths, now)) RebuildList();

    if (trace) {
        // 现在这里记两个时间：
        //   scanCost —— 后台线程枚举的耗时。它不再占 UI 线程，所以再长也不卡界面，
        //              但值得盯着（异常时说明系统里有很难缠的进程）。
        //   dt       —— UI 线程自己花的（拷快照 + 比对），应该永远是零点几毫秒。
        if (scanCost > statRefreshWorstMs_) statRefreshWorstMs_ = scanCost;
        statRefreshTotalMs_ += scanCost;
        ++statRefreshCount_;
        const double dt = (NowUs() - t0) / 1000.0;
        if (dt > 2.0) Log(L"TRACE：UI 线程这一段用了 %.1f ms，不该这么久", dt);
    }
}

// 后台线程枚举完推回来的通知：直接刷一次，不用等 700ms 的定时器。
// 这样「打开一个应用 → Dock 上出现那一格」的延迟从「最多 700ms」变成
// 「线程下一轮醒来就到」（平均 350ms）。
void Dock::OnRunningScanned() {
    RefreshRunning(true);
}

void Dock::StartRunningScan() {
    if (g_runScan.thread) return;
    g_runScan.hwnd = hwnd_;
    g_runScan.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_runScan.stop) {
        Log(L"运行检测：创建退出事件失败，退回同步枚举（每 700ms 一次）");
        return;
    }
    g_runScan.thread = CreateThread(nullptr, 0, RunScanProc, nullptr, 0, nullptr);
    if (!g_runScan.thread) {
        Log(L"运行检测：起不来后台线程，退回同步枚举（每 700ms 一次）");
        CloseHandle(g_runScan.stop);
        g_runScan.stop = nullptr;
    }
}

void Dock::StopRunningScan() {
    // ★ 退出要「先喊停、再等它走完」。
    //
    // 等待必须核实结果：线程最坏情况 = 一轮枚举 + 700ms 睡眠，正常 3 秒足够；
    // 但万一超时，**句柄一个都不能关** —— 关了 stop 事件，线程下一次
    // WaitForSingleObject 就永远等不到退出信号（句柄已无效，WAIT_FAILED），
    // 它就停不下来了；关了 thread 句柄同样不解决任何问题，只是自欺欺人。
    // 这种时候把资源留着，进程退出时系统统一回收。
    if (g_runScan.stop) SetEvent(g_runScan.stop);
    if (g_runScan.thread) {
        const DWORD w = WaitForSingleObject(g_runScan.thread, 3000);
        if (w == WAIT_OBJECT_0) {
            CloseHandle(g_runScan.thread);
            g_runScan.thread = nullptr;
            if (g_runScan.stop) {
                CloseHandle(g_runScan.stop);
                g_runScan.stop = nullptr;
            }
        } else {
            Log(L"运行检测：线程未在 3 秒内退出（等待结果 %lu），资源留给系统回收", w);
        }
    }
    g_runScan.hwnd = nullptr;
}

// 全量扫描完成，UI 线程接手：启动台补齐、Dock 换高清图标
void Dock::OnAppsScanned() {
    // ★ 先把线程收干净（它已经把结果放进 result、PostMessage 完就结束了，
    //   这里等它退出只是走个形式）。不收的话 g_fullScan.thread 一直非空，
    //   StartFullScan 看到「还在扫」就直接返回 —— 「刷新应用列表」点一次之后
    //   就永远不再扫了，这正是以前那个「点了没反应」的新变体。
    if (g_fullScan.thread) {
        WaitForSingleObject(g_fullScan.thread, 3000);
        CloseHandle(g_fullScan.thread);
        g_fullScan.thread = nullptr;
    }

    std::vector<AppItem> all;
    {
        std::lock_guard<std::mutex> lk(g_fullScan.mtx);
        all.swap(g_fullScan.result);
    }
    if (all.empty()) {
        Log(L"全量扫描完成，但没有可用的应用");
        return;
    }
    Log(L"全量扫描完成：%zu 个应用，开始就位", all.size());

    if (!launcherBuilt_) {
        // 启动台第一次拿到全量列表（给一份深拷贝 —— 两边要各自缩放图标位图）
        std::vector<AppItem> copy = DeepCopyAppList(all);
        launcher_.Create(inst_, hwnd_, std::move(copy));
        launcherBuilt_ = true;
    } else {
        // 「刷新应用列表」走这儿：启动台已经有窗口了，只换内容
        std::vector<AppItem> copy = DeepCopyAppList(all);
        launcher_.SetApps(std::move(copy));
    }

    // Dock 这边按清单从全量列表重新挑一遍 —— 这一次能挑到高清 256px 图标。
    // ★ 挑出来的这份要同时更新固定区（pinnedApps_）和显示列表（apps_）：
    //   RebuildList 是从 pinnedApps_ 深拷贝来拼 apps_ 的，只换 apps_ 的话
    //   下一次重建又会退回旧图标。
    std::vector<AppItem> picked = PickFromPinned(all);
    FreeAppIcons(all);
    if (!picked.empty()) {
        FreeAppIcons(pinnedApps_);
        pinnedApps_ = std::move(picked);
        RebuildList();
    }
}

// 2026-10-11 - AI - 刷新「正在跑、但没固定」那一区。返回 true 表示有增删
//
// ★ 顺序靠 vector 的插入顺序天然保证（= 首次出现的顺序），
//   不额外记序号 —— 少一个要同步维护的东西。
bool Dock::UpdateRunningExtras(const std::set<std::wstring>& paths, DWORD now) {
    bool changed = false;

    for (const std::wstring& p : paths) {
        if (p == SelfLowerPath()) continue;      // 自己不给自己留一格
        if (IsSystemComponent(p)) continue;      // 壳组件不算应用

        bool onDock = false;                     // 固定区已经有了，别占两格
        for (const AppItem& a : pinnedApps_) {
            if (!a.target.empty() && NormalizePath(a.target) == p) { onDock = true; break; }
        }
        if (onDock) {
            // 2026-10-11 - AI - 刚被固定到 Dock 的那一个，运行区要立刻撤掉
            //
            // 撤的条件是「进程还在跑」，而它当然还在跑 —— 光靠上面那句 continue，
            // 已有的记录既不会被判超时、也不会被清掉，结果就是固定区和运行区各占一格。
            for (size_t i = 0; i < runEntries_.size();) {
                if (runEntries_[i].path == p) {
                    Log(L"运行区：%ls 已固定到 Dock，撤掉这一格", p.c_str());
                    runEntries_.erase(runEntries_.begin() + static_cast<ptrdiff_t>(i));
                    changed = true;
                } else {
                    ++i;
                }
            }
            continue;
        }

        bool have = false;
        for (RunEntry& e : runEntries_) {
            if (e.path == p) { e.lastSeen = now; have = true; break; }
        }
        if (have) continue;

        if (static_cast<int>(runEntries_.size()) >= kRunMaxItems) continue;
        runEntries_.push_back({p, now});
        Log(L"运行区：新加 %ls", p.c_str());
        changed = true;
    }

    for (size_t i = 0; i < runEntries_.size();) {
        if (paths.count(runEntries_[i].path) == 0 &&
            now - runEntries_[i].lastSeen > kRunGraceMs) {
            Log(L"运行区：移除 %ls", runEntries_[i].path.c_str());
            runEntries_.erase(runEntries_.begin() + static_cast<ptrdiff_t>(i));
            changed = true;
        } else {
            ++i;
        }
    }
    return changed;
}

void Dock::RebuildList() {
    std::vector<AppItem> list = DeepCopyAppList(pinnedApps_);
    pinnedCount_ = static_cast<int>(list.size());
    for (const RunEntry& e : runEntries_) {
        list.push_back(MakeAppItemFromExe(e.path));
    }
    if (!runEntries_.empty()) {
        Log(L"Dock 重建：固定 %d + 运行 %zu 个", pinnedCount_, runEntries_.size());
    }
    ApplyApps(std::move(list));
}

// 从路径里取出「不带扩展名的文件名」，用来做名字对应
std::wstring StemOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\");
    std::wstring s = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const size_t dot = s.find_last_of(L'.');
    if (dot != std::wstring::npos) s = s.substr(0, dot);
    return s;
}

// 兜底：按「显示名」去全量列表里找。
// 路径对不上的情况很常见 —— 比如清单是从别的启动台迁移来的，里面记的是另一处的 .lnk，
// 但用户真正挑中的是「那个应用」。这时候名字才是可靠的身份。
std::vector<AppItem> PickByName(std::vector<AppItem>& all,
                                const std::vector<std::wstring>& names) {
    std::vector<AppItem> out;
    for (const std::wstring& nm : names) {
        if (nm.empty()) continue;
        if (out.size() >= static_cast<size_t>(kMaxIcons)) break;
        for (size_t i = 0; i < all.size(); ++i) {
            if (_wcsicmp(all[i].name.c_str(), nm.c_str()) == 0) {
                out.push_back(std::move(all[i]));
                all.erase(all.begin() + static_cast<ptrdiff_t>(i));
                break;
            }
        }
    }
    return out;
}

std::vector<AppItem> TakeHead(std::vector<AppItem>& all, int count) {
    std::vector<AppItem> out;
    while (static_cast<int>(out.size()) < count && !all.empty()) {
        out.push_back(std::move(all.front()));
        all.erase(all.begin());
    }
    return out;
}

// 从全量列表里挑出 Dock 要显示的那些 —— 顺序严格按 pinned.txt，
// 那是用户在启动台里一个个固定的先后，不该由程序重新排。
std::vector<AppItem> Dock::PickFromPinned(std::vector<AppItem>& all) {
    const std::vector<std::wstring> pinned = LoadPinnedList();
    std::vector<AppItem> out;
    for (const std::wstring& p : pinned) {
        if (p.empty()) continue;
        // 2026-10-11 - AI - 清单开头那两行是给人看的说明，不是应用。
        //   LoadPinnedList 读的时候不认注释，会连它一起读进来 ——
        //   留在这儿只会在日志里留下一句「已经找不到了」。
        if (p[0] == L';') continue;
        if (out.size() >= static_cast<size_t>(kMaxIcons)) break;
        // ★ 内建格子（@launchpad / @trash / @calendar）不是一个文件，
        //   不去全量列表里对路径 —— 直接造一个出来。
        if (IsSpecialLine(p)) {
            out.push_back(MakeSpecialItem(KindFromLine(p)));
            continue;
        }
        bool matched = false;
        // ★ 两边都过一遍 NormalizePath。以前只做大小写不敏感比较（_wcsicmp），
        //   清单里若记着 8.3 短名（C:\PROGRA~1\...）或带 \\?\ 前缀，
        //   跟全量列表里的长路径就对不上 —— 明明是同一个应用，
        //   结果被当成「找不到」，静默跳过。
        const std::wstring key = NormalizePath(p);
        for (size_t i = 0; i < all.size(); ++i) {
            if (NormalizePath(all[i].lnkPath) == key) {
                out.push_back(std::move(all[i]));
                all.erase(all.begin() + static_cast<ptrdiff_t>(i));
                matched = true;
                break;
            }
        }
        // 2026-10-11 - AI - 全量列表里对不上：拿这条路径自己造一格，别默默跳过
        //
        // 典型场景就是在运行区右键 →「添加到 Dock」。写进 pinned.txt 的是 exe 的完整
        // 路径（D:\黑海\clash-verge.exe、Helium 的 chrome.exe 这种），而全量列表只扫
        // 开始菜单 —— 它压根不在里面。以前对不上就跳过，于是「点了没反应」：
        // 清单里悄悄多了一条，Dock 上却永远不出现这一格。
        // .lnk 不在这里造（造出来 target 会是快捷方式本身，跟进程路径对不上），
        // 快捷方式失了效就该消失，不硬留。
        if (!matched) {
            const std::wstring low = NormalizePath(p);
            const bool isLnk = low.size() > 4 && low.compare(low.size() - 4, 4, L".lnk") == 0;
            if (!isLnk && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                out.push_back(MakeAppItemFromExe(p));
                Log(L"固定项 %ls 不在应用列表里，按路径直接造一格", p.c_str());
            } else {
                Log(L"固定项 %ls 已经找不到了，跳过", p.c_str());
            }
        }
    }
    return out;
}

// 换了整个列表之后整套重来：图标个数可能变了，所以尺寸和窗口都要重算。
// 「刷新应用列表」和「用户在启动台里改了固定项」这两条路都走到这里。
void Dock::ApplyApps(std::vector<AppItem>&& list) {
    FreeAppIcons(apps_);
    apps_ = std::move(list);
    const int n = static_cast<int>(apps_.size());
    if (n <= 0) {
        Log(L"应用列表空了，Dock 上暂时不显示任何图标");
        return;
    }

    FitMetrics(n);
    for (auto& a : apps_) a.icon = DownscaleIconToMaxSide(a.icon, baseSize_);

    x_.assign(n, 0.0f);
    curSize_.assign(n, static_cast<float>(baseSize_));
    targetSize_.assign(n, static_cast<float>(baseSize_));
    hl_.assign(n, 0.0f);
    hlTarget_.assign(n, 0.0f);
    running_.assign(n, false);
    hover_ = -1;
    mouseX_ = -1;
    LayoutFromSizes();

    CreateBackBuffer(w_, h_);
    // 尺寸刚变过，位置要跟着重算（同一块屏，不会跳屏）
    ApplyMonitorGeometry();

    CaptureBackdrop();
    BuildPanel();
    RefreshRunning(true);
    Render();
}

void Dock::HideNow() {
    wantVisible_ = false;
    slide_ = 1.0f;
    slideTarget_ = 1.0f;
    mouseX_ = -1;
    mouseInside_ = false;
    hover_ = -1;
    std::fill(hl_.begin(), hl_.end(), 0.0f);
    std::fill(hlTarget_.begin(), hlTarget_.end(), 0.0f);
    if (shown_) {
        ShowWindow(hwnd_, SW_HIDE);
        shown_ = false;
    }
}

void Dock::ReloadApps() {
    std::vector<AppItem> src = DeepCopyAppList(launcher_.AllApps());
    std::vector<AppItem> picked = PickFromPinned(src);
    FreeAppIcons(src);
    // ★ 全量列表还没就绪（刚启动、后台扫描没回来）时 picked 会是空的。
    //   这时候千万别清固定区 —— 不然 Dock 会「有图标 → 空一下 → 再有」，
    //   看着像闪退了一样。保持原样，等 OnAppsScanned 来做真正的替换。
    if (picked.empty() && !pinnedApps_.empty()) {
        Log(L"全量列表还没就绪，固定区保持原样");
        return;
    }
    FreeAppIcons(pinnedApps_);
    pinnedApps_ = std::move(picked);
    // ★ 运行区不在这儿清：正在跑的东西不会因为清单变了就消失，
    //   下次 RebuildList 会把它一起拼回去。
    RebuildList();
    Log(L"Dock 列表已重建：%d 个图标", static_cast<int>(apps_.size()));
}

// 2026-10-11 - AI - 内建格子点下去干什么（它们不是一个应用，没有可启动的文件）

// 废纸篓：开回收站。
// 用 explorer 的 shell: 路径，不要直接去开 C:\$Recycle.Bin —— 那个目录每个盘一个，
// 而且进去看到的是 $I / $R 开头的一堆内部文件名，不是用户认识的那个回收站。
void OpenTrash() {
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                      L"shell:RecycleBinFolder", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        Log(L"废纸篓：打不开回收站（代码 %lld）",
            static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
    }
}

// 日历：Windows 上没有 macOS 那种独立日历应用（系统那个「邮件和日历」已经废弃，
// outlookcal: 协议在这台机器上也没注册），所以打开已装的新版 Outlook 的日历页。
// 协议起不来就退一步只开 Outlook 本体，不要点了没反应。
void OpenCalendar() {
    HINSTANCE r = ShellExecuteW(nullptr, L"open", L"ms-outlook://calendar/",
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        r = ShellExecuteW(nullptr, L"open", L"ms-outlook:", nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        Log(L"日历：Outlook 没起来（代码 %lld）",
            static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
    }
}


void Dock::LaunchOrActivate(int index) {
    if (index < 0 || index >= static_cast<int>(apps_.size())) return;
    const AppItem app = apps_[static_cast<size_t>(index)];

    // ★ 内建格子先各自处理掉。它们没有 target 也没有 lnkPath，
    //   送进 RunAppAction 只会得出「启动不了」。
    switch (app.kind) {
        case AppKind::Launchpad: launcher_.Toggle(); return;
        case AppKind::Trash:     OpenTrash();        return;
        case AppKind::Calendar:  OpenCalendar();     return;
        case AppKind::File:      break;
    }

    // 「已经在跑就切过去」那条判据在 appaction.cpp 里 —— 启动台点格子走的
    // 是同一个函数，两边不会哪天走着走着就不一样了。
    RunAppAction(kAppOpen, app);
}

// 2026-10-11 - AI - 左键点图标：有「别的地方可去」的应用先弹菜单，其余照常直接启动
//
// ★ 判据是「recents.h 给不给得出额外动作」，不是「它叫不叫 VSCode」。
//   Dock 这边不该知道编辑器的存在，以后要给 Trae、Cursor 加同样的东西，
//   只改那一个文件。
void Dock::OnIconClick(int idx, int mx, int my) {
    if (idx < 0 || idx >= static_cast<int>(apps_.size())) return;
    const AppItem app = apps_[static_cast<size_t>(idx)];
    // 内建格子只有一件事可做，没什么可选的
    if (app.kind != AppKind::File) { LaunchOrActivate(idx); return; }
    // ★ 运行区那一格的作用是「切过去」。它已经开着了，再问一遍要去哪反而绕 ——
    //   「最近的项目」那种菜单只给固定区的常驻图标。
    if (idx >= pinnedCount_) { LaunchOrActivate(idx); return; }

    const std::vector<AppExtra> extras = ExtrasForApp(app, 10);
    if (extras.empty()) {
        LaunchOrActivate(idx);
        return;
    }
    ShowExtrasMenu(idx, app, extras, mx, my);
}

void Dock::ShowExtrasMenu(int idx, const AppItem& app,
                          const std::vector<AppExtra>& extras, int mx, int my) {
    std::vector<MenuItem> items;

    MenuItem head;
    head.title = true;
    head.tag = L"应用";
    head.label = app.name;
    items.push_back(std::move(head));

    auto sep = [&items]() {
        MenuItem it;
        it.separator = true;
        items.push_back(std::move(it));
    };
    auto line = [&items](int cmd, const std::wstring& label) {
        MenuItem it;
        it.cmd = cmd;
        it.label = label;
        items.push_back(std::move(it));
    };

    sep();
    // 不带参数起 VSCode，它就按自己的习惯恢复上次那组窗口 —— 这就是「继续上次」
    line(kAppOpen, L"继续上次");

    int lastGroup = -1;
    for (size_t i = 0; i < extras.size(); ++i) {
        if (extras[i].group != lastGroup) {   // 窗口动作一组、最近的项目一组
            if (lastGroup >= 0) sep();
            lastGroup = extras[i].group;
        }
        line(kAppExtraBase + static_cast<int>(i), extras[i].label);
    }

    POINT pt = {mx, my};
    ClientToScreen(hwnd_, &pt);
    menuOpen_ = true;
    const int cmd = menu_.Show(hwnd_, pt.x, pt.y, items);
    menuOpen_ = false;

    // ★ 菜单跑的是它自己的一条消息循环，开着的时候 Dock 照样在收消息。
    //   这段时间里清单要是重建过，这个 idx 就可能已经不指着同一个应用了。
    if (idx < 0 || idx >= static_cast<int>(apps_.size())) return;

    if (cmd == kAppOpen) {
        LaunchOrActivate(idx);
    } else if (cmd >= kAppExtraBase && cmd < kAppExtraBase + kAppExtraMax) {
        RunExtra(apps_[static_cast<size_t>(idx)], extras, cmd - kAppExtraBase);
    }
}

}  // namespace mydock
