@echo off
rem 2026-10-12 - AI - run pure-logic unit tests (NormalizePath / UriDecode / SplitPath)
rem
rem Console subsystem (no -mwindows) so results print to the terminal.
rem tests.cpp unity-includes util.cpp + recents.cpp to reach the anonymous
rem namespace helpers without changing visibility.
rem Non-zero exit on failure, so this can hook into CI later.

setlocal
cd /d "%~dp0"

windres mydock.rc -O coff -o mydock_res.o
g++ -std=c++17 -O1 -static -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 tests\tests.cpp mydock_res.o -o tests.exe -luser32 -lgdi32 -lgdiplus -lshell32 -lole32 -loleaut32 -lshlwapi -luuid -lversion -ladvapi32
if errorlevel 1 (
    echo [FAIL] test build error
    exit /b 1
)

tests.exe
set RC=%ERRORLEVEL%
del /q tests.exe mydock_res.o >nul 2>&1
if not "%RC%"=="0" echo [FAIL] some assertions failed
exit /b %RC%
