// 2026-10-13 - AI - 托盘溢出区 UIA 方案实现（见 trayuia.h 头注）
#include "trayuia.h"

#include "util.h"   // ScheduleBackground / Log / NormalizePath

#include <uiautomationclient.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <tlhelp32.h>
#include <utility>

namespace mydock {
namespace {

// ---- 注册表登记处（菜单数据源） ----
std::mutex g_regMtx;
std::vector<TrayEntry> g_regCache;
bool g_regValid = false;

std::wstring ExeStem(std::wstring p) {
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p = p.substr(slash + 1);
    if (p.size() > 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0)
        p.erase(p.size() - 4);
    return p;
}

// 注册表 NotifyIconSettings → 在跑的托盘应用（一次进程快照过滤全部）
std::vector<TrayEntry> EnumRegTrays() {
    std::vector<TrayEntry> out;
    std::set<std::wstring> dedupe;

    // ① 一次快照拿全部在跑进程的 exe 路径（别每个托盘项查一次 ——
    //    上版就是 56 次快照把菜单拖慢的）
    std::set<std::wstring> running;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, pe.th32ProcessID);
                if (!p) continue;
                wchar_t buf[MAX_PATH] = {};
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameW(p, 0, buf, &sz))
                    running.insert(NormalizePath(buf));
                CloseHandle(p);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring selfLow = NormalizePath(self);

    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Control Panel\\NotifyIconSettings",
                      0, KEY_READ, &k) != ERROR_SUCCESS) return out;

    DWORD maxSub = 0, count = 0;
    RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &count, &maxSub, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr);
    std::vector<wchar_t> sub(maxSub + 2, 0);

    for (DWORD i = 0; i < count; ++i) {
        DWORD len = maxSub + 1;
        if (RegEnumKeyExW(k, i, sub.data(), &len, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) continue;
        HKEY sk = nullptr;
        if (RegOpenKeyExW(k, sub.data(), 0, KEY_READ, &sk) != ERROR_SUCCESS) continue;

        wchar_t exe[MAX_PATH * 2] = {};
        DWORD exeSz = sizeof(exe);
        wchar_t tip[256] = {};
        DWORD tipSz = sizeof(tip);
        RegGetValueW(sk, nullptr, L"ExecutablePath", RRF_RT_REG_SZ,
                     nullptr, exe, &exeSz);
        RegGetValueW(sk, nullptr, L"InitialTooltip", RRF_RT_REG_SZ,
                     nullptr, tip, &tipSz);
        RegCloseKey(sk);

        if (!exe[0]) continue;
        const std::wstring low = NormalizePath(exe);
        if (low == selfLow) continue;              // 自己的托盘图标不进菜单
        if (running.count(low) == 0) continue;     // 没在跑的是历史残留
        if (!dedupe.insert(low).second) continue;  // 同一 exe 多条登记只留一条

        std::wstring name = tip;
        while (!name.empty() && (name.back() == L' ' || name.back() == L'\t'))
            name.pop_back();
        if (name.empty()) name = ExeStem(exe);
        out.push_back({name, exe});
    }
    RegCloseKey(k);

    std::sort(out.begin(), out.end(),
              [](const TrayEntry& a, const TrayEntry& b) {
                  return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    return out;
}

// ---- UIA（点击瞬间定位按钮矩形用） ----

HWND FindBridgeChild(HWND tray) {
    HWND hh = nullptr;
    while (true) {
        hh = FindWindowExW(tray, hh, nullptr, nullptr);
        if (!hh) return nullptr;
        wchar_t cls[64] = {};
        GetClassNameW(hh, cls, 64);
        if (std::wstring(cls) == L"Windows.UI.Composition.DesktopWindowContentBridge")
            return hh;
    }
}

// UIA 会话：RAII 管 COM 初始化与对象释放（SUCCEEDED 全要配对 CoUninit）
struct Uia {
    IUIAutomation* uia = nullptr;
    bool comSucceeded = false;

    bool Init() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comSucceeded = SUCCEEDED(hr);
        if (!comSucceeded) return false;
        const HRESULT cr = CoCreateInstance(
            CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
            __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia));
        return SUCCEEDED(cr) && uia;
    }
    ~Uia() {
        if (uia) uia->Release();
        if (comSucceeded) CoUninitialize();
    }
};

std::wstring FirstLine(const std::wstring& s) {
    const size_t p = s.find(L'\n');
    std::wstring r = (p == std::wstring::npos) ? s : s.substr(0, p);
    while (!r.empty() && (r.back() == L'\r' || r.back() == L' ')) r.pop_back();
    return r;
}

std::wstring FirstToken(const std::wstring& s) {
    const size_t p = s.find_first_of(L" \t\n");
    return (p == std::wstring::npos) ? s : s.substr(0, p);
}

// 收一棵元素树里的按钮：{第一行名, 矩形, 元素引用转交调用方}
size_t CollectButtons(IUIAutomation* uia, IUIAutomationElement* root,
                      std::vector<std::pair<std::wstring, RECT>>& out,
                      std::vector<IUIAutomationElement*>* keep) {
    IUIAutomationCondition* cond = nullptr;
    if (FAILED(uia->CreateTrueCondition(&cond))) return 0;
    IUIAutomationElementArray* arr = nullptr;
    const HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &arr);
    cond->Release();
    if (FAILED(hr) || !arr) return 0;

    int n = 0;
    arr->get_Length(&n);
    size_t cnt = 0;
    for (int i = 0; i < n; ++i) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(arr->GetElement(i, &el)) || !el) continue;
        CONTROLTYPEID ct = 0;
        if (SUCCEEDED(el->get_CurrentControlType(&ct)) &&
            ct == UIA_ButtonControlTypeId) {
            BSTR name = nullptr;
            RECT rc = {};
            if (SUCCEEDED(el->get_CurrentName(&name)) && name) {
                const bool okRect =
                    SUCCEEDED(el->get_CurrentBoundingRectangle(&rc));
                if (okRect) {
                    std::wstring wname = name;
                    out.push_back({FirstLine(wname), rc});
                    if (keep) keep->push_back(el);
                    else el->Release();
                    SysFreeString(name);
                    ++cnt;
                    continue;
                }
                SysFreeString(name);
            }
            if (name) SysFreeString(name);
        }
        el->Release();
    }
    arr->Release();
    return cnt;
}

// 名字匹配：首行精确 / 首 token / tooltip 前缀（G-Helper 温度会漂）
bool NameMatch(const std::wstring& buttonName, const std::wstring& want) {
    const std::wstring fl = FirstLine(buttonName);
    if (_wcsicmp(fl.c_str(), want.c_str()) == 0) return true;
    if (_wcsicmp(FirstToken(fl).c_str(), FirstToken(want).c_str()) == 0)
        return true;
    return _wcsnicmp(fl.c_str(), want.c_str(), want.size()) == 0;
}

// 把 UIA 按钮名剥掉任务栏按钮的修饰尾巴 ——「已固定」「- N 个运行窗口」
//（中文系统）/「pinned」「- N running windows」（英文系统）。
// 不然搜托盘图标时会撞上同名任务栏应用按钮。
std::wstring StripTaskbarDecor(std::wstring s) {
    // 数字个数没法枚举，用「尾巴模式」逐个试
    auto cutSuffix = [&s](const wchar_t* tail) {
        const size_t len = wcslen(tail);
        if (s.size() >= len &&
            _wcsicmp(s.c_str() + s.size() - len, tail) == 0)
            s.erase(s.size() - len);
    };
    for (int guard = 0; guard < 8; ++guard) {   // 防御：最多剥 8 层
        const size_t before = s.size();
        cutSuffix(L" 已固定");
        cutSuffix(L" pinned");
        cutSuffix(L" Pinned");
        // 「- N 个运行窗口 / - N running window(s)」：从「 - 数字」处截断
        const size_t dash = s.rfind(L" - ");
        if (dash != std::wstring::npos) {
            const std::wstring tail = s.substr(dash + 3);
            // 尾巴以数字开头才认为是运行窗口计数
            if (!tail.empty() && tail[0] >= L'0' && tail[0] <= L'9')
                s.erase(dash);
        }
        if (s.size() == before) break;
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    return s;
}

}  // namespace

std::vector<TrayEntry> TrayCachedEntries() {
    std::lock_guard<std::mutex> lk(g_regMtx);
    if (!g_regValid) {
        g_regCache = EnumRegTrays();
        g_regValid = true;
    }
    return g_regCache;
}

void TrayInvalidateCache() {
    std::lock_guard<std::mutex> lk(g_regMtx);
    g_regValid = false;
}

// ---- 真实点击：任务栏亮在真实位置 → 开溢出岛 → 定位按钮 → SendInput ----

// chevron 的名字随系统语言变：中文「显示隐藏的图标」、英文
//「Show hidden icons」。按名单匹配，加一种语言就多一行。
const wchar_t* kChevronNames[] = {
    L"显示隐藏的图标",
    L"Show hidden icons",
    L"Show Hidden Icons",
};

bool IsChevronName(const std::wstring& n) {
    for (const wchar_t* c : kChevronNames)
        if (_wcsicmp(n.c_str(), c) == 0) return true;
    return false;
}

// 找 chevron 并 Invoke，返回是否找到
bool InvokeChevron(IUIAutomation* uia, IUIAutomationElement* bridgeEl) {
    std::vector<std::pair<std::wstring, RECT>> btns;
    std::vector<IUIAutomationElement*> keep;
    CollectButtons(uia, bridgeEl, btns, &keep);
    bool ok = false;
    for (size_t i = 0; i < btns.size(); ++i) {
        if (IsChevronName(btns[i].first)) {
            IUnknown* unk = nullptr;
            if (SUCCEEDED(keep[i]->GetCurrentPattern(UIA_InvokePatternId, &unk)) && unk) {
                IUIAutomationInvokePattern* inv = nullptr;
                if (SUCCEEDED(unk->QueryInterface(__uuidof(IUIAutomationInvokePattern),
                                                  reinterpret_cast<void**>(&inv)))) {
                    inv->Invoke();
                    inv->Release();
                    ok = true;
                }
                unk->Release();
            }
            break;
        }
    }
    for (auto* e : keep) e->Release();
    return ok;
}

// 在按钮列表里找名字匹配的那颗，返回它的屏幕矩形（找不到返回空 RECT）
RECT FindButtonRect(const std::vector<std::pair<std::wstring, RECT>>& btns,
                    const std::wstring& want) {
    for (const auto& b : btns) {
        if (NameMatch(StripTaskbarDecor(b.first), want)) return b.second;
    }
    return RECT{};
}

// 真实点击一个溢出区托盘图标：亮任务栏 → 开溢出岛 → 定位 → SendInput。
// 全程 ~2.5s，任务栏会在屏幕底部短暂可见（这是「真点击」的必要代价）。
static bool TrayRealClick(const std::wstring& want) {
    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;

    // ① 亮在真实位置（记录现场，完事还原）
    RECT r = {};
    GetWindowRect(tray, &r);
    ShowWindow(tray, SW_SHOW);
    Sleep(400);

    bool done = false;
    {
        Uia uia;
        if (uia.Init()) {
            const HWND bridge = FindBridgeChild(tray);
            IUIAutomationElement* bridgeEl = nullptr;
            if (bridge && SUCCEEDED(uia.uia->ElementFromHandle(bridge, &bridgeEl))) {
                // ② 开溢出岛
                if (InvokeChevron(uia.uia, bridgeEl)) {
                    Sleep(900);
                    const HWND isl = FindWindowW(
                        L"TopLevelWindowForOverflowXamlIsland", nullptr);
                    if (isl) {
                        IUIAutomationElement* islEl = nullptr;
                        if (SUCCEEDED(uia.uia->ElementFromHandle(isl, &islEl))) {
                            std::vector<std::pair<std::wstring, RECT>> btns;
                            if (CollectButtons(uia.uia, islEl, btns, nullptr) > 0) {
                                // ③ 定位目标按钮 → 真实点击
                                const RECT rc = FindButtonRect(btns, want);
                                if (rc.left || rc.right) {
                                    const int cx = (rc.left + rc.right) / 2;
                                    const int cy = (rc.top + rc.bottom) / 2;
                                    POINT save = {};
                                    GetCursorPos(&save);
                                    SetCursorPos(cx, cy);
                                    Sleep(80);
                                    INPUT click[2] = {};
                                    click[0].type = INPUT_MOUSE;
                                    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                                    click[1].type = INPUT_MOUSE;
                                    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                                    SendInput(2, click, sizeof(INPUT));
                                    Sleep(150);
                                    SetCursorPos(save.x, save.y);
                                    done = true;
                                    Log(L"托盘触发：已真实点击「%ls」(%d,%d)",
                                        want.substr(0, 24).c_str(), cx, cy);
                                }
                            }
                            islEl->Release();
                        }
                    }
                }
                bridgeEl->Release();
            }
        }
    }

    // ④ 还原：藏回屏幕外
    Sleep(done ? 400 : 100);
    SetWindowPos(tray, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(tray, SW_HIDE);
    return done;
}

void TrayClickByNameAsync(const std::wstring& name) {
    ScheduleBackground(L"托盘触发", [name](HANDLE stop) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;
        if (TrayRealClick(name)) return;
        // 没点中（岛没开成/按钮不在）：日志记下，用户下次再试
        Log(L"托盘触发：「%ls」本次未完成", name.substr(0, 24).c_str());
    });
}

}  // namespace mydock
