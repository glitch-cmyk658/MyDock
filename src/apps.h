// 2026-10-09 - AI - 枚举开始菜单里的应用，并取它们的大图标
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>
#include <vector>

namespace mydock {

// 2026-10-11 - AI - Dock 上除了普通应用，还能放几个「不是一个文件」的格子
//
// macOS 的 Dock 上 Finder 和废纸篓是常驻的，Windows 上没有对应的 .lnk 可指 ——
// 回收站是 shell:RecycleBinFolder，启动台是我们自己画的全屏窗口。
// 所以清单里除了路径还得能写这种「内建项」，用 @ 开头跟文件路径区分开。
enum class AppKind {
    File,       // 普通应用：一个 .lnk / .exe
    Launchpad,  // 启动台：Dock 上点它跟按 F4 一样
    Trash,      // 废纸篓：打开回收站
    Calendar,   // 日历：打开 Outlook 的日历页
};

struct AppItem {
    std::wstring name;     // 显示名（.lnk 文件名去掉扩展名）
    std::wstring target;   // 解析出的目标 exe；UWP 之类取不到时为空
    std::wstring lnkPath;  // 原始 .lnk 路径（启动时优先用它，兼容 UWP）
    std::wstring workDir;  // .lnk 里记的「起始位置」。启动时必须传给 ShellExecute，
                           // 不传它就继承了 MyDock 自己的工作目录 —— 有些程序
                           // （微信就是）会因为工作目录不对当成全新的一次启动。
    AppKind kind = AppKind::File;  // 普通应用还是上面那几个内建格子
    int iconIndex = 0;     // 系统图标列表里的索引
    // 已加载的图标位图，可能为空。
    // 原图多是 256×256，而屏幕上只画 baseSize_ 那么大，所以建 Dock 时就先缩好 ——
    // 这一版图标尺寸恒定（取消了放大），缩一次之后每帧都是 1:1 贴，零重采样。
    Gdiplus::Bitmap* icon = nullptr;
};

// 扫描公共 + 用户两个开始菜单目录，去掉卸载/帮助之类的项，按名字排序去重
std::vector<AppItem> EnumerateStartMenuApps();

// 读 MyDockFinder 启动台的 Pinned 目录 —— 那是用户自己挑过的清单。
// 目录不存在时返回空，由调用方回退到 EnumerateStartMenuApps()。
std::vector<AppItem> LoadPinnedApps();

// 用 IShellItemImageFactory 取 256x256 高清图标（对 .lnk / .url / .exe 都有效）
Gdiplus::Bitmap* LoadIconFromShellItem(const std::wstring& path);

// 释放列表里所有图标
void FreeAppIcons(std::vector<AppItem>& apps);

// 深拷贝一份应用列表（连同图标位图）。
// Dock 和启动台要显示同一批应用，但画到屏幕上的尺寸不一样（Dock 48、启动台 64），
// 而两边都会把原图就地缩到各自那份尺寸并释放原图 —— 所以必须各持一份独立副本。
std::vector<AppItem> DeepCopyAppList(const std::vector<AppItem>& src);

// ---- 「用户自己挑过」的清单 ----
// 存在 %APPDATA%\MyDock\pinned.txt 里，每行一个 .lnk 路径。
// Dock 上出现哪些应用由它决定；增删固定项在启动台里右键完成。
std::vector<std::wstring> LoadPinnedList();
bool SavePinnedList(const std::vector<std::wstring>& paths);
bool IsPinned(const std::vector<std::wstring>& pinned, const std::wstring& lnkPath);
// 2026-10-11 - AI - 固定项排序：把清单里的某一行往前/往后挪一格
// key 是 pinned.txt 里那一行的原文（普通路径或 @launchpad 这类内建行）。
// dir = -1 往前（左移），+1 往后（右移）；已在边界时返回 false 什么都不动。
bool MovePinned(const std::wstring& key, int dir);

// ---- 「从启动台拿掉的应用」 ----
// 存在 %APPDATA%\MyDock\hidden.txt 里，格式同上。
// ★ 与 pinned.txt 是两件事：pinned 管「Dock 上显示谁」，hidden 管「启动台里别出现谁」。
//   移出启动台只写 hidden.txt —— 如果顺手也改了 pinned，用户会发现 Dock 上的图标
//   也跟着没了，那就不是「移出启动台」而是「卸载」了。
std::vector<std::wstring> LoadHiddenList();
bool SaveHiddenList(const std::vector<std::wstring>& paths);
bool IsHidden(const std::vector<std::wstring>& hidden, const std::wstring& lnkPath);

// 从系统大图标列表取一个图标（SHIL_JUMBO，通常是 256x256）
Gdiplus::Bitmap* LoadSystemIcon(int index);

// ---- 「用户手动加进启动台的应用」 ----
// 存在 %APPDATA%\MyDock\added.txt，每行一个 .exe / .lnk 路径。
// 开始菜单扫不到的东西（绿色软件、某个深处的 .exe、自己做的快捷方式）走这条路，
// 入口是启动台左上角那个「+」。
// 跟上面两份清单分清：pinned = 上不上 Dock，hidden = 出不出启动台，
//                     added  = 启动台里本来没有、用户自己请进来的。
std::vector<std::wstring> LoadAddedList();
bool SaveAddedList(const std::vector<std::wstring>& paths);
bool AddAppPath(const std::wstring& path);          // 追加一条，重复则不写
AppItem MakeAppFromFile(const std::wstring& path);  // 名字取文件名去扩展名

// ---- 内建格子（2026-10-11）----
// 清单里写成 @launchpad / @trash / @calendar，跟文件路径一眼分得开。
// 加新的内建项要动三处：这个枚举、SpecialLine 的写法表、MakeSpecialItem 的画法。
std::wstring SpecialLine(AppKind k);              // kind -> 清单里那一行
AppKind     KindFromLine(const std::wstring& line);  // 反解；认不出来当普通应用
bool        IsSpecialLine(const std::wstring& line);
AppItem     MakeSpecialItem(AppKind k);           // 生成条目，含自绘图标

}
