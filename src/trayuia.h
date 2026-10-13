// 2026-10-13 - AI - 托盘溢出区的真实枚举与真实点击（UIA 定位 + SendInput 点击）
//
// 背景：任务栏被挪到屏幕外藏起来（y≈1682 > 屏高 1600），托盘图标只能
// 借任务栏的 XAML 树摸到。上版用 UIA Invoke 触发——实测对一半应用无效
//（任务管理器/Clash/搜狗点了没反应，Invoke 虽返回 S_OK 但应用不理睬）。
//
// 本版：**替用户真实点击**。任务栏临时亮在真实位置（屏幕底部，~2s），
// UIA 找到目标按钮的屏幕矩形，SendInput 派发一次真实的鼠标按下/抬起
// —— 与用户亲手点托盘完全等价，任何应用都无法区分。
// 点击完成后任务栏挪回屏幕外藏好，光标原路返回。
//
// 菜单数据源：注册表 NotifyIconSettings（tooltip + exe 路径，名字稳定，
// 图标从 exe 提取）。UIA 只在点击瞬间用来定位按钮矩形。
#pragma once
#include <string>
#include <vector>

namespace mydock {

// 托盘溢出区的一项：tooltip 名 + exe 路径（图标提取用）。线程安全。
struct TrayEntry {
    std::wstring name;
    std::wstring exePath;
};

// 在跑的托盘应用清单（注册表 NotifyIconSettings × 运行中过滤 × 去重）。
// 同步调用，内部一次进程快照，耗时毫秒级。
std::vector<TrayEntry> TrayCachedEntries();

// 按名字触发一次真实托盘点击（后台线程调用，~2.5s）。
// name = 菜单里展示的名字（= 注册表 tooltip）。重入安全。
void TrayClickByNameAsync(const std::wstring& name);

}  // namespace mydock
