// 2026-10-13 - AI - Dock 实现细节常量：dock.cpp 与 dock_menu.cpp 共用。
// 只给这两个 .cpp include —— 不是公共接口，别放进 dock.h。
#pragma once
#include <windows.h>

namespace mydock {

// 轮询 / 动画定时器编号（SetTimer 的 id）
inline const UINT_PTR kTimerPoll = 1;   // 轮询鼠标，判断该显示还是收起
inline const UINT_PTR kTimerAnim = 2;   // 动画帧，只在需要动的时候开

// 打开/关闭启动台的热键编号（RegisterHotKey 用）
inline constexpr int kHotkeyLaunchpad = 1;
inline const DWORD kFrameMinMs = 7;   // 两帧最小间隔（约 140fps 上限）

// Dock 自己的 WM_APP 消息：后台线程 → UI、托盘图标 → 窗口
inline const UINT kMsgRunningScanned = WM_APP + 41; // 后台线程枚举完推回来的通知
inline const UINT kMsgTray          = WM_APP + 43;   // 托盘图标回传的鼠标消息
inline const UINT kMsgShowNow       = WM_APP + 45;   // 「显示 Dock」的外部请求
inline const UINT kMsgAppsScanned   = WM_APP + 42;   // 全量扫描完成的通知

// 以下定义都在 dock.cpp（原匿名 ns，拆分后转外部链接）
extern int g_dockDpi;                   // 当前 DPI（UpdateMonitor 按屏刷新）
int  DockDpi(int px);                   // 按当前 DPI 换算
bool FullscreenAppOnTop();              // 前台是否全屏应用
int  TraceLevel();                      // MYDOCK_TRACE 的级别
void WaitFullScan();                    // 退出时等全量扫描收尾
HICON MakeTrayIcon();                   // 运行时画托盘图标

// 全量扫描启动器。StartFullScan 本体在 dock.cpp 的匿名 ns 里（内部链接），
// 这里暴露的是 dock.cpp 提供的包装器。
void DockStartFullScan(HWND hwnd);

}  // namespace mydock
