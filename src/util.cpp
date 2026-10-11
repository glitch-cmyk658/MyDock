// 2026-10-09 - AI - 公共工具实现
#include "util.h"

#include <shlobj.h>
#include <tlhelp32.h>   // 2026-10-11 - AI - CreateToolhelp32Snapshot：按 exe 路径查进程在不在
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace mydock {
namespace {

std::mutex g_logLock;
int g_dpi = 0;
std::wstring g_exeDir;
bool g_exeDirReady = false;

}  // namespace

// 转小写。只处理 ASCII —— 路径里的盘符和 8.3 短名都是 ASCII，够用。
std::wstring Lower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& ch : out) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return out;
}

int DpiAt(int px, int dpi) {
    if (dpi <= 0) dpi = 96;
    return MulDiv(px, dpi, 96);
}

std::wstring NormalizePath(const std::wstring& in) {
    if (in.empty()) return in;
    std::wstring s = in;

    // ① 两端空白 + 配对引号。从资源管理器「复制文件地址」粘出来的行常常带着引号，
    //    不剥掉就是「明明写了这一行，Dock 上就是没有」。
    const size_t a = s.find_first_not_of(L" \t\r\n\"'");
    if (a == std::wstring::npos) return std::wstring();
    const size_t b = s.find_last_not_of(L" \t\r\n\"'");
    s = s.substr(a, b - a + 1);

    // ② \\?\ 和 \\.\ 前缀先摘下来：GetLongPathName 不认这两种写法，
    //    带着它这一步会直接失败，下面的 8.3 短名就展不开。
    //    摘了不装回去 —— 同一台机器上两种写法指的是同一个文件，
    //    统一成「没有前缀」更好比较。
    if (s.size() >= 4 && s[0] == L'\\' && s[1] == L'\\' &&
        (s[2] == L'?' || s[2] == L'.') && s[3] == L'\\') {
        s = s.substr(4);
    }

    // ③ 变绝对路径：吃掉 . / .. 和相对路径。
    //    顺带把「当前目录」这个隐含变量去掉了 —— 不这么做，同一条相对路径
    //    在不同时候会指向不同文件。
    constexpr DWORD kBuf = MAX_PATH * 4;
    wchar_t full[kBuf] = {};
    DWORD n = GetFullPathNameW(s.c_str(), kBuf, full, nullptr);
    if (n > 0 && n < kBuf) s.assign(full, n);

    // ④ 展开 8.3 短名。C:\PROGRA~1\... 和 C:\Program Files\... 是同一个文件，
    //    字符串却完全不同。路径不存在时这一步会失败，那就保持原样 ——
    //    不会比原来更差。
    wchar_t lng[kBuf] = {};
    n = GetLongPathNameW(s.c_str(), lng, kBuf);
    if (n > 0 && n < kBuf) s.assign(lng, n);

    // ⑤ 正斜杠统一成反斜杠（有些配置文件里写的是 Unix 风格）
    for (auto& c : s) if (c == L'/') c = L'\\';

    // ⑥ 去掉末尾多余的反斜杠。根目录 X:\ 和 UNC 根 \\server\share 要留着，
    //    所以设了长度下限。
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();

    // ⑦ 统一小写。大小写不敏感的比较用 Lower 比 _wcsicmp 更省事，
    //    因为这边要的是「能直接塞进 std::set 当主键」。
    return Lower(s);
}

// GetDpiForMonitor 住在 shcore.dll（Win8.1 才有）。这里动态加载：
// 静态链接要额外挂 -lshcore，而且在老系统上会因为 dll 不存在直接起不来。
// 拿不到就退回系统 DPI —— 最多是不准，不能是不跑。
typedef HRESULT(WINAPI * PFN_GDFM)(HMONITOR, int, UINT*, UINT*);
int MonitorDpi(HMONITOR mon) {
    // ★ dll 句柄和函数指针缓存起来。以前每次调用都 LoadLibrary/FreeLibrary ——
    //   Dock 的轮询 120ms 一次，等于每秒八九次加载卸载模块，
    //   loader 锁的竞争在系统忙时会放大成可感知的卡顿。
    static HMODULE shcore = LoadLibraryW(L"shcore.dll");   // 故意不卸载，进程活多久它活多久
    static PFN_GDFM gdfm = shcore
        ? reinterpret_cast<PFN_GDFM>(GetProcAddress(shcore, "GetDpiForMonitor"))
        : nullptr;

    int dpi = 0;
    if (mon && gdfm) {
        UINT dx = 0, dy = 0;
        if (SUCCEEDED(gdfm(mon, 0, &dx, &dy)) && dx > 0) {
            dpi = static_cast<int>(dx);
        }
    }
    if (dpi <= 0) {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef UINT(WINAPI * PFN2)();
        PFN2 fn = user32 ? reinterpret_cast<PFN2>(GetProcAddress(user32, "GetDpiForSystem"))
                         : nullptr;
        if (fn) dpi = static_cast<int>(fn());
    }
    return dpi > 0 ? dpi : 96;
}

namespace {

void FillMonitor(HMONITOR mon, MonitorInfo& mi) {
    mi.handle = mon;
    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (mon && GetMonitorInfoW(mon, reinterpret_cast<MONITORINFO*>(&info))) {
        mi.full = info.rcMonitor;
        mi.work = info.rcWork;
    } else {
        // 拿不到监视器信息（理论上不会）：退回 SM_CXSCREEN 的老算法，
        // 也就是 2026-10-11 之前的行为 —— 至少还能在主屏上正常显示。
        mi.full.left = 0;
        mi.full.top = 0;
        mi.full.right = GetSystemMetrics(SM_CXSCREEN);
        mi.full.bottom = GetSystemMetrics(SM_CYSCREEN);
        mi.work = mi.full;
    }
    mi.dpi = MonitorDpi(mi.handle);
}

}  // namespace

MonitorInfo MonitorAt(POINT pt) {
    MonitorInfo mi;
    // DEFAULTTONEAREST：坐标落在所有屏之外时（比如副屏刚拔掉，鼠标还在那儿），
    // 给最近的那一块，而不是主屏 —— 这样 Dock 不会突然跳到另一块屏上。
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!mon) mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    FillMonitor(mon, mi);
    return mi;
}

MonitorInfo PrimaryMonitor() {
    MonitorInfo mi;
    const POINT zero = {0, 0};
    HMONITOR mon = MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY);
    if (!mon) mon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    FillMonitor(mon, mi);
    return mi;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float d = std::min(radius * 2.0f, std::min(r.Width, r.Height));
    if (d <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

int Dpi(int px) {
    if (!g_dpi) {
        int d = 0;
        // GetDpiForSystem 是 Win10 1600+ 的接口
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        typedef UINT(WINAPI * PFN)();
        if (user32) {
            PFN fn = reinterpret_cast<PFN>(GetProcAddress(user32, "GetDpiForSystem"));
            if (fn) d = static_cast<int>(fn());
        }
        if (d <= 0) {
            HDC dc = GetDC(nullptr);
            if (dc) {
                d = GetDeviceCaps(dc, LOGPIXELSX);
                ReleaseDC(nullptr, dc);
            }
        }
        if (d <= 0) d = 96;
        g_dpi = d;
    }
    return MulDiv(px, g_dpi, 96);
}

void Log(const wchar_t* fmt, ...) {
    std::wstring dir = ConfigDir();
    if (dir.empty()) return;
    std::wstring path = dir + L"\\mydock.log";

    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::lock_guard<std::mutex> guard(g_logLock);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
        fwprintf(f, L"[%02d:%02d:%02d] %ls\n", st.wHour, st.wMinute, st.wSecond, buf);
        fclose(f);
    }
}

bool TraceOn() {
    static int cached = -2;
    if (cached == -2) {
        wchar_t buf[8] = {};
        cached = (GetEnvironmentVariableW(L"MYDOCK_TRACE", buf, 8) > 0) ? 1 : 0;
    }
    return cached == 1;
}

std::wstring ExeDir() {
    if (g_exeDirReady) return g_exeDir;
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    g_exeDir = buf;
    size_t pos = g_exeDir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) g_exeDir = g_exeDir.substr(0, pos);
    g_exeDirReady = true;
    return g_exeDir;
}

std::wstring ConfigDir() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return std::wstring();
    std::wstring dir = std::wstring(buf) + L"\\MyDock";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

bool ConfigFlag(const wchar_t* section, const wchar_t* key, bool def) {
    const std::wstring ini = ConfigDir() + L"\\settings.ini";
    const int v = GetPrivateProfileIntW(section, key, def ? 1 : 0, ini.c_str());
    return v != 0;
}

// 2026-10-11 - AI - 修「HICON 转出来的位图带黑底」
//
// 症状：Dock 右侧「正在运行但没固定」那几格，图标的透明部分全变成纯黑方块。
// 根因：原来直接 Gdiplus::Bitmap::FromHICON。它返回的位图内部**不带 HasAlpha 标志**
//       （实测 ImageFlags = 0x52010，缺 0x0002），而 GDI+ 的 DrawImage 一看到这个标志
//       没置位，就把整张图当不透明来处理 —— alpha 被无视，本该透明的地方直接露出
//       底下的 RGB=0，也就是黑。
//       固定区的图标没事，是因为它们走 BitmapFromHBITMAP，位图是自己 new 出来的、
//       天生带 HasAlpha；连 Clone(..., PixelFormat32bppARGB) 都继承不到这个标志，
//       所以只能在这里重建。
// 做法：取出 HICON 的色位图像素，交给 BitmapFromHBITMAP 重建一张 32bppARGB。
//       色位图不带 alpha 的老式图标（靠 AND mask 表达透明）另外补一次 mask。
namespace {

// 这张图里有没有「不是全不透明」的像素 —— 没有就说明它压根没带 alpha
bool HasTranslucentPixels(Gdiplus::Bitmap* bmp) {
    const int w = bmp->GetWidth();
    const int h = bmp->GetHeight();
    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData data = {};
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return true;  // 读不出来就别乱动，当作它自己有 alpha
    bool found = false;
    for (int y = 0; y < h && !found; ++y) {
        const BYTE* p = static_cast<const BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < w; ++x) {
            if (p[x * 4 + 3] != 255) { found = true; break; }
        }
    }
    bmp->UnlockBits(&data);
    return found;
}

// 用图标的 AND mask 把该透明的地方压成 alpha=0（mask 位为 1 = 透明）
void ApplyIconMask(Gdiplus::Bitmap* bmp, HBITMAP hbmMask) {
    const int w = bmp->GetWidth();
    const int h = bmp->GetHeight();

    BITMAP bm = {};
    if (!GetObjectW(hbmMask, sizeof(bm), &bm)) return;

    // mask 是 1bpp 的，直接按 32bpp 取出来，省得自己拆位
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> buf(static_cast<size_t>(bm.bmWidth) * static_cast<size_t>(bm.bmHeight) * 4);
    HDC dc = GetDC(nullptr);
    const int got = GetDIBits(dc, hbmMask, 0, static_cast<UINT>(bm.bmHeight),
                              buf.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (got == 0) return;

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData data = {};
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return;
    for (int y = 0; y < h && y < bm.bmHeight; ++y) {
        BYTE* d = static_cast<BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
        const BYTE* s = buf.data() + static_cast<size_t>(y) * static_cast<size_t>(bm.bmWidth) * 4;
        for (int x = 0; x < w && x < bm.bmWidth; ++x) {
            if (s[x * 4] > 127) d[x * 4 + 3] = 0;
        }
    }
    bmp->UnlockBits(&data);
}

}  // namespace

Gdiplus::Bitmap* BitmapFromIcon(HICON icon) {
    if (!icon) return nullptr;

    ICONINFO ii = {};
    if (GetIconInfo(icon, &ii)) {
        Gdiplus::Bitmap* out = nullptr;
        if (ii.hbmColor) {
            out = BitmapFromHBITMAP(ii.hbmColor);
            if (out && ii.hbmMask && !HasTranslucentPixels(out)) ApplyIconMask(out, ii.hbmMask);
        }
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (out) return out;
    }

    // 兜底：万一位图取不到，还是交给 GDI+ 自己转一次
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHICON(icon);
    if (!bmp) return nullptr;
    if (bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    return bmp;
}

Gdiplus::Bitmap* BitmapFromHBITMAP(HBITMAP hbmp) {
    if (!hbmp) return nullptr;

    BITMAP bm = {};
    if (!GetObjectW(hbmp, sizeof(bm), &bm)) return nullptr;
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    if (w <= 0 || h <= 0) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 负值 = 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    HDC dc = GetDC(nullptr);
    int got = GetDIBits(dc, hbmp, 0, static_cast<UINT>(h), buf.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (got == 0) return nullptr;

    // 有些来源给的是不带 alpha 的位图，整张全 0 时补成不透明
    bool anyAlpha = false;
    for (size_t i = 3; i < buf.size(); i += 4) {
        if (buf[i] != 0) { anyAlpha = true; break; }
    }
    if (!anyAlpha) {
        for (size_t i = 3; i < buf.size(); i += 4) buf[i] = 255;
    }

    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) {
        delete out;
        return nullptr;
    }

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData data = {};
    if (out->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        delete out;
        return nullptr;
    }
    for (int y = 0; y < h; ++y) {
        BYTE* dst = static_cast<BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
        const BYTE* src = buf.data() + static_cast<size_t>(y) * static_cast<size_t>(w) * 4;
        memcpy(dst, src, static_cast<size_t>(w) * 4);
    }
    out->UnlockBits(&data);
    return out;
}

Gdiplus::Bitmap* TrimTransparent(Gdiplus::Bitmap* src) {
    if (!src) return nullptr;

    const int w = src->GetWidth();
    const int h = src->GetHeight();
    if (w <= 0 || h <= 0) return src;

    Gdiplus::Rect rect(0, 0, w, h);
    Gdiplus::BitmapData data = {};
    if (src->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok)
        return src;

    auto at = [&](int x, int y) -> const BYTE* {
        return static_cast<const BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride +
               static_cast<size_t>(x) * 4;
    };
    auto similar = [](const BYTE* a, const BYTE* b, int tol) {
        return std::abs(static_cast<int>(a[0]) - static_cast<int>(b[0])) <= tol &&
               std::abs(static_cast<int>(a[1]) - static_cast<int>(b[1])) <= tol &&
               std::abs(static_cast<int>(a[2]) - static_cast<int>(b[2])) <= tol &&
               std::abs(static_cast<int>(a[3]) - static_cast<int>(b[3])) <= tol;
    };

    // 有些图标（.url 快捷方式尤其常见）是「纯色底板 + 中央小图案」：
    // 四周是不透明的白色，光看 alpha 一点都裁不掉，画到浅色背景上就只剩中间一小块。
    // 四角颜色一致且不透明时，把这种底板色也当成边距。
    const BYTE* corner = at(0, 0);
    const bool uniformCorner = similar(corner, at(w - 1, 0), 24) &&
                               similar(corner, at(0, h - 1), 24) &&
                               similar(corner, at(w - 1, h - 1), 24);
    const bool colorTrim = uniformCorner && corner[3] > 200;

    const int kAlphaLow = 12;
    const int kAlphaHigh = 100;
    const int kColorTol = 26;

    std::vector<int> colLo(static_cast<size_t>(w), 0), rowLo(static_cast<size_t>(h), 0);
    std::vector<int> colHi(static_cast<size_t>(w), 0), rowHi(static_cast<size_t>(h), 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const BYTE* p = at(x, y);
            const BYTE a = p[3];
            bool isBg = a <= kAlphaLow;
            if (!isBg && colorTrim) {
                isBg = std::abs(static_cast<int>(p[0]) - static_cast<int>(corner[0])) <= kColorTol &&
                       std::abs(static_cast<int>(p[1]) - static_cast<int>(corner[1])) <= kColorTol &&
                       std::abs(static_cast<int>(p[2]) - static_cast<int>(corner[2])) <= kColorTol;
            }
            if (isBg) continue;
            colLo[static_cast<size_t>(x)]++;
            rowLo[static_cast<size_t>(y)]++;
            if (a > kAlphaHigh) {
                colHi[static_cast<size_t>(x)]++;
                rowHi[static_cast<size_t>(y)]++;
            }
        }
    }

    // 不能「有一个非背景像素就把这一行算作内容」——很多图标边缘散着几个几乎看不见的
    // 淡像素，那样包围盒会被撑满整张画布，等于白裁。要求这一行/列有足够多的内容像素才算数。
    const int needX = std::max(4, h / 32);
    const int needY = std::max(4, w / 32);

    auto bounds = [&](const std::vector<int>& cols, const std::vector<int>& rows,
                      int& x0, int& y0, int& x1, int& y1) {
        x0 = y0 = -1;
        x1 = y1 = -1;
        for (int x = 0; x < w; ++x) {
            if (cols[static_cast<size_t>(x)] >= needX) {
                if (x0 < 0) x0 = x;
                x1 = x;
            }
        }
        for (int y = 0; y < h; ++y) {
            if (rows[static_cast<size_t>(y)] >= needY) {
                if (y0 < 0) y0 = y;
                y1 = y;
            }
        }
    };

    int minX = -1, minY = -1, maxX = -1, maxY = -1;
    bounds(colLo, rowLo, minX, minY, maxX, maxY);

    // 包围盒几乎等于整张画布时，多半是「中央图案 + 一圈极淡的光晕」那类图标
    // （.url 快捷方式尤其典型：四角是透明的，但四条边的中点 alpha 只有三四十）。
    // 这时用更高的阈值重算一次，只认真正看得见的那些像素。
    if (minX >= 0 && (maxX - minX + 1) > w * 9 / 10 && (maxY - minY + 1) > h * 9 / 10) {
        int hx0 = -1, hy0 = -1, hx1 = -1, hy1 = -1;
        bounds(colHi, rowHi, hx0, hy0, hx1, hy1);
        if (hx0 >= 0 && (hx1 - hx0 + 1) < (maxX - minX + 1)) {
            minX = hx0;
            minY = hy0;
            maxX = hx1;
            maxY = hy1;
        }
    }
    src->UnlockBits(&data);

    if (maxX < 0) return src;                  // 整张全透明，原样交回
    const int nw = maxX - minX + 1;
    const int nh = maxY - minY + 1;
    if (nw >= w && nh >= h) return src;        // 本来就没有边距
    if (nw < w / 5 || nh < h / 5) return src;  // 裁得太狠，多半是误判，宁可不裁

    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(nw, nh, PixelFormat32bppARGB);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) {
        delete out;
        return src;
    }
    {
        Gdiplus::Graphics g(out);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(src, Gdiplus::Rect(0, 0, nw, nh), minX, minY, nw, nh, Gdiplus::UnitPixel);
        g.Flush();
    }
    delete src;
    return out;
}

Gdiplus::Bitmap* DownscaleIconToMaxSide(Gdiplus::Bitmap* src, int maxSide) {
    if (!src || maxSide <= 0) return src;

    const int w = src->GetWidth();
    const int h = src->GetHeight();
    if (w <= 0 || h <= 0) return src;

    const int longest = (w > h) ? w : h;
    if (longest <= maxSide) return src;  // 本来就不大，别多此一举

    const double k = static_cast<double>(maxSide) / static_cast<double>(longest);
    const int nw = std::max(1, static_cast<int>(w * k + 0.5));
    const int nh = std::max(1, static_cast<int>(h * k + 0.5));

    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(nw, nh, PixelFormat32bppARGB);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) {
        delete out;
        return src;
    }
    {
        Gdiplus::Graphics g(out);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(src, Gdiplus::Rect(0, 0, nw, nh), 0, 0, w, h, Gdiplus::UnitPixel);
        g.Flush();
    }
    delete src;
    return out;
}

Gdiplus::Bitmap* CaptureScreenArea(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return nullptr;

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!bmp) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return nullptr;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    // CAPTUREBLT：连分层窗口一起拍进来
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    Gdiplus::Bitmap* out = BitmapFromHBITMAP(bmp);
    DeleteObject(bmp);
    return out;
}

Gdiplus::Bitmap* BlurBitmap(Gdiplus::Bitmap* src, int factor) {
    if (!src) return nullptr;
    if (factor < 2) return src;

    const int w = src->GetWidth();
    const int h = src->GetHeight();
    if (w <= 0 || h <= 0) {
        delete src;
        return nullptr;
    }
    const int sw = std::max(1, w / factor);
    const int sh = std::max(1, h / factor);

    Gdiplus::Bitmap* small = new Gdiplus::Bitmap(sw, sh, PixelFormat32bppARGB);
    if (!small || small->GetLastStatus() != Gdiplus::Ok) {
        delete small;
        delete src;
        return nullptr;
    }
    {
        Gdiplus::Graphics g(small);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(src, Gdiplus::Rect(0, 0, sw, sh), 0, 0, w, h, Gdiplus::UnitPixel);
        g.Flush();
    }
    delete src;

    Gdiplus::Bitmap* out = new Gdiplus::Bitmap(w, h, PixelFormat32bppARGB);
    if (!out || out->GetLastStatus() != Gdiplus::Ok) {
        delete out;
        delete small;
        return nullptr;
    }
    {
        Gdiplus::Graphics g(out);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(small, Gdiplus::Rect(0, 0, w, h), 0, 0, sw, sh, Gdiplus::UnitPixel);
        g.Flush();
    }
    delete small;
    return out;
}

Gdiplus::Bitmap* MakeNoiseTexture(int size) {
    if (size <= 0) return nullptr;
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }

    Gdiplus::Rect rect(0, 0, size, size);
    Gdiplus::BitmapData data = {};
    if (bmp->LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }

    unsigned seed = 0x9E3779B9u;
    for (int y = 0; y < size; ++y) {
        BYTE* row = static_cast<BYTE*>(data.Scan0) + static_cast<size_t>(y) * data.Stride;
        for (int x = 0; x < size; ++x) {
            seed = seed * 1103515245u + 12345u;
            const BYTE v = static_cast<BYTE>((seed >> 16) & 0xFF);
            row[static_cast<size_t>(x) * 4 + 0] = v;
            row[static_cast<size_t>(x) * 4 + 1] = v;
            row[static_cast<size_t>(x) * 4 + 2] = v;
            row[static_cast<size_t>(x) * 4 + 3] = 10;  // 很淡，只是加一点颗粒感
        }
    }
    bmp->UnlockBits(&data);
    return bmp;
}

// ---- 找「属于某个 exe 的窗口」 ----
namespace {

struct FindCtx {
    std::wstring lowerPathW;   // 统一转小写后的 exe 路径
    HWND found;
    bool acceptHidden = false; // 第二轮才打开：见 FindProcessWindowAny 的说明
};

BOOL CALLBACK FindProcEnum(HWND h, LPARAM lp) {
    FindCtx* c = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h)) {
        if (!c->acceptHidden) return TRUE;
        // 藏起来的窗口里混着各种后台东西，多卡两道：
        //   工具窗口不是主窗口；连标题都没有的窗口切过去也没意义。
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
        if (GetWindowTextLengthW(h) == 0) return TRUE;
    }
    // 对话框、工具窗口不算 —— 它们是主窗口的一部分，切过去没意义
    if (GetWindow(h, GW_OWNER)) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return TRUE;

    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return TRUE;

    wchar_t buf[MAX_PATH] = {};
    DWORD sz = MAX_PATH;
    BOOL stop = FALSE;
    if (QueryFullProcessImageNameW(p, 0, buf, &sz)) {
        // 路径大小写可能不一致（C:\ 和 c:\），统一小写再比
        std::wstring s = buf;
        for (auto& ch : s) {
            if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
        if (s == c->lowerPathW) {
            c->found = h;
            stop = TRUE;
        }
    }
    CloseHandle(p);
    return stop ? FALSE : TRUE;
}

}  // namespace

namespace {


HWND FindProcWindowImpl(const std::wstring& exePath, bool acceptHidden) {
    if (exePath.empty()) return nullptr;

    FindCtx ctx = {};
    ctx.lowerPathW = NormalizePath(exePath);
    ctx.found = nullptr;
    ctx.acceptHidden = acceptHidden;

    EnumWindows(FindProcEnum, reinterpret_cast<LPARAM>(&ctx));
    return ctx.found;
}

}  // namespace

HWND FindProcessWindow(const std::wstring& exePath) {
    return FindProcWindowImpl(exePath, false);
}

HWND FindProcessWindowAny(const std::wstring& exePath) {
    // 先按老规矩找看得见的；找不到再回头接受藏起来的
    HWND w = FindProcWindowImpl(exePath, false);
    if (w) return w;
    return FindProcWindowImpl(exePath, true);
}

bool IsProcessRunning(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring want = NormalizePath(exePath);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            // 有些进程（系统保护进程）打不开，跳过去就行，不影响别的
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!p) continue;
            wchar_t buf[MAX_PATH] = {};
            DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(p, 0, buf, &sz) && NormalizePath(buf) == want) found = true;
            CloseHandle(p);
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------- 自启/重启
bool IsAutoStart() {
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf);
    const LSTATUS r = RegGetValueW(HKEY_CURRENT_USER,
                                   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                   L"MyDock", RRF_RT_REG_SZ, nullptr, buf, &sz);
    return r == ERROR_SUCCESS && buf[0];
}

bool SetAutoStart(bool on) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) {
        Log(L"自启：打不开注册表 Run 键（错误码 %lu）", GetLastError());
        return false;
    }
    bool ok = false;
    if (on) {
        const std::wstring v = L"\"" + std::wstring(path) + L"\"";
        ok = RegSetValueExW(k, L"MyDock", 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(v.c_str()),
                            static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(k, L"MyDock") == ERROR_SUCCESS;
    }
    RegCloseKey(k);
    Log(L"自启：%ls%ls", on ? L"开启" : L"关闭", ok ? L"" : L"（失败）");
    return ok;
}

bool QueueRestart() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    // 1 秒后再拉起：等本进程退出、单实例互斥体释放，
    // 否则新进程会被「已在运行」检查挡回去直接退出。
    const std::wstring args =
        L"/c timeout /t 1 /nobreak >nul & start \"\" \"" + std::wstring(path) + L"\"";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", L"cmd.exe", args.c_str(),
                                      nullptr, SW_HIDE);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

}  // namespace mydock
