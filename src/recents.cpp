// 2026-10-11 - AI - 读 VSCode 的真实工作区历史，做成 Dock 左键菜单里的「最近的项目」
#include "recents.h"

#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "util.h"

namespace mydock {
namespace {

// ---- 小工具 ----

std::wstring Stem(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"/\\");
    const std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    return (dot == std::wstring::npos) ? name : name.substr(0, dot);
}

std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"/\\");
    return (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash);
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

std::wstring RoamingAppData() {
    wchar_t* p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        std::wstring r = p;
        CoTaskMemFree(p);
        return r;
    }
    const wchar_t* e = _wgetenv(L"APPDATA");
    return e ? e : std::wstring();
}

// ---- URI 解码 ----

int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// %E6%B1%87 → 汇
//
// ★ 百分号后面跟的是**字节**，不是一个字符。
//   直接 (wchar_t)0xE6 会得到一个八竿子打不着的汉字，中文目录名会全乱。
//   所以先把整条串还原成 UTF-8 字节流，最后一次性转宽字符。
std::wstring UriDecode(const std::wstring& s) {
    std::string bytes;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            const int hi = HexVal(s[i + 1]);
            const int lo = HexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                bytes.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        if (s[i] < 128) {
            bytes.push_back(static_cast<char>(s[i]));
        } else {
            wchar_t w = s[i];
            char buf[8] = {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, &w, 1, buf, 8, nullptr, nullptr);
            if (n > 0) bytes.append(buf, static_cast<size_t>(n));
        }
    }
    if (bytes.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                      static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        &out[0], n);
    return out;
}

std::vector<std::wstring> SplitPath(const std::wstring& p) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (const wchar_t c : p) {
        if (c == L'/' || c == L'\\') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---- 一条工作区记录 ----

struct Entry {
    std::wstring uri;      // 原始的，原样喂给 --folder-uri
    std::wstring path;     // 解码后的可读路径
    std::wstring tag;      // 远程类型（wsl / ssh-remote…）；本地为空
    std::wstring label;    // 消歧之后显示出来的名字
    FILETIME ft = {};      // workspace.json 的修改时间 = 这个工作区最后一次被用到
};

// file:///d%3A/x/y   →  路径 /d:/x/y，tag 空
// vscode-remote://wsl%2Bubuntu/mnt/d/x  →  路径 /mnt/d/x，tag "wsl"
bool SplitUri(const std::wstring& uri, std::wstring& pathOut, std::wstring& tagOut) {
    const size_t k = uri.find(L"://");
    if (k == std::wstring::npos) return false;
    const bool remote = (uri.substr(0, k) != L"file");
    std::wstring rest = uri.substr(k + 3);

    const size_t first = rest.find(L'/');
    if (first == std::wstring::npos) return false;
    if (remote) {
        // 先跳过 wsl+ubuntu 这一段 authority，后面才是真路径
        const size_t second = rest.find(L'/', first + 1);
        if (second == std::wstring::npos) return false;
        std::wstring auth = UriDecode(rest.substr(0, second));
        const size_t plus = auth.find(L'+');
        tagOut = (plus == std::wstring::npos) ? auth : auth.substr(0, plus);
        rest = rest.substr(second);
    } else {
        rest = rest.substr(first);
    }
    pathOut = UriDecode(rest);
    return !pathOut.empty();
}

// 从最后一段开始显示，撞名了就多带一段，最多带三段。
//
// ★ C:\...\C++ code\C++\algorithm 和 WSL 里的同名目录，末段都是 algorithm ——
//   只显示末段的话，菜单上会并排出现两个一模一样的「algorithm」，点了才知道是哪个。
void PickLabels(std::vector<Entry>& es) {
    for (int depth = 1; depth <= 3; ++depth) {
        std::vector<std::wstring> names;
        names.reserve(es.size());
        for (const Entry& e : es) {
            const std::vector<std::wstring> seg = SplitPath(e.path);
            std::wstring n;
            const int from = std::max(0, static_cast<int>(seg.size()) - depth);
            for (size_t i = static_cast<size_t>(from); i < seg.size(); ++i) {
                if (!n.empty()) n += L'\\';
                n += seg[i];
            }
            if (!e.tag.empty()) n = L"[" + e.tag + L"] " + n;
            names.push_back(n);
        }
        std::vector<std::wstring> sorted = names;
        std::sort(sorted.begin(), sorted.end());
        bool dup = false;
        for (size_t i = 1; i < sorted.size(); ++i) {
            if (sorted[i] == sorted[i - 1]) { dup = true; break; }
        }
        for (size_t i = 0; i < es.size(); ++i) es[i].label = names[i];
        if (!dup) return;
    }
    // 带三段还重名就随它去，用户看完整路径也认得出
}

// ---- 读 VSCode 的 workspaceStorage ----

std::wstring ReadTextFileUtf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz = {};
    std::string bytes;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart < 4 * 1024 * 1024) {
        bytes.resize(static_cast<size_t>(sz.QuadPart));
        DWORD got = 0;
        ReadFile(h, &bytes[0], static_cast<DWORD>(bytes.size()), &got, nullptr);
        bytes.resize(got);
    }
    CloseHandle(h);
    if (bytes.empty()) return {};
    // ★ 去掉 BOM：VSCode 有些 json 带、有些不带，留着会让下面的 find 全部失手。
    //   偏移要在算长度**之前**扣掉 —— 先按全长算 n、再少喂几个字节，
    //   出来的字符串末尾会挂一个多余的 L'\0'。
    size_t off = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        off = 3;
    }
    const char* p = bytes.data() + off;
    const int nb = static_cast<int>(bytes.size() - off);
    const int n = MultiByteToWideChar(CP_UTF8, 0, p, nb, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, nb, &out[0], n);
    return out;
}

// workspace.json 长这样：{"folder":"file:///d%3A/x/y"}
// 多根工作区则是：{"workspace":"file:///.../foo.code-workspace"}
//
// ★ 不引第三方 json 库、也不写完整解析器：这里只需要抠一个字符串值出来。
std::wstring ExtractUri(const std::wstring& text) {
    const wchar_t* keys[] = {L"folder", L"workspace"};
    for (const wchar_t* key : keys) {
        const std::wstring pat = std::wstring(L"\"") + key + L"\"";
        size_t p = text.find(pat);
        if (p == std::wstring::npos) continue;
        p = text.find(L':', p + pat.size());
        if (p == std::wstring::npos) continue;
        p = text.find(L'"', p);
        if (p == std::wstring::npos) continue;
        const size_t q = text.find(L'"', p + 1);
        if (q == std::wstring::npos) continue;
        return text.substr(p + 1, q - p - 1);
    }
    return {};
}

// Code.exe → %APPDATA%\Code ；Code - Insiders.exe → %APPDATA%\Code - Insiders
bool VsCodeUserDir(const std::wstring& exePath, std::wstring& out) {
    const std::wstring stem = Lower(Stem(exePath));
    std::wstring leaf;
    if (stem == L"code") {
        leaf = L"Code";
    } else if (stem == L"code - insiders") {
        leaf = L"Code - Insiders";
    } else {
        return false;
    }
    const std::wstring appdata = RoamingAppData();
    if (appdata.empty()) return false;
    out = appdata + L"\\" + leaf;
    return true;
}

std::vector<Entry> ScanVsCode(const std::wstring& userDir) {
    std::vector<Entry> out;
    const std::wstring base = userDir + L"\\User\\workspaceStorage";
    const std::wstring pattern = base + L"\\*";

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"最近项目：找不到 %ls（VSCode 数据目录不在预期位置）", base.c_str());
        return out;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;

        const std::wstring dir = base + L"\\" + fd.cFileName;
        const std::wstring wj = dir + L"\\workspace.json";

        std::wstring uri = ExtractUri(ReadTextFileUtf8(wj));
        if (uri.empty()) continue;

        Entry e;
        e.uri = uri;
        if (!SplitUri(uri, e.path, e.tag)) continue;

        // ★ 用 workspace.json 自己的修改时间，不是目录的。
        //   VSCode 关掉一个窗口时会回写这个文件，目录时间反而会被别的子目录带着动。
        WIN32_FILE_ATTRIBUTE_DATA ad = {};
        if (GetFileAttributesExW(wj.c_str(), GetFileExInfoStandard, &ad)) {
            e.ft = ad.ftLastWriteTime;
        }
        out.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        FILETIME x = a.ft, y = b.ft;
        return CompareFileTime(&x, &y) > 0;   // 最近用过的在前
    });
    return out;
}

}  // namespace

std::vector<AppExtra> ExtrasForApp(const AppItem& app, int maxRecents) {
    std::vector<AppExtra> out;
    if (app.target.empty()) return out;      // 认不出 exe 就没有参数可传（UWP 之类）

    std::wstring userDir;
    if (!VsCodeUserDir(app.target, userDir)) return out;   // 不是 VSCode 家族

    AppExtra nw;
    nw.label = L"新建窗口";
    nw.args = L"--new-window";
    nw.group = 0;
    out.push_back(std::move(nw));

    std::vector<Entry> es = ScanVsCode(userDir);
    if (es.empty()) return out;              // 只有「新建窗口」，也够用

    if (maxRecents > 0 && es.size() > static_cast<size_t>(maxRecents)) {
        es.resize(static_cast<size_t>(maxRecents));
    }
    PickLabels(es);

    for (const Entry& e : es) {
        AppExtra it;
        it.label = e.label;
        // ★ --reuse-window：让已经在跑的 VSCode 切过去，而不是每点一次多一个窗口。
        //   它没在跑的时候这个参数同样成立（会新建一个窗口打开这个目录）。
        // ★ uri 原样传，本地和 WSL 远程都是同一个入口。
        it.args = L"--reuse-window --folder-uri \"" + e.uri + L"\"";
        it.group = 1;
        out.push_back(std::move(it));
    }

    Log(L"最近项目：%ls 读到 %zu 项", app.name.c_str(), es.size());
    return out;
}

bool RunExtra(const AppItem& app, const std::vector<AppExtra>& extras, int i) {
    if (i < 0 || i >= static_cast<int>(extras.size())) return false;
    if (app.target.empty()) return false;

    // ★ 工作目录不能传 nullptr：那样新进程继承的是 MyDock 自己的目录。
    //   清单里记的「起始位置」没有时，退到 exe 所在目录。
    std::wstring dir = app.workDir;
    if (dir.empty()) dir = DirOf(app.target);

    const HINSTANCE h = ShellExecuteW(nullptr, L"open", app.target.c_str(),
                                      extras[static_cast<size_t>(i)].args.c_str(),
                                      dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        Log(L"最近项目：启动失败 %ls %ls（代码 %lld）", app.target.c_str(),
            extras[static_cast<size_t>(i)].args.c_str(),
            static_cast<long long>(reinterpret_cast<INT_PTR>(h)));
        return false;
    }
    Log(L"最近项目：%ls → %ls", app.name.c_str(),
        extras[static_cast<size_t>(i)].label.c_str());
    return true;
}

}  // namespace mydock
