@echo off
rem ===========================================================================
rem  FB2 Converter - build script.
rem  MinGW-w64 i686 (GCC 8.1.0), static linking, Windows XP SP3 compatible.
rem  Output: build\fb2conv.exe - one portable exe: window without switches,
rem  console mode with switches (-f, -o, -h ...).
rem
rem  This file is intentionally ASCII-only with CRLF line endings:
rem  cmd.exe misparses UTF-8 batch files with LF endings.
rem ===========================================================================
setlocal
cd /d "%~dp0"

set MINGW=C:\dev\mingw32
if not exist "%MINGW%\bin\g++.exe" (
    echo Compiler not found: %MINGW%\bin\g++.exe
    echo Install MinGW-w64 i686 to C:\dev\mingw32 or edit MINGW in this file.
    exit /b 1
)
set PATH=%MINGW%\bin;%PATH%

set SRC=src
set OUT=build
set CXXFLAGS=-m32 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -DUNICODE -D_UNICODE -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare
set LDFLAGS=-static -static-libgcc -static-libstdc++ -Wl,--gc-sections -s
set SOURCES=%SRC%\util.cpp %SRC%\markup.cpp %SRC%\zip.cpp %SRC%\book.cpp %SRC%\convert.cpp %SRC%\readers.cpp %SRC%\rtf.cpp %SRC%\pdf.cpp %SRC%\doc.cpp %SRC%\djvu.cpp %SRC%\writers.cpp %SRC%\notrace.cpp %SRC%\gui.cpp %SRC%\main.cpp

if not exist "%OUT%" mkdir "%OUT%"
if exist "%OUT%\fb2conv-cli.exe" del "%OUT%\fb2conv-cli.exe"
rem a failed build must not leave the previous exe behind to be tested by mistake
if exist "%OUT%\fb2conv.exe" del "%OUT%\fb2conv.exe"

echo === resources ===
windres -I %SRC% -O coff %SRC%\app.rc %OUT%\app.res
if errorlevel 1 goto fail

echo === fb2conv.exe ===
g++ %CXXFLAGS% -mwindows %SOURCES% %OUT%\app.res -o %OUT%\fb2conv.exe %LDFLAGS% -lz -lole32 -lshell32 -lshlwapi -ladvapi32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32
if errorlevel 1 goto fail

echo.
echo Build OK:
for %%f in (%OUT%\fb2conv.exe) do echo   %%~ff  %%~zf bytes
exit /b 0

:fail
echo.
echo BUILD FAILED.
exit /b 1
