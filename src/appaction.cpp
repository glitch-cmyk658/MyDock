// 2026-10-10 - AI - 应用操作的实现：开文件夹、上下 Dock、移出启动台、启动/切窗口
#include "appaction.h"

#include <shellapi.h>
#include <windows.h>

#include <algorithm>

#include "util.h"

namespace mydock {
namespace {

// 在一份「每行一个路径」的清单里找有没有它（大小写不敏感，Windows 路径就这样）
bool ListHas(const std::vector<std::wstring>& list, const std::wstring& path) {
    if (path.empty()) return false;
    for (const std::wstring& p : list) {
        if (_wcsicmp(p.c_str(), path.c_str()) == 0) return true;
    }
    return false;
}

// 一个文件是不是真的还在
bool Exists(const std::wstring& p) {
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// 往 pinned.txt 里加一条 / 去掉一条。
// ★ Dock 和启动台改的是同一份清单，所以这件事只能有一份实现 ——
//   两边各写一遍的话，哪天格式或排序规则一改，就会有一边漏掉。
bool SetPin(const AppItem& app, bool want) {
    if (app.lnkPath.empty()) return false;
    std::vector<std::wstring> pinned = LoadPinnedList();

    bool has = false;
    for (size_t i = 0; i < pinned.size(); ++i) {
        if (_wcsicmp(pinned[i].c_str(), app.lnkPath.c_str()) == 0) {
            has = true;
            if (!want) pinned.erase(pinned.begin() + static_cast<ptrdiff_t>(i));
            break;
        }
    }
    if (want && !has) pinned.push_back(app.lnkPath);
    if (want == has) return false;                 // 本来就是这状态，不用动
    if (!SavePinnedList(pinned)) return false;

    Log(L"应用操作：%ls %ls Dock", app.name.c_str(), want ? L"已加到" : L"已从");
    return true;
}

// 把一条从启动台拿掉。
// ★ 两件事都要做：写 hidden.txt（下次启动别再放进来），
//   以及——如果它是用户自己「+」进来的——把 added.txt 里那条也清掉。
//   只写 hidden 的话，added.txt 还在，下次启动 MergeAddedApps() 又会把它塞回去，
//   用户会看到「删了又回来」。
bool HideFromLaunchpad(const AppItem& app) {
    if (app.lnkPath.empty()) return false;
    bool changed = false;

    std::vector<std::wstring> hidden = LoadHiddenList();
    if (!ListHas(hidden, app.lnkPath)) {
        hidden.push_back(app.lnkPath);
        changed = SaveHiddenList(hidden) || changed;
    }

    std::vector<std::wstring> added = LoadAddedList();
    if (ListHas(added, app.lnkPath)) {
        added.erase(std::remove_if(added.begin(), added.end(),
                                   [&](const std::wstring& p) {
                                       return _wcsicmp(p.c_str(), app.lnkPath.c_str()) == 0;
                                   }),
                    added.end());
        changed = SaveAddedList(added) || changed;
    }

    if (changed) Log(L"应用操作：%ls 已移出启动台", app.name.c_str());
    return changed;
}

}  // namespace

bool IsAppCmd(int cmd) { return cmd >= kAppOpen && cmd <= kAppHide; }

bool AppIsPinned(const AppItem& app) {
    return IsPinned(LoadPinnedList(), app.lnkPath);
}

bool AppIsUserAdded(const AppItem& app) {
    return ListHas(LoadAddedList(), app.lnkPath);
}

std::wstring RevealTargetPath(const AppItem& app) {
    if (Exists(app.target)) return app.target;
    if (Exists(app.lnkPath)) return app.lnkPath;
    return std::wstring();
}

void RevealInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    // ★ 必须是 /select,"完整路径" 这一种写法：
    //   路径里有空格（Program Files）时不加引号，explorer 只认前半截，
    //   然后会去打开一个不存在的目录 —— 表现是「点了没反应」。
    const std::wstring args = L"/select,\"" + path + L"\"";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"explorer.exe",
                                      args.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        Log(L"应用操作：打不开所在文件夹 %ls（代码 %lld）", path.c_str(),
            static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
    } else {
        // 成功也记一笔：这一项点的"没反应"和"真打开了但窗口在别的显示器上"
        // 从屏幕上看一模一样，日志里有个痕迹才好排查。
        Log(L"应用操作：已在文件夹中定位 %ls", path.c_str());
    }
}

void AppendAppActions(std::vector<MenuItem>& out, const AppItem& app,
                      const AppPlaces& places) {
    auto line = [&out](int cmd, const wchar_t* label) {
        MenuItem it;
        it.cmd = cmd;
        it.label = label;
        out.push_back(std::move(it));
    };

    line(kAppOpen, L"打开");

    // 找不到文件就别摆这一项 —— 点了没反应比没这一项更让人摸不着头脑
    if (!RevealTargetPath(app).empty()) line(kAppReveal, L"打开所在文件夹");

    // Dock 和启动台共用：已经在 Dock 上就只剩「移除」，否则是「添加」。
    if (places.onDock) line(kAppUnpinFromDock, L"从 Dock 移除");
    else               line(kAppPinToDock,   L"添加到 Dock");

    if (places.onLaunchpad) {
        line(kAppHide, places.userAdded ? L"从启动台删除" : L"移出启动台");
    }
}

AppOutcome RunAppAction(int cmd, const AppItem& app) {
    AppOutcome r;
    if (!IsAppCmd(cmd)) return r;
    r.handled = true;

    switch (cmd) {
        case kAppOpen: {
            // 已经开着就切过去，不要重复启动。
            // ★ 这条判据以前在 Dock 和启动台各抄了一份，一模一样 ——
            //   干脆收到这里，只有一处需要维护。
            // ★ 2026-10-11：光找「看得见的窗口」不够。
            //   微信点了关闭是缩进托盘，主窗口被藏起来、进程还活着 ——
            //   只找可见窗口会得出「它没在运行」，于是再启动一次，微信把这次当成
            //   新的一次登录，结果每次从 Dock 打开都要重新扫码。
            if (!app.target.empty()) {
                HWND found = FindProcessWindowAny(app.target, app.name);
                if (found) {
                    // 诊断：这次切的是哪个窗口。多窗口进程（微信）「找错窗口」
                    // 是恢复失败的头号嫌疑 —— 记下类名/标题/矩形，再出事日志能对出找没找错。
                    wchar_t cls[64] = {}, ttl[128] = {};
                    GetClassNameW(found, cls, 64);
                    GetWindowTextW(found, ttl, 128);
                    RECT rr = {};
                    GetWindowRect(found, &rr);
                    Log(L"应用操作：%ls 切换目标 hwnd=%p class=%ls title=%ls rect=(%ld,%ld,%ld,%ld) 可见=%d",
                        app.name.c_str(), found, cls, ttl,
                        static_cast<long>(rr.left), static_cast<long>(rr.top),
                        static_cast<long>(rr.right), static_cast<long>(rr.bottom),
                        static_cast<int>(IsWindowVisible(found)));
                    // ★ 恢复+激活的完整序列收拢到 util 的 RestoreAndActivate
                    //   （Dock/启动台共用一份）：WM_SYSCOMMAND 状态切换 +
                    //   ALT 豁免 + attach 前台线程 + SetActiveWindow/SetFocus。
                    //   以前缺 SetActiveWindow/SetFocus，微信窗口有前台名分
                    //   但内部还是 inactive —— 单击全被吞、双击反而有效。
                    RestoreAndActivate(found, app.name, app.target);
                    r.launched = true;
                    return r;
                }
                // ★ 最后一道：进程还在，但一个窗口都找不到。
                //   这时候再 ShellExecute 一次同样会让它重新登录，所以什么都不做。
                if (IsProcessRunning(app.target)) {
                    Log(L"应用操作：%ls 的进程还在但没有窗口（多半缩在托盘里），不再重复启动",
                        app.name.c_str());
                    r.launched = true;
                    return r;
                }
            }
            const std::wstring what = app.lnkPath.empty() ? app.target : app.lnkPath;
            if (what.empty()) return r;
            // ★ lpDirectory 必须给。传 NULL 时新进程继承的是 MyDock 自己的工作目录，
            //   微信这一类程序会因为工作目录不对认不出已经登录过的自己。
            const wchar_t* dir = app.workDir.empty() ? nullptr : app.workDir.c_str();
            const HINSTANCE h = ShellExecuteW(nullptr, L"open", what.c_str(),
                                              nullptr, dir, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(h) <= 32) {
                Log(L"应用操作：启动失败 %ls（代码 %lld）", what.c_str(),
                    static_cast<long long>(reinterpret_cast<INT_PTR>(h)));
                r.handled = true;
                return r;   // 起不来就别把启动台关了，让用户看见它还开着
            }
            r.launched = true;
            return r;
        }

        case kAppReveal: {
            const std::wstring p = RevealTargetPath(app);
            if (!p.empty()) RevealInExplorer(p);
            return r;
        }

        case kAppPinToDock:
            r.pinnedChanged = SetPin(app, true);
            return r;

        case kAppUnpinFromDock:
            r.pinnedChanged = SetPin(app, false);
            return r;

        case kAppHide:
            r.hiddenChanged = HideFromLaunchpad(app);
            return r;

        default:
            r.handled = false;
            return r;
    }
}

}  // namespace mydock
