// 2026-10-09 - AI - 开始菜单扫描与应用图标加载
#include "apps.h"
#include "util.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <commoncontrols.h>
#include <objbase.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace mydock {
namespace {

// IImageList 的 IID，自己写一份免得依赖 uuid 里的符号
const GUID kIID_IImageList =
    {0x46EB5926, 0x582E, 0x4017, {0x9F, 0xDF, 0xE8, 0x99, 0x8D, 0xAA, 0x09, 0x50}};

void ScanDir(const std::wstring& dir, std::vector<std::wstring>& out, int depth) {
    if (depth > 4) return;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanDir(full, out, depth + 1);
        } else if (_wcsicmp(PathFindExtensionW(fd.cFileName), L".lnk") == 0) {
            out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool ShouldSkip(const std::wstring& stem) {
    static const wchar_t* kBad[] = {
        L"uninstall", L"卸载", L"remove", L"readme", L"read me", L"help", L"帮助",
        L"documentation", L"website", L"官网", L"主页", L"更新", L"update",
        L"setup", L"install", L"repair", L"修复", L"license", L"许可",
        L"changelog", L"release note", L"命令行", L"command prompt",
        L"配置", L"config", L"troubleshoot", L"诊断",
        // ---- 下面这些是照着本机开始菜单里真实存在的噪音一条条加的 ----
        L"administrative tools", L"管理工具",        // 文件夹本身也被扫出来了
        L"deactivate", L"activate matlab",           // 激活/停用，不是「打开应用」
        L"manuals", L"module docs", L"faqs",         // 文档
        L"support center", L"on the web", L"参考文档",  // 网页入口
        L"gpu compatibility mode", L"gpu safe mode",  // 同一程序的兼容模式副本
        L"reset keymapper", L"reset options", L"video instructions",
        L"steps recorder", L"language preference", L"语言首选项",
        // 搜狗输入法「链接」文件夹里的推广项
        L"官方网站", L"意见反馈", L"打字入门", L"皮肤下载", L"细胞词库",
        L"关于 java", L"访问 java",
    };
    std::wstring low = stem;
    for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* b : kBad) {
        if (low.find(b) != std::wstring::npos) return true;
    }
    return false;
}

// 按「快捷方式在哪个文件夹里」过滤。
//
// 为什么非要这一层：扫一遍开始菜单能出 141 项，真正的日常应用只占一小半，
// 剩下全是系统管理和 SDK —— Administrative Tools 18 个、Windows Kits 13 个、
// Visual Studio Tools 7 个、Accessibility 5 个、Windows PowerShell 4 个……
// 光按名字判断漏得很厉害（"任务管理器"这四个字里没有任何可疑片段），
// 所以改成看目录。这也是 macOS Launchpad 的做法：系统工具不进启动台。
//
// 这条过滤可以在 settings.ini 里关掉：[launchpad] hideTools=0
bool InToolsFolder(const std::wstring& lnkPath) {
    static const wchar_t* kBad[] = {
        L"\\administrative tools\\", L"\\system tools\\",
        L"\\windows powershell\\", L"\\windows kits\\",
        L"\\accessibility\\", L"\\visual studio tools\\",
        L"\\windows software development kit\\", L"\\windows performance toolkit\\",
        L"\\windows easy transfer\\", L"\\maintenance\\",
        // 这几个目录也是照着本机实际扫出来的结果加的。
        // 注意结尾**不带**反斜杠：它们的子目录（Nsight Compute 2026.1.1、Extras\Video）
        // 才是真正要拦的那一层。
        L"\\nvidia corporation\\nsight",
        L"\\dosbox-0.74-3\\extras",
        L"\\dosbox-0.74-3\\options",
        L"\\java development kit\\",
        L"\\搜狗输入法\\链接\\",
    };
    std::wstring low = lnkPath;
    for (auto& c : low) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    for (const wchar_t* b : kBad) {
        if (low.find(b) != std::wstring::npos) return true;
    }
    return false;
}

// 2026-10-11 - AI - 顺带把「起始位置」也读出来
//
// 原来只读目标 exe。工作目录看着无关紧要，其实不是：ShellExecute 的 lpDirectory
// 传 NULL 时，新进程会继承「启动它的那个进程」的当前目录 —— 也就是 MyDock 自己的
// 安装目录。微信这类程序会因此认不出已经登录过的自己，每次都走一遍扫码登录。
bool ResolveLnk(const std::wstring& lnk, std::wstring& target, std::wstring& workDir) {
    target.clear();
    workDir.clear();
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void**>(&sl))) && sl) {
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&pf))) && pf) {
            if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
                wchar_t buf[MAX_PATH] = {};
                if (SUCCEEDED(sl->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH))) target = buf;
                wchar_t wbuf[MAX_PATH] = {};
                if (SUCCEEDED(sl->GetWorkingDirectory(wbuf, MAX_PATH))) workDir = wbuf;
            }
            pf->Release();
        }
        sl->Release();
    }
    return !target.empty();
}

}  // namespace

Gdiplus::Bitmap* LoadSystemIcon(int index) {
    IImageList* list = nullptr;
    if (FAILED(SHGetImageList(SHIL_JUMBO, kIID_IImageList, reinterpret_cast<void**>(&list))) || !list)
        return nullptr;
    HICON ico = nullptr;
    if (FAILED(list->GetIcon(index, ILD_TRANSPARENT, &ico)) || !ico) {
        list->Release();
        return nullptr;
    }
    Gdiplus::Bitmap* bmp = BitmapFromIcon(ico);
    DestroyIcon(ico);
    list->Release();
    return bmp;
}

Gdiplus::Bitmap* LoadIconFromShellItem(const std::wstring& path) {
    IShellItemImageFactory* factory = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return nullptr;

    HBITMAP hb = nullptr;
    SIZE sz = {256, 256};
    hr = factory->GetImage(sz, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb);
    factory->Release();
    if (FAILED(hr) || !hb) return nullptr;

    Gdiplus::Bitmap* bmp = BitmapFromHBITMAP(hb);
    DeleteObject(hb);
    if (!bmp) {
        Log(L"图标转换失败: %ls", path.c_str());
        return nullptr;
    }

    // 裁掉透明边距 / 极淡的光晕：不少图标是「大画布 + 中间一小块图案」，
    // 不裁的话画到浅色背景上就只剩中间那一小块，看着特别小。
    const int bw = bmp->GetWidth();
    const int bh = bmp->GetHeight();
    Gdiplus::Bitmap* trimmed = TrimTransparent(bmp);
    if (trimmed && (trimmed->GetWidth() * 100 < bw * 85 ||
                    trimmed->GetHeight() * 100 < bh * 85)) {
        size_t slash = path.find_last_of(L"\\");
        std::wstring nm = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        Log(L"图标裁剪 %ls：%dx%d -> %dx%d", nm.c_str(), bw, bh,
            trimmed->GetWidth(), trimmed->GetHeight());
    }
    return trimmed;
}

std::vector<AppItem> LoadPinnedApps() {
    std::vector<AppItem> apps;

    static const wchar_t* kDirs[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\MyDockFinder\\launchpad\\Pinned",
        L"C:\\Program Files\\Steam\\steamapps\\common\\MyDockFinder\\launchpad\\Pinned",
    };

    std::wstring dir;
    for (const wchar_t* d : kDirs) {
        DWORD attr = GetFileAttributesW(d);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            dir = d;
            break;
        }
    }
    if (dir.empty()) {
        Log(L"没找到启动台 Pinned 目录，将回退到开始菜单");
        return apps;
    }

    std::vector<std::wstring> files;
    std::wstring pattern = dir + L"\\*";
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return apps;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring ext = PathFindExtensionW(fd.cFileName);
        if (_wcsicmp(ext.c_str(), L".lnk") == 0 || _wcsicmp(ext.c_str(), L".url") == 0) {
            files.push_back(dir + L"\\" + fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end());

    for (const std::wstring& f : files) {
        size_t slash = f.find_last_of(L"\\");
        std::wstring stem = (slash == std::wstring::npos) ? f : f.substr(slash + 1);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem = stem.substr(0, dot);

        // 试过发现不好用、已淘汰的工具，不要再放上 Dock
        if (_wcsicmp(stem.c_str(), L"TaskbarPopup") == 0) continue;

        AppItem it;
        it.name = stem;
        it.lnkPath = f;
        ResolveLnk(f, it.target, it.workDir);   // .url 解析不出目标，留空即可
        it.icon = LoadIconFromShellItem(f);
        if (!it.icon) Log(L"取不到图标: %ls", stem.c_str());
        apps.push_back(std::move(it));
    }

    Log(L"启动台清单读取完成：%zu 个应用", apps.size());
    return apps;
}

std::vector<AppItem> EnumerateStartMenuApps() {
    std::vector<std::wstring> lnks;
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, 0, buf)))
        ScanDir(buf, lnks, 0);
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, buf)))
        ScanDir(buf, lnks, 0);

    const bool hideTools = ConfigFlag(L"launchpad", L"hideTools", true);
    std::vector<AppItem> apps;
    int skipped = 0;
    for (const std::wstring& lnk : lnks) {
        if (hideTools && InToolsFolder(lnk)) {
            ++skipped;
            continue;
        }
        size_t slash = lnk.find_last_of(L"\\");
        std::wstring stem = (slash == std::wstring::npos) ? lnk : lnk.substr(slash + 1);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem = stem.substr(0, dot);
        if (ShouldSkip(stem)) continue;

        SHFILEINFOW sfi = {};
        if (!SHGetFileInfoW(lnk.c_str(), 0, &sfi, sizeof(sfi),
                            SHGFI_ICON | SHGFI_SYSICONINDEX | SHGFI_LARGEICON))
            continue;
        if (sfi.hIcon) DestroyIcon(sfi.hIcon);

        AppItem it;
        it.name = stem;
        it.lnkPath = lnk;
        ResolveLnk(lnk, it.target, it.workDir);
        it.iconIndex = sfi.iIcon;
        apps.push_back(std::move(it));
    }

    std::sort(apps.begin(), apps.end(),
              [](const AppItem& a, const AppItem& b) { return a.name < b.name; });

    std::vector<AppItem> out;
    std::wstring last;
    for (auto& a : apps) {
        if (a.name == last) continue;
        last = a.name;
        out.push_back(std::move(a));
    }

    for (auto& a : out) {
        a.icon = LoadIconFromShellItem(a.lnkPath);
        if (!a.icon) a.icon = LoadSystemIcon(a.iconIndex);
    }
    Log(L"开始菜单扫描完成，可用应用 %zu 个%s", out.size(),
        skipped > 0 ? L"（另有系统工具被过滤）" : L"");
    return out;
}

void FreeAppIcons(std::vector<AppItem>& apps) {
    for (auto& a : apps) {
        delete a.icon;
        a.icon = nullptr;
    }
}

std::vector<AppItem> DeepCopyAppList(const std::vector<AppItem>& src) {
    std::vector<AppItem> out;
    out.reserve(src.size());
    for (const AppItem& a : src) {
        AppItem b = a;
        b.icon = nullptr;
        if (a.icon) {
            // Clone 出来的是独立的一张图。之后两边各自就地缩放、各自释放，互不影响。
            Gdiplus::Bitmap* c = a.icon->Clone(0, 0, a.icon->GetWidth(), a.icon->GetHeight(),
                                               PixelFormat32bppARGB);
            if (c && c->GetLastStatus() == Gdiplus::Ok) b.icon = c;
            else delete c;
        }
        out.push_back(std::move(b));
    }
    return out;
}

// ---------------------------------------------------------- 清单读写的公共部分
// 2026-10-11 - AI - 三份清单统一改成「写临时文件 + 原子换上去」
//
// 以前是 fopen(path,"w") 直接覆盖。这条路上有一个致命的中间态：
// 文件被截断成 0 字节之后、新内容写进去之前那几毫秒 —— 进程崩了、断电了、
// 或者杀毒软件伸手拦一下，清单就永久变成空文件。用户看到的是「我的 Dock 空了」，
// 而且没法恢复（旧内容已经被截掉了）。
//
// 改成先写同目录的 .tmp，再一次性换上去。同一个分区内的重命名由文件系统
// 保证原子性：任何时刻看到的要么是旧文件、要么是新文件，不存在「半个文件」。
static bool WriteLinesAtomic(const std::wstring& finalPath,
                             const std::vector<std::wstring>& header,
                             const std::vector<std::wstring>& lines) {
    const std::wstring tmp = finalPath + L".tmp";

    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"w, ccs=UTF-16LE") != 0 || !f) return false;
    bool ok = true;
    for (const std::wstring& h : header) {
        if (fwprintf(f, L"%ls\n", h.c_str()) < 0) ok = false;
    }
    for (const std::wstring& p : lines) {
        if (p.empty()) continue;
        if (fwprintf(f, L"%ls\n", p.c_str()) < 0) ok = false;
    }
    // ★ 落盘这一步不能省。fclose 只是把 CRT 的缓冲交还给系统，内容还躺在
    //   系统缓存里 —— 这时候换上去，断电照样丢。fflush 才逼着它真写下去。
    if (ok && fflush(f) != 0) ok = false;
    fclose(f);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }

    // 目标已经存在 → ReplaceFileW：它连文件属性和安全描述符一起从旧文件搬到
    // 新文件上，比 MoveFileEx 更"无缝"（后者会丢掉旧文件的自定义 ACL）。
    // 目标不存在（第一次运行）→ ReplaceFileW 直接失败，退回 MoveFileEx。
    if (GetFileAttributesW(finalPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (ReplaceFileW(finalPath.c_str(), tmp.c_str(), nullptr,
                         REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
            return true;
        }
    }
    if (MoveFileExW(tmp.c_str(), finalPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    // 两条路都失败：把临时文件删掉，别在同目录留一地垃圾
    Log(L"清单写不进去：%ls（错误码 %lu）", finalPath.c_str(), GetLastError());
    DeleteFileW(tmp.c_str());
    return false;
}

// 读一份清单：统一跳过空行、注释行，顺手把两端空白和配对的引号剥掉。
//
// 为什么要 trim：这几份文件用户会自己拿记事本改，很容易在行尾留下空格，
// 或者从别处复制过来时带着引号。以前不 trim 的结果就是
// 「明明写了这一行，Dock 上就是没有」—— 路径对不上，而且没有任何提示。
static std::vector<std::wstring> LoadLines(const std::wstring& path) {
    std::vector<std::wstring> out;
    FILE* f = nullptr;
    // ccs=UTF-16LE：直接按宽字符读写，省掉自己转 UTF-8 的麻烦
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-16LE") != 0 || !f) return out;
    wchar_t buf[4096] = {};
    while (fgetws(buf, 4096, f)) {
        std::wstring s = buf;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        const size_t a = s.find_first_not_of(L" \t");
        if (a == std::wstring::npos) continue;              // 整行空白
        const size_t b = s.find_last_not_of(L" \t");
        s = s.substr(a, b - a + 1);
        if (s.empty() || s[0] == L';') continue;            // 注释行
        // 整行被一对引号包住（从资源管理器「复制文件地址」粘过来的常见形态）
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') {
            s = s.substr(1, s.size() - 2);
        }
        out.push_back(s);
    }
    fclose(f);
    return out;
}

static std::wstring PinnedFilePath() { return ConfigDir() + L"\\pinned.txt"; }

std::vector<std::wstring> LoadPinnedList() {
    return LoadLines(PinnedFilePath());
}

bool SavePinnedList(const std::vector<std::wstring>& paths) {
    // 注释行保留 —— 用户打开文件时得看得懂这是干嘛的。
    // 代价是每次读都要跳过它们，上面 LoadLines 已经统一处理了。
    return WriteLinesAtomic(PinnedFilePath(),
                            {L"; MyDock —— Dock 上要显示的应用（每行一个 .lnk 路径）",
                             L"; 在启动台里对着某个应用点右键，可以「固定到 Dock」或「从 Dock 移除」"},
                            paths);
}

// 2026-10-11 - AI - 固定项排序：交换清单里相邻两行
//
// Dock 图标的顺序就是 pinned.txt 的行序，所以「左移/右移」落到数据层
// 就是交换相邻两行。匹配用 NormalizePath（清单和 key 都过一遍）——
// 8.3 短名、引号这些差异不能再让「明明有这行却找不到」重演。
// 内建格子（@launchpad / @trash / @calendar）是精确匹配，它们本来就没路径。
bool MovePinned(const std::wstring& key, int dir) {
    if (key.empty() || (dir != -1 && dir != 1)) return false;
    std::vector<std::wstring> lines = LoadPinnedList();
    int idx = -1;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::wstring& l = lines[i];
        if (l.empty() || l[0] == L';') continue;
        bool same;
        if (l[0] == L'@' || key[0] == L'@') {
            same = _wcsicmp(l.c_str(), key.c_str()) == 0;
        } else {
            same = NormalizePath(l) == NormalizePath(key);
        }
        if (same) { idx = static_cast<int>(i); break; }
    }
    if (idx < 0) {
        Log(L"排序：清单里没找到 %ls", key.c_str());
        return false;
    }
    const int swap = idx + dir;
    if (swap < 0 || swap >= static_cast<int>(lines.size())) return false;   // 已在边界
    std::swap(lines[static_cast<size_t>(idx)], lines[static_cast<size_t>(swap)]);
    return SavePinnedList(lines);   // 原子写（WriteLinesAtomic）
}

bool IsPinned(const std::vector<std::wstring>& pinned, const std::wstring& lnkPath) {
    if (lnkPath.empty()) return false;
    for (const std::wstring& p : pinned) {
        if (_wcsicmp(p.c_str(), lnkPath.c_str()) == 0) return true;
    }
    return false;
}

// ---- 「用户从启动台里拿掉的应用」 ----
// 存在 %APPDATA%\MyDock\hidden.txt，格式和 pinned.txt 一样（每行一个 .lnk 路径）。
// ★ 和 pinned.txt 是两件完全不同的事，别搞混：
//     pinned.txt = 「Dock 上要显示谁」——不移出启动台，只是没上 Dock；
//     hidden.txt = 「启动台里不要出现谁」——从启动台整个消失。
//   所以移出启动台时**不能**顺手改 pinned，否则连 Dock 上的图标也一起没了，
//   用户以为只是藏起来，结果 Dock 上也没了。
static std::wstring HiddenFilePath() { return ConfigDir() + L"\\hidden.txt"; }

std::vector<std::wstring> LoadHiddenList() {
    return LoadLines(HiddenFilePath());
}

bool SaveHiddenList(const std::vector<std::wstring>& paths) {
    return WriteLinesAtomic(HiddenFilePath(),
                            {L"; MyDock —— 从启动台移除的应用（每行一个 .lnk 路径）",
                             L"; 在启动台里对着某个应用点右键可以「移出启动台」，"
                             L"移错了就把这行删掉或加个分号注释掉"},
                            paths);
}

bool IsHidden(const std::vector<std::wstring>& hidden, const std::wstring& lnkPath) {
    if (lnkPath.empty()) return false;
    for (const std::wstring& p : hidden) {
        if (_wcsicmp(p.c_str(), lnkPath.c_str()) == 0) return true;
    }
    return false;
}

// ---- 「用户手动加进启动台的应用」 ----
static std::wstring AddedFilePath() { return ConfigDir() + L"\\added.txt"; }

std::vector<std::wstring> LoadAddedList() {
    return LoadLines(AddedFilePath());
}

bool SaveAddedList(const std::vector<std::wstring>& paths) {
    return WriteLinesAtomic(AddedFilePath(),
                            {L"; MyDock —— 手动加进启动台的应用（每行一个 .exe / .lnk 路径）",
                             L"; 在启动台左上角点「+」添加；对着某个应用右键「移出启动台」可以删掉"},
                            paths);
}

bool AddAppPath(const std::wstring& path) {
    if (path.empty()) return false;
    std::vector<std::wstring> list = LoadAddedList();
    for (const std::wstring& p : list) {
        if (_wcsicmp(p.c_str(), path.c_str()) == 0) return false;   // 已经记过了
    }
    list.push_back(path);
    return SaveAddedList(list);
}

AppItem MakeAppFromFile(const std::wstring& path) {
    AppItem it;
    size_t slash = path.find_last_of(L"\\/");
    std::wstring stem = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);
    it.name = stem.empty() ? path : stem;

    // ★ lnkPath 这里是「身份」而不是字面意义的快捷方式：pinned / hidden 两份清单
    //   都拿它当主键，所以 .exe 也要填上，否则手动加进来的东西没法上 Dock。
    it.lnkPath = path;

    // .lnk 得解出真正的 exe —— 不然「已经在运行就切过去」这条永远不成立，
    // 每点一次都会再开一个窗口。
    if (_wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0) {
        std::wstring t, wd;
        ResolveLnk(path, t, wd);
        it.target = t.empty() ? path : t;
        it.workDir = wd;
    } else {
        it.target = path;
        // 直接给 .exe 的时候没有快捷方式可问，就让它自己在自己家里启动
        it.workDir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash);
    }
    return it;
}

// ---------------------------------------------------------------- 内建格子
// 2026-10-11 - AI - 启动台 / 废纸篓 / 日历：名字、清单写法、图标
namespace {

struct SpecialDef {
    AppKind kind;
    const wchar_t* line;    // 清单里那一行的写法
    const wchar_t* name;    // Dock 上鼠标悬停时显示的名字
};

const SpecialDef kSpecials[] = {
    {AppKind::Launchpad, L"@launchpad", L"启动台"},
    {AppKind::Trash,     L"@trash",     L"废纸篓"},
    {AppKind::Calendar,  L"@calendar",  L"日历"},
};

// 画一个圆角矩形路径。GDI+ 没有现成的圆角矩形，每次都得自己绕。
void AddRoundRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float rad) {
    const float d = rad * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// 启动台：银灰底上排九个白色圆角方块 —— macOS 那个图标就是这个样子。
Gdiplus::Bitmap* DrawLaunchpadIcon(int size) {
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    Gdiplus::Graphics g(bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    const float s = static_cast<float>(size);
    // 图标本身不占满画布：留一圈空白，跟系统图标的「内边距」看起来才一致
    const float pad = s * 0.055f;
    Gdiplus::RectF box(pad, pad, s - pad * 2.0f, s - pad * 2.0f);
    const float rad = s * 0.20f;

    Gdiplus::GraphicsPath p;
    AddRoundRect(p, box, rad);
    Gdiplus::LinearGradientBrush grad(box, Gdiplus::Color(255, 236, 239, 244),
                                      Gdiplus::Color(255, 176, 182, 191),
                                      Gdiplus::LinearGradientModeVertical);
    g.FillPath(&grad, &p);

    // 九个格子：三行三列，格子和格子之间留缝
    const float inner = s * 0.20f;
    const float gap   = s * 0.055f;
    const float cell  = (s - inner * 2.0f - gap * 2.0f) / 3.0f;
    const float cRad  = cell * 0.26f;
    Gdiplus::SolidBrush white(Gdiplus::Color(252, 255, 255, 255));
    for (int ry = 0; ry < 3; ++ry) {
        for (int cx = 0; cx < 3; ++cx) {
            Gdiplus::RectF c(inner + static_cast<float>(cx) * (cell + gap),
                             inner + static_cast<float>(ry) * (cell + gap), cell, cell);
            Gdiplus::GraphicsPath cp;
            AddRoundRect(cp, c, cRad);
            g.FillPath(&white, &cp);
        }
    }
    return bmp;
}

// 废纸篓：先用系统回收站那张真图标（有空/满两态，跟 Explorer 里看到的一样）。
// 取不到才自己画 —— 没有图标的格子在 Dock 上就是一块空白，必须有退路。
Gdiplus::Bitmap* DrawTrashIcon(int size) {
    Gdiplus::Bitmap* sys = LoadIconFromShellItem(L"shell:RecycleBinFolder");
    if (sys) return sys;

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    Gdiplus::Graphics g(bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    const float s = static_cast<float>(size);
    const float top = s * 0.22f, bot = s * 0.88f;
    const float halfTop = s * 0.30f, halfBot = s * 0.24f;
    const float midX = s * 0.5f;

    // 桶身是个上宽下窄的梯形
    Gdiplus::PointF body[] = {
        {midX - halfTop, top}, {midX + halfTop, top},
        {midX + halfBot, bot}, {midX - halfBot, bot},
    };
    Gdiplus::SolidBrush can(Gdiplus::Color(255, 214, 220, 228));
    Gdiplus::Pen edge(Gdiplus::Color(255, 150, 158, 170), s * 0.022f);
    g.FillPolygon(&can, body, 4);
    g.DrawPolygon(&edge, body, 4);

    // 竖条纹
    Gdiplus::Pen rib(Gdiplus::Color(120, 150, 158, 170), s * 0.016f);
    for (int i = 1; i <= 3; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        const float x = (midX - halfTop) + (halfTop * 2.0f) * t;
        g.DrawLine(&rib, x, top + s * 0.05f, midX + (x - midX) * 0.8f, bot - s * 0.03f);
    }

    // 盖子 + 把手
    Gdiplus::RectF lid(midX - s * 0.34f, s * 0.14f, s * 0.68f, s * 0.075f);
    Gdiplus::SolidBrush lb(Gdiplus::Color(255, 190, 198, 210));
    g.FillRectangle(&lb, lid);
    g.DrawRectangle(&edge, lid);
    Gdiplus::RectF handle(midX - s * 0.09f, s * 0.075f, s * 0.18f, s * 0.06f);
    g.DrawRectangle(&edge, handle);
    return bmp;
}

// 日历：仿 macOS 那个 —— 白底、顶上一条红、中间是今天的日期。
Gdiplus::Bitmap* DrawCalendarIcon(int size) {
    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(size, size, PixelFormat32bppARGB);
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) { delete bmp; return nullptr; }
    Gdiplus::Graphics g(bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    const float s = static_cast<float>(size);
    const float pad = s * 0.055f;
    Gdiplus::RectF box(pad, pad, s - pad * 2.0f, s - pad * 2.0f);
    const float rad = s * 0.18f;

    Gdiplus::GraphicsPath p;
    AddRoundRect(p, box, rad);
    Gdiplus::SolidBrush paper(Gdiplus::Color(255, 252, 252, 250));
    g.FillPath(&paper, &p);

    // 顶部红条：只画在圆角矩形内部，所以要拿路径裁一下
    Gdiplus::RectF red(box.X, box.Y, box.Width, box.Height * 0.235f);
    g.SetClip(&p);
    Gdiplus::SolidBrush rb(Gdiplus::Color(255, 226, 62, 62));
    g.FillRectangle(&rb, red);
    g.ResetClip();
    Gdiplus::Pen border(Gdiplus::Color(90, 0, 0, 0), s * 0.014f);
    g.DrawPath(&border, &p);

    // 今天的日子
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    const std::wstring day = std::to_wstring(st.wDay);

    Gdiplus::FontFamily fam(L"Segoe UI");
    Gdiplus::Font font(&fam, box.Height * 0.46f, Gdiplus::FontStyleBold,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF text(box.X, box.Y + box.Height * 0.24f, box.Width, box.Height * 0.62f);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 70, 70, 74));
    g.DrawString(day.c_str(), -1, &font, text, &sf, &ink);
    return bmp;
}

}  // namespace

std::wstring SpecialLine(AppKind k) {
    for (const SpecialDef& d : kSpecials) {
        if (d.kind == k) return d.line;
    }
    return std::wstring();
}

AppKind KindFromLine(const std::wstring& line) {
    if (line.empty() || line[0] != L'@') return AppKind::File;
    for (const SpecialDef& d : kSpecials) {
        if (_wcsicmp(line.c_str(), d.line) == 0) return d.kind;
    }
    return AppKind::File;
}

bool IsSpecialLine(const std::wstring& line) {
    return KindFromLine(line) != AppKind::File;
}

AppItem MakeSpecialItem(AppKind k) {
    AppItem it;
    it.kind = k;
    for (const SpecialDef& d : kSpecials) {
        if (d.kind == k) it.name = d.name;
    }
    switch (k) {
        case AppKind::Launchpad: it.icon = DrawLaunchpadIcon(256); break;
        case AppKind::Trash:     it.icon = DrawTrashIcon(256);     break;
        case AppKind::Calendar:  it.icon = DrawCalendarIcon(256);  break;
        default: break;
    }
    if (!it.icon) Log(L"内建格子 %ls 的图标没画出来", it.name.c_str());
    return it;
}

}  // namespace mydock
