// 2026-10-11 - AI - 控制中心面板：替代 Win+A 的系统磁贴
//
// 点顶栏那个「两排开关」图标（原先是发 Win+A 唤系统控制中心），
// 现在弹自己的 macOS 风格面板：2×2 磁贴 + 亮度/音量滑条 + 电池行。
// 外观照 macOS Big Sur 控制中心：白（深色 #1C1C1E）圆角卡、
// 打开的磁贴圆钮是系统蓝 #007AFF（深色 #0A84FF）、关着是灰。
//
// 功能边界（查过 API 之后定下的，不装假）：
//   Wi-Fi   —— 真开关（wlanapi 软件无线电，见 status.cpp SetWifiEnabled）
//   蓝牙    —— Windows 没有公开的无线电开关 API，点击开蓝牙设置页
//   显示器  —— 发 Win+P 唤系统投影菜单（那个菜单本来就是系统的，借它用）
//   静音    —— 真开关（IAudioEndpointVolume，status.cpp SetMuted）
//   亮度    —— dxva2 物理显示器亮度；机器不支持就整条滑条不画
//   音量    —— 真滑条（SetVolumePct）
#pragma once

#include <windows.h>
#include <gdiplus.h>
#include <string>

#include "status.h"

namespace mydock {

class ControlCenter {
    // 持有裸指针资源（Bitmap/HDC/HBITMAP），默认拷贝语义 = 双重释放隐患。
    // cppcheck 六个类各报一条之后统一禁掉拷贝（2026-10-12）
        ControlCenter(const ControlCenter&) = delete;
    ControlCenter& operator=(const ControlCenter&) = delete;

public:
    ControlCenter() = default;
    ~ControlCenter();

    // (ax, ay) = 锚点（屏幕坐标）；dark = 深浅色；sys = 打开瞬间的系统状态。
    // 阻塞到用户关掉为止。
    bool Toggle(HINSTANCE inst, int ax, int ay, bool dark, const SysStatus& sys);

    bool IsOpen() const { return open_; }

private:
    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void EnsureWindow(HINSTANCE inst);
    void CreateBackBuffer(int w, int h);
    void CaptureBackdrop();
    void Render();
    void DrawPanel(Gdiplus::Graphics& g);
    void DrawTile(Gdiplus::Graphics& g, int idx, const wchar_t* title,
                  const wchar_t* sub, const char* lucideName, bool on);
    void DrawSlider(Gdiplus::Graphics& g, int idx, const char* lucideName, int pct);
    void Finish();
    void RefreshSys();                     // 改了状态后重新采一遍

    HWND     hwnd_ = nullptr;
    HINSTANCE inst_ = nullptr;
    HDC      dc_ = nullptr;
    HBITMAP  bmp_ = nullptr;
    void*    bits_ = nullptr;
    Gdiplus::Bitmap* backdrop_ = nullptr;  // 毛玻璃底（拖滑条重画不重拍）

    int  w_ = 0, h_ = 0, pad_ = 0;
    int  winX_ = 0, winY_ = 0;
    bool open_ = false;
    bool looping_ = false;
    bool dark_ = true;

    SysStatus sys_;
    bool  wifiOn_ = true;
    std::wstring ssid_;          // 打开时缓存一次，Render 不再每帧去问 wlan
    int   bright_ = 80;
    bool  hasBright_ = false;   // 这台机器能不能调亮度（不能就藏滑条）

    // 布局缓存（Toggle 时算好，点击命中直接用）
    struct Rct { int x, y, w, h; };
    Rct tiles_[4] = {};         // 2×2 磁贴（0 WiFi 1 BT 2 显示器 3 静音）
    Rct knob_[4] = {};          // 每个磁贴里左侧的圆形开关按钮
    Rct slider_[2] = {};        // 亮度 / 音量（整行，含图标和轨道）
    Rct gear_ = {};             // 右下齿轮
    int  tileW_ = 0, tileH_ = 0;
    int  drag_ = -1;            // 正在拖哪条滑条：槽位下标（0/1），-1 = 没在拖
};

}  // namespace mydock
