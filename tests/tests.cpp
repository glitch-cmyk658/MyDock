// 2026-10-12 - AI - 纯逻辑单元测试（NormalizePath / UriDecode / SplitPath）
//
// 为什么存在：GUI 程序的验证一直靠 MYDOCK_TRACE 埋点 + 人工录屏，
// 但清单匹配和 URI 解码这些纯函数的边界完全可以自动化 —— 而且它们
// 恰恰是出过事故的地方（8.3 短名、引号、UTF-8 百分号解码都翻过车）。
//
// 结构：直接 #include 源文件（unity build），这样 recents.cpp 匿名
// namespace 里的 UriDecode / SplitPath 也能测到，不必为测试改可见性。
// 用控制台子系统编译运行（tests.cmd），不碰任何窗口/GUI 初始化。

#include "../src/util.cpp"
#include "../src/recents.cpp"

#include <cstdio>
#include <io.h>
#include <fcntl.h>

static int g_failed = 0;
static int g_total = 0;

static std::wstring WLower(std::wstring s) {
    for (auto& c : s) if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

#define CHECK_EQ(actual, expect)                                            \
    do {                                                                    \
        ++g_total;                                                          \
        const std::wstring a_ = (actual), e_ = (expect);                    \
        if (a_ != e_) {                                                     \
            ++g_failed;                                                     \
            wprintf(L"FAIL %s:%d\n  实际: [%s]\n  期望: [%s]\n",             \
                    __FILE__, __LINE__, a_.c_str(), e_.c_str());            \
        }                                                                   \
    } while (0)

#define CHECK_TRUE(cond, msg)                                               \
    do {                                                                    \
        ++g_total;                                                          \
        if (!(cond)) { ++g_failed; wprintf(L"FAIL %s\n", msg); }            \
    } while (0)

static void TestNormalizePath() {
    wprintf(L"-- NormalizePath --\n");

    // 引号与空白：资源管理器「复制文件地址」的形态
    CHECK_EQ(mydock::NormalizePath(L"  \"C:\\Foo\\Bar.exe\" "),
             L"c:\\foo\\bar.exe");

    // \\?\ 前缀：摘掉、末尾反斜杠去掉
    CHECK_EQ(mydock::NormalizePath(L"\\\\?\\C:\\Foo\\Bar\\"),
             L"c:\\foo\\bar");

    // 正斜杠统一 + 大小写归一
    CHECK_EQ(mydock::NormalizePath(L"C:/Foo/Bar/"),
             L"c:\\foo\\bar");

    // 空串 / 纯空白 → 空串（不能崩、不能返回垃圾）
    CHECK_EQ(mydock::NormalizePath(L""), L"");
    CHECK_EQ(mydock::NormalizePath(L"   \t "), L"");
    CHECK_EQ(mydock::NormalizePath(L"\"\"''"), L"");

    // 相对路径 → 绝对路径（带上盘符），且以当前目录为基准
    const std::wstring rel = mydock::NormalizePath(L"some_app.exe");
    CHECK_TRUE(rel.size() > 2 && rel[1] == L':', "相对路径应变绝对");
    CHECK_TRUE(WLower(rel).rfind(L"some_app.exe") != std::wstring::npos,
               "相对路径应保留文件名");

    // 根目录的反斜杠必须保留（长度下限保护）
    CHECK_EQ(mydock::NormalizePath(L"c:\\\\"), L"c:\\");

    // 8.3 短名：只有卷上真有短名时才断言（NTFS 默认有，个别系统关了）
    wchar_t lng[512] = {};
    if (GetLongPathNameW(L"C:\\PROGRA~1", lng, 512) > 0) {
        CHECK_EQ(WLower(mydock::NormalizePath(L"C:\\PROGRA~1")), WLower(lng));
    }
}

static void TestUriDecode() {
    wprintf(L"-- UriDecode --\n");

    // 基本百分号
    CHECK_EQ(mydock::UriDecode(L"file:///C:/Program%20Files/x"),
             L"file:///C:/Program Files/x");

    // UTF-8 多字节：D:/黑海（微信真实路径，E9 BB 91 = 黑，E6 B5 B7 = 海）
    CHECK_EQ(mydock::UriDecode(L"D:/%E9%BB%91%E6%B5%B7/clash-verge.exe"),
             L"D:/黑海/clash-verge.exe");

    // 末尾孤立的 %：不是转义序列，原样保留（不能越界、不能吞字）
    CHECK_EQ(mydock::UriDecode(L"100%"), L"100%");
    CHECK_EQ(mydock::UriDecode(L"a%2"), L"a%2");      // 只有一个 hex 位
    CHECK_EQ(mydock::UriDecode(L"a%zz"), L"a%zz");    // 两个都不是 hex

    // 非 ASCII 原字符：先转 UTF-8 再整体解码回来，应当无损
    CHECK_EQ(mydock::UriDecode(L"D:\\黑海"), L"D:\\黑海");

    // 空串
    CHECK_EQ(mydock::UriDecode(L""), L"");
}

static void TestSplitPath() {
    wprintf(L"-- SplitPath --\n");

    // 以 / 分段（VSCode 的 file:// URI 是正斜杠）
    const std::vector<std::wstring> v = mydock::SplitPath(L"D:/projects/asm/lab1");
    CHECK_TRUE(v.size() == 4 &&
               v[0] == L"D:" && v[1] == L"projects" &&
               v[2] == L"asm" && v[3] == L"lab1",
               "按 / 切成四段");

    // 反斜杠混用 + 连续分隔符：不产生空段
    const std::vector<std::wstring> m = mydock::SplitPath(L"D:////a////////b/");
    CHECK_TRUE(m.size() == 3 && m[0] == L"D:" && m[1] == L"a" && m[2] == L"b",
               "混合分隔符与连续分隔符");

    const std::vector<std::wstring> e = mydock::SplitPath(L"");
    CHECK_TRUE(e.empty(), "空路径切出空");
}

int main() {
    _setmode(0, 0x10000);  // _O_U16TEXT，让 wprintf 在控制台正常出中文
    TestNormalizePath();
    TestUriDecode();
    TestSplitPath();
    wprintf(L"\n%d 项断言，%d 失败\n", g_total, g_failed);
    return g_failed == 0 ? 0 : 1;
}
