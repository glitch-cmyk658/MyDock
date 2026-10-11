@echo off
rem 2026-10-10 - AI - 一键编译 MyDock（MinGW）。用法：双击，或在终端里跑 build.cmd
rem
rem ★★ -mwindows 绝对不能漏 ★★
rem
rem   漏掉它，链接器会按「控制台子系统」出 exe，后果有两个，都很难一眼看出关联：
rem
rem     1. 每次启动都会带一个黑底控制台窗口（用户 2026-10-10 报的"连续弹出终端黑窗"）。
rem        程序本身是 wWinMain，用不着控制台 —— 这个窗口是白送的。
rem     2. DPI 感知会被"启动它的那个进程"污染。从终端启动时，终端已经是一个
rem        固定的 DPI 上下文，我们的 SetProcessDpiAwarenessContext 会静默失败，
rem        GetSystemMetrics 开始返回逻辑像素：175% 的屏上拿到 1463 而不是 2560，
rem        画好的位图被系统再拉伸一遍 —— 图标和字全糊。
rem        （main.cpp 里那条注释记的就是这个现象，根因在这里。）
rem
rem   用 CMake 构建时不用操心：CMakeLists.txt 里的 add_executable(MyDock WIN32 ...)
rem   就是同一件事。这条命令是手写的，所以必须自己带上。

set SRC=src\main.cpp src\util.cpp src\apps.cpp src\dock.cpp src\launcher.cpp src\appaction.cpp src\recents.cpp src\menu.cpp src\topbar.cpp src\status.cpp src\svgicon.cpp
rem -lversion: 读 exe 里的 FileDescription（Dock 运行区的显示名），2026-10-11 加
set LIBS=-luser32 -lgdi32 -lgdiplus -lshell32 -lole32 -loleaut32 -lcomctl32 -lshlwapi -ldwmapi -luuid -lwinmm -limm32 -liphlpapi -lwlanapi -lbthprops -lversion

rem 2026-10-11 - 资源（exe 图标 + 版本信息）：windres 编成 .o 一起链进去
windres mydock.rc -O coff -o mydock_res.o
if errorlevel 1 (
    echo.
    echo [失败] 资源编译有错（windres），看上面的输出
    exit /b 1
)

g++ -std=c++17 -O2 -municode -static -mwindows ^
    -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
    %SRC% mydock_res.o -o MyDock.exe %LIBS%

if errorlevel 1 (
    echo.
    echo [失败] 编译有错，看上面的输出
    exit /b 1
)

echo.
echo [完成] MyDock.exe
objdump -p MyDock.exe | findstr /i "Subsystem"
echo   上面必须显示 (Windows GUI)。显示 (Windows CUI) 说明 -mwindows 没生效。
