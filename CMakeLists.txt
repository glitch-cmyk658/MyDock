# 2026-10-09 - AI - MyDock 构建脚本（MinGW / MSVC 都可用）
cmake_minimum_required(VERSION 3.16)
project(MyDock LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MyDock WIN32
    src/main.cpp
    src/util.cpp
    src/apps.cpp
    src/dock.cpp
    src/launcher.cpp
    src/appaction.cpp
    src/recents.cpp
    src/menu.cpp
    src/topbar.cpp
    src/status.cpp
    src/svgicon.cpp
    src/flyout.cpp
    src/ccenter.cpp
)

target_compile_definitions(MyDock PRIVATE
    UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)

# iphlpapi/wlanapi/bthprops：顶部栏右侧那排状态图标要用
# （电池走 user32，音量走 COM，Wi-Fi 走 wlanapi，蓝牙走 bthprops）
target_link_libraries(MyDock PRIVATE
    user32 gdi32 gdiplus shell32 ole32 oleaut32 comctl32 shlwapi dwmapi uuid winmm imm32
    iphlpapi wlanapi bthprops
    version)   # 2026-10-11 - AI - 读 exe 里的 FileDescription，运行区的显示名要用

if(MSVC)
    target_compile_options(MyDock PRIVATE /utf-8 /W4)
else()
    target_compile_options(MyDock PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
    target_link_options(MyDock PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
