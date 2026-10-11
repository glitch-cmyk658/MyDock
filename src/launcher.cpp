// 2026-10-09 - AI - 启动台实现：全屏毛玻璃 + 应用网格 + 打字筛选 + 固定到 Dock
#include "launcher.h"
#include "appaction.h"
#include "util.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <imm.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace mydock {
namespace {

const wchar_t* kClass = L"MyDockLauncherClass";
const UINT_PTR kTimerAnim = 1;

std::wstring ToLower(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return r;
}

// 从输入法取正在拼的字 / 上屏的字
std::wstring ImmStr(HIMC h, DWORD type) {
    const LONG len = ImmGetCompositionStringW(h, type, nullptr, 0);
    if (len <= 0) return {};
    std::wstring s(static_cast<size_t>(len) / sizeof(wchar_t), L'\0');
    ImmGetCompositionStringW(h, type, &s[0], len);
    return s;
}

// 只在 TRACE 时用的现场记录：Close() 那一行要能说清楚「是谁关的」
UINT g_lastMsg = 0;
int g_lastMouseX = -1;
int g_lastMouseY = -1;

}  // namespace

Launcher::~Launcher() {
    if (hwnd_) DestroyWindow(hwnd_);
    delete glass_;
    delete noise_;
    delete bg_;
    delete canvas_;
    delete menuShadow_;
    FreeAppIcons(apps_);
    if (dc_) DeleteDC(dc_);
    if (bmp_) DeleteObject(bmp_);
}

// ★ 把「已移出启动台」的那些（清单在 hidden.txt）从 apps_ 里剔掉。
//   Create 和 SetApps 都要走这一条 —— 只在启动时过滤是不够的：
//   Dock 菜单里的「刷新应用列表」/「恢复已移出的应用」都会重建整个列表，
//   漏了过滤的话刚移出去的应用刷新一下又冒回来了。
void Launcher::DropHidden() {
    const std::vector<std::wstring> hidden = LoadHiddenList();
    if (hidden.empty()) return;

    std::vector<AppItem> keep;
    keep.reserve(apps_.size());
    int removed = 0;
    for (AppItem& a : apps_) {
        if (IsHidden(hidden, a.lnkPath)) {
            delete a.icon;      // 图标是深拷贝来的，留着白占内存
            ++removed;
        } else {
            keep.push_back(std::move(a));
        }
    }
    if (removed > 0) {
        apps_ = std::move(keep);
        Log(L"启动台：按 hidden.txt 移除了 %d 个应用，剩 %zu 个", removed, apps_.size());
    }
}

void Launcher::SetApps(std::vector<AppItem>&& list) {
    FreeAppIcons(apps_);
    apps_ = std::move(list);
    DropHidden();
    MergeAddedApps();
}

// 把用户手动加过的应用（added.txt）并进列表。
// Create 和 SetApps 都要走：Dock 菜单里的「刷新应用列表」会重建整个列表，
// 漏了这一步的话手动加的那几个刷新一下就没了。
void Launcher::MergeAddedApps() {
    const std::vector<std::wstring> added = LoadAddedList();
    if (added.empty()) return;

    int n = 0;
    for (const std::wstring& p : added) {
        bool dup = false;
        for (const AppItem& a : apps_) {
            if (_wcsicmp(a.lnkPath.c_str(), p.c_str()) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            Log(L"启动台：手动添加项已经不在了 %s（还留在 added.txt 里）", p.c_str());
        }
        AppItem it = MakeAppFromFile(p);
        it.icon = nullptr;          // 图标等打开时再取（ EnsureIcons 里做）
        apps_.push_back(std::move(it));
        ++n;
    }
    if (n <= 0) return;

    // 按名字排回去 —— 列表本来是有序的，插在末尾的话同一个字母区间会冒出来两块
    std::sort(apps_.begin(), apps_.end(),
              [](const AppItem& a, const AppItem& b) { return a.name < b.name; });
    Log(L"启动台：按 added.txt 并进 %d 个手动添加的应用，现在共 %zu 个", n, apps_.size());
}

// ---- 左上角的「+」：往启动台里加一个 exe / lnk ----
//
// ★ 为什么不是「macOS 那种拖拽」：macOS 的启动台是常驻全屏的，拖得动；
//   我们这个一开就是盖住整块屏幕的最上层窗口，任何文件管理器都会在它**下面** ——
//   拖不进来。所以这里开的是系统那个「选择文件」对话框：它是模态的，
//   会浮在全屏之上，找到 exe/lnk 点一下打开就加进来了。
//   （要是以后真想要「先打开资源管理器慢慢找、再拖进来」，那得把
//    启动台先收起来、再接一套 IDropTarget，是另一件事。）
void Launcher::AddButtonRect(int& x, int& y, int& w, int& h) const {
    w = h = Dpi(46);
    x = Dpi(40);
    y = Dpi(46);        // 跟搜索框同一高度，视觉上一条线上
}

bool Launcher::HitAddButton(int x, int y) const {
    int bx = 0, by = 0, bw = 0, bh = 0;
    AddButtonRect(bx, by, bw, bh);
    return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

void Launcher::DrawAddButton(Gdiplus::Graphics& g) {
    int bx = 0, by = 0, bw = 0, bh = 0;
    AddButtonRect(bx, by, bw, bh);

    Gdiplus::GraphicsPath cp;
    AddRoundRect(cp, Gdiplus::RectF(static_cast<float>(bx), static_cast<float>(by),
                                    static_cast<float>(bw), static_cast<float>(bh)),
                 static_cast<float>(bh) / 2.0f);
    Gdiplus::SolidBrush cb(addHover_ ? Gdiplus::Color(120, 255, 255, 255)
                                     : Gdiplus::Color(52, 255, 255, 255));
    g.FillPath(&cb, &cp);
    Gdiplus::Pen ce(Gdiplus::Color(70, 255, 255, 255), 1.0f);
    g.DrawPath(&ce, &cp);

    const float cx = static_cast<float>(bx) + static_cast<float>(bw) / 2.0f;
    const float cy = static_cast<float>(by) + static_cast<float>(bh) / 2.0f;
    const float s = static_cast<float>(Dpi(8));
    Gdiplus::Pen plus(Gdiplus::Color(addHover_ ? 255 : 205, 255, 255, 255),
                      std::max(1.8f, static_cast<float>(Dpi(2))));
    g.DrawLine(&plus, cx - s, cy, cx + s, cy);
    g.DrawLine(&plus, cx, cy - s, cx, cy + s);
}

void Launcher::AddAppByPath(const std::wstring& path) {
    if (path.empty()) return;
    AppItem it = MakeAppFromFile(path);
    const std::wstring nm = it.name;
    apps_.push_back(std::move(it));
    std::sort(apps_.begin(), apps_.end(),
              [](const AppItem& a, const AppItem& b) { return a.name < b.name; });

    AddAppPath(path);       // 记下来，下次启动还在

    query_.clear();
    compQuery_.clear();
    active_ = -1;
    ApplyFilter();
    LayoutGrid();
    EnsureIcons();
    dirty_ = true;
    RequestAnim();
    Log(L"启动台：已加入 %s（现在 %zu 个）", nm.c_str(), apps_.size());
}

void Launcher::AddAppFromDialog() {
    // 用系统那个「选择文件」对话框（IFileDialog）：比 GetOpenFileName 现代，
    // 而且 COM 在 main 里已经初始化过了。
    IFileDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) {
        Log(L"启动台：打不开文件选择框（0x%08lX）", static_cast<unsigned long>(hr));
        return;
    }

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                        FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR);
    }
    dlg->SetTitle(L"选择要加入启动台的应用");
    const COMDLG_FILTERSPEC ft[] = {
        {L"应用与快捷方式 (*.exe;*.lnk)", L"*.exe;*.lnk"},
        {L"所有文件 (*.*)", L"*.*"},
    };
    dlg->SetFileTypes(2, ft);
    dlg->SetFileTypeIndex(1);

    // 从桌面开始找：手动添加的多半是手动下载的绿色软件，桌面可能性最大
    wchar_t desk[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk))) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(desk, nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }

    hr = dlg->Show(hwnd_);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(hr)) {
        dlg->Release();
        return;
    }

    IShellItem* item = nullptr;
    hr = dlg->GetResult(&item);
    dlg->Release();
    if (FAILED(hr) || !item) return;

    wchar_t* raw = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
        const std::wstring path = raw;
        CoTaskMemFree(raw);
        item->Release();
        AddAppByPath(path);
        return;
    }
    item->Release();
}

bool Launcher::Create(HINSTANCE inst, HWND owner, std::vector<AppItem> allApps) {
    inst_ = inst;
    dock_ = owner;
    apps_ = std::move(allApps);
    DropHidden();
    MergeAddedApps();

    if (apps_.empty()) {
        Log(L"启动台：没有可用应用，不创建窗口");
        return false;
    }

    // ★ 启动台固定在主屏 —— 这是刻意的，不是漏改：
    //   Dock 是「鼠标在哪跟到哪」，启动台是「一次只有一个的全屏遮罩」，
    //   macOS 的 Launchpad 也只出现在主屏。副屏用户按 F4 时，遮罩亮在主屏
    //   与其习惯一致（眼睛通常正看着主屏）。真想要「跟鼠标」的话，
    //   把这里和 UpdateTheme 里的主屏矩形换成 MonitorAt(鼠标) 即可。
    w_ = GetSystemMetrics(SM_CXSCREEN);
    h_ = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // ★ 这里刻意不加 WS_EX_NOACTIVATE。
    //   启动台要能打字搜索，就必须拿得到键盘焦点；NOACTIVATE 的窗口收不到 WM_CHAR。
    //   （Dock 那条是 NOACTIVATE 的，因为它只需要看鼠标，不抢焦点反而更好。）
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kClass, L"MyDock 启动台", WS_POPUP,
        0, 0, w_, h_, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        Log(L"启动台：创建窗口失败，错误码 %lu", GetLastError());
        return false;
    }

    CreateBackBuffer(w_, h_);
    noise_ = MakeNoiseTexture(96);

    // ★ 输入法：留着，不摘。
    //
    // 走过一段弯路，记下来免得以后又踩：
    //   最早发现中文输入法激活时按字母键，窗口只收到 vk=229（VK_PROCESSKEY）的
    //   WM_KEYDOWN，既没有 WM_CHAR 也没有 WM_IME_COMPOSITION，字就凭空消失了。
    //   当时的办法是 ImmAssociateContext(hwnd_, nullptr) 直接把输入法摘掉 ——
    //   字母确实进来了，但**中文名就永远搜不到了**（拼音组字必须靠输入法）。
    //
    //   实测里那个 229 是模拟按键打出来的，行为的参考价值有限；真实键盘下输入法
    //   是照常发 WM_IME_COMPOSITION 的。所以现在改成三条路并存：
    //     ① 输入法切到英文模式（搜狗/微软拼音按一下 Shift）→ 走 WM_CHAR，最顺；
    //     ② 输入法在中文模式 → 走 WM_IME_COMPOSITION，拼音边打边搜，回车上屏成中文；
    //     ③ 万一输入法吞了键又没给 composition → VK_PROCESSKEY 兜底反推原键。
    //   ① 和 ② 都通，就既能搜英文名也能搜中文名了。

    Log(L"启动台就绪：%zu 个应用，窗口 %dx%d", apps_.size(), w_, h_);
    return true;
}

void Launcher::CreateBackBuffer(int w, int h) {
    if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
    if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }

    HDC screen = GetDC(nullptr);
    dc_ = CreateCompatibleDC(screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    bmp_ = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (bmp_) SelectObject(dc_, bmp_);
    ReleaseDC(nullptr, screen);
}

// ---- 布局 ----
//
// ★ 关键：格子是按「全部应用的数量」定的，不是按筛选后的结果。
// 若按筛选结果算，每敲一个字条目数就变少、格子就会变大 —— 边打字边看着整个网格在
// 缩放位移，非常晃。按总数定死，几何就稳定了，打字时只有「条目变少」这一件事在变。
void Launcher::LayoutGrid() {
    const int n = static_cast<int>(apps_.size());
    if (n <= 0) return;

    const int marginX = Dpi(80);
    const int topArea = Dpi(120);     // 上面留给搜索框
    const int bottomArea = Dpi(70);
    const int availW = w_ - marginX * 2;
    const int availH = h_ - topArea - bottomArea;

    iconSize_ = Dpi(64);
    // 不为了「硬塞进一页」把图标压得越来越小 —— 那样名字会挤成一团。
    // 到 Dpi(44) 就停，装不下的靠翻页。
    const int minSize = Dpi(44);
    while (iconSize_ > minSize) {
        cellW_ = iconSize_ + Dpi(48);
        cellH_ = iconSize_ + Dpi(74);   // 名字最多占两行，所以格子要比图标高出一截
        cols_ = std::max(1, availW / cellW_);
        rows_ = std::max(1, availH / cellH_);
        if (cols_ * rows_ >= n) break;
        iconSize_ -= Dpi(8);
    }
    cellW_ = iconSize_ + Dpi(48);
    cellH_ = iconSize_ + Dpi(74);
    cols_ = std::max(1, availW / cellW_);
    // 一页最多这么多行；放不下的那些靠翻页去够（跟 Launchpad 一样）
    const int maxRows = std::max(1, availH / cellH_);
    rows_ = std::min((n + cols_ - 1) / cols_, maxRows);

    perPage_ = cols_ * rows_;
    const int gridW = cols_ * cellW_;
    const int gridH = rows_ * cellH_;
    gridX_ = (w_ - gridW) / 2;
    gridY_ = topArea + std::max(0, (availH - gridH) / 2);

    // 把几何打进日志：外面用脚本模拟点鼠标时要靠它算图标中心，
    // 之前凭感觉猜坐标，正好戳在两格中间的空隙里，白折腾了一轮。
    if (TraceOn()) {
        Log(L"启动台 TRACE：网格 %dx%d 格（每格 %dx%d），原点 (%d,%d)，一页 %d 格，共 %d 页",
            cols_, rows_, cellW_, cellH_, gridX_, gridY_, perPage_, PageCount());
        Log(L"启动台 TRACE：第 0 格中心 (%d,%d)，第 1 行各列中心间隔 %d",
            gridX_ + cellW_ / 2, gridY_ + cellH_ / 2, cellW_);
    }
}

int Launcher::PageCount() const {
    if (shown_.empty() || perPage_ <= 0) return 1;
    return (static_cast<int>(shown_.size()) + perPage_ - 1) / perPage_;
}

int Launcher::PageSlots() const {
    if (perPage_ <= 0) return 0;
    const int remain = static_cast<int>(shown_.size()) - page_ * perPage_;
    return std::max(0, std::min(perPage_, remain));
}

int Launcher::AppIndex(int slot) const {
    if (slot < 0 || perPage_ <= 0) return -1;
    const int idx = page_ * perPage_ + slot;
    if (idx < 0 || idx >= static_cast<int>(shown_.size())) return -1;
    return shown_[static_cast<size_t>(idx)];
}

bool Launcher::ChangePage(int delta) {
    const int pages = PageCount();
    if (pages <= 1) return false;
    int p = page_ + delta;
    if (p < 0) p = 0;
    if (p >= pages) p = pages - 1;
    if (p == page_) return false;
    page_ = p;
    dirty_ = true;
    RequestAnim();
    return true;
}

void Launcher::ApplyFilter() {
    shown_.clear();
    if (query_.empty() && compQuery_.empty()) {
        shown_.reserve(apps_.size());
        for (int i = 0; i < static_cast<int>(apps_.size()); ++i) shown_.push_back(i);
    } else {
        const std::wstring q = ToLower(query_ + compQuery_);
        for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
            if (ToLower(apps_[i].name).find(q) != std::wstring::npos) shown_.push_back(i);
        }
    }
    // 选中项：没输入查询的时候谁也不选。
    // 一开始是「总选第一个」，结果一打开启动台，第一个图标就顶着一个选中框，
    // 跟鼠标悬停的高亮长得几乎一样，分不出哪个是鼠标指着的。
    // 一旦开始输入就选第一个 —— 这样敲完字直接回车，启动的就是最匹配的那个。
    const bool searching = !(query_ + compQuery_).empty();
    if (active_ >= static_cast<int>(shown_.size())) active_ = -1;
    if (active_ < 0 && searching && !shown_.empty()) active_ = 0;

    // 筛选后条目变少，页码可能已经越界 —— 拉回最后一页
    const int pages = PageCount();
    if (page_ >= pages) page_ = pages - 1;
    if (page_ < 0) page_ = 0;
    dirty_ = true;

    if (TraceOn()) {
        std::wstring names;
        const int lim = static_cast<int>(shown_.size()) < 10 ? static_cast<int>(shown_.size()) : 10;
        for (int i = 0; i < lim; ++i) {
            if (i) names += L"、";
            names += apps_[static_cast<size_t>(shown_[static_cast<size_t>(i)])].name;
        }
        Log(L"启动台 TRACE：筛选[%s] → %zu 个（最多列 10 个）：%s",
            (query_ + compQuery_).c_str(), shown_.size(), names.c_str());
    }
}

void Launcher::EnsureIcons() {
    const DWORD t0 = GetTickCount();
    int loaded = 0;
    for (int slot = 0; slot < static_cast<int>(shown_.size()); ++slot) {
        AppItem& a = apps_[static_cast<size_t>(shown_[static_cast<size_t>(slot)])];
        if (a.icon) continue;
        // 手动加进来的 .exe 没有 .lnk 路径，只能用它自己的路径去问 Shell 要图标
        const std::wstring& src = a.lnkPath.empty() ? a.target : a.lnkPath;
        Gdiplus::Bitmap* ic = src.empty() ? nullptr : LoadIconFromShellItem(src);
        if (!ic) ic = LoadSystemIcon(a.iconIndex);
        if (ic) ic = DownscaleIconToMaxSide(ic, iconSize_);
        a.icon = ic;
        ++loaded;
    }
    if (loaded > 0) {
        Log(L"启动台：加载 %d 个图标，用时 %lums", loaded,
            static_cast<unsigned long>(GetTickCount() - t0));
    }
}

// 搜索框的矩形。画面里要画它，输入法候选窗也要对齐它，所以抽出来只算一次。
void Launcher::SearchBoxRect(int& x, int& y, int& w, int& h) const {
    w = Dpi(360);
    h = Dpi(46);
    x = (w_ - w) / 2;
    y = Dpi(46);
}

// 自绘的搜索框没有真正的「光标」，输入法不知道该把候选窗放哪 —— 默认会飘到屏幕
// 左上角去。这里主动告诉它：候选窗跟在搜索框正下方。不设的话打中文时选字窗会
// 跑到离搜索框十万八千里的地方。
void Launcher::ImeFollowCaret() {
    if (!hwnd_) return;
    HIMC h = ImmGetContext(hwnd_);
    if (!h) return;

    int bx = 0, by = 0, bw = 0, bh = 0;
    SearchBoxRect(bx, by, bw, bh);

    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = bx + bw / 2;      // 框里的字是居中的，光标就取中间
    cf.ptCurrentPos.y = by + bh + Dpi(4);
    ImmSetCompositionWindow(h, &cf);

    CANDIDATEFORM cdf = {};
    cdf.dwStyle = CFS_CANDIDATEPOS;
    cdf.ptCurrentPos = cf.ptCurrentPos;
    ImmSetCandidateWindow(h, &cdf);

    ImmReleaseContext(hwnd_, h);
}

void Launcher::CaptureBackdrop() {
    if (!hwnd_ || w_ <= 0 || h_ <= 0) return;
    // 拍的时候自己不能露在屏幕上，否则会把自己的窗口也拍进去
    const bool wasShown = IsWindowVisible(hwnd_) != FALSE;
    if (wasShown) ShowWindow(hwnd_, SW_HIDE);

    Gdiplus::Bitmap* shot = CaptureScreenArea(0, 0, w_, h_);
    if (wasShown) ShowWindow(hwnd_, SW_SHOW);

    Gdiplus::Bitmap* blurred = shot ? BlurBitmap(shot, 20) : nullptr;
    if (!blurred) return;
    delete glass_;
    glass_ = blurred;
}

void Launcher::BuildBackground() {
    delete bg_;
    bg_ = nullptr;
    if (!glass_ || w_ <= 0 || h_ <= 0) return;

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w_, h_, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return;
    }
    {
        Gdiplus::Graphics g(bmp);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(glass_, Gdiplus::Rect(0, 0, w_, h_),
                    0, 0, glass_->GetWidth(), glass_->GetHeight(), Gdiplus::UnitPixel);
        // 压暗到能看见桌面的程度，但不让它跟前景图标抢注意力。
        // （这里不跟着明暗主题走 —— 底下的桌面已经被压暗，前景一律浅色才稳。）
        Gdiplus::SolidBrush dim(Gdiplus::Color(118, 10, 10, 14));
        g.FillRectangle(&dim, 0, 0, w_, h_);
        if (noise_) {
            Gdiplus::TextureBrush grain(noise_);
            g.FillRectangle(&grain, 0, 0, w_, h_);
        }
        g.Flush();
    }
    bg_ = bmp;
}

// 把「底图 + 所有图标 + 名字」合成到 canvas_。
// 这一层每帧不动，只有筛选字变了才需要重做 —— 打字时每敲一下重做一次，
// 之后鼠标在上面晃就只是贴这张现成的图。
void Launcher::Composite() {
    if (w_ <= 0 || h_ <= 0) return;
    if (!canvas_ || canvas_->GetWidth() != w_ || canvas_->GetHeight() != h_) {
        delete canvas_;
        canvas_ = new Gdiplus::Bitmap(w_, h_, PixelFormat32bppARGB);
    }
    Gdiplus::Graphics g(canvas_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    if (bg_) {
        // 尺寸完全一样，用最近邻 —— 用插值只会白白多一次全屏重采样
        g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        g.DrawImage(bg_, Gdiplus::Rect(0, 0, w_, h_), 0, 0, w_, h_, Gdiplus::UnitPixel);
    } else {
        Gdiplus::SolidBrush fb(Gdiplus::Color(255, 20, 20, 26));
        g.FillRectangle(&fb, 0, 0, w_, h_);
    }

    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);

    Gdiplus::Font font(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(13)),
                       Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    // 刻意不设 NoWrap：名字长就折成两行。
    // 只有一个 Anaconda 的时候单行够用，但本机「Anaconda Navigator / Anaconda Prompt /
    // Anaconda PowerShell Prompt」三条前面十来个字符一模一样，单行全挤成「Anaconda…」，
    // 根本分不出谁是谁。
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentNear);
    sf.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush fg(Gdiplus::Color(232, 255, 255, 255));
    Gdiplus::SolidBrush shadow(Gdiplus::Color(90, 0, 0, 0));

    const int nShow = PageSlots();
    for (int slot = 0; slot < nShow; ++slot) {
        const AppItem& a = apps_[static_cast<size_t>(AppIndex(slot))];
        const int col = slot % cols_;
        const int row = slot / cols_;
        const float cx = static_cast<float>(gridX_ + col * cellW_ + cellW_ / 2);
        const float iy = static_cast<float>(gridY_ + row * cellH_ + Dpi(10));
        const float s = static_cast<float>(iconSize_);

        if (a.icon) {
            const float iw = static_cast<float>(a.icon->GetWidth());
            const float ih = static_cast<float>(a.icon->GetHeight());
            const float k = (iw > ih) ? (s / iw) : (s / ih);
            const float dw = iw * k;
            const float dh = ih * k;
            g.DrawImage(a.icon,
                        Gdiplus::RectF(cx - dw / 2.0f, iy + (s - dh) / 2.0f, dw, dh),
                        0, 0, a.icon->GetWidth(), a.icon->GetHeight(), Gdiplus::UnitPixel);
        } else {
            Gdiplus::SolidBrush ph(Gdiplus::Color(255, 74, 78, 88));
            g.FillEllipse(&ph, cx - s / 2.0f, iy, s, s);
        }

        Gdiplus::RectF tr(static_cast<float>(gridX_ + col * cellW_ + Dpi(6)),
                          iy + s + static_cast<float>(Dpi(8)),
                          static_cast<float>(cellW_ - Dpi(12)),
                          static_cast<float>(Dpi(36)));
        // 一层很淡的描边当衬底，图标里有浅色图案时名字才不会糊在一起看不清
        g.DrawString(a.name.c_str(), static_cast<INT>(a.name.size()), &font,
                     Gdiplus::RectF(tr.X, tr.Y + 1.0f, tr.Width, tr.Height), &sf, &shadow);
        g.DrawString(a.name.c_str(), static_cast<INT>(a.name.size()), &font, tr, &sf, &fg);
    }
}

void Launcher::Render() {
    if (!dc_ || !bits_) return;
    if (!canvas_ || canvas_->GetWidth() != w_ || canvas_->GetHeight() != h_) return;

    memset(bits_, 0, static_cast<size_t>(w_) * static_cast<size_t>(h_) * 4);

    Gdiplus::Graphics g(dc_);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);

    g.DrawImage(canvas_, Gdiplus::Rect(0, 0, w_, h_), 0, 0, w_, h_, Gdiplus::UnitPixel);

    // ---- 当前选中的那一格：背后浮起一块淡底 ----
    if (active_ >= 0 && active_ < PageSlots()) {
        const int col = active_ % cols_;
        const int row = active_ / cols_;
        Gdiplus::GraphicsPath gp;
        AddRoundRect(gp,
                     Gdiplus::RectF(static_cast<float>(gridX_ + col * cellW_ + Dpi(4)),
                                    static_cast<float>(gridY_ + row * cellH_ + Dpi(4)),
                                    static_cast<float>(cellW_ - Dpi(8)),
                                    static_cast<float>(cellH_ - Dpi(8))),
                     static_cast<float>(Dpi(12)));
        Gdiplus::SolidBrush br(Gdiplus::Color(46, 255, 255, 255));
        g.FillPath(&br, &gp);
        Gdiplus::Pen pe(Gdiplus::Color(70, 255, 255, 255), 1.0f);
        g.DrawPath(&pe, &gp);
    }

    // ---- 搜索框 ----
    int bx = 0, by = 0, boxW = 0, boxH = 0;
    SearchBoxRect(bx, by, boxW, boxH);

    Gdiplus::GraphicsPath sp;
    AddRoundRect(sp, Gdiplus::RectF(static_cast<float>(bx), static_cast<float>(by),
                                    static_cast<float>(boxW), static_cast<float>(boxH)),
                 static_cast<float>(Dpi(23)));
    Gdiplus::SolidBrush sb(Gdiplus::Color(70, 255, 255, 255));
    g.FillPath(&sb, &sp);
    Gdiplus::Pen spen(Gdiplus::Color(110, 255, 255, 255), 1.5f);
    g.DrawPath(&spen, &sp);

    const std::wstring shown = query_ + compQuery_;
    const std::wstring& txt = shown.empty() ? L"搜索" : shown;
    Gdiplus::Font sf2(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(17)),
                      Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat fmt(Gdiplus::StringFormatFlagsNoWrap);
    fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    Gdiplus::SolidBrush txb(shown.empty() ? Gdiplus::Color(150, 255, 255, 255)
                                          : Gdiplus::Color(255, 40, 40, 48));
    const Gdiplus::RectF area(static_cast<float>(bx + Dpi(16)), static_cast<float>(by),
                              static_cast<float>(boxW - Dpi(32)),
                              static_cast<float>(boxH));

    if (compQuery_.empty()) {
        g.DrawString(txt.c_str(), static_cast<INT>(txt.size()), &sf2, area, &fmt, &txb);
    } else {
        // 有两段：已经上屏的字 + 输入法正在拼、还没上屏的字。
        // 后一段画上下划线 —— 跟真实输入框里的「正在组字」一个意思，
        // 用户才知道这几个字还没定下来，按空格/回车才会真的去搜。
        Gdiplus::RectF rq, rc;
        g.MeasureString(query_.c_str(), static_cast<INT>(query_.size()), &sf2,
                        Gdiplus::PointF(0.0f, 0.0f), &rq);
        g.MeasureString(compQuery_.c_str(), static_cast<INT>(compQuery_.size()), &sf2,
                        Gdiplus::PointF(0.0f, 0.0f), &rc);
        const float total = rq.Width + rc.Width;
        const float x0 = area.X + (area.Width - total) / 2.0f;

        Gdiplus::StringFormat nearFmt(Gdiplus::StringFormatFlagsNoWrap);
        nearFmt.SetAlignment(Gdiplus::StringAlignmentNear);
        nearFmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        if (!query_.empty()) {
            g.DrawString(query_.c_str(), static_cast<INT>(query_.size()), &sf2,
                         Gdiplus::RectF(x0, area.Y, rq.Width, area.Height), &nearFmt, &txb);
        }
        Gdiplus::SolidBrush cb(Gdiplus::Color(255, 25, 110, 200));   // 拼字用蓝色区分
        g.DrawString(compQuery_.c_str(), static_cast<INT>(compQuery_.size()), &sf2,
                     Gdiplus::RectF(x0 + rq.Width, area.Y, rc.Width, area.Height),
                     &nearFmt, &cb);
        Gdiplus::Pen up(Gdiplus::Color(255, 25, 110, 200), 1.5f);
        const float uy = area.Y + area.Height / 2.0f + static_cast<float>(Dpi(17)) * 0.62f;
        g.DrawLine(&up, x0 + rq.Width, uy, x0 + total, uy);
    }

    // 没有匹配结果时给个提示，否则会以为程序卡住了
    if (!query_.empty() && shown_.empty()) {
        Gdiplus::Font nf(L"Microsoft YaHei UI", static_cast<Gdiplus::REAL>(Dpi(15)),
                         Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush nb(Gdiplus::Color(170, 255, 255, 255));
        const std::wstring hint = L"没有匹配的应用";
        g.DrawString(hint.c_str(), static_cast<INT>(hint.size()), &nf,
                     Gdiplus::PointF(static_cast<float>(w_) / 2.0f - Dpi(80),
                                     static_cast<float>(Dpi(150))), &nb);
    }

    // ---- 页码指示：超过一页才画 ----
    const int pages = PageCount();
    if (pages > 1) {
        const float r = static_cast<float>(Dpi(4));
        const float gap = static_cast<float>(Dpi(15));
        const float totalW = gap * static_cast<float>(pages - 1);
        const float startX = static_cast<float>(w_) / 2.0f - totalW / 2.0f;
        const float cy = static_cast<float>(h_) - static_cast<float>(Dpi(52));
        for (int p = 0; p < pages; ++p) {
            const bool cur = (p == page_);
            Gdiplus::SolidBrush db(cur ? Gdiplus::Color(225, 255, 255, 255)
                                       : Gdiplus::Color(90, 255, 255, 255));
            g.FillEllipse(&db, startX + gap * static_cast<float>(p) - r, cy - r,
                          r * 2.0f, r * 2.0f);
        }
    }

    // ★ 菜单画在最上面。放在 Render() 而不是 Composite() 里，是因为菜单只在
    //   右键之后才存在、而且悬停要每帧变 —— Composite() 是「底图 + 图标」那张
    //   缓存好的底图，把菜单塞进去等于每挪一下鼠标就要重合成一次全屏。
    DrawAddButton(g);
    DrawMenu(g);

    g.Flush();

    HDC screen = GetDC(nullptr);
    POINT src = {0, 0};
    POINT dst = {0, 0};
    SIZE size = {w_, h_};
    BYTE alpha = static_cast<BYTE>(fade_ * 255.0f + 0.5f);
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, screen, &dst, &size, dc_, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

bool Launcher::StepFade(double dtMs) {
    if (dtMs <= 0.0) dtMs = 16.0;
    if (dtMs > 64.0) dtMs = 64.0;
    const double tau = 80.0;
    const double k = 1.0 - std::exp(-dtMs / tau);
    const double d = static_cast<double>(fadeTarget_) - fade_;
    if (std::fabs(d) < 0.004) {
        fade_ = fadeTarget_;
        return false;
    }
    fade_ += static_cast<float>(d * k);
    return true;
}

void Launcher::RequestAnim() {
    // ★ 除了淡入淡出，这里也可能是「画布没变，但得重画一遍」——
    //   鼠标换个格子就是这种。以前这里只起定时器，Tick 里却只看 fade 和 dirty_，
    //   于是悬停换格时定时器空转一轮、一帧都不画，高亮自然出不来。
    redraw_ = true;
    if (animating_) return;
    animating_ = true;
    SetTimer(hwnd_, kTimerAnim, 16, nullptr);
}

void Launcher::Tick() {
    const DWORD now = GetTickCount();
    const double dt = static_cast<double>(now - fadeLastTick_);
    fadeLastTick_ = now;

    bool moving = StepFade(dt);
    if (dirty_) {
        Composite();
        dirty_ = false;
        moving = true;
    }
    // 换选中格、翻页这类「底图没变但得再贴一次」的情况走这里
    if (redraw_) {
        redraw_ = false;
        moving = true;
    }
    if (moving) Render();

    // 收起动画走完才真正隐藏，并把焦点还给打开之前那个窗口
    if (fade_ <= 0.001f && fadeTarget_ <= 0.0f) {
        if (IsWindowVisible(hwnd_)) {
            ShowWindow(hwnd_, SW_HIDE);
            if (prevFocus_ && IsWindow(prevFocus_)) SetForegroundWindow(prevFocus_);
        }
        prevFocus_ = nullptr;
        KillTimer(hwnd_, kTimerAnim);
        animating_ = false;
        return;
    }
    if (!moving) {
        KillTimer(hwnd_, kTimerAnim);
        animating_ = false;
        return;
    }
}

void Launcher::Open() {
    if (open_ || !hwnd_) return;
    open_ = true;
    prevFocus_ = GetForegroundWindow();
    query_.clear();
    compQuery_.clear();
    active_ = -1;      // 刚打开时谁也不选中（理由见 ApplyFilter 里那段注释）
    ApplyFilter();
    LayoutGrid();
    EnsureIcons();

    const DWORD t0 = GetTickCount();
    // ★ 拍照之前先让 Dock 立刻收起。
    //   Dock 平时是靠 120ms 一次的轮询才收起来的（还带滑动动画），
    //   而这里是打开的那一瞬间就拍 —— 不等这一下的话，Dock 会被拍进背景，
    //   模糊压暗之后看着就像它根本没藏，还浮在启动台上。
    //   SendMessage 是同步的，所以喊完这一声，Dock 一定已经不在屏幕上了。
    if (dock_ && IsWindow(dock_)) SendMessageW(dock_, kMsgHideNow, 0, 0);

    CaptureBackdrop();    // 必须在 ShowWindow 之前，否则会把自己也拍进去
    BuildBackground();
    Log(L"启动台：拍照+模糊 %lums", static_cast<unsigned long>(GetTickCount() - t0));

    dirty_ = true;
    Composite();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    fade_ = 0.0f;
    fadeTarget_ = 1.0f;
    fadeLastTick_ = GetTickCount();
    Render();
    RequestAnim();
    Log(L"启动台打开：%zu 个应用", apps_.size());
}

void Launcher::Close() {
    if (!open_) return;
    if (TraceOn()) {
        // 追一下「谁把它关了」：配合 WM_MOUSEMOVE 的记录，能区分是鼠标点空白、
        // 按了 ESC、还是别的什么。
        Log(L"启动台 TRACE：Close() 被调用（最近一次消息 0x%04X，鼠标 %d,%d）",
            g_lastMsg, g_lastMouseX, g_lastMouseY);
    }
    open_ = false;
    fadeTarget_ = 0.0f;
    fadeLastTick_ = GetTickCount();
    RequestAnim();
}

void Launcher::Toggle() {
    if (IsOpen()) Close();
    else Open();
}

int Launcher::HitTest(int x, int y) const {
    if (shown_.empty() || cols_ <= 0 || cellW_ <= 0 || cellH_ <= 0) return -1;
    if (x < gridX_ || y < gridY_) return -1;
    const int col = (x - gridX_) / cellW_;
    const int row = (y - gridY_) / cellH_;
    if (col >= cols_ || row >= rows_) return -1;
    const int slot = row * cols_ + col;
    if (slot < 0 || slot >= PageSlots()) return -1;
    return slot;
}

void Launcher::LaunchIndex(int slot) {
    const int idx = AppIndex(slot);
    if (idx < 0) return;
    const AppItem& a = apps_[static_cast<size_t>(idx)];

    // 已经开着就切过去，不要重复启动（跟 Dock 点图标是同一套逻辑）
    //
    // ★ 2026-10-11：这里没走 RunAppAction 是历史原因（启动台还要决定什么时候
    //   关掉自己），但「算不算已经在运行」的判据必须跟那边一致 ——
    //   只找可见窗口的话，缩在托盘里的微信会被当成没开，一点就重新扫码。
    if (!a.target.empty()) {
        HWND found = FindProcessWindowAny(a.target);
        if (found) {
            if (!IsWindowVisible(found)) ShowWindow(found, SW_SHOW);
            if (IsIconic(found)) ShowWindow(found, SW_RESTORE);
            SetForegroundWindow(found);
            Close();
            return;
        }
        if (IsProcessRunning(a.target)) {
            Log(L"启动台：%ls 的进程还在但没有窗口（多半缩在托盘里），不再重复启动",
                a.name.c_str());
            Close();
            return;
        }
    }

    const std::wstring what = a.lnkPath.empty() ? a.target : a.lnkPath;
    if (what.empty()) return;
    // ★ 工作目录同上：不给的话微信会继承 MyDock 的目录，认不出已登录的自己
    const wchar_t* dir = a.workDir.empty() ? nullptr : a.workDir.c_str();
    HINSTANCE r = ShellExecuteW(nullptr, L"open", what.c_str(), nullptr, dir, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        Log(L"启动台：启动失败 %ls（代码 %lld）", what.c_str(),
            static_cast<long long>(reinterpret_cast<INT_PTR>(r)));
    }
    Close();
}

// 把一个应用从这一页里拿掉。
//
// ★ 这里只管**界面这一份**：从 apps_ 里删掉、重排、重画。
//   「写 hidden.txt / 清 added.txt」那份属于文件层，在 appaction.cpp 里 ——
//   Dock 和启动台都要做同一件事，所以只能有一份实现。
//   两边合在一起才是完整的「移出启动台」：先写清单（免得下次又冒出来），
//   再把眼前这一格删掉（免得用户看着它还在）。
void Launcher::RemoveFromList(int idx) {
    if (idx < 0 || idx >= static_cast<int>(apps_.size())) return;
    const std::wstring name = apps_[static_cast<size_t>(idx)].name;

    // 图标位图也一起释放 —— 不然白占一份内存，
    // 而且 hidden.txt 只在启动时过滤一次，光写文件不删的话这一页还照旧显示它。
    delete apps_[static_cast<size_t>(idx)].icon;
    apps_.erase(apps_.begin() + static_cast<ptrdiff_t>(idx));
    Log(L"启动台：%s 已移出启动台（现在剩 %zu 个）", name.c_str(), apps_.size());

    query_.clear();
    compQuery_.clear();
    active_ = -1;
    CloseMenu();
    ApplyFilter();
    LayoutGrid();
    EnsureIcons();
    dirty_ = true;
    RequestAnim();
}

void Launcher::BuildMenu(int slot) {
    const int idx = AppIndex(slot);
    if (idx < 0) return;
    const AppItem& a = apps_[static_cast<size_t>(idx)];

    menuItems_.clear();
    auto sep = [&]() {
        MenuItem it;
        it.separator = true;
        menuItems_.push_back(it);
    };
    // 第一行是应用名本身（粗体、灰、不可点）—— macOS 启动台右键就是这么排的，
    // 一眼就知道自己在对谁操作。本机应用重名的不少，光看菜单不知道是谁。
    MenuItem head;
    head.title = true;
    head.label = a.name;
    head.tag = L"应用";
    menuItems_.push_back(std::move(head));
    sep();

    // ★ 中间这一整组（打开 / 打开所在文件夹 / 上下 Dock / 移出启动台）
    //   跟 Dock 的右键菜单是同一份代码 —— 见 appaction.h。
    //   加一项、改一个说法，两边一起变，不会再出现"一边有一边没有"。
    AppPlaces places;
    places.onDock = AppIsPinned(a);
    places.onLaunchpad = true;
    places.userAdded = AppIsUserAdded(a);
    AppendAppActions(menuItems_, a, places);

    sep();
    MenuItem quit;
    quit.cmd = 4;
    quit.label = L"关闭启动台";
    menuItems_.push_back(std::move(quit));
}

void Launcher::LayoutMenu(int x, int y) {
    // 宽度/高度交给 menu.cpp 里那份共用的算法（跟 Dock 的菜单同一套尺寸规则）
    MeasureMenu(menuItems_, menuW_, menuH_, menuRowH_);

    // 尺寸定下来了，把投影算一次存着 —— 菜单开着的时候鼠标一动就要重画，
    // 每帧重来一次模糊是撑不住的。
    delete menuShadow_;
    menuShadowPad_ = MenuShadowPad();
    menuShadow_ = MakeMenuShadow(menuW_, menuH_, menuShadowPad_,
                                 static_cast<float>(Dpi(6)));

    // ★ 先把左上角放到鼠标上。这两行不能省：下面的「往回收」是在这个基础上判的，
    //   少了赋值，menuX_ 还是成员初值 0，判据永远不成立，菜单就被最后那条
    //   「不能小于 12」夹死在屏幕最左边（2026-10-10 踩过：右键菜单不跟鼠标，
    //   永远贴在最左侧）。
    menuX_ = x;
    menuY_ = y;
    // ★ 菜单可能整个跑到屏幕外面 —— 图标在屏幕右边缘/下边缘时，
    //   照鼠标位置往下摆会有一半在外面，看上去像菜单被屏幕切掉了。
    //   所以摆完要往回收：右边放不下就改成往左展开，下边放不下就往上。
    if (menuX_ + menuW_ > w_ - Dpi(12)) menuX_ = x - menuW_;
    if (menuY_ + menuH_ > h_ - Dpi(12)) menuY_ = y - menuH_;
    if (menuX_ < Dpi(12)) menuX_ = Dpi(12);
    if (menuY_ < Dpi(12)) menuY_ = Dpi(12);

    if (TraceOn()) {
        Log(L"启动台 TRACE：菜单 %dx%d 落在 (%d,%d)，%zu 项",
            menuW_, menuH_, menuX_, menuY_, menuItems_.size());
    }
}

void Launcher::DrawMenu(Gdiplus::Graphics& g) {
    if (!menuOpen_ || menuItems_.empty()) return;
    // 长相交给 menu.cpp 里那份共用的画法（Dock 的右键菜单走同一个函数）。
    // 这里之所以在 Render() 里画而不并进 Composite()：菜单只在右键之后才存在，
    // 而且鼠标挪一下就要重画，塞进那张缓存好的全屏底图等于每次都重合成一遍。
    DrawMenuBox(g, menuItems_, menuX_, menuY_, menuW_, menuH_, menuRowH_, menuHover_,
                246, menuShadow_);
}

int Launcher::MenuHit(int x, int y) const {
    if (!menuOpen_) return -1;
    return MenuHitItem(menuItems_, menuX_, menuY_, menuW_, menuRowH_, x, y);
}

void Launcher::CloseMenu() {
    if (!menuOpen_) return;
    menuOpen_ = false;
    menuHover_ = -1;
    RequestAnim();
}

void Launcher::RunMenuCmd(int cmd, int slot) {
    // 应用操作那一组走共用的执行器（appaction.cpp）。它只碰文件和进程，
    // 不碰界面 —— 界面这边按它回的旗子做该做的事。
    if (IsAppCmd(cmd)) {
        const int idx = AppIndex(slot);
        if (idx < 0) return;
        // ★ 拷一份再动手：下面「移出启动台」会把这一项从 apps_ 里删掉，
        //   拿着引用往下走就是在读一块已经释放的字符串。
        const AppItem app = apps_[static_cast<size_t>(idx)];

        AppPlaces places;
        places.onDock = AppIsPinned(app);
        places.onLaunchpad = true;
        places.userAdded = AppIsUserAdded(app);

        const AppOutcome r = RunAppAction(cmd, app, places);
        if (r.launched) { Close(); return; }
        // 清单变了就喊 Dock 一声，让它重建自己那一行图标
        if (r.pinnedChanged && dock_) PostMessageW(dock_, kMsgPinnedChanged, 0, 0);
        if (r.hiddenChanged) RemoveFromList(idx);
        return;
    }

    switch (cmd) {
        case 4: Close(); break;      // 关闭启动台
        default: break;
    }
}

void Launcher::ShowAppMenu(int x, int y, int slot) {
    if (TraceOn()) Log(L"启动台 TRACE：ShowAppMenu 收到右键 (%d,%d) 格 %d", x, y, slot);
    if (AppIndex(slot) < 0) return;

    menuSlot_ = slot;
    BuildMenu(slot);
    if (menuItems_.empty()) return;

    // 菜单活在启动台这张全屏画布上，不是独立窗口 —— 所以直接用窗口内坐标，
    // 还能顺手跟着启动台一起淡入淡出。
    menuHover_ = -1;
    menuOpen_ = true;
    LayoutMenu(x, y);
    RequestAnim();
    SetFocus(hwnd_);
}

LRESULT CALLBACK Launcher::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Launcher* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Launcher*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = h;
    } else {
        self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (self) return self->OnMessage(msg, wp, lp);
    return DefWindowProcW(h, msg, wp, lp);
}

LRESULT Launcher::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    g_lastMsg = msg;
    if (msg == WM_MOUSEMOVE) {
        g_lastMouseX = static_cast<short>(LOWORD(lp));
        g_lastMouseY = static_cast<short>(HIWORD(lp));
    }
    switch (msg) {
        case WM_CHAR: {
            // 直接输入的字母/数字/符号。中文走 WM_IME_COMPOSITION → 上屏时补送 WM_CHAR
            if (wp >= 0x20 && wp != 0x7F) {
                query_ += static_cast<wchar_t>(wp);
                if (TraceOn()) {
                    Log(L"启动台 TRACE：收到 WM_CHAR 0x%04X 收下后查询[%s]",
                        static_cast<unsigned>(wp), query_.c_str());
                }
                ApplyFilter();
                RequestAnim();
            }
            return 0;
        }
        case WM_IME_STARTCOMPOSITION:
            // 开始组字了 —— 先把候选窗的位置摆好，晚了就飘左上角去了
            ImeFollowCaret();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        case WM_IME_COMPOSITION: {
            HIMC h = ImmGetContext(hwnd_);
            std::wstring comp, res;
            if (lp & (GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS)) {
                // 正在拼、还没上屏的字。让它参与筛选，这样拼到一半就能看到结果在收窄
                comp = ImmStr(h, GCS_COMPSTR);
                compQuery_ = comp;
            }
            if (lp & GCS_RESULTSTR) {
                // ★ 上屏的字**这里不要往 query_ 里加**。
                //
                // 实测踩过：打 ste 再回车，日志是
                //     IME_COMPOSITION 上屏[ste]
                //     WM_CHAR s / WM_CHAR t / WM_CHAR e
                // 也就是说把消息交还给 DefWindowProc 之后，输入法会把上屏的每一个
                // 字符再以 WM_CHAR 补送回来。这里要是自己也加一次，"ste" 就变成
                // "steste" 了。所以上屏这一步只做一件事：把「正在拼」的那段清掉，
                // 真正的入账交给下面补送来的 WM_CHAR —— 中文上屏时补送的是汉字，
                // 一样能进 query_，中文名也就搜得到了。
                res = ImmStr(h, GCS_RESULTSTR);
                compQuery_.clear();
            }
            ImmReleaseContext(hwnd_, h);
            if (TraceOn()) {
                Log(L"启动台 TRACE：IME_COMPOSITION 拼字[%s] 上屏[%s]（上屏的字交给 WM_CHAR）",
                    comp.c_str(), res.c_str());
            }
            ApplyFilter();
            RequestAnim();
            // 交还给系统处理：候选词窗口靠它弹出，上屏字符的 WM_CHAR 也是它补的
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        case WM_IME_ENDCOMPOSITION: {
            compQuery_.clear();
            ApplyFilter();
            RequestAnim();
            return DefWindowProcW(hwnd_, msg, wp, lp);
        }
        case WM_SETFOCUS:
            // 搜狗/微软拼音这类输入法，在窗口拿到焦点时会自己弹个状态条。
            // 借这个时机把候选窗位置报给它。
            ImeFollowCaret();
            return 0;
        case WM_KEYDOWN: {
            if (TraceOn()) Log(L"启动台 TRACE：收到 WM_KEYDOWN vk=%u", static_cast<unsigned>(wp));

            // ★ 输入法把字母键整个吃掉了。
            //
            // 实测：在中文输入法激活时按 g，窗口收到的不是 WM_KEYDOWN('G') + WM_CHAR('g')，
            // 而是 **一个 vk=229 的 WM_KEYDOWN** —— 229 就是 VK_PROCESSKEY，
            // 意思是「这个键我（输入法）拿去处理了」。可是输入法既没有给 WM_CHAR，
            // 也没有给 WM_IME_COMPOSITION，于是字符消失，搜索框一个字都打不进去。
            //
            // 解法是向输入法要回这个键的真实身份：ImmGetVirtualKey 给出被替换前的 vk，
            // 再用 ToUnicode 把它翻成字符，自己收下。
            // 只在 vk == VK_PROCESSKEY 这一条路上做，所以输入法不吞键时（英文模式）
            // 仍然走正常的 WM_CHAR，不会重复输入。
            if (wp == VK_PROCESSKEY) {
                // ★ 输入法要是正在组字（compQuery_ 有内容），说明它自己会发
                //   WM_IME_COMPOSITION，这里就别插手了 —— 不然同一个字要被记两遍：
                //   兜底记一次、上屏那会儿再记一次。
                if (!compQuery_.empty()) {
                    if (TraceOn()) {
                        Log(L"启动台 TRACE：PROCESSKEY，输入法正在组字（%s），交给它",
                            compQuery_.c_str());
                    }
                    return DefWindowProcW(hwnd_, msg, wp, lp);
                }

                // 键盘扫描码在 lParam 的 16~23 位，它是物理键的编号，
                // **不受输入法替换影响** —— 所以就算 IME 那边问不出东西，也能从它反推出原键。
                const UINT sc = (static_cast<UINT>(lp) >> 16) & 0xFF;
                UINT real = 0;
                HIMC imc = ImmGetContext(hwnd_);
                if (imc) {
                    real = ImmGetVirtualKey(hwnd_);
                    ImmReleaseContext(hwnd_, imc);
                }
                const UINT viaImm = real;
                if (!real) real = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);

                BYTE ks[256] = {};
                GetKeyboardState(ks);
                wchar_t buf[8] = {};
                const int got = real ? ToUnicode(real, sc, ks, buf, 8, 0) : 0;
                if (TraceOn()) {
                    Log(L"启动台 TRACE：VK_PROCESSKEY 扫描码=%u IME给的vk=%u 反推vk=%u "
                        L"ToUnicode=%d", sc, viaImm, real, got);
                }
                bool added = false;
                for (int i = 0; i < got; ++i) {
                    if (buf[i] >= 0x20 && buf[i] != 0x7F) {
                        query_ += buf[i];
                        added = true;
                    }
                }
                if (added) {
                    ApplyFilter();
                    RequestAnim();
                }
                return 0;
            }

            switch (wp) {
                case VK_BACK: {
                    if (!compQuery_.empty()) {
                        compQuery_.clear();
                    } else if (!query_.empty()) {
                        query_.pop_back();
                    }
                    ApplyFilter();
                    RequestAnim();
                    return 0;
                }
                case VK_ESCAPE:
                    // 菜单开着时，ESC 只收菜单，不连带把启动台也关了 ——
                    // 否则按一次 ESC 两层都没了，用户想退一层退不掉。
                    if (menuOpen_) {
                        CloseMenu();
                        return 0;
                    }
                    if (!query_.empty()) {
                        query_.clear();
                        compQuery_.clear();
                        ApplyFilter();
                        RequestAnim();
                    } else {
                        Close();
                    }
                    return 0;
                case VK_RETURN:
                    LaunchIndex(active_);
                    return 0;
                case VK_PRIOR:      // PageUp
                    ChangePage(-1);
                    active_ = 0;
                    RequestAnim();
                    return 0;
                case VK_NEXT:       // PageDown
                    ChangePage(1);
                    active_ = 0;
                    RequestAnim();
                    return 0;
                case VK_TAB:
                    ChangePage(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
                    active_ = 0;
                    RequestAnim();
                    return 0;
                case VK_LEFT:
                case VK_RIGHT:
                case VK_UP:
                case VK_DOWN: {
                    const int slots = PageSlots();
                    if (slots <= 0) return 0;
                    int a = active_;
                    if (a < 0) a = 0;
                    if (wp == VK_LEFT || wp == VK_RIGHT) {
                        const int row = a / cols_;
                        const bool first = ((a % cols_) == 0);
                        const bool last = ((a % cols_) == cols_ - 1);
                        if (wp == VK_LEFT && first) {
                            // 已经在最左一列：翻到上一页，并落到同一行的最后一列
                            if (ChangePage(-1)) a = row * cols_ + (cols_ - 1);
                        } else if (wp == VK_RIGHT && last) {
                            if (ChangePage(1)) a = row * cols_;
                        } else {
                            a += (wp == VK_LEFT) ? -1 : 1;
                        }
                    } else if (wp == VK_UP) {
                        if (a < cols_) ChangePage(-1);   // 在第一行就往前翻一页
                        else a -= cols_;
                    } else {
                        a += cols_;
                        if (a >= slots) {
                            if (!ChangePage(1)) a -= cols_;   // 已经是最后一页就别乱动
                            else a = active_;
                        }
                    }
                    if (a < 0) a = 0;
                    if (a >= PageSlots()) a = PageSlots() - 1;
                    if (a != active_) {
                        active_ = a;
                        RequestAnim();
                    }
                    return 0;
                }
                default:
                    break;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const int x = static_cast<short>(LOWORD(lp));
            const int y = static_cast<short>(HIWORD(lp));
            // 菜单开着时，鼠标是在菜单里挑，不是挑图标 —— 底下的高亮要冻住，
            // 不然菜单上面飘着一个图标高亮，看着像焦点跑到那儿去了。
            if (menuOpen_) {
                const int mi = MenuHit(x, y);
                const int hov = (mi >= 0) ? mi : -1;
                if (hov != menuHover_) {
                    menuHover_ = hov;
                    RequestAnim();
                }
                return 0;
            }
            const int hit = HitTest(x, y);
            if (hit != active_) {
                if (TraceOn()) {
                    Log(L"启动台 TRACE：鼠标 (%d,%d) 命中格 %d（原来 %d）", x, y, hit, active_);
                }
                active_ = hit;
                RequestAnim();
            }
            const bool overAdd = HitAddButton(x, y);
            if (overAdd != addHover_) {
                addHover_ = overAdd;
                RequestAnim();
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (delta != 0) {
                ChangePage(delta > 0 ? -1 : 1);
                active_ = 0;
                RequestAnim();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const int mx = static_cast<short>(LOWORD(lp));
            const int my = static_cast<short>(HIWORD(lp));
            // 菜单开着的时候，左键只服务菜单：点中哪项就执行哪项，
            // 点在菜单外面（或者点的是标题、分隔线这种不可点的行）只把菜单收掉，
            // **不能**顺手把底下的应用也启动了 —— 那一下用户是想点菜单的。
            if (menuOpen_) {
                const int mi = MenuHit(mx, my);
                if (mi >= 0) {
                    const int cmd = menuItems_[static_cast<size_t>(mi)].cmd;
                    const int slot = menuSlot_;
                    CloseMenu();
                    RunMenuCmd(cmd, slot);
                } else {
                    CloseMenu();
                }
                return 0;
            }
            // 左上角那个「+」：先于网格判断，因为它本来就在网格外面
            if (HitAddButton(mx, my)) {
                AddAppFromDialog();
                return 0;
            }
            const int hit = HitTest(mx, my);
            // 点到网格外（空白 / 搜索框上方）就关掉，跟 Launchpad 一个习惯
            if (hit >= 0) LaunchIndex(hit);
            else Close();
            return 0;
        }
        case WM_RBUTTONUP: {
            const int mx = static_cast<short>(LOWORD(lp));
            const int my = static_cast<short>(HIWORD(lp));
            // 菜单开着时再右键：点在同一个图标上就收掉菜单（toggle），
            // 点在别处就换一个图标重新开 —— 不然菜单会卡在那儿关不掉。
            if (menuOpen_) {
                const int mi = MenuHit(mx, my);
                if (mi >= 0 || HitTest(mx, my) != menuSlot_) {
                    CloseMenu();
                    if (mi >= 0) return 0;
                } else {
                    CloseMenu();
                    return 0;
                }
            }
            ShowAppMenu(mx, my, HitTest(mx, my));
            return 0;
        }
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_TIMER:
            if (wp == kTimerAnim) {
                Tick();
                return 0;
            }
            break;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;   // 要能拿到键盘焦点，所以这里照常激活
        case WM_ERASEBKGND:
            return 1;
        default:
            break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace mydock
