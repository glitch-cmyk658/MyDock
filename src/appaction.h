// 2026-10-10 - AI - 「对一个应用能做什么」只有这一份定义：Dock 与启动台共用
#pragma once

#include <string>
#include <vector>

#include "apps.h"
#include "menu.h"

namespace mydock {

// 「对某个应用能做什么」的命令表。
//
// ★ 为什么要单独抽出来：Dock 的右键菜单和启动台的右键菜单是两套代码
//   （一个开分层窗口、一个画在全屏画布上），但它们要干的事是同一件 ——
//   打开、在文件夹里找到它、决定上不上 Dock。以前两边各写一份，
//   加个功能就得改两处，很容易长歪（启动台有「添加到 Dock」，Dock 上反而没有
//   任何针对应用的项）。
//
// ★ 值从 101 起：调用方自己还有一批局部命令（刷新列表、退出 MyDock、
//   关闭启动台……），1..99 留给它们，两边永远不会撞号。
enum AppCmd {
    kAppOpen = 101,               // 打开；已经在跑就切到前台
    kAppReveal = 102,             // 打开它所在的文件夹并选中它
    kAppPinToDock = 103,          // 加到 Dock（写 pinned.txt）
    kAppUnpinFromDock = 104,      // 从 Dock 移除（写 pinned.txt）
    kAppHide = 105,               // 从启动台拿掉（写 hidden.txt，必要时也清 added.txt）
};

// 「额外动作」那一段的命令号（新建窗口、最近打开的项目……），定义在 recents.h。
//
// ★ 紧跟在上面这批后面，中间别留空：调用方用一段连续区间把它们一起挑出来
//   （先判是不是 extra，不是再交给 IsAppCmd），中间隔一截就会互相串。
constexpr int kAppExtraBase = 106;
constexpr int kAppExtraMax = 20;

// 这个应用现在待在哪儿 —— 决定菜单里写「添加到 Dock」还是「从 Dock 移除」。
struct AppPlaces {
    bool onDock = false;        // 在 pinned.txt 里
    bool onLaunchpad = false;   // 这一格是从启动台弹出来的
    bool userAdded = false;     // 它是用户自己「+」进来的一条（在 added.txt 里）
};

// 执行结果。调用方按这几面旗子决定接下来要重建什么。
// ★ 刻意不做成「直接调回调」：Dock 改完清单要重建整个图标列表，
//   启动台改完要重排网格，两者动作差得远，塞进 appaction 反而更乱。
struct AppOutcome {
    bool handled = false;          // 这条 cmd 归我管，调用方别再往下分发
    bool launched = false;         // 启动了 / 切到前台了（启动台据此关掉自己）
    bool pinnedChanged = false;    // pinned.txt 变了 → 调用方重建 Dock 的列表
    bool hiddenChanged = false;    // 启动台里少了一条 → 调用方重排网格
    bool needsReload = false;      // 清单变了、但这次不想立刻生效（留给下次刷新）
};

// 这条 cmd 是不是「应用操作」那一组
bool IsAppCmd(int cmd);

// 这一个应用在不在 pinned.txt 里（Dock 上有没有它）
bool AppIsPinned(const AppItem& app);

// 它是不是「用户自己 + 进来的」那一条（在 added.txt 里）
bool AppIsUserAdded(const AppItem& app);

// 「打开所在文件夹」该定位到哪个文件。
// 优先 .lnk 指向的真实 exe —— 用户想看的是「这程序装在哪」，
// 开始菜单那个快捷方式目录对他没什么意义。取不到再退回 .lnk 自己。
// 两个都不存在就返回空串，菜单上那一项直接不出现。
std::wstring RevealTargetPath(const AppItem& app);

// 往菜单里追加「应用操作」那一段（不含标题行，也不含前后的分隔线）
void AppendAppActions(std::vector<MenuItem>& out, const AppItem& app,
                      const AppPlaces& places);

// 执行一条应用操作。跟界面无关的部分全在这里：开文件夹、写清单、启动应用。
AppOutcome RunAppAction(int cmd, const AppItem& app);

// 「打开所在文件夹并选中它」——单独拎出来，别处也能用
void RevealInExplorer(const std::wstring& path);

}  // namespace mydock
