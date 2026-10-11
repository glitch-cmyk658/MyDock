// 2026-10-09 - AI - 系统状态采集的实现：电池走 GetSystemPowerStatus，
// 音量走 IAudioEndpointVolume，Wi-Fi 走 wlanapi，蓝牙走 bthprops
#include "status.h"
#include "util.h"

#include <algorithm>
#include <cmath>

#include <iphlpapi.h>
#include <wlanapi.h>
#include <bluetoothapis.h>

#include <mmdeviceapi.h>
#include <endpointvolume.h>

namespace mydock {
namespace {

// ★ COM 的两个 GUID 只能自己写死。
//
//   MSVC 里 CLSID_MMDeviceEnumerator / IID_IAudioEndpointVolume 是从 uuid.lib 里
//   链接进来的；MinGW 没有对应的 .a（libuuid.a 里也没有这一对），
//   照直写 &CLSID_MMDeviceEnumerator 会是「未定义的引用」。
//   所以按文档把这两个 GUID 抄成常量。
//   —— 名字不能叫 CLSID_MMDeviceEnumerator，那样会和头文件里的 extern 声明打架。
const CLSID kClsidMMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioEndpointVolume =
    {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

// ---- 音量：默认播放设备的音量接口，进程里只建一次 ----
//
// ★ 为什么是「只建一次」而不是每次问都新建：默认设备会变（插耳机、切音箱），
//   但真按设备变化去重订阅通知太重了。这里的取舍是：接口建一次，插拔设备后
//   有可能读到的是已经不存在的那个端点 —— 这时 GetMute 会返回失败，
//   我们就退回「不知道」，下次发现连不上就重新建一次。
IAudioEndpointVolume* g_vol = nullptr;
bool g_comTried = false;

bool EnsureVolume() {
    if (g_vol) return true;

    if (!g_comTried) {
        g_comTried = true;
        // ★ 进程级一次性初始化，故意不配对 CoUninitialize：音量接口要活到
        //   进程结束，计数随进程由系统回收。S_OK / S_FALSE 都算成功
        //   （S_FALSE 同样 +1，但我们本来就不打算减，与 dock.cpp 的规矩不冲突 ——
        //   那条规矩约束的是「可以配对的地方」）；RPC_E_CHANGED_MODE 才算失败，
        //   此时主线程已用别的并发模式初始化过，CoCreateInstance 照常能用。
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    }

    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL,
                                  kIID_IMMDeviceEnumerator,
                                  reinterpret_cast<void**>(&en));
    if (FAILED(hr) || !en) return false;

    IMMDevice* dev = nullptr;
    hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    en->Release();
    if (FAILED(hr) || !dev) return false;

    IAudioEndpointVolume* v = nullptr;
    hr = dev->Activate(kIID_IAudioEndpointVolume, CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(&v));
    dev->Release();
    if (FAILED(hr) || !v) return false;

    g_vol = v;
    return true;
}

bool ReadVolume(bool& muted, int& vol) {
    if (!EnsureVolume()) return false;

    BOOL m = FALSE;
    float lvl = 0.0f;
    HRESULT h1 = g_vol->GetMute(&m);
    if (FAILED(h1)) {
        // 设备八成被拔了：把接口扔掉，下一次重新建
        g_vol->Release();
        g_vol = nullptr;
        return false;
    }
    if (FAILED(g_vol->GetMasterVolumeLevelScalar(&lvl))) return false;

    muted = (m != FALSE);
    vol = static_cast<int>(lvl * 100.0f + 0.5f);
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    return true;
}

// ---- Wi-Fi：两秒问一次 ----
//
// ★ 为什么不用「有没有网」来判断：GetAdaptersAddresses / InternetGetConnectedState
//   分不出有线还是无线，插着网线的时候也会说「连着」。
//   wlanapi 能直接说清「无线网卡现在连到哪个 AP、信号多强」，
//   代价只是要开一个句柄、枚举一遍接口 —— 两秒一次完全能接受。
void ReadWifi(bool& present, bool& up, int& bars) {
    present = false;
    up = false;
    bars = 0;

    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return;

    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const WLAN_INTERFACE_INFO& it = list->InterfaceInfo[i];
            present = true;

            DWORD sz = 0;
            void* data = nullptr;
            if (WlanQueryInterface(h, &it.InterfaceGuid, wlan_intf_opcode_interface_state,
                                   nullptr, &sz, &data, nullptr) != ERROR_SUCCESS) {
                continue;
            }
            const WLAN_INTERFACE_STATE st =
                *reinterpret_cast<const WLAN_INTERFACE_STATE*>(data);
            WlanFreeMemory(data);
            if (st != wlan_interface_state_connected) continue;

            up = true;

            // 信号强度：RSSI 是个负数 dBm，-50 以内满格，-75 往下就快断了。
            data = nullptr;
            if (WlanQueryInterface(h, &it.InterfaceGuid, wlan_intf_opcode_rssi,
                                   nullptr, &sz, &data, nullptr) == ERROR_SUCCESS) {
                const LONG rssi = *reinterpret_cast<const LONG*>(data);
                WlanFreeMemory(data);
                bars = (rssi >= -58) ? 3 : (rssi >= -70 ? 2 : 1);
            } else {
                bars = 3;   // 连上了但问不到信号：按满格画，别让人觉得网断了
            }
            break;          // 有多块无线网卡也只报第一块
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(h, nullptr);
}

// ---- 蓝牙：三十秒问一次 ----
//
// ★ 这一路只能问出「机器上有没有蓝牙无线电」，问不出「开着没开」。
//   想知道开关状态得走 WinRT 的 Windows.Devices.Radios（要 C++/WinRT，太重）。
//   所以策略是：有无线电就正常画，没有就把图标压暗 —— 不假装知道开关状态。
void ReadBluetooth(bool& present) {
    present = false;
    BLUETOOTH_FIND_RADIO_PARAMS p = {};
    p.dwSize = sizeof(p);
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&p, &radio);
    if (!find) return;
    present = true;
    if (radio) CloseHandle(radio);
    BluetoothFindRadioClose(find);
}

// ---- 下面两个是给「图标菜单」用的按需查询做铺垫的 ----

// 挑一块无线网卡：优先挑**正在连着**的那块，都没有就用第一块。
// （笔记本可能同时有 Wi-Fi 网卡和虚拟网卡，随便拿第一块会拿到没在用的那个。）
bool FirstWlanInterface(HANDLE h, GUID& guid) {
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) != ERROR_SUCCESS || !list) return false;

    bool found = false;
    bool hasFirst = false;
    GUID first{};
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const WLAN_INTERFACE_INFO& it = list->InterfaceInfo[i];
        if (!hasFirst) { first = it.InterfaceGuid; hasFirst = true; }
        if (it.isState == wlan_interface_state_connected) {
            guid = it.InterfaceGuid;
            found = true;
            break;
        }
    }
    if (!found && hasFirst) { guid = first; found = true; }
    WlanFreeMemory(list);
    return found;
}

// ★ SSID 是**一段不带结尾 0 的字节**，不是 C 字符串，也不能假定它是合法 UTF-16。
//   直接 (wchar_t*)ucSSID 会读到越界的乱码。这里按字节收进来后按 UTF-8 转宽字符
//   （中文网络名普遍是 UTF-8），转不出来就退回按本地代码页转一次。
std::wstring SsidToStr(const DOT11_SSID& ss) {
    if (ss.uSSIDLength == 0) return L"";
    char buf[DOT11_SSID_MAX_LENGTH + 1] = {};
    const size_t n = std::min<size_t>(ss.uSSIDLength, DOT11_SSID_MAX_LENGTH);
    memcpy(buf, ss.ucSSID, n);

    auto conv = [&](UINT cp) -> std::wstring {
        const int need = MultiByteToWideChar(cp, 0, buf, static_cast<int>(n), nullptr, 0);
        if (need <= 0) return L"";
        std::wstring s(static_cast<size_t>(need), L'\0');
        MultiByteToWideChar(cp, 0, buf, static_cast<int>(n), &s[0], need);
        // 有些网络名里塞了控制字符，照直画进菜单会出来一堆方框
        s.erase(std::remove_if(s.begin(), s.end(),
                               [](wchar_t c) { return c < 0x20 || c == 0x7F; }),
                s.end());
        return s;
    };

    std::wstring s = conv(CP_UTF8);
    if (s.empty()) s = conv(CP_ACP);
    return s;
}

}  // namespace

bool PollSysStatus(SysStatus& s) {
    const SysStatus old = s;

    // ---- 电池 ----
    SYSTEM_POWER_STATUS ps = {};
    if (GetSystemPowerStatus(&ps)) {
        s.hasBattery = (ps.BatteryFlag != 128);          // 128 = 没有电池
        s.onAC = (ps.ACLineStatus == 1);
        // 255 = 未知（有些虚拟机会这么回）。宁可显示满格，也不要显示 0% 吓人。
        s.pct = (ps.BatteryLifePercent == 255) ? 100 : ps.BatteryLifePercent;
        s.charging = s.onAC && (ps.BatteryFlag & 8) != 0;   // 8 = 正在充电
    }

    // ---- 音量 ----
    bool muted = false;
    int vol = 0;
    if (ReadVolume(muted, vol)) {
        s.volKnown = true;
        s.muted = muted;
        s.volume = vol;
    } else {
        s.volKnown = false;
    }

    const DWORD now = GetTickCount();

    // ---- Wi-Fi（节流 2s）----
    if (s.stamp == 0 || now - s.stamp >= 2000 || now < s.stamp) {
        ReadWifi(s.wifiPresent, s.wifiUp, s.wifiBars);
    }

    // ---- 蓝牙（节流 30s）----
    static DWORD lastBt = 0;
    static bool btCache = false;
    if (lastBt == 0 || now - lastBt >= 30000 || now < lastBt) {
        lastBt = now;
        ReadBluetooth(btCache);
    }
    s.btPresent = btCache;

    s.stamp = now;

    // 只比较「画出来会不一样」的字段。volume 细微变化也重画，反正一条很小的条子。
    return old.hasBattery != s.hasBattery || old.pct != s.pct ||
           old.charging != s.charging || old.onAC != s.onAC ||
           old.volKnown != s.volKnown || old.muted != s.muted ||
           old.volume != s.volume || old.wifiPresent != s.wifiPresent ||
           old.wifiUp != s.wifiUp || old.wifiBars != s.wifiBars ||
           old.btPresent != s.btPresent;
}

// ===================================================================
// 图标菜单用的按需查询
//
// 这一组只在菜单要弹开的时候调，所以可以慢一点、问得细一点。
// 跟上面那份半秒一次的轮询分开，免得每半秒都背上一次网络扫描。
// ===================================================================

std::wstring WifiCurrentSsid() {
    std::wstring out;
    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return out;

    GUID guid{};
    if (FirstWlanInterface(h, guid)) {
        DWORD sz = 0;
        void* data = nullptr;
        if (WlanQueryInterface(h, &guid, wlan_intf_opcode_current_connection,
                               nullptr, &sz, &data, nullptr) == ERROR_SUCCESS && data) {
            const WLAN_CONNECTION_ATTRIBUTES* a =
                reinterpret_cast<const WLAN_CONNECTION_ATTRIBUTES*>(data);
            // ★ SSID 不在这一层的 dot11Ssid 里，在 wlanAssociationAttributes 里
            //   （那一层装的是「连上之后的实际链路参数」）。
            if (a->isState == wlan_interface_state_connected) {
                out = SsidToStr(a->wlanAssociationAttributes.dot11Ssid);
            }
            WlanFreeMemory(data);
        }
    }
    WlanCloseHandle(h, nullptr);
    return out;
}

std::vector<WifiNet> EnumWifiNetworks(int maxCount) {
    std::vector<WifiNet> out;
    if (maxCount <= 0) return out;

    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return out;

    GUID guid{};
    if (FirstWlanInterface(h, guid)) {
        PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
        if (WlanGetAvailableNetworkList(h, &guid, 0, nullptr, &list) == ERROR_SUCCESS && list) {
            for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
                const WLAN_AVAILABLE_NETWORK& n = list->Network[i];

                // ★ 只留「现在能连」的。wlanapi 会把以前连过、现在不在范围内的
                //   历史网络一起报出来，不做这个过滤的话菜单里会有一半是连不上的
                //   名字 —— 点了没反应，还不如不列。
                //
                //   「能不能连」是结构里的 bNetworkConnectable，**不是 dwFlags 里的位**
                //   （MSDN 那份文档里确实有个叫 WLAN_AVAILABLE_NETWORK_CONNECTABLE 的
                //   东西，但 MinGW 的 wlanapi.h 里没有定义，别照着名字写）。
                if (!n.bNetworkConnectable) continue;

                const std::wstring ssid = SsidToStr(n.dot11Ssid);
                if (ssid.empty()) continue;   // 隐藏网络（SSID 广播关了）

                WifiNet w;
                w.ssid = ssid;
                // wlanSignalQuality 是 0..100，跟 RSSI 的 -58/-70 那套门槛对齐
                w.bars = (n.wlanSignalQuality >= 75) ? 3
                       : (n.wlanSignalQuality >= 45 ? 2 : 1);
                w.secured   = (n.bSecurityEnabled != FALSE);
                w.connected = (n.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
                // 有没有存过配置 = 点一下能不能直接连。没存过就得要密码，
                // 那种情况 profile 留空，菜单那边会改成「打开设置」。
                if (n.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) {
                    w.profile = std::wstring(n.strProfileName);
                }

                // 同一个名字会被好几块 AP 各报一次，留信号最强的那份
                auto it = std::find_if(out.begin(), out.end(),
                                       [&](const WifiNet& a) { return a.ssid == ssid; });
                if (it == out.end()) out.push_back(w);
                else if (w.bars > it->bars) *it = w;
            }
            WlanFreeMemory(list);
        }
    }
    WlanCloseHandle(h, nullptr);

    // 当前连着的排最前，其余按信号从强到弱
    std::stable_sort(out.begin(), out.end(), [](const WifiNet& a, const WifiNet& b) {
        if (a.connected != b.connected) return a.connected;
        return a.bars > b.bars;
    });
    if (out.size() > static_cast<size_t>(maxCount)) out.resize(static_cast<size_t>(maxCount));
    return out;
}

bool ConnectWifi(const std::wstring& profile) {
    if (profile.empty()) return false;

    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return false;

    bool ok = false;
    GUID guid{};
    if (FirstWlanInterface(h, guid)) {
        WLAN_CONNECTION_PARAMETERS cp = {};
        cp.wlanConnectionMode = wlan_connection_mode_profile;
        cp.strProfile         = profile.c_str();
        // ★ 不填 pDot11Ssid：填了就变成「连某个指定的 AP」，
        //   而我们要的是「按这份配置连」，同一份配置下换 AP 是系统的活。
        cp.pDot11Ssid         = nullptr;
        cp.pDesiredBssidList  = nullptr;
        cp.dot11BssType       = dot11_BSS_type_infrastructure;
        cp.dwFlags            = 0;

        const DWORD r = WlanConnect(h, &guid, &cp, nullptr);
        ok = (r == ERROR_SUCCESS);
        if (!ok) Log(L"Wi-Fi：连「%ls」失败（%lu）", profile.c_str(), r);
        else     Log(L"Wi-Fi：正在连「%ls」", profile.c_str());
    }
    WlanCloseHandle(h, nullptr);
    return ok;
}

std::vector<std::wstring> EnumBtDevices(int maxCount) {
    std::vector<std::wstring> out;
    if (maxCount <= 0) return out;

    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;   // 配对过的
    sp.fReturnRemembered    = TRUE;
    sp.fReturnConnected     = TRUE;
    sp.fReturnUnknown       = FALSE;
    // ★ 不主动发起搜寻（fIssueInquiry）：那要十几秒，菜单会卡在那儿不动。
    //   只列系统已经知道的设备 —— 菜单本来就是给「连一个已知设备」用的，
    //   要找新设备该去设置页，那儿有专门的搜寻按钮。
    sp.fIssueInquiry        = FALSE;
    sp.cTimeoutMultiplier   = 1;

    BLUETOOTH_DEVICE_INFO dev = {};
    dev.dwSize = sizeof(dev);
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&sp, &dev);
    if (!find) return out;

    do {
        if (dev.szName[0]) {
            const std::wstring nm(dev.szName);
            if (std::find(out.begin(), out.end(), nm) == out.end()) out.push_back(nm);
        }
    } while (static_cast<int>(out.size()) < maxCount && BluetoothFindNextDevice(find, &dev));

    BluetoothFindDeviceClose(find);
    return out;
}

// 2026-10-11 - AI - 点蓝牙设备真正发起连接
//
// 以前这一步直接开设置页 —— 因为「连蓝牙设备」没有一条像 WlanConnect 那样的
// 一键 API。但其实有条半公开的路：BluetoothSetServiceState 把已配对设备的
// 音频服务（A2DP / 免提）**启用**一次，蓝牙栈就会去建立这条服务链路 ——
// 对耳机来说这就是「连接」。键盘鼠标这类 HID 设备系统自己管重连，点了也没坏处
// （服务启用对它们多半是 no-op），所以统一走这条路，失败再退回设置页。
//
// ★ 连接是**异步**的：这个函数返回 true 只表示「请求已被蓝牙栈接受」，
//   耳机真正「叮」一声还要一两秒。别在这儿等 —— 菜单会僵住。
bool ConnectBtDevice(const std::wstring& name) {
    if (name.empty()) return false;

    // 找第一块蓝牙无线电。绝大多数机器只有一块；多块机器上漏掉第二块
    // 的设备可以接受 —— 为罕见情形把句柄管理写复杂不值得。
    HANDLE radio = nullptr;
    BLUETOOTH_FIND_RADIO_PARAMS rp = {};
    rp.dwSize = sizeof(rp);
    HBLUETOOTH_RADIO_FIND rf = BluetoothFindFirstRadio(&rp, &radio);
    if (!rf) {
        Log(L"蓝牙：找不到无线电，没法发起连接");
        return false;
    }

    bool ok = false;
    // A2DP 音频（耳机/音箱）和免提（通话通道）。两条都试着启用，
    // 有一条成功就算发起了 —— 单声道耳麦可能只有免提服务。
    static const GUID kA2dp    = {0x0000110b, 0x0000, 0x1000,
                                  {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
    static const GUID kHandsfree = {0x0000111e, 0x0000, 0x1000,
                                  {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};

    BLUETOOTH_DEVICE_SEARCH_PARAMS sp = {};
    sp.dwSize = sizeof(sp);
    sp.fReturnAuthenticated = TRUE;   // 只在配对过的里面找
    sp.fReturnRemembered    = TRUE;
    sp.fReturnConnected     = TRUE;
    sp.fIssueInquiry        = FALSE;  // 不扫描，秒回
    sp.cTimeoutMultiplier   = 0;
    sp.hRadio = radio;

    BLUETOOTH_DEVICE_INFO dev = {};
    dev.dwSize = sizeof(dev);
    HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&sp, &dev);
    if (find) {
        do {
            if (_wcsicmp(dev.szName, name.c_str()) == 0) {
                const DWORD r1 = BluetoothSetServiceState(radio, &dev, &kA2dp,
                                                          BLUETOOTH_SERVICE_ENABLE);
                const DWORD r2 = BluetoothSetServiceState(radio, &dev, &kHandsfree,
                                                          BLUETOOTH_SERVICE_ENABLE);
                ok = (r1 == ERROR_SUCCESS || r2 == ERROR_SUCCESS);
                Log(L"蓝牙：向「%ls」发起连接 —— A2DP %lu，免提 %lu",
                    name.c_str(), r1, r2);
                break;
            }
        } while (BluetoothFindNextDevice(find, &dev));
        BluetoothFindDeviceClose(find);
    }
    BluetoothFindRadioClose(rf);
    CloseHandle(radio);
    return ok;
}

// ---------------------------------------------------------------- 亮度
// dxva2 的显示器亮度 API 动态加载：要给 MinGW 找对头文件还得加链接库，
// 而且内屏经常根本不支持 —— 动态加载失败/调用失败都干净地返回 false，
// 调用方把滑条藏掉就行，不要画一个拖了没反应的假滑条。
namespace {
// dxva2 的 PHYSICAL_MONITOR 在很多 MinGW 环境没被 windows.h 带出来，
// 自己定义一份等价的（就一个句柄 + 描述串），避免为这一个结构引头文件。
struct PhyMon { HANDLE hPhysicalMonitor; WCHAR szPhysicalMonitorDescription[128]; };
typedef BOOL(WINAPI *PFN_GetNumPhy)(HMONITOR, DWORD*);
typedef BOOL(WINAPI *PFN_GetPhy)(HMONITOR, DWORD, PhyMon*);
typedef BOOL(WINAPI *PFN_DestPhy)(PhyMon*, DWORD);
typedef BOOL(WINAPI *PFN_GetBr)(HANDLE, LPDWORD, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *PFN_SetBr)(HANDLE, DWORD);

bool BrFunctions(PFN_GetNumPhy& n, PFN_GetPhy& gp, PFN_DestPhy& dp,
                 PFN_GetBr& gb, PFN_SetBr& sb) {
    HMODULE dx = LoadLibraryW(L"dxva2.dll");
    if (!dx) return false;
    n  = reinterpret_cast<PFN_GetNumPhy>(GetProcAddress(dx, "GetNumberOfPhysicalMonitorsFromHMONITOR"));
    gp = reinterpret_cast<PFN_GetPhy>(GetProcAddress(dx, "GetPhysicalMonitorsFromHMONITOR"));
    dp = reinterpret_cast<PFN_DestPhy>(GetProcAddress(dx, "DestroyPhysicalMonitors"));
    gb = reinterpret_cast<PFN_GetBr>(GetProcAddress(dx, "GetMonitorBrightness"));
    sb = reinterpret_cast<PFN_SetBr>(GetProcAddress(dx, "SetMonitorBrightness"));
    return n && gp && dp && gb && sb;
}
}  // namespace

bool GetBrightness(int& pct) {
    // ★ 亮度能力是机器属性，探测一次就够 —— 结果缓存下来。
    //   这条 dxva2 探测链在这台机器上崩过两次（空句柄 + 更诡异的第二次崩），
    //   不能让「每次打开控制中心」都重新踩一遍雷。失败过就永远走缓存。
    static int known = -1;      // -1 还没探过 / 0 确认不可用 / 1 确认可用
    if (known == 0) return false;

    PFN_GetNumPhy n; PFN_GetPhy gp; PFN_DestPhy dp; PFN_GetBr gb; PFN_SetBr sb;
    if (!BrFunctions(n, gp, dp, gb, sb)) { known = 0; return false; }
    const POINT zero = {0, 0};
    HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) return false;
    DWORD cnt = 0;
    if (!n(mon, &cnt) || cnt == 0) return false;
    std::vector<PhyMon> pm(cnt);
    if (!gp(mon, cnt, pm.data())) return false;
    // ★ 驱动可能返回 TRUE 但句柄是空的（笔记本内屏不支持 DDC/CI 就这样，
    //   实测：desc="Generic PnP Monitor"，句柄 0x0）。拿着空句柄往下走，
    //   GetMonitorBrightness 返回失败还算客气，DestroyPhysicalMonitors
    //   会直接访问违例把进程带走 —— 实测崩的就是它。所以先验句柄。
    if (!pm[0].hPhysicalMonitor) {
        // ★ 此时连 DestroyPhysicalMonitors 都不能调 —— 它对空句柄直接
        //   访问违例（python ctypes 实测，崩点就在它内部）。
        known = 0;
        return false;
    }
    DWORD lo = 0, cur = 0, hi = 0;
    const bool ok = gb(pm[0].hPhysicalMonitor, &lo, &cur, &hi) && hi > lo;
    dp(pm.data(), cnt);
    if (!ok) { known = 0; return false; }
    known = 1;
    pct = static_cast<int>((cur - lo) * 100 / (hi - lo));
    return true;
}

bool SetBrightness(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // GetBrightness 探测过不可用的话，滑条根本不会出现，这里也不该被调到；
    // 保险起见再拦一层（静态标记两边共用一份）。
    static bool setDisabled = false;
    if (setDisabled) return false;
    PFN_GetNumPhy n; PFN_GetPhy gp; PFN_DestPhy dp; PFN_GetBr gb; PFN_SetBr sb;
    if (!BrFunctions(n, gp, dp, gb, sb)) return false;
    const POINT zero = {0, 0};
    HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) return false;
    DWORD cnt = 0;
    if (!n(mon, &cnt) || cnt == 0) return false;
    std::vector<PhyMon> pm(cnt);
    if (!gp(mon, cnt, pm.data())) return false;
    if (!pm[0].hPhysicalMonitor) {          // 同 GetBrightness：连 Destroy 都不能调
        setDisabled = true;
        return false;
    }
    DWORD lo = 0, cur = 0, hi = 0;
    bool ok = false;
    if (gb(pm[0].hPhysicalMonitor, &lo, &cur, &hi) && hi > lo) {
        ok = sb(pm[0].hPhysicalMonitor, lo + (hi - lo) * static_cast<DWORD>(pct) / 100) != 0;
    }
    dp(pm.data(), cnt);
    return ok;
}

// ---------------------------------------------------------------- Wi-Fi 软开关
bool SetWifiEnabled(bool on) {
    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return false;

    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    bool ok = false;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
        // set 走单个 PHY 的 WLAN_PHY_RADIO_STATE（MinGW 头里就是这个形状）。
        // dwPhyIndex = 0 是最常见的 Wi-Fi PHY；个别网卡多 PHY 时只关 0 号
        // 可能不够 —— 但先对齐最普遍的情况，遇到再说。
        WLAN_PHY_RADIO_STATE st = {};
        st.dwPhyIndex = 0;
        st.dot11SoftwareRadioState = on ? dot11_radio_state_on : dot11_radio_state_off;
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
            const GUID& guid = list->InterfaceInfo[i].InterfaceGuid;
            const DWORD r = WlanSetInterface(h, &guid, wlan_intf_opcode_radio_state,
                                             sizeof(st), &st, nullptr);
            if (r == ERROR_SUCCESS) ok = true;
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(h, nullptr);
    return ok;
}

bool GetWifiEnabled() {
    HANDLE h = nullptr;
    DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return true;
    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    bool on = true;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list &&
        list->dwNumberOfItems > 0) {
        DWORD sz = 0;
        WLAN_RADIO_STATE* rs = nullptr;
        if (WlanQueryInterface(h, &list->InterfaceInfo[0].InterfaceGuid,
                               wlan_intf_opcode_radio_state, nullptr, &sz,
                               reinterpret_cast<void**>(&rs), nullptr) == ERROR_SUCCESS &&
            rs) {
            // ★ 返回的数据在系统分配的内存里，用完必须 WlanFreeMemory ——
            //   以前传栈变量地址，API 把堆指针写到栈上，读出来的是野数据还泄漏。
            if (rs->dwNumberOfPhys > 0) {
                on = rs->PhyRadioState[0].dot11SoftwareRadioState != dot11_radio_state_off;
            }
            WlanFreeMemory(rs);
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(h, nullptr);
    return on;
}

bool SetVolumePct(int pct) {
    if (!EnsureVolume() || !g_vol) return false;
    const float f = std::clamp(pct, 0, 100) / 100.0f;
    const HRESULT hr = g_vol->SetMasterVolumeLevelScalar(f, nullptr);
    if (FAILED(hr)) {
        Log(L"音量：设成 %d%% 失败（0x%08lx）", pct, static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool SetMuted(bool mute) {
    if (!EnsureVolume() || !g_vol) return false;
    const HRESULT hr = g_vol->SetMute(mute ? TRUE : FALSE, nullptr);
    if (FAILED(hr)) {
        Log(L"音量：%s失败（0x%08lx）", mute ? L"静音" : L"取消静音",
            static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

int BatteryMinutesLeft() {
    SYSTEM_POWER_STATUS ps = {};
    if (!GetSystemPowerStatus(&ps)) return -1;
    // BatteryLifeTime 是**秒**，-1 表示系统说不知道。
    // 插着电的时候这个值经常是 -1（没有在放电，没得算），这不是错误。
    if (ps.BatteryLifeTime == static_cast<DWORD>(-1)) return -1;
    return static_cast<int>(ps.BatteryLifeTime) / 60;
}

}  // namespace mydock
