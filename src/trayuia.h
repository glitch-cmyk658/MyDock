// 2026-10-12 - AI - 托盘溢出区的真实枚举与触发（UIA 方案）
//
// 原理：这台机器的任务栏被整体挪到屏幕外藏起来（y≈1682 > 屏高 1600），
// 但把它 SW_SHOW 后 **UIA 树照样成型** —— 挪回屏幕外也能枚举/触发，
// 用户全程看不见任何闪动。托盘图标以 Button 形式暴露：
//   • 主任务栏 bridge 子窗口里有「显示隐藏的图标」（^ 溢出开关）
//   • Invoke 它 → TopLevelWindowForOverflowXamlIsland 出现，
//     里面的 Button 就是溢出区里那些「平常看不见」的托盘应用
//   • 对按钮 Invoke = 触发该应用**自己的托盘点击行为**（GHelper 弹面板、
//     Flow Launcher 弹搜索框……），比「激活/拉起」的近似精确得多
//
// 名字匹配：按钮名可能是多行状态（G-Helper 带温度），展示与匹配都用
// 第一行；触发时温度变了首行对不上，退而比对首 token。
#pragma once
#include <string>
#include <vector>

namespace mydock {

// 缓存的托盘溢出区名单（按钮名第一行）。线程安全。
std::vector<std::wstring> TrayCachedNames();

// 缓存是否有效（没有任何一次成功枚举时为 false）
bool TrayCacheReady();

// 缓存年龄（毫秒）；从未成功过返回最大值
unsigned long long TrayCacheAgeMs();

// 后台线程重走舞步刷新缓存（~2.5s，屏外进行用户不可见）。重入安全。
void TrayRefreshAsync();

// 按名字触发一次真实托盘点击（后台线程重走舞步 + UIA Invoke）。重入安全。
void TrayInvokeByNameAsync(const std::wstring& firstNameLine);

}  // namespace mydock
