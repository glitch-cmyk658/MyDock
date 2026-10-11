// 2026-10-11 - AI - 应用「打开」之外的额外动作：新建窗口、最近打开的项目（目前只有 VSCode 家族）
#pragma once

#include <string>
#include <vector>

#include "apps.h"

namespace mydock {

// 这个应用在「打开」之外，还能直接一步跳到哪儿。
struct AppExtra {
    std::wstring label;   // 菜单上显示什么
    std::wstring args;    // 传给 exe 的命令行参数（不含 exe 自己）
    int group = 0;        // 分组；调用方在 group 变化的地方插一条分隔线
};

// 有就返回至少一项，没有返回空。
//
// ★ 调用方只认「空 / 非空」，不认应用名字：
//     Dock 那边不该知道 VSCode 的存在，它只需要知道「这个应用有没有别的地方可去，
//     有的话左键就弹个菜单让用户挑，没有就照常直接启动」。
//     以后要支持 Trae、Cursor，只改这个文件，Dock 一个字都不用动。
//
// ★ 为什么不用 VSCode 自己的「欢迎页」那条路：用户的 settings.json 里
//   workbench.startupEditor 是 none，欢迎页被关掉了，「新建窗口」出来是个空白
//   编辑页，反而比 Ctrl+R 还慢一圈。
std::vector<AppExtra> ExtrasForApp(const AppItem& app, int maxRecents);

// 执行第 i 个额外动作。i 越界返回 false。
//
// ★ 这里起的是 exe 不是 .lnk —— 快捷方式不收命令行参数，
//   ShellExecute(lnk, "--new-window") 传进去的东西会被当成要打开的文件名。
bool RunExtra(const AppItem& app, const std::vector<AppExtra>& extras, int i);

}
