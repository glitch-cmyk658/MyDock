// 2026-10-12 - AI - 托盘溢出区 UIA 方案实现（见 trayuia.h 头注）
#include "trayuia.h"

#include "util.h"   // ScheduleBackground / Log / ConfigDir

#include <uiautomationclient.h>
#include <mutex>
#include <utility>

namespace mydock {
namespace {

// ---- 缓存（菜单打开时直接用，后台线程负责刷新） ----
std::mutex g_cacheMtx;
std::vector<TrayEntry> g_cache;
bool g_cacheValid = false;
unsigned long long g_cacheAt = 0;

// UIA 元素 → 所属进程的 exe 路径（图标提取用）。拿不到返回空。
std::wstring ExeOfElement(IUIAutomationElement* el) {
    DWORD pid0 = 0;
    int ipid = 0;
    if (FAILED(el->get_CurrentProcessId(&ipid)) || !ipid) return L"";
    DWORD pid = static_cast<DWORD>(ipid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD sz = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf, &sz)) out = buf;
    CloseHandle(p);
    return out;
}

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

// UIA 会话：RAII 管 COM 初始化与对象释放。
// ★ COM 规矩（与 main.cpp 同一条）：SUCCEEDED（含 S_FALSE）都 +1 计数，
//   都要配对 CoUninitialize。
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

// 枚举一棵元素树里的全部按钮（保留原始多行名；元素引用转交调用方释放）
size_t CollectButtons(IUIAutomation* uia, IUIAutomationElement* root,
                      std::vector<std::pair<std::wstring, IUIAutomationElement*>>& out) {
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
            if (SUCCEEDED(el->get_CurrentName(&name)) && name) {
                out.push_back({name, el});   // el 引用转交调用方
                SysFreeString(name);
                ++cnt;
                continue;
            }
            if (name) SysFreeString(name);
        }
        el->Release();
    }
    arr->Release();
    return cnt;
}

bool InvokeEl(IUIAutomationElement* el) {
    IUnknown* unk = nullptr;
    if (FAILED(el->GetCurrentPattern(UIA_InvokePatternId, &unk)) || !unk) return false;
    IUIAutomationInvokePattern* inv = nullptr;
    const HRESULT hr = unk->QueryInterface(__uuidof(IUIAutomationInvokePattern),
                                           reinterpret_cast<void**>(&inv));
    unk->Release();
    if (FAILED(hr) || !inv) return false;
    inv->Invoke();
    inv->Release();
    return true;
}

}  // namespace

// 完整舞步：names 非空 → 枚举溢出区；invokeName 非空 → 按名触发。
// stop 有信号时在各个等待点尽快撤退（先恢复任务栏原位再返回）。
static bool TrayDance(std::vector<TrayEntry>* entries,
                      const std::wstring* invokeName,
                      const std::wstring* invokePrefix, HANDLE stop) {
    auto snooze = [&](DWORD ms) {
        return stop && WaitForSingleObject(stop, ms) == WAIT_OBJECT_0;
    };

    const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return false;
    RECT r = {};
    if (!GetWindowRect(tray, &r)) return false;

    // ① 挪到屏幕外再亮 —— 任务栏内容照常成型，用户全程看不见
    SetWindowPos(tray, nullptr, 0, GetSystemMetrics(SM_CYSCREEN) + 5,
                 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(tray, SW_SHOW);
    if (snooze(450)) { ShowWindow(tray, SW_HIDE); return false; }

    bool done = false;
    {
        Uia uia;
        if (uia.Init()) {
            const HWND bridge = FindBridgeChild(tray);
            IUIAutomationElement* rootEl = nullptr;
            if (bridge &&
                SUCCEEDED(uia.uia->ElementFromHandle(bridge, &rootEl)) && rootEl) {
                std::vector<std::pair<std::wstring, IUIAutomationElement*>> btns;
                CollectButtons(uia.uia, rootEl, btns);

                // ②-b 前缀触发（输入法切换这类主栏按钮，不用开溢出岛）
                if (invokePrefix) {
                    for (auto& b : btns) {
                        if (b.first.rfind(*invokePrefix, 0) == 0) {
                            InvokeEl(b.second);
                            done = true;
                            break;
                        }
                    }
                }

                // ② Invoke「显示隐藏的图标」→ 溢出岛出现
                if (!invokePrefix) for (auto& b : btns) {
                    if (b.first == L"显示隐藏的图标") { InvokeEl(b.second); break; }
                }
                if (snooze(900)) {
                    for (auto& b : btns) if (b.second) b.second->Release();
                    rootEl->Release();
                    SetWindowPos(tray, nullptr, r.left, r.top, 0, 0,
                                 SWP_NOSIZE | SWP_NOZORDER);
                    ShowWindow(tray, SW_HIDE);
                    return false;
                }

                // ③ 溢出岛里就是那些「平常看不见」的托盘应用
                const HWND isl = invokePrefix
                                     ? nullptr
                                     : FindWindowW(
                                           L"TopLevelWindowForOverflowXamlIsland",
                                           nullptr);
                if (isl) {
                    IUIAutomationElement* islEl = nullptr;
                    if (SUCCEEDED(uia.uia->ElementFromHandle(isl, &islEl)) && islEl) {
                        std::vector<std::pair<std::wstring, IUIAutomationElement*>> ib;
                        CollectButtons(uia.uia, islEl, ib);
                        if (entries) {
                            for (auto& b : ib) {
                                const std::wstring fl = FirstLine(b.first);
                                if (fl.empty() || fl == L"MyDock") continue;
                                entries->push_back({fl, ExeOfElement(b.second)});
                            }
                            done = !entries->empty();
                        }
                        if (invokeName) {
                            for (auto& b : ib) {
                                const std::wstring fl = FirstLine(b.first);
                                // 首行精确匹配；G-Helper 温度这类会漂的，
                                // 退而比对首 token
                                const bool hit =
                                    _wcsicmp(fl.c_str(), invokeName->c_str()) == 0 ||
                                    _wcsicmp(FirstToken(fl).c_str(),
                                             FirstToken(*invokeName).c_str()) == 0;
                                if (hit) { InvokeEl(b.second); done = true; break; }
                            }
                        }
                        for (auto& b : ib) if (b.second) b.second->Release();
                        islEl->Release();
                    }
                }
                // ④ 再 Invoke 一次 chevron 收起溢出岛
                if (!invokePrefix) for (auto& b : btns) {
                    if (b.first == L"显示隐藏的图标") { InvokeEl(b.second); break; }
                }
                for (auto& b : btns) if (b.second) b.second->Release();
                rootEl->Release();
            }
        }
    }

    if (snooze(250)) { ShowWindow(tray, SW_HIDE); return done; }
    // ⑤ 还原：挪回原位 + 藏起来
    SetWindowPos(tray, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(tray, SW_HIDE);
    return done;
}

std::vector<TrayEntry> TrayCachedEntries() {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    return g_cache;
}

bool TrayCacheReady() {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    return g_cacheValid;
}

unsigned long long TrayCacheAgeMs() {
    std::lock_guard<std::mutex> lk(g_cacheMtx);
    if (!g_cacheValid) return ~0ull;
    return GetTickCount64() - g_cacheAt;
}

void TrayRefreshAsync() {
    ScheduleBackground(L"托盘枚举", [](HANDLE stop) {
        std::vector<TrayEntry> entries;
        if (TrayDance(&entries, nullptr, nullptr, stop)) {
            std::lock_guard<std::mutex> lk(g_cacheMtx);
            g_cache = std::move(entries);
            g_cacheValid = true;
            g_cacheAt = GetTickCount64();
        }
    });
}

void TrayInvokeByNameAsync(const std::wstring& firstNameLine) {
    ScheduleBackground(L"托盘触发", [firstNameLine](HANDLE stop) {
        TrayDance(nullptr, &firstNameLine, nullptr, stop);
    });
}

}  // namespace mydock
