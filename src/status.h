// 2026-10-09 - AI - 系统状态采集：电池 / 音量 / Wi-Fi / 蓝牙，给顶部状态栏右侧那排图标用
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace mydock {

// 顶部栏右侧那排图标要显示的东西。全是**真实状态**，不是画着好看的装饰：
// 电池图标里灌多少、喇叭有没有斜杠、Wi-Fi 有几格，都从系统里现问。
struct SysStatus {
    // ---- 电池（GetSystemPowerStatus，user32，最省事的一路）----
    bool hasBattery = false;    // 台式机没电池，就不画这个图标
    int  pct        = 100;      // 0..100；系统回 255（未知）时折算成 100
    bool charging   = false;    // 插着电**并且**在充（光插着不算）
    bool onAC       = false;

    // ---- 音量（IAudioEndpointVolume，COM）----
    bool volKnown   = false;    // 取不到（没音频设备 / COM 起不来）就画不静音的喇叭
    bool muted      = false;
    int  volume     = 60;       // 0..100

    // ---- Wi-Fi（wlanapi，只认 802.11 类网卡）----
    bool wifiPresent = false;   // 机器上有没有无线网卡
    bool wifiUp      = false;   // 有网卡且已连上
    int  wifiBars    = 0;       // 0..3

    // ---- 蓝牙（bthprops，问有没有无线电）----
    bool btPresent  = false;

    DWORD stamp = 0;            // 这次采集的时间（GetTickCount）
};

// 采集一次，状态有变化返回 true（调用方据此决定要不要重画）。
// 内部各自有节流：电池/音量每次都问（都很便宜），Wi-Fi 两秒一次，蓝牙三十秒一次
// —— 枚举无线电要几十毫秒，每半秒来一次会把出帧节奏拖垮。
bool PollSysStatus(SysStatus& s);

// ===================================================================
// 下面这几个是给「右上角图标点开的菜单」用的（2026-10-10）。
//
// 跟上面那份轮询的区别：轮询是**常驻**的，半秒一次，只回几个数；
// 这一组是**按需**的 —— 只有菜单真的要弹开时才调一次，可以慢一点、
// 也可以问得更细。混在一起会让每半秒的开销都背上一个网络扫描。
// ===================================================================

// 一个能看见的 Wi-Fi 网络
struct WifiNet {
    std::wstring ssid;      // 网络名
    std::wstring profile;   // 系统里存的配置名；空 = 没连过，要密码
    int  bars    = 0;       // 0..3，跟图标用同一套换算
    bool secured = false;   // 要不要密码
    bool connected = false; // 现在连的是不是它
};

// 现在连着的那个网络叫什么；没连上就是空串。
std::wstring WifiCurrentSsid();

// 扫一遍看得见的网络，信号从强到弱，最多 maxCount 个。
//
// ★ 为什么要过滤：wlanapi 会把「以前连过但现在不在范围内」的历史网络也报出来，
//   直接全列进菜单的话，里面会有一堆连不上的名字 —— 那是最典型的假信息。
//   只留 dwFlags 里标着「现在能连」的那些。
std::vector<WifiNet> EnumWifiNetworks(int maxCount);

// 连到某个网络（用它在系统里存的那份配置）。
//
// ★ 只管「连过的」网络：没连过的新网络要输密码，我们没有地方让用户输，
//   硬去调 WlanConnect 只会失败并且看不出为什么。那种情况 profile 是空串，
//   调用方应该改成打开系统的网络设置页，让用户自己连 —— 别假装能连。
bool ConnectWifi(const std::wstring& profile);

// 已经配对过的蓝牙设备名，最多 maxCount 个。
//
// ★ 拿不到就是拿不到（没蓝牙无线电 / 设备没配对），返回空 ——
//   菜单里宁可只写一句「没有已配对的设备」，也不编几个名字出来。
std::vector<std::wstring> EnumBtDevices(int maxCount);

// 2026-10-11 - AI - 点蓝牙设备真正发起连接
//
// 按名字在已配对设备里找到地址，把它的音频服务（A2DP/免提）启用一次 ——
// 蓝牙栈会去建这条链路，对耳机来说就是「连接」。
// 返回 true 只表示请求被接受（连接本身是异步的，耳机响还要一两秒）；
// 返回 false 时调用方应退回打开设置页。
bool ConnectBtDevice(const std::wstring& name);

// 2026-10-11 - AI - Wi-Fi 无线电软开关（控制中心磁贴用）
// 走 wlan_intf_opcode_radio_state 的软件状态，不需要驱动层硬开关。
// 返回 false = 没有网卡或系统拒绝（那种情况磁贴点击后仍回原状）。
bool SetWifiEnabled(bool on);
bool GetWifiEnabled();   // 当前软开关状态；拿不到返回 true（别把能用显示成关了）

// 屏幕亮度 0..100。笔记本内屏多数支持；外接显示器看型号。
// 返回 false = 这台机器取不到亮度控制（调用方把滑条藏掉，别画一个没用的）。
bool GetBrightness(int& pct);
bool SetBrightness(int pct);

// 音量：0..100。返回 false = 没拿到音量设备，别装作改成功了。
bool SetVolumePct(int pct);
bool SetMuted(bool mute);

// 电池还能撑几分钟；-1 = 系统说不知道（插着电、或者刚开机还没算出来）。
int BatteryMinutesLeft();

}  // namespace mydock
