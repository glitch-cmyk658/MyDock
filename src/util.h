// 2026-10-09 - AI - 公共工具：DPI 换算、日志、图标转位图
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>

namespace mydock {

// 按系统 DPI 把一个逻辑像素值换算成实际像素
int Dpi(int px);

// 按「指定显示器的 DPI」换算。多显示器缩放不一样时，Dock 得用它所在那块屏的
// DPI，而不是系统 DPI —— 否则 100% 的副屏上会画出 175% 的尺寸。
int DpiAt(int px, int dpi);

// ---------------------------------------------------------------- 应用身份
// 2026-10-11 - AI - 把一条路径收拾成「同一形态」，专门用来判断「是不是同一个程序」
//
// 以前各处是自己 Lower() 一下就比。字符串比较有个要命的特点：
// 两个路径指向同一个文件，字符串却可以完全不一样 ——
//   C:\PROGRA~1\Tencent\Weixin\Weixin.exe   （8.3 短名）
//   c:\program files\tencent\weixin\weixin.exe
//   \\?\C:\Program Files\Tencent\Weixin\Weixin.exe  （扩展前缀）
//   C:\Program Files\Tencent\Weixin\Weixin.exe\     （末尾多一个反斜杠）
// 结果是同一个程序在 Dock 上占两格、点「切换过去」永远切不到。
// 所以集中在这里收拾一遍，之后所有比较都用它的输出。
//
// ★ 为什么不直接用「文件 ID」（卷序列号 + 文件索引）当身份：
//   那样确实最准，能穿过重命名和硬链接。但代价是本末倒置 ——
//   清单里存的是路径，跨机器/重装系统后路径还在、文件 ID 早变了；
//   而且取 ID 要打开文件句柄，对每个进程都来一次，开销比规范化大两个数量级。
//   路径规范化已经能吃掉现实中 99% 的差异，剩下的（符号链接指向同一文件）
//   本来就该当成两个不同入口看待。
std::wstring NormalizePath(const std::wstring& path);

// ---------------------------------------------------------------- 多显示器
// SM_CXSCREEN / SM_CYSCREEN 只对应**主屏**，多显示器时其它屏的位置和尺寸
// 从这儿拿。
struct MonitorInfo {
    RECT  work{};      // 工作区：整个屏幕扣掉任务栏
    RECT  full{};      // 整块屏幕
    int   dpi = 96;    // 这块屏的有效 DPI（96 = 100%，192 = 200%）
    HMONITOR handle = nullptr;
};
// 鼠标（或任意一点）所在的那块屏
MonitorInfo MonitorAt(POINT pt);
// 主屏。顶部状态栏这种「只出现一次」的东西用它。
MonitorInfo PrimaryMonitor();
// 某块屏的 DPI；传 nullptr 时返回系统 DPI
int MonitorDpi(HMONITOR mon);

// 给 GraphicsPath 追加一个圆角矩形。
// Dock 的胶囊、启动台的选中框、菜单底板都要它，挪到这儿来免得三处各抄一份。
void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);

// 写一行诊断日志到 %LOCALAPPDATA%\MyDock\mydock.log
void Log(const wchar_t* fmt, ...);

// 设了环境变量 MYDOCK_TRACE 时为真。
// 各处的 TRACE 日志都用它兜着 —— 平时一次环境变量读取就返回，
// 不会让正式运行的日志被调试信息淹掉。
bool TraceOn();

// 本程序 exe 所在目录（不带结尾反斜杠）
std::wstring ExeDir();

// 读 %APPDATA%\MyDock\settings.ini 里的一个开关：非 0 为真，缺省用 def
bool ConfigFlag(const wchar_t* section, const wchar_t* key, bool def);

// 配置目录 %APPDATA%\MyDock，不存在就创建
std::wstring ConfigDir();

// 由 HICON 造一个 GDI+ 位图；失败返回 nullptr，用完由调用方 delete
Gdiplus::Bitmap* BitmapFromIcon(HICON icon);

// 由 HBITMAP 造 GDI+ 位图（保留 32 位 alpha）；失败返回 nullptr，用完 delete
Gdiplus::Bitmap* BitmapFromHBITMAP(HBITMAP hbmp);

// 裁掉四周完全透明的边距，让图标里的图案真正填满画布。
// 有些图标（尤其是 .url / .ico）是「大画布 + 中间小图案」，不裁的话画出来会显得很小。
// 成功时返回新的位图并接管传入的那张（调用方不要再 delete 原指针）；无需裁剪时原样返回。
Gdiplus::Bitmap* TrimTransparent(Gdiplus::Bitmap* src);

// 把图标预先缩到「显示时可能用到的最大边长」。
//
// 为什么需要这一步：图标原图是 256×256，而画到屏幕上最大也就 120 px 上下，
// 每帧都从 256 的原图重采样一次，16 个图标就是十几毫秒 —— 帧率直接被卡在 30 以下。
// 先花一次代价缩到最大显示尺寸，之后每帧的采样面积只有原来的四分之一左右，
// 视觉上几乎看不出差别（缩两次的损失很小），但每帧省下十几毫秒。
// 会接管并释放传入的位图；不需要缩小时原样返回。
Gdiplus::Bitmap* DownscaleIconToMaxSide(Gdiplus::Bitmap* src, int maxSide);

// 抓取屏幕上一块矩形（物理坐标）
Gdiplus::Bitmap* CaptureScreenArea(int x, int y, int w, int h);

// 把位图模糊：先降到 1/factor 再放大回去，等价于低通滤波。
// 会接管并释放传入的位图，返回新的；失败时返回 nullptr。
Gdiplus::Bitmap* BlurBitmap(Gdiplus::Bitmap* src, int factor);

// 生成一张很淡的噪点纹理，给玻璃材质加一点颗粒感（返回的位图由调用方 delete）
Gdiplus::Bitmap* MakeNoiseTexture(int size);

// 找出「哪个窗口属于这个 exe」，找不到返回 nullptr。
// Dock 点图标、启动台点格子都得先问一句「它是不是已经开着」，共用一个。
// 只认得看见的窗口。
HWND FindProcessWindow(const std::wstring& exePath);

// 2026-10-11 - AI - 找进程主窗口（含缩托盘的）。
// titleHint：应用显示名（如「微信」）。标题包含它的候选按主窗口优先 ——
// 主窗口的标题就是应用名，托盘/辅助窗口不是。找错窗口的代价实测过：
// 显出一块黑屏或点不动的死壳。
HWND FindProcessWindowAny(const std::wstring& exePath,
                          const std::wstring& titleHint = L"");

// 2026-10-11 - AI - 先找看得见的，找不到再回头接受藏起来的窗口
//
// 微信点了关闭之后是缩进托盘，主窗口被 ShowWindow(SW_HIDE) 藏掉，进程还活着。
// 只看得见的窗口就会得出「它没在运行」，于是再 ShellExecute 一次 —— 微信把这次
// 当成一个新的登录，结果就是每次从 Dock 打开都要重新扫码。
// 所以先按老规矩找可见的，找不到再捞隐藏的（并且先 ShowWindow 把它显出来）。

// 2026-10-11 晚 - AI - 把缩托盘/最小化的窗口恢复到前台并**真正激活**。
// 网上取证（Stack Overflow "SetForegroundWindow unreliable" 高票 + AHK 社区微信帖）
// 后的三条修正：
//   ① AttachThreadInput 接的是「当前前台窗口的线程」（Hans Passant），不只目标线程；
//   ② SetForegroundWindow 之后必须补 SetActiveWindow + SetFocus —— 缺了这两个，
//      窗口有名分但应用内部还是 inactive：单击被当激活点击吃掉、双击反而有效
//      （正是用户在微信 4.x 上实测的症状）；
//   ③ 状态切换走 WM_SYSCOMMAND（应用自己处理 SC_MINIMIZE/SC_RESTORE 并更新
//      激活状态），比 ShowWindow 更贴近用户手动点最小化按钮的路径。
// exePath 用于判断是否微信/Weixin：只有它有「打开微信」热键可借
bool RestoreAndActivate(HWND w, const std::wstring& name,
                        const std::wstring& exePath = L"");
// 上面的后台线程版：立刻返回，恢复/激活序列（含最多 1.5s 的热键等待）
// 全在 detached 线程里跑，Dock 消息循环一毫秒都不停。
// 结果不再返回给调用方（两个调用方都不看返回值），成败只在日志里。
// 线程安全依赖：Log 自带锁；线程里只碰目标窗口的句柄和 SendInput。
void RestoreAndActivateAsync(HWND w, const std::wstring& name,
                             const std::wstring& exePath = L"");

// 这个 exe 有没有进程在跑（不管它有没有窗口）。
// 用来兜住最后一种情况：进程还在、但一个窗口都找不到 ——
// 这时候再启动一次同样会让微信重新登录，宁可什么都不做。
bool IsProcessRunning(const std::wstring& exePath);

// 2026-10-11 - AI - 开机自启（HKCU Run 键）与「重启自己」
// 托盘菜单和顶栏 ⏏ 菜单共用，所以放公共区。
bool IsAutoStart();
bool SetAutoStart(bool on);
// 安排 1 秒后由系统重新拉起自己（等本进程退出、互斥体释放），
// 返回后调用方自己 DestroyWindow 即可。
bool QueueRestart();

}
